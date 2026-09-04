// CaptureProbe/d3d11_observer.cpp

#include "CaptureProbe/d3d11_observer.h"

#include "CaptureProbe/iat_hook.h"
#include "CaptureProbe/probe_logger.h"
#include "CaptureProbe/texture_info.h"

#include <cstring>
#include <mutex>
#include <unordered_set>
#include <vector>

namespace hdrfix {

namespace {

constexpr int kCreateTexture2DIndex = 5; // ID3D11Device vtable：QI,AddRef,Release,Buffer,Tex1D,Tex2D
constexpr int kReleaseIndex = 2;
constexpr int kVtableCloneEntries = 128; // ID3D11Device 公有方法 < 64，128 覆盖 11.x 扩展

bool IsInterestingFormat(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_NV12:
    case DXGI_FORMAT_P010:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return true;
    default:
        return false;
    }
}

struct DeviceHookRecord {
    ID3D11Device* device = nullptr;
    bool singleThreaded = false;
    void** clonedVtable = nullptr;
    void** origVtableSlot = nullptr;
    void* origVtable = nullptr;
    void* origCreateTexture2D = nullptr;
    void* origRelease = nullptr;
};

void* g_origD3D11CreateDevice = nullptr; // IAT 中的原始函数
std::mutex g_mutex;
ObserverConfig g_cfg;
std::vector<DeviceHookRecord> g_hooks;
bool g_singleThreadedDeviceSeen = false;

// ---------- 候选纹理注册表（弱引用 + Release 自动回收，杜绝悬垂） ----------

struct RegisteredTexture {
    ID3D11Texture2D* tex = nullptr;
    TextureInfo info{};
};
std::vector<RegisteredTexture> g_registered;
constexpr size_t kMaxRegistered = 16;
std::unordered_set<std::string> g_loggedDescs;
UINT64 g_sampleCount = 0;

void EvictIfPresent(ID3D11Texture2D* dead)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto it = g_registered.begin(); it != g_registered.end(); ++it) {
        if (it->tex == dead) { g_registered.erase(it); break; }
    }
}

void LogTextureCreated(ID3D11Texture2D* tex, const D3D11_TEXTURE2D_DESC* desc)
{
    if (desc->Usage == D3D11_USAGE_STAGING) return; // 探针自身 staging 噪音，不记录
    TextureInfo info;
    info.width = desc->Width; info.height = desc->Height;
    info.format = desc->Format; info.mipLevels = desc->MipLevels; info.arraySize = desc->ArraySize;
    info.sampleDesc = desc->SampleDesc; info.usage = desc->Usage;
    info.bindFlags = desc->BindFlags; info.cpuAccessFlags = desc->CPUAccessFlags;
    info.miscFlags = desc->MiscFlags;

    std::string line = info.ToString();
    bool firstSeen;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        firstSeen = g_loggedDescs.insert(line).second;
        if (firstSeen && g_cfg.minTextureWidth <= desc->Width &&
            g_cfg.minTextureHeight <= desc->Height && IsInterestingFormat(desc->Format)) {
            RegisteredTexture rt;
            rt.tex = tex;
            rt.info = info;
            g_registered.push_back(rt);
            if (g_registered.size() > kMaxRegistered) g_registered.erase(g_registered.begin());
        }
    }
    if (firstSeen) {
        FrameLogRecord r;
        r.tid = ::GetCurrentThreadId();
        r.sourceTexture = tex;
        r.tex = info;
        r.path = "TextureCreated";
        r.force = true; // 去重保护下极少发生，不可被限流吞掉
        ProbeLogger::Instance().LogFrame(r);
    }
}

ULONG STDMETHODCALLTYPE HookRelease(IUnknown* self)
{
    void* orig = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (auto& h : g_hooks) {
            if (reinterpret_cast<IUnknown*>(h.device) == self) { orig = h.origRelease; break; }
        }
    }
    if (!orig) return 0;
    using Fn = ULONG(STDMETHODCALLTYPE*)(IUnknown*);
    ULONG ret = reinterpret_cast<Fn>(orig)(self);
    if (ret == 0) EvictIfPresent(reinterpret_cast<ID3D11Texture2D*>(self));
    return ret;
}

void SafeLogTextureCreated(ID3D11Texture2D* tex, const D3D11_TEXTURE2D_DESC* desc)
{
    __try {
        LogTextureCreated(tex, desc);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // 观察器绝不影响原调用结果
    }
}

