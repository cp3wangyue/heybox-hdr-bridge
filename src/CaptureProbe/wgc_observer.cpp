// CaptureProbe/wgc_observer.cpp

#include "CaptureProbe/wgc_observer.h"

#include <d3d11.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <wrl/client.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "CaptureProbe/probe_logger.h"

#pragma comment(lib, "windowsapp.lib")

using Microsoft::WRL::ComPtr;
namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgd = winrt::Windows::Graphics::DirectX;

namespace hdrfix {

namespace {

float HalfToFloat(unsigned short h)
{
    unsigned sign = (h >> 15) & 1;
    unsigned exp = (h >> 10) & 0x1F;
    unsigned man = h & 0x3FF;
    float v;
    if (exp == 0) {
        v = static_cast<float>(man) * (1.0f / 16384.0f);
    } else if (exp == 31) {
        v = 65504.0f;
    } else {
        v = (man + 1024.0f) *
            (exp < 25 ? 1.0f / static_cast<float>(1u << (25 - exp))
                      : static_cast<float>(1u << (exp - 25)));
    }
    return sign ? -v : v;
}

void LogEvent(const std::string& tag)
{
    FrameLogRecord r;
    r.tid = ::GetCurrentThreadId();
    r.path = tag;
    r.force = true;
    ProbeLogger::Instance().LogFrame(r);
}

struct Fp16Stats {
    float overWhiteFrac = 0.f;
    float maxChannel = 0.f;
    float meanLuma = 0.f;
};

Fp16Stats StatsFp16(const BYTE* data, UINT rowPitch, UINT w, UINT h)
{
    Fp16Stats out;
    UINT step = (w / 256) | 1;
    UINT64 over = 0, total = 0;
    double sum = 0.0;
    float mx = 0.f;
    for (UINT y = 0; y < h; y += step) {
        const unsigned short* row =
            reinterpret_cast<const unsigned short*>(data + static_cast<size_t>(y) * rowPitch);
        for (UINT x = 0; x < w; x += step) {
            const unsigned short* px = row + x * 4;
            float r = HalfToFloat(px[0]), g = HalfToFloat(px[1]), b = HalfToFloat(px[2]);
            float m = r > g ? (r > b ? r : b) : (g > b ? g : b);
            if (m > mx) mx = m;
            if (m > 1.0f) ++over;
            sum += 0.2126 * r + 0.7152 * g + 0.0722 * b;
            ++total;
        }
    }
    out.overWhiteFrac = total ? static_cast<float>(static_cast<double>(over) / total) : 0.f;
    out.maxChannel = mx;
    out.meanLuma = total ? static_cast<float>(sum / total) : 0.f;
    return out;
}

struct ObserverState {
    std::atomic<bool> running{false};
    std::atomic<bool> started{false};

    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;

    // 帧缓冲（等尺寸纹理池）
    ID3D11Texture2D* copyTex[3] = {nullptr, nullptr, nullptr};
    ID3D11Texture2D* staging = nullptr;
    UINT texW = 0, texH = 0;
    DXGI_FORMAT texFmt = DXGI_FORMAT_UNKNOWN;
    int copyIdx = 0;

    UINT64 frameCount = 0;
    UINT64 copyOps = 0;
    UINT64 statsDone = 0;
    long long lastTs = 0;
    double accDtMs = 0;
    int accFrames = 0;
    double fps = 0;

    void ReleaseGpu()
    {
        for (auto& t : copyTex) {
            if (t) { t->Release(); t = nullptr; }
        }
        if (staging) { staging->Release(); staging = nullptr; }
        texW = texH = 0;
        texFmt = DXGI_FORMAT_UNKNOWN;
    }

    bool EnsureTextures(UINT w, UINT h, DXGI_FORMAT fmt)
    {
        if (texW == w && texH == h && texFmt == fmt && copyTex[0]) return true;
        ReleaseGpu();
        D3D11_TEXTURE2D_DESC d{};
        d.Width = w; d.Height = h;
        d.MipLevels = 1; d.ArraySize = 1;
        d.Format = fmt;
        d.SampleDesc.Count = 1;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        for (auto& t : copyTex) {
            if (FAILED(device->CreateTexture2D(&d, nullptr, &t))) return false;
        }
        d.BindFlags = 0;
        d.Usage = D3D11_USAGE_STAGING;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device->CreateTexture2D(&d, nullptr, &staging))) return false;
        texW = w; texH = h; texFmt = fmt;
        return true;
    }
};

