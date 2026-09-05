// ToneMapCore.cpp — P4 GPU Tone Mapping 核心实现
#include "ToneMapCore.h"

#include <d3dcompiler.h>
#include <cstdio>
#include <vector>

#pragma comment(lib, "d3dcompiler.lib")

namespace hdrfix {

namespace {

// 辅助查找 shader 文件（兼容不同当前工作目录）
std::wstring ResolveShaderPath(const std::wstring& relPath)
{
    const wchar_t* candidates[] = {
        relPath.c_str(),
        (L"../" + relPath).c_str(),
        (L"../../" + relPath).c_str(),
        (L"../../../" + relPath).c_str(),
        (L"hdr-share-fix/" + relPath).c_str()
    };
    for (const auto* path : candidates) {
        DWORD attr = GetFileAttributesW(path);
        if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
            return path;
        }
    }
    return relPath;
}

const char g_EmbeddedShaderSource[] = R"(
Texture2D<float4> g_InputTexture : register(t0);
SamplerState      g_LinearSampler : register(s0);

cbuffer ToneMapConstants : register(b0)
{
    float sdrWhiteNits;
    float sourcePeakNits;
    float exposure;
    uint  toneMapper;
    float highlightRollOff;
    float sdrTargetNits;
    uint  oetfType;
    float pad;
};

struct VSOutput
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOutput ToneMapVS(uint id : SV_VertexID)
{
    VSOutput output;
    output.uv = float2((id << 1) & 2, id & 2);
    output.pos = float4(output.uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return output;
}

float LinearToRec709_Scalar(float L)
{
    L = saturate(L);
    return (L < 0.018f) ? (4.5f * L) : (1.099f * pow(L, 0.45f) - 0.099f);
}

float3 LinearToRec709(float3 rgb)
{
    return float3(LinearToRec709_Scalar(rgb.r), LinearToRec709_Scalar(rgb.g), LinearToRec709_Scalar(rgb.b));
}

float LinearToSRGB_Scalar(float L)
{
    L = saturate(L);
    return (L <= 0.0031308f) ? (12.92f * L) : (1.055f * pow(L, 1.0f / 2.4f) - 0.055f);
}

float3 LinearToSRGB(float3 rgb)
{
    return float3(LinearToSRGB_Scalar(rgb.r), LinearToSRGB_Scalar(rgb.g), LinearToSRGB_Scalar(rgb.b));
}

float3 HableCurve(float3 x)
{
    const float A = 0.15f, B = 0.50f, C = 0.10f, D = 0.20f, E = 0.02f, F = 0.30f;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

float3 ToneMap_Hable(float3 linearCol, float peakWhite)
{
    const float exposureBias = 2.0f;
    float3 curr = HableCurve(linearCol * exposureBias);
    float3 whiteScale = 1.0f / HableCurve(float3(peakWhite * exposureBias, peakWhite * exposureBias, peakWhite * exposureBias));
    return curr * whiteScale;
}

float3 ToneMap_ACES(float3 color)
{
    float3 x = color * 0.6f;
    const float a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e)) / 1.033f;
}

float3 ToneMap_Reinhard(float3 color, float peakWhite, float rollOff)
{
    float maxCh = max(color.r, max(color.g, color.b));
    if (maxCh < 1e-6f) return color;
    float white = max(1.0f, peakWhite * rollOff);
    float white2 = white * white;
    float mapped = (maxCh * (1.0f + (maxCh / white2))) / (1.0f + maxCh);
    return color * (mapped / maxCh);
}

float3 ToneMap_LumaHuePreserving(float3 color, float peakWhite, float rollOff)
{
    float maxCh = max(color.r, max(color.g, color.b));
    if (maxCh < 1e-6f) return color;
    const float k = 0.70f;
    if (maxCh <= k) return color;
    float delta = maxCh - k;
    float S = (1.0f - k) * max(0.2f, rollOff);
    float compressed = k + ((1.0f - k) * delta) / (delta + S);
    return color * (compressed / maxCh);
}

float4 ToneMapPS(VSOutput input) : SV_Target
{
    float4 raw = g_InputTexture.Sample(g_LinearSampler, input.uv);
    float3 linearRgb = max(0.0f, raw.rgb);
    linearRgb *= exp2(exposure);

    float safeSdrWhite = max(10.0f, sdrWhiteNits);
    float sdrScale = 80.0f / safeSdrWhite;
    float3 normRgb = linearRgb * sdrScale;
    float peakScale = max(1.0001f, sourcePeakNits / safeSdrWhite);

    float3 mappedRgb = normRgb;
    switch (toneMapper)
    {
    case 0: mappedRgb = saturate(normRgb); break;
    case 1: mappedRgb = ToneMap_Reinhard(normRgb, peakScale, highlightRollOff); break;
    case 2: mappedRgb = ToneMap_Hable(normRgb, peakScale); break;
    case 3: mappedRgb = ToneMap_ACES(normRgb); break;
    case 4:
    default:
        mappedRgb = ToneMap_LumaHuePreserving(normRgb, peakScale, highlightRollOff);
        break;
    }
    mappedRgb = saturate(mappedRgb);

    float3 finalSdr;
    if (oetfType == 0) finalSdr = LinearToRec709(mappedRgb);
    else if (oetfType == 1) finalSdr = LinearToSRGB(mappedRgb);
    else finalSdr = mappedRgb;

    return float4(finalSdr, raw.a);
}
)";

} // namespace

