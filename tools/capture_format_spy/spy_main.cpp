// capture_format_spy — 只读系统级捕获格式探测工具
//
// 目的：在不接触小黑盒进程的前提下，回答"HDR 开/关时系统捕获 API 给出的纹理格式是什么"：
//   1. DXGI Desktop Duplication：输出当前颜色空间/HDR 状态、复制表面格式；
//      并用 DuplicateOutput1 试探 FP16(R16G16B16A16_FLOAT) 复制是否可用。
//   2. Windows.Graphics.Capture：对同一显示器分别用 B8G8R8A8 / FP16 / RGB10A2 建池，
//      打印实际送达帧的纹理格式。
//
// 本工具只读、独立进程、不注入；HDR Off / On 各跑一次即可对比格式变化：
//   capture_format_spy.exe --backend both --duration 5
// 结果建议保存到 docs/recon/capture-format-spy-<日期-HDR状态>.log

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>
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

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "windowsapp.lib")

namespace {

using Microsoft::WRL::ComPtr;
using winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool;
using winrt::Windows::Graphics::Capture::GraphicsCaptureItem;
using winrt::Windows::Graphics::Capture::GraphicsCaptureSession;
namespace gfx = winrt::Windows::Graphics::DirectX;

// ---------- 常用格式名 ----------

const char* FormatName(DXGI_FORMAT fmt)
{
    switch (fmt) {
    case DXGI_FORMAT_UNKNOWN: return "UNKNOWN";
    case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
    case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
    case DXGI_FORMAT_B8G8R8X8_UNORM: return "B8G8R8X8_UNORM";
    case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
    case DXGI_FORMAT_R16G16B16A16_UNORM: return "R16G16B16A16_UNORM";
    case DXGI_FORMAT_NV12: return "NV12";
    case DXGI_FORMAT_P010: return "P010";
    default: return nullptr;
    }
}

const char* FmtNameOrHex(DXGI_FORMAT fmt)
{
    const char* n = FormatName(fmt);
    static thread_local char buf[32];
    if (n) return n;
    sprintf_s(buf, "0x%08X", static_cast<unsigned>(fmt));
    return buf;
}

const char* ColorSpaceName(DXGI_COLOR_SPACE_TYPE cs)
{
    switch (cs) {
    case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709: return "RGB_FULL_G22_NONE_P709";
    case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709: return "RGB_FULL_G10_NONE_P709";
    case DXGI_COLOR_SPACE_RGB_STUDIO_G22_NONE_P709: return "RGB_STUDIO_G22_NONE_P709";
    case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020: return "RGB_FULL_G2084_NONE_P2020";
    case DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020: return "RGB_STUDIO_G2084_NONE_P2020";
    case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P2020: return "RGB_FULL_G22_NONE_P2020";
    default: return nullptr;
    }
}

// ---------- D3D 设备 ----------

ComPtr<ID3D11Device> CreateDevice(IDXGIAdapter1* adapter)
{
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL fl{};
    ComPtr<ID3D11Device> device;
    HRESULT hr = ::D3D11CreateDevice(adapter,
                                     adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
                                     nullptr, flags, nullptr, 0, D3D11_SDK_VERSION,
                                     device.GetAddressOf(), &fl, nullptr);
    if (FAILED(hr)) {
        printf("    D3D11CreateDevice failed hr=0x%08lX\n", hr);
        return nullptr;
    }
    printf("    [DEV] D3D11 device ok, feature level 0x%04X\n", fl);
    return device;
}

// ---------- DDA 后端 ----------

struct DdaStats {
    int frames = 0;
    std::vector<DXGI_FORMAT> seen;
    void Track(DXGI_FORMAT f)
    {
        ++frames;
        bool found = false;
        for (DXGI_FORMAT s : seen) found = found || (s == f);
        if (!found) seen.push_back(f);
    }
};

void SampleDdaFrames(IDXGIOutputDuplication* dup, int durationSec, const char* tag)
{
    DdaStats stats;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(durationSec);
    while (std::chrono::steady_clock::now() < deadline) {
        DXGI_OUTDUPL_FRAME_INFO info{};
        ComPtr<IDXGIResource> resource;
        HRESULT hr = dup->AcquireNextFrame(200, &info, resource.GetAddressOf());
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            continue;
        }
        if (FAILED(hr)) {
            printf("    [%s] AcquireNextFrame failed hr=0x%08lX\n", tag, hr);
            break;
        }
        ComPtr<ID3D11Texture2D> tex;
        if (SUCCEEDED(resource.As(&tex))) {
            D3D11_TEXTURE2D_DESC d{};
            tex->GetDesc(&d);
            bool firstFew = stats.frames < 3;
            stats.Track(d.Format);
            if (firstFew) {
                printf("    [%s] frame#%d tex: %ux%u fmt=%s\n", tag, stats.frames,
                       d.Width, d.Height, FmtNameOrHex(d.Format));
            }
        }
        dup->ReleaseFrame();
    }
    printf("    [%s] sampled %d frames, distinct formats:", tag, stats.frames);
    if (stats.seen.empty()) printf(" none");
    for (DXGI_FORMAT f : stats.seen) printf(" %s", FmtNameOrHex(f));
    printf("\n");
}

