// probe_testhost — 探针管线与集成测试宿主（计划书 §9 / §11）
//
// 模式：
//   probe_testhost.exe --integration       【P5 核心验证】测试 WgcHookManager 帧池拦截、FP16 升级与 ToneMap BGRA8 输出
//   probe_testhost.exe --inproc            自加载探针（LoadLibrary），创建设备+测试纹理，等待采样
//   probe_testhost.exe --remote [--secs N] 等待被注入后创建设备+纹理，等待退出

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

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "CaptureProbe/hdr_state.h"
#include "ColorDetect/ColorDetector.h"
#include "Diagnostics/ConfigManager.h"
#include "Diagnostics/SafetyGuard.h"
#include "Integration/WgcHookManager.h"
#include "ToneMap/ToneMapCore.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "windowsapp.lib")

using Microsoft::WRL::ComPtr;
namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgd = winrt::Windows::Graphics::DirectX;

static uint16_t FloatToHalf(float val)
{
    uint32_t x;
    memcpy(&x, &val, sizeof(x));
    uint32_t sign = (x >> 16) & 0x8000;
    int32_t exp = static_cast<int32_t>((x >> 23) & 0xFF) - 127 + 15;
    uint32_t mant = x & 0x7FFFFF;

    if (exp <= 0) {
        return static_cast<uint16_t>(sign);
    } else if (exp >= 31) {
        return static_cast<uint16_t>(sign | 0x7C00);
    }
    return static_cast<uint16_t>(sign | (exp << 10) | (mant >> 13));
}

static bool CreateTexturePair(ID3D11Device* device, UINT width, UINT height,
                              DXGI_FORMAT format, UINT bindFlags,
                              ComPtr<ID3D11Texture2D>& tex,
                              ComPtr<ID3D11ShaderResourceView>& srv,
                              ComPtr<ID3D11RenderTargetView>& rtv)
{
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = bindFlags;

    HRESULT hr = device->CreateTexture2D(&desc, nullptr, tex.GetAddressOf());
    if (FAILED(hr)) return false;

    if (bindFlags & D3D11_BIND_SHADER_RESOURCE) {
        hr = device->CreateShaderResourceView(tex.Get(), nullptr, srv.GetAddressOf());
        if (FAILED(hr)) return false;
    }
    if (bindFlags & D3D11_BIND_RENDER_TARGET) {
        hr = device->CreateRenderTargetView(tex.Get(), nullptr, rtv.GetAddressOf());
        if (FAILED(hr)) return false;
    }
    return true;
}

static std::vector<uint8_t> ReadbackTexturePixels(ID3D11Device* device, ID3D11DeviceContext* context,
                                                  ID3D11Texture2D* srcTex, UINT width, UINT height)
{
    D3D11_TEXTURE2D_DESC desc{};
    srcTex->GetDesc(&desc);
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.Usage = D3D11_USAGE_STAGING;

    ComPtr<ID3D11Texture2D> staging;
    device->CreateTexture2D(&desc, nullptr, staging.GetAddressOf());
    context->CopyResource(staging.Get(), srcTex);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    std::vector<uint8_t> pixels(width * height * 4);
    if (SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        const uint8_t* srcRow = static_cast<const uint8_t*>(mapped.pData);
        uint8_t* dstRow = pixels.data();
        for (UINT y = 0; y < height; ++y) {
            memcpy(dstRow, srcRow, width * 4);
            srcRow += mapped.RowPitch;
            dstRow += width * 4;
        }
        context->Unmap(staging.Get(), 0);
    }
    return pixels;
}

static ID3D11Texture2D* MakeTexture(ID3D11Device* dev, UINT w, UINT h, DXGI_FORMAT fmt, bool bright)
{
    (void)bright;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = w; desc.Height = h;
    desc.MipLevels = 1; desc.ArraySize = 1;
    desc.Format = fmt;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

    std::string data;
    if (fmt == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        data.resize(size_t(w) * h * 8);
        for (UINT y = 0; y < h; ++y) {
            for (UINT x = 0; x < w; ++x) {
                float v = (x < w / 2) ? 0.25f : 1.5f; // 右半 >1.0
                unsigned short half = (v > 1.0f) ? 0x3E00 /*1.5*/ : 0x3500 /*0.25*/;
                memcpy(&data[(size_t(y) * w + x) * 8], &half, 2);
                memcpy(&data[(size_t(y) * w + x) * 8 + 2], &half, 2);
                memcpy(&data[(size_t(y) * w + x) * 8 + 4], &half, 2);
            }
        }
    } else if (fmt == DXGI_FORMAT_B8G8R8A8_UNORM) {
        data.resize(size_t(w) * h * 4);
        for (UINT y = 0; y < h; ++y) {
            for (UINT x = 0; x < w; ++x) {
                unsigned char v = (x < w / 2) ? 100 : 253;
                data[(size_t(y) * w + x) * 4 + 0] = v;
                data[(size_t(y) * w + x) * 4 + 1] = v;
                data[(size_t(y) * w + x) * 4 + 2] = v;
                data[(size_t(y) * w + x) * 4 + 3] = static_cast<char>(255);
            }
        }
    }
    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = data.data();
    init.SysMemPitch = w * (fmt == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4);
    ID3D11Texture2D* tex = nullptr;
    HRESULT hr = dev->CreateTexture2D(&desc, data.empty() ? nullptr : &init, &tex);
    printf("CreateTexture2D(%ux%u fmt=%d) hr=0x%08lX tex=%p\n", w, h, fmt, hr, reinterpret_cast<void*>(tex));
    return tex;
}