bool ToneMapCore::Initialize(ID3D11Device* device, const std::wstring& shaderPath)
{
    if (!device) return false;
    m_device = device;

    std::wstring resolvedPath = ResolveShaderPath(shaderPath);
    DWORD attr = GetFileAttributesW(resolvedPath.c_str());
    bool useFile = (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY));

    // 1. 编译全屏顶点着色器 ToneMapVS
    Microsoft::WRL::ComPtr<ID3DBlob> vsBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    HRESULT hr = S_OK;

    if (useFile) {
        hr = D3DCompileFromFile(resolvedPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                "ToneMapVS", "vs_5_0", D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                                vsBlob.GetAddressOf(), errors.GetAddressOf());
    }
    if (!useFile || FAILED(hr)) {
        errors.Reset();
        hr = D3DCompile(g_EmbeddedShaderSource, sizeof(g_EmbeddedShaderSource) - 1, "tonemap_embedded.hlsl", nullptr, nullptr,
                        "ToneMapVS", "vs_5_0", D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                        vsBlob.GetAddressOf(), errors.GetAddressOf());
    }
    if (FAILED(hr)) {
        printf("[ToneMapCore] Failed to compile ToneMapVS (hr=0x%08lX)\n", hr);
        if (errors) {
            printf("  %.*s\n", static_cast<int>(errors->GetBufferSize()), static_cast<const char*>(errors->GetBufferPointer()));
        }
        return false;
    }

    hr = m_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, m_vs.GetAddressOf());
    if (FAILED(hr)) return false;

    // 2. 编译色调映射像素着色器 ToneMapPS
    Microsoft::WRL::ComPtr<ID3DBlob> psBlob;
    errors.Reset();
    if (useFile) {
        hr = D3DCompileFromFile(resolvedPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                "ToneMapPS", "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                                psBlob.GetAddressOf(), errors.GetAddressOf());
    }
    if (!useFile || FAILED(hr)) {
        errors.Reset();
        hr = D3DCompile(g_EmbeddedShaderSource, sizeof(g_EmbeddedShaderSource) - 1, "tonemap_embedded.hlsl", nullptr, nullptr,
                        "ToneMapPS", "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                        psBlob.GetAddressOf(), errors.GetAddressOf());
    }
    if (FAILED(hr)) {
        printf("[ToneMapCore] Failed to compile ToneMapPS (hr=0x%08lX)\n", hr);
        if (errors) {
            printf("  %.*s\n", static_cast<int>(errors->GetBufferSize()), static_cast<const char*>(errors->GetBufferPointer()));
        }
        return false;
    }

    hr = m_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, m_ps.GetAddressOf());
    if (FAILED(hr)) return false;

    // 3. 创建管线状态对象
    if (!CreatePipelineStates(m_device.Get())) {
        return false;
    }

    m_initialized = true;
    m_paramsDirty = true;
    return true;
}