HRESULT STDMETHODCALLTYPE HookCreateTexture2D(ID3D11Device* self, const D3D11_TEXTURE2D_DESC* pDesc,
                                              const D3D11_SUBRESOURCE_DATA* pInitialData,
                                              ID3D11Texture2D** ppTexture2D)
{
    void* orig = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (auto& h : g_hooks) {
            if (h.device == self) { orig = h.origCreateTexture2D; break; }
        }
    }
    if (!orig) return E_FAIL;
    using Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*,
                                           const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
    HRESULT hr = reinterpret_cast<Fn>(orig)(self, pDesc, pInitialData, ppTexture2D);
    if (SUCCEEDED(hr) && ppTexture2D && *ppTexture2D && pDesc) {
        SafeLogTextureCreated(*ppTexture2D, pDesc);
    }
    return hr;
}

bool HookDeviceInstance(ID3D11Device* device, bool singleThreaded)
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (auto& h : g_hooks) {
            if (h.device == device) return true;
        }
    }

    void** vtable = *reinterpret_cast<void***>(device);
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(vtable, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
    SIZE_T regionAvail = reinterpret_cast<BYTE*>(mbi.BaseAddress) + mbi.RegionSize -
                         reinterpret_cast<BYTE*>(vtable);
    int entries = static_cast<int>(regionAvail / sizeof(void*));
    if (entries > kVtableCloneEntries) entries = kVtableCloneEntries;
    if (entries <= kCreateTexture2DIndex + 1) return false;

    auto clone = static_cast<void**>(VirtualAlloc(nullptr, entries * sizeof(void*),
                                                  MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!clone) return false;
    memcpy(clone, vtable, entries * sizeof(void*));
    void* origCreate = clone[kCreateTexture2DIndex];
    void* origRelease = clone[kReleaseIndex];
    DWORD oldProtect = 0;
    if (!VirtualProtect(clone, entries * sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        VirtualFree(clone, 0, MEM_RELEASE);
        return false;
    }
    clone[kCreateTexture2DIndex] = &HookCreateTexture2D;
    clone[kReleaseIndex] = &HookRelease;
    VirtualProtect(clone, entries * sizeof(void*), PAGE_EXECUTE_READ, &oldProtect);

    void** slot = reinterpret_cast<void**>(device);
    void* origVtable = *slot;
    *slot = clone; // 8 字节对齐原子写

    DeviceHookRecord rec;
    rec.device = device;
    rec.singleThreaded = singleThreaded;
    rec.clonedVtable = clone;
    rec.origVtableSlot = slot;
    rec.origVtable = origVtable;
    rec.origCreateTexture2D = origCreate;
    rec.origRelease = origRelease;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_hooks.push_back(rec);
        if (singleThreaded) g_singleThreadedDeviceSeen = true;
    }

    FrameLogRecord r;
    r.tid = ::GetCurrentThreadId();
    r.sourceDevice = device;
    char path[128];
    sprintf_s(path, "DeviceHooked vtable=%p clone=%p singleThreaded=%d", origVtable,
              reinterpret_cast<void*>(clone), singleThreaded ? 1 : 0);
    r.path = path;
    r.force = true;
    ProbeLogger::Instance().LogFrame(r);
    return true;
}

void UnhookDevices()
{
    std::vector<DeviceHookRecord> hooks;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        hooks.swap(g_hooks);
    }
    for (auto& h : hooks) {
        *h.origVtableSlot = h.origVtable;
    }
    Sleep(200); // 等待可能正在执行的 hook 调用返回后再释放克隆表
    for (auto& h : hooks) {
        VirtualFree(h.clonedVtable, 0, MEM_RELEASE);
    }
}

HRESULT STDMETHODCALLTYPE HookD3D11CreateDevice(IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE driverType,
                                                HMODULE software, UINT flags,
                                                const D3D_FEATURE_LEVEL* pFeatureLevels, UINT featureLevels,
                                                UINT sdkVersion, ID3D11Device** ppDevice,
                                                D3D_FEATURE_LEVEL* pFeatureLevel,
                                                ID3D11DeviceContext** ppImmediateContext)
{
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
                                           const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**,
                                           D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
    HRESULT hr = reinterpret_cast<Fn>(g_origD3D11CreateDevice)(
        pAdapter, driverType, software, flags, pFeatureLevels, featureLevels, sdkVersion,
        ppDevice, pFeatureLevel, ppImmediateContext);
    if (SUCCEEDED(hr) && ppDevice && *ppDevice) {
        bool singleThreaded = (flags & D3D11_CREATE_DEVICE_SINGLETHREADED) != 0;
        FrameLogRecord r;
        r.tid = ::GetCurrentThreadId();
        r.sourceDevice = *ppDevice;
        char path[160];
        sprintf_s(path, "DeviceCreated flags=0x%X%s featureLevel=0x%04X", flags,
                  singleThreaded ? " SINGLETHREADED" : "",
                  pFeatureLevel ? static_cast<unsigned>(*pFeatureLevel) : 0);
        r.path = path;
        r.force = true;
        ProbeLogger::Instance().LogFrame(r);

        if (!singleThreaded) {
            HookDeviceInstance(*ppDevice, false);
        } else {
            HookDeviceInstance(*ppDevice, true); // 挂上以便记录创建事件；采样器会跳过
        }
    }
    return hr;
}