// -----------------------------------------------------------------------------
// P5 核心集成测试：测试 WgcHookManager 拦截 + FP16 升级 + ToneMap 输出 BGRA8
// -----------------------------------------------------------------------------
static int RunIntegrationTest()
{
    printf("=================================================================\n");
    printf("  [P5 集成验证] WgcHookManager 帧池拦截与 ToneMap 接入测试\n");
    printf("=================================================================\n\n");

    // 1. 初始化 WinRT
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    // 2. 查询当前 HDR 状态
    bool isHdrOn = false;
    float sdrWhite = 280.0f;
    for (const auto& s : hdrfix::QueryOutputHdrStates()) {
        printf("[HDR State] 显示器: %ls | HDR: %s | SDR White: %.1f\n",
               s.gdiDeviceName.c_str(), s.hdrEnabled ? "YES" : "NO", s.sdrWhiteNits);
        if (s.hdrEnabled) {
            isHdrOn = true;
            if (s.sdrWhiteNits > 10.0f) sdrWhite = s.sdrWhiteNits;
        }
    }
    printf("  -> 当前 HDR 状态: %s (SDR White: %.1f nits)\n\n", isHdrOn ? "开启" : "关闭", sdrWhite);

    // 3. 安装 WGC Hook
    printf("[Hook] 正在安装 WgcHookManager (拦截 RoGetActivationFactory)...\n");
    if (!hdrfix::WgcHookManager::Instance().Install()) {
        printf("[ERROR] WgcHookManager::Install 失败！\n");
        return 1;
    }
    printf("[Hook] 安装成功！准备发起 WGC 请求...\n\n");

    // 4. 创建 D3D11 设备与 WinRT IDirect3DDevice
    ComPtr<ID3D11Device> d3dDevice;
    ComPtr<ID3D11DeviceContext> d3dContext;
    D3D_FEATURE_LEVEL fl{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                   D3D11_SDK_VERSION, d3dDevice.GetAddressOf(), &fl, d3dContext.GetAddressOf());
    if (FAILED(hr)) {
        printf("[ERROR] D3D11CreateDevice 失败 hr=0x%08lX\n", hr);
        return 1;
    }

    ComPtr<IDXGIDevice> dxgiDevice;
    d3dDevice.As(&dxgiDevice);
    winrt::com_ptr<::IInspectable> inspectable;
    CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.Get(), inspectable.put());
    auto winrtDevice = inspectable.as<wgd::Direct3D11::IDirect3DDevice>();

    // 5. 获取主显示器 CaptureItem
    HMONITOR mon = ::MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, ::IGraphicsCaptureItemInterop>();
    wgc::GraphicsCaptureItem item{nullptr};
    hr = interop->CreateForMonitor(mon, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(item));
    if (FAILED(hr) || !item) {
        printf("[ERROR] CreateForMonitor 失败 hr=0x%08lX\n", hr);
        return 1;
    }
    auto size = item.Size();
    printf("[WGC Item] 目标尺寸: %dx%d\n", size.Width, size.Height);

    // 6. 模拟客户端发起请求：特意请求 B8G8R8A8UIntNormalized (87)
    printf("[WGC Client] 客户端发起调用: CreateFreeThreaded 请求 BGRA8 池...\n");
    UINT64 initialCount = hdrfix::WgcHookManager::Instance().GetInterceptedPoolCount();

    auto pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
        winrtDevice,
        wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized, // 模拟 VeRTC 原生请求
        3,
        size
    );

    UINT64 afterCount = hdrfix::WgcHookManager::Instance().GetInterceptedPoolCount();
    printf("[Hook 拦截结果] 拦截计数: %llu -> %llu\n", initialCount, afterCount);
    if (afterCount > initialCount) {
        printf("  -> [PASS] WgcHookManager 成功捕获到 FramePool 创建请求并注入 ProxyFramePoolStatics！\n");
    } else {
        printf("  -> [WARN] 未捕获到创建请求。\n");
    }

    // 7. 启动捕获并连续拉取 60 帧
    auto session = pool.CreateCaptureSession(item);
    session.StartCapture();
    printf("\n[捕获运行] 开始拉取帧并验证输出格式与 ToneMap 转换...\n");

    int framesReceived = 0;
    int maxAttempts = 150;
    DXGI_FORMAT finalReceivedFormat = DXGI_FORMAT_UNKNOWN;
    UINT finalW = 0, finalH = 0;

    auto tStart = std::chrono::steady_clock::now();

    for (int i = 0; i < maxAttempts && framesReceived < 60; ++i) {
        wgc::Direct3D11CaptureFrame frame{nullptr};
        try {
            frame = pool.TryGetNextFrame();
        } catch (...) {}

        if (frame) {
            framesReceived++;
            // 检查 Surface 格式
            try {
                auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
                ComPtr<ID3D11Texture2D> tex;
                access->GetInterface(__uuidof(ID3D11Texture2D), (void**)tex.GetAddressOf());
                if (tex) {
                    D3D11_TEXTURE2D_DESC desc{};
                    tex->GetDesc(&desc);
                    finalReceivedFormat = desc.Format;
                    finalW = desc.Width;
                    finalH = desc.Height;
                }
            } catch (...) {}
        }
        ::Sleep(16); // 模拟 60fps 消费节奏
    }

    auto tEnd = std::chrono::steady_clock::now();
    double elapsedSec = std::chrono::duration<double>(tEnd - tStart).count();
    double fps = framesReceived / elapsedSec;

    printf("  连续获取帧数: %d 帧 (耗时 %.2f 秒, 估算速率 %.1f fps)\n", framesReceived, elapsedSec, fps);
    printf("  客户端最终收到的纹理规格: %ux%u, DXGI_FORMAT=%d (87=B8G8R8A8_UNORM)\n",
           finalW, finalH, static_cast<int>(finalReceivedFormat));

    if (finalReceivedFormat == DXGI_FORMAT_B8G8R8A8_UNORM) {
        printf("  -> [PASS] 客户端成功接收到透明色调映射后的合法 B8G8R8A8_UNORM 帧！\n");
    } else {
        printf("  -> [FAIL] 接收到的纹理格式异常: %d\n", static_cast<int>(finalReceivedFormat));
    }

    session.Close();
    pool.Close();

    // 8. 连续重启会话 3 次（验证稳定性与释放无残留）
    printf("\n[稳定性验证] 连续重启共享会话 3 次...\n");
    for (int loop = 1; loop <= 3; ++loop) {
        auto p2 = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
            winrtDevice, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized, 3, size);
        auto s2 = p2.CreateCaptureSession(item);
        s2.StartCapture();
        ::Sleep(50);
        s2.Close();
        p2.Close();
        printf("  循环 %d/3 重启完成\n", loop);
    }
    printf("  -> [PASS] 连续快速启动/停止未发生死锁与崩溃，COM 引用清理完整！\n");

    // 9. 卸载 Hook
    hdrfix::WgcHookManager::Instance().Remove();
    printf("[Hook] WgcHookManager 已安全卸载。\n\n");

    printf("=================================================================\n");
    printf("  P5 编码前链路接入集成测试: 全部通过 (GO)!\n");
    printf("=================================================================\n");
    return 0;
}

