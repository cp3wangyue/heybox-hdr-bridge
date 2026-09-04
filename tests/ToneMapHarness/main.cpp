// ToneMapHarness — P4 独立算法测试台入口（计划书 §8.6）
//
// Gate P2 未通过前不实现调参逻辑（计划书红线："不要先写 Tone Mapping"）。
// 当前入口只做两件事：
//   1) 打印 GPU 设备/适配器信息，作为后续 4K60 性能基线的环境记录；
//   2) 若 shaders/*.hlsl 存在则尝试按 ps_5_0 编译，报告编译结果，
//      为 P4 实现 shader 时提供即时编译反馈。

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <wrl/client.h>

#include <cstdio>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;

namespace {

void TryCompileShader(const wchar_t* file, const char* entry, const char* target)
{
    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> errors;
    HRESULT hr = D3DCompileFromFile(file, nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                    entry, target, D3DCOMPILE_ENABLE_STRICTNESS, 0,
                                    blob.GetAddressOf(), errors.GetAddressOf());
    if (SUCCEEDED(hr)) {
        printf("[harness] compile %ls:%s (%s) OK (%zu bytes)\n", file, entry, target,
               blob->GetBufferSize());
    } else {
        printf("[harness] compile %ls:%s FAILED hr=0x%08lX\n", file, entry, hr);
        if (errors) {
            printf("    %.*s\n", static_cast<int>(errors->GetBufferSize()),
                   static_cast<const char*>(errors->GetBufferPointer()));
        } else if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) {
            printf("    (shader not present; P4 not started — expected before Gate P2)\n");
        }
    }
}

} // namespace

int main()
{
    printf("== ToneMapHarness (P4 scaffolding) ==\n");
    printf("红线：Gate P2（真实帧路径五项证据）未确认前，不在此实现 Tone Mapping。\n\n");

    D3D_FEATURE_LEVEL fl{};
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                   nullptr, 0, D3D11_SDK_VERSION, &device, &fl, &context);
    if (FAILED(hr)) {
        printf("D3D11CreateDevice failed hr=0x%08lX\n", hr);
        return 1;
    }

    IDXGIDevice* dxgiDevice = nullptr;
    if (SUCCEEDED(device->QueryInterface(&dxgiDevice))) {
        IDXGIAdapter* adapter = nullptr;
        if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
            DXGI_ADAPTER_DESC ad{};
            adapter->GetDesc(&ad);
            printf("adapter: %ls  (dedicated VRAM %.1f GB)\n", ad.Description,
                   ad.DedicatedVideoMemory / (1024.0 * 1024.0 * 1024.0));
            adapter->Release();
        }
        dxgiDevice->Release();
    }
    printf("feature level: 0x%04X\n\n", fl);

    TryCompileShader(L"shaders/tonemap_scrgb.hlsl", "ToneMapPS", "ps_5_0");
    TryCompileShader(L"shaders/tonemap_pq.hlsl", "ToneMapPS", "ps_5_0");
    TryCompileShader(L"shaders/rgb_to_nv12.hlsl", "RGBToNV12CS", "cs_5_0");

    context->Release();
    device->Release();
    return 0;
}