void RunDda(IDXGIAdapter1* adapter, IDXGIOutput* output, int durationSec)
{
    printf("\n===== DXGI Desktop Duplication =====\n");

    DXGI_OUTPUT_DESC desc{};
    if (FAILED(output->GetDesc(&desc))) {
        printf("    GetDesc failed\n");
        return;
    }

    ComPtr<IDXGIOutput6> output6;
    DXGI_OUTPUT_DESC1 desc1{};
    if (SUCCEEDED(output->QueryInterface(IID_PPV_ARGS(&output6))) && SUCCEEDED(output6->GetDesc1(&desc1))) {
        bool hdr = desc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
        const char* cs = ColorSpaceName(desc1.ColorSpace);
        printf("    output %ls desktop=(%ld,%ld)-(%ld,%ld) bits/color=%u colorspace=%s%s%s\n",
               desc1.DeviceName, desc1.DesktopCoordinates.left, desc1.DesktopCoordinates.top,
               desc1.DesktopCoordinates.right, desc1.DesktopCoordinates.bottom,
               desc1.BitsPerColor,
               cs ? cs : "unknown", hdr ? "  HDR-OUTPUT-ACTIVE" : "", "");
        if (!hdr) {
            // 把 GetDesc1 判不出 PQ 的情况也标出来，方便人工对照系统设置
            printf("    (system HDR setting should be compared with Settings > Display > HDR)\n");
        }
    } else {
        printf("    output %ls (IDXGIOutput6 unavailable, HDR status unknown)\n", desc.DeviceName);
    }

    ComPtr<ID3D11Device> device = CreateDevice(adapter);
    if (!device) return;

    ComPtr<IDXGIOutput1> output1;
    if (FAILED(output->QueryInterface(IID_PPV_ARGS(&output1)))) {
        printf("    IDXGIOutput1 unavailable\n");
        return;
    }
    ComPtr<IDXGIOutputDuplication> dup;
    HRESULT hr = output1->DuplicateOutput(device.Get(), dup.GetAddressOf());
    if (FAILED(hr)) {
        printf("    DuplicateOutput failed hr=0x%08lX\n", hr);
        return;
    }
    DXGI_OUTDUPL_DESC ddup{};
    dup->GetDesc(&ddup);
    printf("    default duplication: %ux%u fmt=%s rotation=%d desktopInSysMem=%d\n",
           ddup.ModeDesc.Width, ddup.ModeDesc.Height, FmtNameOrHex(ddup.ModeDesc.Format),
           static_cast<int>(ddup.Rotation), ddup.DesktopImageInSystemMemory ? 1 : 0);
    SampleDdaFrames(dup.Get(), durationSec, "DDA-default");
    dup.Reset();

    // FP16 复制能力试探：HDR 桌面的完整 scRGB 数据需要 R16G16B16A16_FLOAT 复制
    ComPtr<IDXGIOutput5> output5;
    if (SUCCEEDED(output->QueryInterface(IID_PPV_ARGS(&output5)))) {
        DXGI_FORMAT fp16 = DXGI_FORMAT_R16G16B16A16_FLOAT;
        ComPtr<IDXGIOutputDuplication> dupF16;
        hr = output5->DuplicateOutput1(device.Get(), 0, 1, &fp16, dupF16.GetAddressOf());
        if (SUCCEEDED(hr)) {
            printf("    DuplicateOutput1(FP16): SUCCEEDED -> HDR-capable duplication available\n");
            SampleDdaFrames(dupF16.Get(), 2, "DDA-fp16");
        } else {
            printf("    DuplicateOutput1(FP16): failed hr=0x%08lX -> %s\n", hr,
                   hr == E_ACCESSDENIED ? "access denied (another duplication session?)"
                                        : "not supported / no HDR signal");
        }
    }
}

// ---------- WGC 后端 ----------

winrt::com_ptr<ID3D11Texture2D> TextureFromSurface(
    gfx::Direct3D11::IDirect3DSurface const& surface)
{
    auto access = surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    winrt::com_ptr<ID3D11Texture2D> tex;
    winrt::check_hresult(access->GetInterface(__uuidof(ID3D11Texture2D), tex.put_void()));
    return tex;
}