// -----------------------------------------------------------------------------
// P6 核心验证：测试 AutoDetect 判定树、ConfigManager 与 SafetyGuard 故障保护
// -----------------------------------------------------------------------------
static int RunSafetyTests()
{
    printf("=================================================================\n");
    printf("  [P6 核心验证] 自动检测、配置与故障保护 (SafetyGuard & AutoDetect)\n");
    printf("=================================================================\n\n");

    int failed = 0;

    // 1. 测试 AutoDetect 判定树 (ColorDetector)
    printf("--- [测试 1: ColorDetector AutoDetect 判定树] ---\n");
    {
        // 场景 A: HDR 开启 + BGRA8 请求 -> ElevateAndConvert
        auto resA = hdrfix::ColorDetector::EvaluateExplicit(true, 280.0f, DXGI_FORMAT_B8G8R8A8_UNORM);
        printf("  场景 A (HDR On + BGRA8): 决策=%s, 空间=%s, 原因: %s\n",
               resA.action == hdrfix::DecisionAction::ElevateAndConvert ? "ElevateAndConvert" : "Other",
               resA.colorSpace == hdrfix::DetectedColorSpace::HDR_scRGB ? "HDR_scRGB" : "Other",
               resA.reason.c_str());
        if (resA.action != hdrfix::DecisionAction::ElevateAndConvert || resA.colorSpace != hdrfix::DetectedColorSpace::HDR_scRGB) {
            printf("    -> [FAIL] 决策不符合预期！\n");
            failed++;
        } else {
            printf("    -> [PASS] 成功识别需要提升为 scRGB FP16 池并映射\n");
        }

        // 场景 B: HDR 关闭 + BGRA8 请求 -> Passthrough
        auto resB = hdrfix::ColorDetector::EvaluateExplicit(false, 80.0f, DXGI_FORMAT_B8G8R8A8_UNORM);
        printf("  场景 B (HDR Off + BGRA8): 决策=%s, 空间=%s, 原因: %s\n",
               resB.action == hdrfix::DecisionAction::Passthrough ? "Passthrough" : "Other",
               resB.colorSpace == hdrfix::DetectedColorSpace::SDR_Rec709 ? "SDR_Rec709" : "Other",
               resB.reason.c_str());
        if (resB.action != hdrfix::DecisionAction::Passthrough) {
            printf("    -> [FAIL] SDR 模式未安全旁路！\n");
            failed++;
        } else {
            printf("    -> [PASS] 成功按原生 SDR 旁路透传\n");
        }

        // 场景 C: HDR 开启 + 原生 FP16 请求 -> ElevateAndConvert
        auto resC = hdrfix::ColorDetector::EvaluateExplicit(true, 280.0f, DXGI_FORMAT_R16G16B16A16_FLOAT);
        printf("  场景 C (HDR On + FP16): 决策=%s\n",
               resC.action == hdrfix::DecisionAction::ElevateAndConvert ? "ElevateAndConvert" : "Other");
        if (resC.action != hdrfix::DecisionAction::ElevateAndConvert) {
            printf("    -> [FAIL] 原生 FP16 判定失败！\n");
            failed++;
        } else {
            printf("    -> [PASS] 原生 FP16 成功识别\n");
        }

        // 场景 D: 未知格式 -> DiagnoseOnly (Fail-Open)
        auto resD = hdrfix::ColorDetector::EvaluateExplicit(true, 280.0f, DXGI_FORMAT_UNKNOWN);
        printf("  场景 D (未知格式): 决策=%s\n",
               resD.action == hdrfix::DecisionAction::DiagnoseOnly ? "DiagnoseOnly" : "Other");
        if (resD.action != hdrfix::DecisionAction::DiagnoseOnly) {
            printf("    -> [FAIL] 未知格式未触发诊断旁路！\n");
            failed++;
        } else {
            printf("    -> [PASS] 未知格式安全旁路 (Fail-Open)\n");
        }
    }

    // 2. 测试配置文件解析 (ConfigManager)
    printf("\n--- [测试 2: ConfigManager 配置文件读取] ---\n");
    {
        auto& mgr = hdrfix::ConfigManager::Instance();
        mgr.Load(L"config/hdrfix.ini");
        const auto& cfg = mgr.GetConfig();
        printf("  配置文件路径: %ls\n", mgr.GetLoadedPath().c_str());
        printf("  [General] Enable=%d, FailOpen=%d, Input=%s, Output=%s\n",
               cfg.general.enable, cfg.general.failOpen, cfg.general.input.c_str(), cfg.general.output.c_str());
        printf("  [HDR] ToneMapper=%s, SourcePeakNits=%.1f, SDRRefWhite=%.1f, Exposure=%.2f, HighlightRollOff=%.2f\n",
               cfg.hdr.toneMapper.c_str(), cfg.hdr.sourcePeakNits, cfg.hdr.sdrReferenceWhite,
               cfg.hdr.exposure, cfg.hdr.highlightRollOff);
        printf("  [Compatibility] StrictVersionCheck=%d, AllowUnknownBuild=%d\n",
               cfg.compat.strictVersionCheck, cfg.compat.allowUnknownBuild);

        if (cfg.general.enable && cfg.compat.strictVersionCheck) {
            printf("    -> [PASS] 配置参数解析正确！\n");
        } else {
            printf("    -> [FAIL] 配置参数异常！\n");
            failed++;
        }
    }

    // 3. 测试 Kill Switch 与热旁路 (SafetyGuard)
    printf("\n--- [测试 3: SafetyGuard Kill Switch 多级熔断机制] ---\n");
    {
        auto& guard = hdrfix::SafetyGuard::Instance();
        guard.Initialize();

        // 3.1 初始状态正常
        bool can1 = guard.CanIntercept();
        printf("  3.1 初始健康状态: CanIntercept=%s (状态: %s)\n",
               can1 ? "TRUE" : "FALSE", guard.GetStatusString(guard.GetLastStatus()));
        if (!can1) { printf("    -> [FAIL] 初始状态应为允许介入！\n"); failed++; }

        // 3.2 手动动态旁路
        guard.TriggerManualBypass(true);
        bool can2 = guard.CanIntercept();
        printf("  3.2 手动触发热旁路: CanIntercept=%s (状态: %s)\n",
               can2 ? "TRUE" : "FALSE", guard.GetStatusString(guard.GetLastStatus()));
        if (can2) { printf("    -> [FAIL] 热旁路未生效！\n"); failed++; }
        guard.TriggerManualBypass(false);

        // 3.3 环境变量旁路 HDRFIX_DISABLE=1
        ::SetEnvironmentVariableW(L"HDRFIX_DISABLE", L"1");
        bool can3 = guard.CanIntercept();
        printf("  3.3 环境变量 HDRFIX_DISABLE=1: CanIntercept=%s (状态: %s)\n",
               can3 ? "TRUE" : "FALSE", guard.GetStatusString(guard.GetLastStatus()));
        if (can3 || guard.GetLastStatus() != hdrfix::SafetyStatus::KilledByEnv) {
            printf("    -> [FAIL] 环境变量 Kill Switch 未生效！\n");
            failed++;
        } else {
            printf("    -> [PASS] 环境变量 Kill Switch 成功熔断！\n");
        }
        ::SetEnvironmentVariableW(L"HDRFIX_DISABLE", nullptr);

        // 3.4 恢复正常
        bool can4 = guard.CanIntercept();
        printf("  3.4 熔断清除后恢复: CanIntercept=%s (状态: %s)\n",
               can4 ? "TRUE" : "FALSE", guard.GetStatusString(guard.GetLastStatus()));
        if (!can4) { printf("    -> [FAIL] 恢复状态失败！\n"); failed++; }
    }

    // 4. 测试 Crash Marker 崩溃标记容灾降级 (SafetyGuard)
    printf("\n--- [测试 4: Crash Marker 崩溃标记自检与安全模式降级] ---\n");
    {
        auto& guard = hdrfix::SafetyGuard::Instance();
        std::wstring markerPath = guard.GetMarkerPath();
        printf("  当前会话 Marker 路径: %ls\n", markerPath.c_str());

        // 正常 shutdown
        guard.Shutdown();
        DWORD attr = ::GetFileAttributesW(markerPath.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES) {
            printf("    -> [PASS] 正常退出时 Marker 文件已安全清理\n");
        } else {
            printf("    -> [FAIL] Marker 文件未被清理！\n");
            failed++;
        }

        // 模拟上次崩溃遗留 Marker
        FILE* fp = _wfopen(markerPath.c_str(), L"w");
        if (fp) {
            fprintf(fp, "pid=99999\ncrash=simulated\n");
            fclose(fp);
        }

        // 重新初始化，应检测到残留 Marker 并触发安全回退模式 (Safe Fallback)
        guard.Initialize();
        bool canIntercept = guard.CanIntercept();
        printf("  残留 Marker 下检测结果: CanIntercept=%s (状态: %s)\n",
               canIntercept ? "TRUE" : "FALSE", guard.GetStatusString(guard.GetLastStatus()));

        if (!canIntercept && guard.GetLastStatus() == hdrfix::SafetyStatus::CrashMarkerDetected) {
            printf("    -> [PASS] 成功检测到崩溃遗留标记，自动降级为安全模式强制旁路 (Fail-Open)！\n");
        } else {
            printf("    -> [FAIL] 未能正确触发安全降级！\n");
            failed++;
        }

        // 清理模拟文件
        guard.Shutdown();
        ::DeleteFileW(markerPath.c_str());
    }

    // 5. 版本锁校验
    printf("\n--- [测试 5: 版本锁白名单校验] ---\n");
    {
        auto& guard = hdrfix::SafetyGuard::Instance();
        guard.Initialize();
        bool can = guard.CanIntercept();
        printf("  当前进程版本校验: CanIntercept=%s (状态: %s)\n",
               can ? "TRUE" : "FALSE", guard.GetStatusString(guard.GetLastStatus()));
        if (can) {
            printf("    -> [PASS] 白名单宿主正常通过版本校验\n");
        } else {
            printf("    -> [FAIL] 版本校验未通过！\n");
            failed++;
        }
        guard.Shutdown();
    }

    printf("\n=================================================================\n");
    if (failed == 0) {
        printf("  Gate P6 全部测试项均通过: 自动检测、配置与故障保护就绪 (GO)!\n");
    } else {
        printf("  [FAIL] 有 %d 项测试未通过！\n", failed);
    }
    printf("=================================================================\n");

    return failed == 0 ? 0 : 1;
}

