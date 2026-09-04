// probe_testhost — 探针管线测试宿主（不接触客户端）
//
// 模式：
//   probe_testhost.exe --inproc            自加载探针（LoadLibrary），创建设备+测试纹理，等待采样
//   probe_testhost.exe --remote [--secs 12]  等待被注入（探针 ready 事件）后创建设备+纹理，等待退出
//
// 测试纹理集（桌面级，验证观察器过滤与采样统计）：
//   3840x2160 R16G16B16A16_FLOAT  —— 部分像素 >1.0（模拟 HDR 高光）
//   3840x2160 B8G8R8A8_UNORM      —— 部分像素亮度 250~255（模拟裁白）
//   1920x1080 NV12                —— 应被登记但跳过亮度采样
// 期望：日志出现 [CREATE] 去重记录与 PixelSample 统计（overWhite≈0.5%、bright≈0.5% 量级）。

#define WIN32_LEAN_AND_MEAN
#include <d3d11.h>
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

#pragma comment(lib, "d3d11.lib")

static ID3D11Texture2D* MakeTexture(ID3D11Device* dev, UINT w, UINT h, DXGI_FORMAT fmt, bool bright)
{
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
                unsigned short half = (unsigned short)(0x3C00 | ((v > 1.0f) ? 0x0040 : 0)); // 粗略编码
                // 更精确：直接把 1.5 与 0.25 的 half 位写死
                half = (v > 1.0f) ? 0x3E00 /*1.5*/ : 0x3500 /*0.25*/;
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
                data[(size_t(y) * w + x) * 4 + 3] = 255;
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

int main(int argc, char** argv)
{
    bool inproc = false, remote = false;
    int secs = 12;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--inproc")) inproc = true;
        else if (!strcmp(argv[i], "--remote")) remote = true;
        else if (!strcmp(argv[i], "--secs") && i + 1 < argc) secs = atoi(argv[++i]);
    }
    if (!inproc && !remote) {
        printf("usage: probe_testhost.exe --inproc | --remote [--secs N]\n");
        return 1;
    }

    if (inproc) {
        // 探针 dll 与本 exe 同目录构建输出
        char path[MAX_PATH]{};
        ::GetModuleFileNameA(nullptr, path, MAX_PATH);
        std::string dir(path);
        dir = dir.substr(0, dir.find_last_of("\\/") + 1) + "hdrfix_probe.dll";
        // 测试目标模块 = 本进程 exe（IAT hook 归属）
        SetEnvironmentVariableA("HDRFIX_PROBE_MODULE", "probe_testhost.exe");
        SetEnvironmentVariableA("HDRFIX_PROBE_LOG", "logs\\probe_testhost.log");
        SetEnvironmentVariableA("HDRFIX_PROBE_MAXSEC", "30");
        CreateDirectoryA("logs", nullptr);
        HMODULE probe = LoadLibraryA(dir.c_str());
        printf("probe dll=%s load=%p\n", dir.c_str(), reinterpret_cast<void*>(probe));
        if (!probe) { printf("gle=%lu\n", GetLastError()); return 1; }
        Sleep(500); // 让 worker 先装 hook
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

    // remote：等待注入探针 ready（probe 安装 hook 后不设 ready 事件——这里简化为
    // 先等 HDRFIX_TEST_GO 事件由外部触发，或固定延迟后创建设备）
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