void RunWgc(IDXGIAdapter1* adapter, HMONITOR monitor)
{
    printf("\n===== Windows.Graphics.Capture =====\n");
    if (!GraphicsCaptureSession::IsSupported()) {
        printf("    WGC not supported on this OS\n");
        return;
    }

    ComPtr<ID3D11Device> device = CreateDevice(adapter);
    if (!device) return;

    ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(device.As(&dxgiDevice))) {
        printf("    QueryInterface(IDXGIDevice) failed\n");
        return;
    }

    winrt::com_ptr<::IInspectable> inspectable;
    HRESULT hr = ::CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.Get(), inspectable.put());
    if (FAILED(hr)) {
        printf("    CreateDirect3D11DeviceFromDXGIDevice failed hr=0x%08lX\n", hr);
        return;
    }
    auto interopDevice = inspectable.as<gfx::Direct3D11::IDirect3DDevice>();

    auto interop = winrt::get_activation_factory<GraphicsCaptureItem, ::IGraphicsCaptureItemInterop>();
    GraphicsCaptureItem item{nullptr};
    winrt::check_hresult(interop->CreateForMonitor(
        monitor, winrt::guid_of<GraphicsCaptureItem>(), winrt::put_abi(item)));
    auto size = item.Size();
    printf("    item: %ls %dx%d\n", item.DisplayName().c_str(), size.Width, size.Height);

    const DXGI_FORMAT pools[] = {
        DXGI_FORMAT_B8G8R8A8_UNORM,
        DXGI_FORMAT_R16G16B16A16_FLOAT,
        DXGI_FORMAT_R10G10B10A2_UNORM,
    };
    for (DXGI_FORMAT fmt : pools) {
        try {
            auto pool = Direct3D11CaptureFramePool::CreateFreeThreaded(
                interopDevice, static_cast<gfx::DirectXPixelFormat>(fmt), 2, size);
            auto session = pool.CreateCaptureSession(item);
            session.StartCapture();

            int got = 0;
            std::vector<DXGI_FORMAT> seen;
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (std::chrono::steady_clock::now() < deadline && got < 3) {
                auto frame = pool.TryGetNextFrame();
                if (!frame) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    continue;
                }
                try {
                    auto tex = TextureFromSurface(frame.Surface());
                    if (tex) {
                        D3D11_TEXTURE2D_DESC d{};
                        tex->GetDesc(&d);
                        bool found = false;
                        for (DXGI_FORMAT s : seen) found = found || (s == d.Format);
                        if (!found) seen.push_back(d.Format);
                        if (got < 2) {
                            printf("    [WGC pool %s] frame#%d tex: %ux%u fmt=%s\n",
                                   FmtNameOrHex(fmt), got, d.Width, d.Height, FmtNameOrHex(d.Format));
                        }
                        ++got;
                    }
                } catch (winrt::hresult_error const& e) {
                    printf("    [WGC pool %s] surface->texture failed hr=0x%08X (%hs)\n",
                           FmtNameOrHex(fmt), static_cast<unsigned>(e.code().value),
                           winrt::to_string(e.message()).c_str());
                    break;
                }
            }
            session.Close();
            pool.Close();
            if (got == 0) {
                printf("    [WGC pool %s] no frame in 2s (static screen?)\n", FmtNameOrHex(fmt));
            } else {
                printf("    [WGC pool %s] %d frames, distinct tex formats:", FmtNameOrHex(fmt), got);
                for (DXGI_FORMAT f : seen) printf(" %s", FmtNameOrHex(f));
                printf("\n");
            }
        } catch (winrt::hresult_error const& e) {
            printf("    [WGC pool %s] create/start failed hr=0x%08X (%hs)\n",
                   FmtNameOrHex(fmt), static_cast<unsigned>(e.code().value),
                   winrt::to_string(e.message()).c_str());
        }
    }
}

// ---------- main ----------

void PrintUsage()
{
    printf("usage: capture_format_spy.exe [--backend dda|wgc|both] [--duration N]\n");
    printf("  Read-only probe of system capture texture formats. Run once with HDR off,\n");
    printf("  once with HDR on, and diff the printed formats (plan appendix D steps 4-5).\n");
}

} // namespace

int main(int argc, char** argv)
{
    int durationSec = 5;
    std::string backend = "both";

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--backend") == 0 && i + 1 < argc) {
            backend = argv[++i];
        } else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
            durationSec = atoi(argv[++i]);
        } else {
            PrintUsage();
            return 0;
        }
    }
    if (durationSec < 1) durationSec = 1;
    if (durationSec > 30) durationSec = 30;

    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (...) {
        // COM 已初始化等情况不影响功能
    }

    printf("capture_format_spy (read-only)  backend=%s duration=%ds\n", backend.c_str(), durationSec);

    // 枚举桌面输出
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(::CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf())))) {
        printf("CreateDXGIFactory1 failed\n");
        return 1;
    }

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, adapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++a) {
        DXGI_ADAPTER_DESC1 ad{};
        adapter->GetDesc1(&ad);
        printf("\n--- adapter %u: %ls (LUID %08X:%08X) ---\n", a, ad.Description,
               ad.AdapterLuid.HighPart, ad.AdapterLuid.LowPart);

        ComPtr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, output.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++o) {
            DXGI_OUTPUT_DESC od{};
            if (FAILED(output->GetDesc(&od)) || !od.AttachedToDesktop) {
                continue;
            }
            if (backend == "dda" || backend == "both") {
                RunDda(adapter.Get(), output.Get(), durationSec);
            } else {
                printf("\n===== DXGI output (status only) =====\n    %ls\n", od.DeviceName);
            }
        }

        if (backend == "wgc" || backend == "both") {
            HMONITOR primary = ::MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
            RunWgc(adapter.Get(), primary);
        }
    }

    printf("\ndone. Re-run with HDR off/on and diff formats; save logs under docs/recon/.\n");
    return 0;
}
