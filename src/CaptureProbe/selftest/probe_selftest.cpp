// CaptureProbe 自检：验证 HDR 状态查询、纹理描述快照与采样日志可用。
// 运行：probe_selftest.exe [日志目录]

#include <d3d11.h>
#include <windows.h>

#include <cstdio>
#include <string>

#include "CaptureProbe/hdr_state.h"
#include "CaptureProbe/probe_logger.h"
#include "CaptureProbe/texture_info.h"

using namespace hdrfix;

int main(int argc, char** argv)
{
    printf("== CaptureProbe selftest ==\n\n");

    // 1) 系统 HDR 状态
    printf("[1] Output HDR states:\n");
    for (const auto& st : QueryOutputHdrStates()) {
        char line[512]{};
        sprintf_s(line, "    %ls: hdrEnabled=%d hdrSupported=%d bits/color=%u sdrWhite=%.1f nits colorspace=%s",
                  st.gdiDeviceName.c_str(), st.hdrEnabled ? 1 : 0, st.hdrSupported ? 1 : 0,
                  st.bitsPerColor, st.sdrWhiteNits,
                  ColorSpaceName(st.colorSpace) ? ColorSpaceName(st.colorSpace) : "unknown");
        printf("%s\n", line);
    }

    // 2) 纹理描述快照
    printf("\n[2] TextureInfo snapshot:\n");
    UINT createFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL fl{};
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createFlags,
                                   nullptr, 0, D3D11_SDK_VERSION, &device, &fl, &context);
    if (FAILED(hr)) {
        printf("    D3D11CreateDevice failed hr=0x%08lX\n", hr);
        return 1;
    }
    printf("    device created, feature level 0x%04X\n", fl);

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = 1920;
    desc.Height = 1080;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D* tex = nullptr;
    hr = device->CreateTexture2D(&desc, nullptr, &tex);
    if (SUCCEEDED(hr)) {
        printf("    %s\n", TextureInfo::FromTexture2D(tex).ToString().c_str());
        tex->Release();
    } else {
        printf("    CreateTexture2D(FP16) failed hr=0x%08lX\n", hr);
    }

    // 3) 采样日志（1 条强制 + 2 条被限流丢弃）
    printf("\n[3] ProbeLogger sampling:\n");
    std::wstring logDir = (argc > 1) ? (std::wstring(argv[1], argv[1] + strlen(argv[1])) + L"\\") : L"logs\\";
    ::CreateDirectoryW(logDir.c_str(), nullptr); // 不存在则创建；已存在忽略
    std::wstring logPath = logDir + L"probe_selftest.log";
    if (!ProbeLogger::Instance().Start(logPath, std::chrono::milliseconds(1000))) {
        printf("    logger start failed: %ls\n", logPath.c_str());
        return 1;
    }
    FrameLogRecord r;
    r.frame = 1;
    r.tid = ::GetCurrentThreadId();
    r.sourceDevice = device;
    r.sourceTexture = nullptr;
    r.tex.width = 3840; r.tex.height = 2160; r.tex.format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    r.tex.bindFlags = D3D11_BIND_SHADER_RESOURCE;
    r.windowsHdr = true;
    r.outputColorSpace = "RGB_FULL_G2084_NONE_P2020";
    r.sdrWhiteNits = 80.f;
    r.path = "ToneMapScRGB->Rec709RGB->NV12Limited";
    r.encoderInputFormat = "NV12";
    r.bypass = true;
    r.force = true; // 逐帧采样（Debug）
    ProbeLogger::Instance().LogFrame(r);
    r.force = false;
    ProbeLogger::Instance().LogFrame(r); // 限流窗口内 → dropped
    r.frame = 2;
    ProbeLogger::Instance().LogFrame(r); // 仍被限流 → dropped=2
    printf("    dropped (throttled) = %llu\n",
           static_cast<unsigned long long>(ProbeLogger::Instance().DroppedCount()));
    ProbeLogger::Instance().Stop();
    printf("    log written: %ls\n", logPath.c_str());

    context->Release();
    device->Release();
    printf("\nselftest OK\n");
    return 0;
}