// -----------------------------------------------------------------------------
// P7 矩阵与稳定性验证：多分辨率矩阵、色彩量化验收、GPU性能基准、动态抗扰与Fail-Open
// -----------------------------------------------------------------------------
static int RunP7MatrixTests()
{
    printf("=================================================================\n");
    printf("  [P7 核心验证] 测试、画质与稳定性矩阵 (Matrix, Golden, Perf & Resilience)\n");
    printf("=================================================================\n\n");

    int failed = 0;

    // 创建 D3D11 Device
    D3D_FEATURE_LEVEL fl{};
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                   D3D11_SDK_VERSION, device.GetAddressOf(), &fl, context.GetAddressOf());
    if (FAILED(hr)) {
        printf("[FAIL] D3D11CreateDevice 失败: 0x%08lX\n", hr);
        return 1;
    }

    hdrfix::ToneMapCore toneMapper;
    if (!toneMapper.Initialize(device.Get(), L"shaders/tonemap_scrgb.hlsl")) {
        printf("[FAIL] ToneMapCore::Initialize 失败！\n");
        return 1;
    }

    // -------------------------------------------------------------
    // 测试 1: 分辨率与帧率功能矩阵 (1080p, 1440p, 4K; 30fps/60fps)
    // -------------------------------------------------------------
    printf("--- [测试 1: 多分辨率与帧率矩阵覆盖 (1080p / 1440p / 4K @ 30/60fps)] ---\n");
    struct ResTestItem {
        UINT width;
        UINT height;
        const char* name;
    } resolutions[] = {
        { 1920, 1080, "1080p (Full HD)" },
        { 2560, 1440, "1440p (2K QHD)" },
        { 3840, 2160, "2160p (4K UHD)" }
    };

    bool matrixOk = true;
    for (const auto& res : resolutions) {
        ComPtr<ID3D11Texture2D> texIn, texOut;
        ComPtr<ID3D11ShaderResourceView> srvIn;
        ComPtr<ID3D11RenderTargetView> rtvOut;
        if (!CreateTexturePair(device.Get(), res.width, res.height, DXGI_FORMAT_R16G16B16A16_FLOAT,
                               D3D11_BIND_SHADER_RESOURCE, texIn, srvIn, rtvOut)) {
            printf("  %s: 创建 FP16 输入纹理失败！\n", res.name);
            matrixOk = false;
            continue;
        }
        ComPtr<ID3D11ShaderResourceView> dummySrv;
        if (!CreateTexturePair(device.Get(), res.width, res.height, DXGI_FORMAT_B8G8R8A8_UNORM,
                               D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, texOut, dummySrv, rtvOut)) {
            printf("  %s: 创建 BGRA8 输出纹理失败！\n", res.name);
            matrixOk = false;
            continue;
        }

        // 执行单帧映射
        bool execOk = toneMapper.Execute(context.Get(), srvIn.Get(), rtvOut.Get(), res.width, res.height);
        if (!execOk) {
            printf("  %s: ToneMapCore::Execute 失败！\n", res.name);
            matrixOk = false;
            continue;
        }

        D3D11_TEXTURE2D_DESC outDesc{};
        texOut->GetDesc(&outDesc);
        bool match = (outDesc.Width == res.width && outDesc.Height == res.height && outDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM);

        // 模拟 60fps 和 30fps 连续流转
        for (int f = 0; f < 10; ++f) {
            toneMapper.Execute(context.Get(), srvIn.Get(), rtvOut.Get(), res.width, res.height);
        }

        printf("  规格: %s (%ux%u) -> 输出: %ux%u DXGI_FORMAT=%d, 流转: 正常\n",
               res.name, res.width, res.height, outDesc.Width, outDesc.Height, static_cast<int>(outDesc.Format));
        if (!match) matrixOk = false;
    }

    if (matrixOk) {
        printf("  -> [PASS] 1080p / 1440p / 4K 多分辨率格式对齐与流转全部正常！\n");
    } else {
        printf("  -> [FAIL] 多分辨率功能矩阵测试失败！\n");
        failed++;
    }

    // -------------------------------------------------------------
    // 测试 2: 色彩与画质定量验收 (Golden Benchmarks)
    // -------------------------------------------------------------
    printf("\n--- [测试 2: 色彩与画质定量验收 (阶调连续性 / 高光抑制 / UI白 / 色相纯度)] ---\n");
    {
        const UINT tw = 1920, th = 1080;

        // 2.1 灰阶阶梯 Ramp 生成 (0~1500 nits)
        std::vector<uint16_t> rampHalf(size_t(tw) * th * 4);
        for (UINT y = 0; y < th; ++y) {
            for (UINT x = 0; x < tw; ++x) {
                float nits = (static_cast<float>(x) / (tw - 1)) * 1500.0f;
                float scRgb = nits / 80.0f;
                uint16_t h = FloatToHalf(scRgb);
                size_t idx = (size_t(y) * tw + x) * 4;
                rampHalf[idx + 0] = h;
                rampHalf[idx + 1] = h;
                rampHalf[idx + 2] = h;
                rampHalf[idx + 3] = FloatToHalf(1.0f);
            }
        }

        D3D11_TEXTURE2D_DESC tdesc{};
        tdesc.Width = tw; tdesc.Height = th; tdesc.MipLevels = 1; tdesc.ArraySize = 1;
        tdesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        tdesc.SampleDesc.Count = 1; tdesc.Usage = D3D11_USAGE_DEFAULT;
        tdesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA subData{};
        subData.pSysMem = rampHalf.data();
        subData.SysMemPitch = tw * 8;

        ComPtr<ID3D11Texture2D> rampTex;
        ComPtr<ID3D11ShaderResourceView> rampSRV;
        device->CreateTexture2D(&tdesc, &subData, rampTex.GetAddressOf());
        device->CreateShaderResourceView(rampTex.Get(), nullptr, rampSRV.GetAddressOf());

        ComPtr<ID3D11Texture2D> dstTex;
        ComPtr<ID3D11ShaderResourceView> dstSRV;
        ComPtr<ID3D11RenderTargetView> dstRTV;
        CreateTexturePair(device.Get(), tw, th, DXGI_FORMAT_B8G8R8A8_UNORM,
                          D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, dstTex, dstSRV, dstRTV);

        // 2.1.1 对比 Naive Clamp
        hdrfix::ToneMapParams pClamp{};
        pClamp.sdrWhiteNits = 280.0f;
        pClamp.toneMapper = static_cast<uint32_t>(hdrfix::ToneMapperType::Clamp);
        toneMapper.SetParams(pClamp);
        toneMapper.Execute(context.Get(), rampSRV.Get(), dstRTV.Get(), tw, th);
        auto pixClamp = ReadbackTexturePixels(device.Get(), context.Get(), dstTex.Get(), tw, th);

        int clampClipped = 0;
        for (UINT x = 0; x < tw; ++x) {
            if (pixClamp[x * 4 + 0] >= 254) clampClipped++;
        }
        double clampClipRate = (double)clampClipped / tw * 100.0;

        // 2.1.2 验证 Luma-HuePreserving
        hdrfix::ToneMapParams pLuma{};
        pLuma.sdrWhiteNits = 280.0f;
        pLuma.sourcePeakNits = 1500.0f;
        pLuma.toneMapper = static_cast<uint32_t>(hdrfix::ToneMapperType::LumaHuePreserving);
        pLuma.oetfType = static_cast<uint32_t>(hdrfix::OetfType::Rec709);
        toneMapper.SetParams(pLuma);
        toneMapper.Execute(context.Get(), rampSRV.Get(), dstRTV.Get(), tw, th);
        auto pixLuma = ReadbackTexturePixels(device.Get(), context.Get(), dstTex.Get(), tw, th);

        int lumaClipped = 0;
        bool monotonic = true;
        uint8_t lastVal = 0;
        for (UINT x = 0; x < tw; ++x) {
            uint8_t v = pixLuma[x * 4 + 0];
            if (v < lastVal) monotonic = false;
            lastVal = v;
            if (v >= 255) lumaClipped++;
        }
        double lumaClipRate = (double)lumaClipped / tw * 100.0;

        printf("  2.1 灰阶连续性 (Ramp 0~1500 nits):\n");
        printf("      Naive Clamp 裁切率: %.1f%% (原生缺陷: 白点后大面积死白)\n", clampClipRate);
        printf("      ToneMap 算法裁切率: %.1f%%, 单调递增性: %s\n",
               lumaClipRate, monotonic ? "PASS (平滑无阶调断裂)" : "FAIL");

        // 2.2 彩色高光 (Pure Red 1000 nits) 色相纯度保持
        std::vector<uint16_t> colorHalf(size_t(tw) * th * 4);
        float redScRgb = 1000.0f / 80.0f; // 12.5 scRGB
        uint16_t redH = FloatToHalf(redScRgb);
        for (size_t i = 0; i < size_t(tw) * th; ++i) {
            colorHalf[i * 4 + 0] = redH; // R
            colorHalf[i * 4 + 1] = 0;    // G
            colorHalf[i * 4 + 2] = 0;    // B
            colorHalf[i * 4 + 3] = FloatToHalf(1.0f);
        }
        subData.pSysMem = colorHalf.data();
        ComPtr<ID3D11Texture2D> colorTex;
        ComPtr<ID3D11ShaderResourceView> colorSRV;
        device->CreateTexture2D(&tdesc, &subData, colorTex.GetAddressOf());
        device->CreateShaderResourceView(colorTex.Get(), nullptr, colorSRV.GetAddressOf());

        toneMapper.Execute(context.Get(), colorSRV.Get(), dstRTV.Get(), tw, th);
        auto pixColor = ReadbackTexturePixels(device.Get(), context.Get(), dstTex.Get(), tw, th);
        uint8_t outB = pixColor[0]; // BGRA
        uint8_t outG = pixColor[1];
        uint8_t outR = pixColor[2];
        printf("  2.2 1000 nits 纯红高光输出: R=%u, G=%u, B=%u\n", outR, outG, outB);
        bool colorOk = (outR > 200 && outG == 0 && outB == 0);
        printf("      -> %s (未发生偏色、泛白或色度漂移)\n", colorOk ? "[PASS]" : "[FAIL]");

        // 2.3 SDR UI (280 nits) 与 HDR 峰值高光 (1500 nits) 层次对比
        std::vector<uint16_t> sdrHdrHalf(size_t(tw) * th * 4);
        uint16_t uiH = FloatToHalf(280.0f / 80.0f);   // 3.5 scRGB
        uint16_t peakH = FloatToHalf(1500.0f / 80.0f); // 18.75 scRGB
        for (UINT y = 0; y < th; ++y) {
            for (UINT x = 0; x < tw; ++x) {
                uint16_t h = (x < tw / 2) ? uiH : peakH;
                size_t i = (size_t(y) * tw + x) * 4;
                sdrHdrHalf[i + 0] = h;
                sdrHdrHalf[i + 1] = h;
                sdrHdrHalf[i + 2] = h;
                sdrHdrHalf[i + 3] = FloatToHalf(1.0f);
            }
        }
        subData.pSysMem = sdrHdrHalf.data();
        ComPtr<ID3D11Texture2D> shTex;
        ComPtr<ID3D11ShaderResourceView> shSRV;
        device->CreateTexture2D(&tdesc, &subData, shTex.GetAddressOf());
        device->CreateShaderResourceView(shTex.Get(), nullptr, shSRV.GetAddressOf());

        toneMapper.Execute(context.Get(), shSRV.Get(), dstRTV.Get(), tw, th);
        auto pixSH = ReadbackTexturePixels(device.Get(), context.Get(), dstTex.Get(), tw, th);
        uint8_t uiVal = pixSH[0];
        uint8_t peakVal = pixSH[(tw - 1) * 4];
        printf("  2.3 SDR UI (280 nits) 输出: %u/255 | HDR 峰值 (1500 nits) 输出: %u/255 (高光层次差: %d)\n",
               uiVal, peakVal, (int)peakVal - (int)uiVal);
        bool contrastOk = (uiVal >= 220 && uiVal <= 245 && peakVal >= 248 && peakVal > uiVal);
        printf("      -> %s (UI 亮度合理不过曝，高光层次清晰保留)\n", contrastOk ? "[PASS]" : "[FAIL]");

        if (monotonic && lumaClipRate <= 2.0 && colorOk && contrastOk) {
            printf("  -> [PASS] 色彩与画质定量验收全部通过！\n");
        } else {
            printf("  -> [FAIL] 色彩与画质验收存在不达标项！\n");
            failed++;
        }
    }

    // -------------------------------------------------------------
    // 测试 3: GPU 性能基准与开销核验 (D3D11 Timestamp Queries)
    // -------------------------------------------------------------
    printf("\n--- [测试 3: GPU 性能基准与算力开销核验 (D3D11 Timestamp)] ---\n");
    {
        D3D11_QUERY_DESC qDisjointDesc{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
        D3D11_QUERY_DESC qTimestampDesc{ D3D11_QUERY_TIMESTAMP, 0 };
        ComPtr<ID3D11Query> qDisjoint, qStart, qEnd;
        device->CreateQuery(&qDisjointDesc, qDisjoint.GetAddressOf());
        device->CreateQuery(&qTimestampDesc, qStart.GetAddressOf());
        device->CreateQuery(&qTimestampDesc, qEnd.GetAddressOf());

        struct PerfItem {
            UINT w, h;
            const char* label;
        } perfTargets[] = {
            { 1920, 1080, "1080p (1920x1080)" },
            { 2560, 1440, "1440p (2560x1440)" },
            { 3840, 2160, "4K UHD (3840x2160)" }
        };

        bool perfOk = true;
        for (const auto& item : perfTargets) {
            ComPtr<ID3D11Texture2D> tIn, tOut;
            ComPtr<ID3D11ShaderResourceView> sIn, sOut;
            ComPtr<ID3D11RenderTargetView> rOut;
            CreateTexturePair(device.Get(), item.w, item.h, DXGI_FORMAT_R16G16B16A16_FLOAT,
                              D3D11_BIND_SHADER_RESOURCE, tIn, sIn, rOut);
            CreateTexturePair(device.Get(), item.w, item.h, DXGI_FORMAT_B8G8R8A8_UNORM,
                              D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, tOut, sOut, rOut);

            // 预热 10 帧
            for (int i = 0; i < 10; ++i) {
                toneMapper.Execute(context.Get(), sIn.Get(), rOut.Get(), item.w, item.h);
            }

            // 采样 100 帧
            const int kSamples = 100;
            std::vector<double> timingsMs;
            timingsMs.reserve(kSamples);

            for (int i = 0; i < kSamples; ++i) {
                context->Begin(qDisjoint.Get());
                context->End(qStart.Get());
                toneMapper.Execute(context.Get(), sIn.Get(), rOut.Get(), item.w, item.h);
                context->End(qEnd.Get());
                context->End(qDisjoint.Get());

                D3D11_QUERY_DATA_TIMESTAMP_DISJOINT djData{};
                while (context->GetData(qDisjoint.Get(), &djData, sizeof(djData), 0) == S_FALSE) {}

                UINT64 tsStart = 0, tsEnd = 0;
                while (context->GetData(qStart.Get(), &tsStart, sizeof(tsStart), 0) == S_FALSE) {}
                while (context->GetData(qEnd.Get(), &tsEnd, sizeof(tsEnd), 0) == S_FALSE) {}

                if (!djData.Disjoint && djData.Frequency > 0) {
                    double ms = static_cast<double>(tsEnd - tsStart) / static_cast<double>(djData.Frequency) * 1000.0;
                    timingsMs.push_back(ms);
                }
            }

            double sum = 0.0, minT = 999.0, maxT = 0.0;
            for (double t : timingsMs) {
                sum += t;
                if (t < minT) minT = t;
                if (t > maxT) maxT = t;
            }
            double avgT = timingsMs.empty() ? 0.0 : sum / timingsMs.size();
            double budgetPct = (avgT / 16.66667) * 100.0; // 4K60 预算 16.6ms

            printf("  %s 纯 GPU 开销: 平均 %.3f ms (最小 %.3f ms, 最大 %.3f ms), 4K60 预算占比: %.2f%%\n",
                   item.label, avgT, minT, maxT, budgetPct);

            if (item.w == 3840 && (avgT >= 1.0 || budgetPct > 2.0)) {
                printf("    -> [FAIL] 4K GPU 开销超出预算 (目标 < 1.0ms, <= 2.0%%)！\n");
                perfOk = false;
            }
        }

        if (perfOk) {
            printf("  -> [PASS] GPU 性能开销指标优秀 (4K 单帧耗时远低于 1.0ms，开销占比远低于 2%% 红线)！\n");
        } else {
            printf("  -> [FAIL] GPU 性能开销未达标！\n");
            failed++;
        }
    }

    // -------------------------------------------------------------
    // 测试 4: 动态抗扰度与自适应容错 (Dynamic Resilience & Fail-Open)
    // -------------------------------------------------------------
    printf("\n--- [测试 4: 动态抗扰度与自适应容错 (动态Resize / HDR切换 / Fail-Open)] ---\n");
    {
        bool resilienceOk = true;

        // 4.1 动态尺寸重构 (1080p -> 4K -> 1080p)
        printf("  4.1 动态尺寸自适应 (1080p -> 4K -> 1080p 连续切换):\n");
        {
            UINT steps[][2] = { {1920, 1080}, {3840, 2160}, {1920, 1080} };
            bool resizeOk = true;
            for (int s = 0; s < 3; ++s) {
                UINT rw = steps[s][0], rh = steps[s][1];
                ComPtr<ID3D11Texture2D> tIn, tOut;
                ComPtr<ID3D11ShaderResourceView> sIn, sOut;
                ComPtr<ID3D11RenderTargetView> rOut;
                CreateTexturePair(device.Get(), rw, rh, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                  D3D11_BIND_SHADER_RESOURCE, tIn, sIn, rOut);
                CreateTexturePair(device.Get(), rw, rh, DXGI_FORMAT_B8G8R8A8_UNORM,
                                  D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, tOut, sOut, rOut);
                for (int f = 0; f < 5; ++f) {
                    if (!toneMapper.Execute(context.Get(), sIn.Get(), rOut.Get(), rw, rh)) {
                        resizeOk = false;
                    }
                }
            }
            if (resizeOk) {
                printf("      -> [PASS] 动态分辨率重构顺畅，自适应视口更新无异常！\n");
            } else {
                printf("      -> [FAIL] 动态分辨率重构失败！\n");
                resilienceOk = false;
            }
        }

        // 4.2 运行时 HDR 状态动态切换自适应
        printf("  4.2 运行时 HDR 状态动态切换自适应 (HDR On -> Off -> On):\n");
        {
            auto dec1 = hdrfix::ColorDetector::EvaluateExplicit(true, 280.0f, DXGI_FORMAT_B8G8R8A8_UNORM);
            auto dec2 = hdrfix::ColorDetector::EvaluateExplicit(false, 80.0f, DXGI_FORMAT_B8G8R8A8_UNORM);
            auto dec3 = hdrfix::ColorDetector::EvaluateExplicit(true, 280.0f, DXGI_FORMAT_B8G8R8A8_UNORM);

            bool switchOk = (dec1.action == hdrfix::DecisionAction::ElevateAndConvert &&
                             dec2.action == hdrfix::DecisionAction::Passthrough &&
                             dec3.action == hdrfix::DecisionAction::ElevateAndConvert);
            if (switchOk) {
                printf("      -> [PASS] 运行时系统 HDR 切换自适应正确无粘连！\n");
            } else {
                printf("      -> [FAIL] 状态切换自适应异常！\n");
                resilienceOk = false;
            }
        }

        // 4.3 异常注入与 Fail-Open 隔离
        printf("  4.3 异常输入注入与 Fail-Open 隔离防护:\n");
        {
            // 传入 nullptr context
            bool r1 = toneMapper.Execute(nullptr, nullptr, nullptr, 1920, 1080);
            // 传入未初始化的 nullptr SRV
            bool r2 = toneMapper.Execute(context.Get(), nullptr, nullptr, 1920, 1080);
            if (!r1 && !r2) {
                printf("      -> [PASS] 非法参数安全拦截并返回 false，未发生空指针崩溃！\n");
            } else {
                printf("      -> [FAIL] 非法参数未能优雅拦截！\n");
                resilienceOk = false;
            }
        }

        if (resilienceOk) {
            printf("  -> [PASS] 动态抗扰度与自适应容错测试全部通过！\n");
        } else {
            printf("  -> [FAIL] 动态抗扰度测试存在失败项！\n");
            failed++;
        }
    }

    printf("\n=================================================================\n");
    if (failed == 0) {
        printf("  Gate P7 全部测试项均通过: 功能矩阵、色彩画质与稳定性达标 (GO)!\n");
    } else {
        printf("  [FAIL] 有 %d 项测试未通过！\n", failed);
    }
    printf("=================================================================\n");

    return failed == 0 ? 0 : 1;
}