// ---------- 像素采样 ----------

float HalfToFloat(unsigned short h)
{
    unsigned sign = (h >> 15) & 1;
    unsigned exp = (h >> 10) & 0x1F;
    unsigned man = h & 0x3FF;
    float v;
    if (exp == 0) {
        v = static_cast<float>(man) * (1.0f / 16384.0f); // 2^-24 * man
    } else if (exp == 31) {
        v = 65504.0f; // inf/nan 粗略处理
    } else {
        v = (man + 1024.0f) * (exp - 25.0f < 0
                                   ? 1.0f / (1u << (25 - exp))
                                   : static_cast<float>(1u << (exp - 25)));
    }
    return sign ? -v : v;
}

// 采样核心：无 C++ 析构对象（SEH 兼容）
bool SampleCore(ID3D11Device* device, ID3D11Texture2D* tex, const TextureInfo& info,
                PixelSampleResult* out)
{
    __try {
        ID3D11DeviceContext* ctx = nullptr;
        device->GetImmediateContext(&ctx);
        if (!ctx) return false;

        D3D11_TEXTURE2D_DESC sdesc{};
        sdesc.Width = info.width;
        sdesc.Height = info.height;
        sdesc.MipLevels = 1;
        sdesc.ArraySize = 1;
        sdesc.Format = info.format;
        sdesc.SampleDesc.Count = 1;
        sdesc.Usage = D3D11_USAGE_STAGING;
        sdesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sdesc.BindFlags = 0;

        ID3D11Texture2D* staging = nullptr;
        HRESULT hr = device->CreateTexture2D(&sdesc, nullptr, &staging);
        bool ok = false;
        if (SUCCEEDED(hr) && staging) {
            ctx->CopyResource(staging, tex);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            hr = ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
            if (SUCCEEDED(hr)) {
                out->ok = true;
                out->source = tex;
                out->width = info.width;
                out->height = info.height;
                out->format = info.format;
                ++g_sampleCount;
                out->sampleCount = g_sampleCount;

                const UINT w = info.width, h = info.height;
                if (info.format == DXGI_FORMAT_NV12) {
                    // NV12：Y 平面统计，用于判定 RGB→YUV 的 full/limited range
                    UINT step = (w / 256) | 1;
                    UINT64 over = 0, under = 0, total = 0;
                    double sumY = 0.0;
                    float maxY = 0.f;
                    for (UINT y = 0; y < h; y += step) {
                        const BYTE* row = reinterpret_cast<const BYTE*>(mapped.pData) + y * mapped.RowPitch;
                        for (UINT x = 0; x < w; x += step) {
                            float Y = row[x];
                            if (Y > maxY) maxY = Y;
                            if (Y >= 250.f) ++over;
                            if (Y <= 16.f) ++under;
                            sumY += Y;
                            ++total;
                        }
                    }
                    out->maxChannel = maxY;
                    out->meanLuma = total ? static_cast<float>(sumY / total) : 0.f;
                    out->brightFrac = total ? static_cast<float>(static_cast<double>(over) / total) : 0.f;
                    out->underBlackFrac = total ? static_cast<float>(static_cast<double>(under) / total) : 0.f;
                } else if (info.format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
                    UINT step = (w / 256) | 1;
                    UINT64 over = 0, total = 0;
                    double sumLuma = 0.0;
                    float maxCh = 0.f;
                    for (UINT y = 0; y < h; y += step) {
                        const unsigned short* row =
                            reinterpret_cast<const unsigned short*>(
                                reinterpret_cast<const BYTE*>(mapped.pData) + y * mapped.RowPitch);
                        for (UINT x = 0; x < w; x += step) {
                            const unsigned short* px = row + x * 4;
                            float r = HalfToFloat(px[0]), g = HalfToFloat(px[1]), b = HalfToFloat(px[2]);
                            float mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
                            if (mx > maxCh) maxCh = mx;
                            if (mx > 1.0f) ++over;
                            sumLuma += 0.2126 * r + 0.7152 * g + 0.0722 * b;
                            ++total;
                        }
                    }
                    out->overWhiteFrac = total ? static_cast<float>(static_cast<double>(over) / total) : 0.f;
                    out->maxChannel = maxCh;
                    out->meanLuma = total ? static_cast<float>(sumLuma / total) : 0.f;
                } else {
                    UINT step = (w / 256) | 1;
                    UINT64 over = 0, total = 0;
                    double sumLuma = 0.0;
                    float maxCh = 0.f;
                    for (UINT y = 0; y < h; y += step) {
                        const BYTE* row = reinterpret_cast<const BYTE*>(mapped.pData) + y * mapped.RowPitch;
                        for (UINT x = 0; x < w; x += step) {
                            const BYTE* px = row + x * 4; // BGRA / RGBA 同取前 3 通道
                            float b = px[0], g = px[1], r = px[2];
                            float luma = 0.2126f * r + 0.7152f * g + 0.0722f * b;
                            if (luma > maxCh) maxCh = luma;
                            if (luma >= 250.f) ++over;
                            sumLuma += luma;
                            ++total;
                        }
                    }
                    out->brightFrac = total ? static_cast<float>(static_cast<double>(over) / total) : 0.f;
                    out->maxChannel = maxCh;
                    out->meanLuma = total ? static_cast<float>(sumLuma / total) : 0.f;
                }
                ctx->Unmap(staging, 0);
                ok = true;
            }
            staging->Release();
        }
        ctx->Release();
        return ok;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace

struct D3D11Observer::Impl {
    HMODULE targetModule = nullptr;
    bool iatHooked = false;
};

D3D11Observer& D3D11Observer::Instance()
{
    static D3D11Observer inst;
    return inst;
}

bool D3D11Observer::Start(const ObserverConfig& cfg)
{
    if (!impl_) impl_.reset(new Impl());
    g_cfg = cfg;

    impl_->targetModule = ::GetModuleHandleW(cfg.targetModule.c_str());
    if (!impl_->targetModule) return false;
    HMODULE d3d11 = ::GetModuleHandleW(L"d3d11.dll");
    if (!d3d11) return false;

    void* real = reinterpret_cast<void*>(::GetProcAddress(d3d11, "D3D11CreateDevice"));
    if (!real) return false;
    if (impl_->iatHooked) return true;

    void* previous = real;
    impl_->iatHooked = InstallIATHook(impl_->targetModule, "d3d11.dll", "D3D11CreateDevice",
                                      reinterpret_cast<void*>(&HookD3D11CreateDevice), &previous);
    if (impl_->iatHooked) {
        g_origD3D11CreateDevice = previous; // IAT 原值（= d3d11.dll 导出）
    }
    return impl_->iatHooked;
}

void D3D11Observer::Stop()
{
    if (!impl_ || !impl_->iatHooked) return;
    RemoveIATHook(impl_->targetModule, "d3d11.dll", "D3D11CreateDevice", g_origD3D11CreateDevice);
    impl_->iatHooked = false;
    UnhookDevices();
}

bool D3D11Observer::TakePixelSample(PixelSampleResult* out)
{
    *out = {};
    ID3D11Texture2D* tex = nullptr;
    TextureInfo info;
    bool singleThreaded = false;
    ID3D11Device* device = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_singleThreadedDeviceSeen) return false; // SINGLETHREADED 设备禁止跨线程
        // 选择：偶数次优先 FP16（scRGB 高光证据），其次 BGRA；奇数次优先 NV12（range 判定）
        bool preferNV12 = (g_sampleCount % 2) == 1;
        for (int pass = 0; pass < 3 && !tex; ++pass) {
            for (auto it = g_registered.rbegin(); it != g_registered.rend(); ++it) {
                bool isFP16 = it->info.format == DXGI_FORMAT_R16G16B16A16_FLOAT;
                bool isBGRA = it->info.format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                              it->info.format == DXGI_FORMAT_R8G8B8A8_UNORM;
                bool isNV12 = it->info.format == DXGI_FORMAT_NV12;
                bool pick = (pass == 0 && ((preferNV12 && isNV12) || (!preferNV12 && isFP16))) ||
                            (pass == 1 && ((preferNV12 && isFP16) || (!preferNV12 && isBGRA))) ||
                            (pass == 2 && ((preferNV12 && isBGRA) || (!preferNV12 && isNV12)));
                if (pick) {
                    it->tex->AddRef(); // 双保险（Release 钩子已同步回收注册表）
                    tex = it->tex;
                    info = it->info;
                    break;
                }
            }
        }
        for (auto& h : g_hooks) {
            device = h.device;
            singleThreaded = h.singleThreaded;
        }
    }
    if (!tex || !device || singleThreaded) {
        if (tex) tex->Release();
        return false;
    }

    // 资源归属校验：拿设备的 Debug 接口验证纹理属于该设备代价高，
    // P2 用"注册表与钩子同源"的近似；CopyResource 跨设备会失败并返回 false。
    bool ok = SampleCore(device, tex, info, out);
    if (!ok) {
        // 设备可能不匹配：换下一个已钩设备重试一次
        ID3D11Device* other = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            for (auto& h : g_hooks) {
                if (h.device != device && !h.singleThreaded) { other = h.device; break; }
            }
        }
        if (other) {
            ok = SampleCore(other, tex, info, out);
            if (ok) out->source = tex;
        }
    }
    tex->Release();
    return ok && out->ok;
}

} // namespace hdrfix