void ObserverLoop(ObserverState* st)
{
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    try {
        if (!wgc::GraphicsCaptureSession::IsSupported()) {
            LogEvent("WGC:not supported");
            return;
        }

        // 自有设备（不与 SDK 共享，保证观察路径完全独立）
        ComPtr<ID3D11Device> rawDevice;
        D3D_FEATURE_LEVEL fl{};
        HRESULT hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                         D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                         D3D11_SDK_VERSION, rawDevice.GetAddressOf(), &fl, nullptr);
        if (FAILED(hr)) {
            char buf[96];
            sprintf_s(buf, "WGC:device failed hr=0x%08lX", hr);
            LogEvent(buf);
            return;
        }
        st->device.copy_from(rawDevice.Get());
        st->device->GetImmediateContext(st->context.put());

        ComPtr<IDXGIDevice> dxgiDevice;
        st->device->QueryInterface(IID_PPV_ARGS(dxgiDevice.GetAddressOf()));

        winrt::com_ptr<::IInspectable> inspectable;
        hr = ::CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.Get(), inspectable.put());
        if (FAILED(hr)) {
            LogEvent("WGC:interop device failed");
            return;
        }
        auto d3dDevice = inspectable.as<wgd::Direct3D11::IDirect3DDevice>();

        HMONITOR monitor = ::MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
        auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem,
                                                     ::IGraphicsCaptureItemInterop>();
        wgc::GraphicsCaptureItem item{nullptr};
        winrt::check_hresult(interop->CreateForMonitor(
            monitor, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(item)));

        auto size = item.Size();
        auto pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
            d3dDevice,
            static_cast<wgd::DirectXPixelFormat>(DXGI_FORMAT_R16G16B16A16_FLOAT), 3, size);
        auto session = pool.CreateCaptureSession(item);
        session.StartCapture();

        char buf[160];
        sprintf_s(buf, "WGC:ObserverStarted pool=FP16 %dx%d", size.Width, size.Height);
        LogEvent(buf);
        st->started = true;

        auto lastSizeCheck = std::chrono::steady_clock::now();
        auto lastStats = std::chrono::steady_clock::time_point::min();

        while (st->running) {
            wgc::Direct3D11CaptureFrame frame{nullptr};
            try {
                frame = pool.TryGetNextFrame();
            } catch (...) {
                ::Sleep(50);
                continue;
            }
            if (!frame) {
                ::Sleep(8);
                continue;
            }
            ++st->frameCount;

            // fps（SystemRelativeTime，100ns tick）
            long long ts = frame.SystemRelativeTime().count();
            if (st->lastTs > 0) {
                double dtMs = (ts - st->lastTs) / 10000.0;
                if (dtMs > 0 && dtMs < 1000) {
                    st->accDtMs += dtMs;
                    ++st->accFrames;
                    if (st->accFrames >= 60) {
                        st->fps = st->accFrames * 1000.0 / st->accDtMs;
                        st->accFrames = 0;
                        st->accDtMs = 0;
                    }
                }
            }
            st->lastTs = ts;

            // 帧纹理
            winrt::com_ptr<ID3D11Texture2D> tex;
            try {
                auto access = frame.Surface().as<
                    ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
                winrt::check_hresult(
                    access->GetInterface(__uuidof(ID3D11Texture2D), tex.put_void()));
            } catch (...) {
                frame = nullptr;
                continue;
            }

            D3D11_TEXTURE2D_DESC desc{};
            tex->GetDesc(&desc);
            if (!st->EnsureTextures(desc.Width, desc.Height, desc.Format)) {
                LogEvent("WGC:textures failed");
                break;
            }
            if (st->frameCount == 1 ||
                (st->texW != desc.Width || st->texH != desc.Height)) {
                sprintf_s(buf, "WGC:frame size %ux%u fmt=%d", desc.Width, desc.Height,
                          static_cast<int>(desc.Format));
                LogEvent(buf);
            }

            // GPU CopyResource（每帧，3 张轮转）
            st->context->CopyResource(st->copyTex[st->copyIdx], tex.get());
            st->copyIdx = (st->copyIdx + 1) % 3;
            ++st->copyOps;

            // 节流像素统计（1s）：量化 BGRA 路径丢失的 HDR 数据
            auto now = std::chrono::steady_clock::now();
            if (lastStats == std::chrono::steady_clock::time_point::min() ||
                now - lastStats >= std::chrono::seconds(1)) {
                lastStats = now;
                st->context->CopyResource(st->staging, tex.get());
                D3D11_MAPPED_SUBRESOURCE mapped{};
                if (SUCCEEDED(st->context->Map(st->staging, 0, D3D11_MAP_READ, 0, &mapped))) {
                    auto s = StatsFp16(reinterpret_cast<const BYTE*>(mapped.pData),
                                       mapped.RowPitch, desc.Width, desc.Height);
                    ++st->statsDone;
                    sprintf_s(buf,
                              "WGC:FP16Frame #%llu %ux%u overWhite=%.4f max=%.3f mean=%.4f fps=%.1f copies=%llu",
                              static_cast<unsigned long long>(st->frameCount), desc.Width,
                              desc.Height, s.overWhiteFrac, s.maxChannel, s.meanLuma, st->fps,
                              static_cast<unsigned long long>(st->copyOps));
                    LogEvent(buf);
                    st->context->Unmap(st->staging, 0);
                }
            }

            // 分辨率变化跟踪（每 2s）
            if (now - lastSizeCheck > std::chrono::seconds(2)) {
                lastSizeCheck = now;
                auto ns = item.Size();
                if (ns.Width != size.Width || ns.Height != size.Height) {
                    size = ns;
                    pool.Recreate(d3dDevice,
                                  static_cast<wgd::DirectXPixelFormat>(
                                      DXGI_FORMAT_R16G16B16A16_FLOAT),
                                  3, size);
                    sprintf_s(buf, "WGC:Recreate %dx%d", size.Width, size.Height);
                    LogEvent(buf);
                }
            }

            frame = nullptr; // 及时归还帧
        }

        sprintf_s(buf, "WGC:ObserverStopped frames=%llu copies=%llu stats=%llu fps=%.1f",
                  static_cast<unsigned long long>(st->frameCount),
                  static_cast<unsigned long long>(st->copyOps),
                  static_cast<unsigned long long>(st->statsDone), st->fps);
        LogEvent(buf);

        try {
            session.Close();
            pool.Close();
        } catch (...) {
        }
    } catch (winrt::hresult_error const& e) {
        char buf[160];
        sprintf_s(buf, "WGC:fatal hr=0x%08X %hs", static_cast<unsigned>(e.code().value),
                  winrt::to_string(e.message()).c_str());
        LogEvent(buf);
    } catch (...) {
        LogEvent("WGC:fatal unknown");
    }
    st->ReleaseGpu();
    st->device = nullptr;
    st->context = nullptr;
    winrt::clear_factory_cache();
}

} // namespace

struct WgcObserver::Impl {
    ObserverState state;
    HANDLE thread = nullptr;
};

WgcObserver& WgcObserver::Instance()
{
    static WgcObserver inst;
    return inst;
}

bool WgcObserver::Start()
{
    if (!impl_) impl_ = new Impl();
    if (impl_->state.running.exchange(true)) return true;
    impl_->thread = ::CreateThread(
        nullptr, 0,
        [](LPVOID param) -> DWORD {
            ObserverLoop(reinterpret_cast<ObserverState*>(param));
            return 0;
        },
        &impl_->state, 0, nullptr);
    return impl_->thread != nullptr;
}

void WgcObserver::Stop()
{
    if (!impl_) return;
    impl_->state.running = false;
    if (impl_->thread) {
        if (::WaitForSingleObject(impl_->thread, 8000) == WAIT_OBJECT_0) {
            ::CloseHandle(impl_->thread);
        }
        impl_->thread = nullptr;
    }
}

} // namespace hdrfix