bool ToneMapCore::CreatePipelineStates(ID3D11Device* device)
{
    // 常量缓冲
    D3D11_BUFFER_DESC cbDesc{};
    cbDesc.ByteWidth = sizeof(ToneMapParams);
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    HRESULT hr = device->CreateBuffer(&cbDesc, nullptr, m_cbuffer.GetAddressOf());
    if (FAILED(hr)) return false;

    // 采样器状态
    D3D11_SAMPLER_DESC sampDesc{};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampDesc.MinLOD = 0;
    sampDesc.MaxLOD = D3D11_FLOAT32_MAX;
    hr = device->CreateSamplerState(&sampDesc, m_sampler.GetAddressOf());
    if (FAILED(hr)) return false;

    // 栅格化状态
    D3D11_RASTERIZER_DESC rastDesc{};
    rastDesc.FillMode = D3D11_FILL_SOLID;
    rastDesc.CullMode = D3D11_CULL_NONE;
    rastDesc.DepthClipEnable = FALSE;
    hr = device->CreateRasterizerState(&rastDesc, m_rasterizerState.GetAddressOf());
    if (FAILED(hr)) return false;

    // 混合状态（完全覆盖，无透明混合）
    D3D11_BLEND_DESC blendDesc{};
    blendDesc.RenderTarget[0].BlendEnable = FALSE;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = device->CreateBlendState(&blendDesc, m_blendState.GetAddressOf());
    if (FAILED(hr)) return false;

    return true;
}

void ToneMapCore::SetParams(const ToneMapParams& params)
{
    m_params = params;
    m_paramsDirty = true;
}

bool ToneMapCore::UpdateConstantBuffer(ID3D11DeviceContext* context)
{
    if (!m_cbuffer) return false;
    if (!m_paramsDirty) return true;

    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = context->Map(m_cbuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) return false;

    memcpy(mapped.pData, &m_params, sizeof(ToneMapParams));
    context->Unmap(m_cbuffer.Get(), 0);
    m_paramsDirty = false;
    return true;
}

bool ToneMapCore::Execute(ID3D11DeviceContext* context,
                          ID3D11ShaderResourceView* inputSRV,
                          ID3D11RenderTargetView* outputRTV,
                          UINT width,
                          UINT height)
{
    if (!m_initialized || !context || !inputSRV || !outputRTV) return false;

    if (!UpdateConstantBuffer(context)) return false;

    // 视口设置
    D3D11_VIEWPORT vp{};
    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.Width = static_cast<float>(width);
    vp.Height = static_cast<float>(height);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    context->RSSetViewports(1, &vp);

    // 绑定拓扑与着色器（无输入布局，全屏三角形由顶点 ID 生成）
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    context->VSSetShader(m_vs.Get(), nullptr, 0);
    context->PSSetShader(m_ps.Get(), nullptr, 0);

    ID3D11Buffer* cbs[] = { m_cbuffer.Get() };
    context->PSSetConstantBuffers(0, 1, cbs);

    ID3D11ShaderResourceView* srvs[] = { inputSRV };
    context->PSSetShaderResources(0, 1, srvs);

    ID3D11SamplerState* samplers[] = { m_sampler.Get() };
    context->PSSetSamplers(0, 1, samplers);

    context->RSSetState(m_rasterizerState.Get());
    context->OMSetBlendState(m_blendState.Get(), nullptr, 0xffffffff);

    ID3D11RenderTargetView* rtvs[] = { outputRTV };
    context->OMSetRenderTargets(1, rtvs, nullptr);

    // 触发全屏绘制 (3 顶点覆盖屏幕)
    context->Draw(3, 0);

    // 绘制完成后解绑，防止后续管道 Hazard
    ID3D11ShaderResourceView* nullSRVs[] = { nullptr };
    context->PSSetShaderResources(0, 1, nullSRVs);

    ID3D11RenderTargetView* nullRTVs[] = { nullptr };
    context->OMSetRenderTargets(1, nullRTVs, nullptr);

    return true;
}

bool ToneMapCore::Execute(ID3D11DeviceContext* context,
                          ID3D11Texture2D* inputTexture,
                          ID3D11Texture2D* outputTexture)
{
    if (!m_initialized || !context || !inputTexture || !outputTexture) return false;

    D3D11_TEXTURE2D_DESC inDesc{};
    inputTexture->GetDesc(&inDesc);

    D3D11_TEXTURE2D_DESC outDesc{};
    outputTexture->GetDesc(&outDesc);

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    HRESULT hr = m_device->CreateShaderResourceView(inputTexture, nullptr, srv.GetAddressOf());
    if (FAILED(hr)) return false;

    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
    hr = m_device->CreateRenderTargetView(outputTexture, nullptr, rtv.GetAddressOf());
    if (FAILED(hr)) return false;

    return Execute(context, srv.Get(), rtv.Get(), outDesc.Width, outDesc.Height);
}

} // namespace hdrfix