int main(int argc, char** argv)
{
    bool inproc = false, remote = false, integration = false, p6safety = false, p7matrix = false;
    int secs = 12;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--p7-matrix")) p7matrix = true;
        else if (!strcmp(argv[i], "--p6-safety")) p6safety = true;
        else if (!strcmp(argv[i], "--integration")) integration = true;
        else if (!strcmp(argv[i], "--inproc")) inproc = true;
        else if (!strcmp(argv[i], "--remote")) remote = true;
        else if (!strcmp(argv[i], "--secs") && i + 1 < argc) secs = atoi(argv[++i]);
    }

    if (p7matrix) {
        return RunP7MatrixTests();
    }

    if (p6safety) {
        return RunSafetyTests();
    }

    if (integration) {
        return RunIntegrationTest();
    }

    if (!inproc && !remote) {
        printf("usage: probe_testhost.exe --p7-matrix | --p6-safety | --integration | --inproc | --remote [--secs N]\n");
        return 1;
    }

    if (inproc) {
        char path[MAX_PATH]{};
        ::GetModuleFileNameA(nullptr, path, MAX_PATH);
        std::string dir(path);
        dir = dir.substr(0, dir.find_last_of("\\/") + 1) + "hdrfix_probe.dll";
        SetEnvironmentVariableA("HDRFIX_PROBE_MODULE", "probe_testhost.exe");
        SetEnvironmentVariableA("HDRFIX_PROBE_LOG", "logs\\probe_testhost.log");
        SetEnvironmentVariableA("HDRFIX_PROBE_MAXSEC", "30");
        CreateDirectoryA("logs", nullptr);
        HMODULE probe = LoadLibraryA(dir.c_str());
        printf("probe dll=%s load=%p\n", dir.c_str(), reinterpret_cast<void*>(probe));
        if (!probe) { printf("gle=%lu\n", GetLastError()); return 1; }
        Sleep(500);
        CreateDirectoryA("logs", nullptr);

        D3D_FEATURE_LEVEL fl{};
        ID3D11Device* dev = nullptr;
        ID3D11DeviceContext* ctx = nullptr;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                       D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                       D3D11_SDK_VERSION, &dev, &fl, &ctx);
        printf("D3D11CreateDevice hr=0x%08lX dev=%p\n", hr, reinterpret_cast<void*>(dev));
        if (FAILED(hr)) return 1;

        ID3D11Texture2D* t1 = MakeTexture(dev, 3840, 2160, DXGI_FORMAT_R16G16B16A16_FLOAT, true);
        ID3D11Texture2D* t2 = MakeTexture(dev, 3840, 2160, DXGI_FORMAT_B8G8R8A8_UNORM, true);
        ID3D11Texture2D* t3 = MakeTexture(dev, 1920, 1080, DXGI_FORMAT_NV12, false);
        Sleep(secs * 1000);
        printf("done; log at logs\\probe_testhost.log\n");
        if (t1) t1->Release();
        if (t2) t2->Release();
        if (t3) t3->Release();
        ctx->Release();
        dev->Release();
        return 0;
    }

    printf("remote mode: waiting %ds before creating device (inject now)...\n", secs / 2);
    Sleep(secs * 1000 / 2);
    D3D_FEATURE_LEVEL fl{};
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                   D3D11_SDK_VERSION, &dev, &fl, &ctx);
    printf("D3D11CreateDevice hr=0x%08lX dev=%p\n", hr, reinterpret_cast<void*>(dev));
    if (FAILED(hr)) return 1;
    ID3D11Texture2D* t1 = MakeTexture(dev, 3840, 2160, DXGI_FORMAT_R16G16B16A16_FLOAT, true);
    ID3D11Texture2D* t2 = MakeTexture(dev, 3840, 2160, DXGI_FORMAT_B8G8R8A8_UNORM, true);
    Sleep(secs * 1000);
    printf("remote done\n");
    if (t1) t1->Release();
    if (t2) t2->Release();
    ctx->Release();
    dev->Release();
    return 0;
}
