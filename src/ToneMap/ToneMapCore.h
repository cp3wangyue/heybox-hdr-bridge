#pragma once
// ToneMapCore.h — GPU Tone Mapping 核心模块
//
// 负责在 D3D11 GPU 端将 FP16 scRGB 输入转换为 Rec.709/sRGB SDR 输出。
// 全程 GPU 处理，无 CPU Readback，支持参数动态注入。
// 提供严格的 RAII 管线状态隔离（D3D11StateGuard），保护宿主 Immediate Context。

#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>

namespace hdrfix {

enum class ToneMapperType : uint32_t {
    Clamp = 0,             // 基线对照组（模拟无映射直接裁切）
    ExtendedReinhard = 1,  // 带峰值白点控制的扩展 Reinhard
    Hable = 2,             // Filmic Hable / Uncharted 2 曲线
    ACES = 3,              // 电影级 ACES Fitted
    LumaHuePreserving = 4, // 基于亮度的色度保持滚降
    BT2390 = 5,            // 工业级 ITU-R BT.2390 EETF（OBS Studio 28+ 官方画质对齐，推荐默认）
    OBSReinhard = 6        // OBS 风格 Rec.2020 宽色域 Reinhard
};

enum class OetfType : uint32_t {
    Rec709 = 0,  // ITU-R BT.709 OETF（摄像机传统传递函数）
    sRGB = 1,    // 标准 IEC 61966-2-1 sRGB 曲线（OBS 默认，对比度鲜明暗部扎实）
    Linear = 2,  // 保持线性（调试用途）
    Gamma24 = 3  // ITU-R BT.1886 纯 Gamma 2.4
};

// 与 HLSL cbuffer ToneMapConstants 内存对齐一致 (16 bytes 边界)
struct ToneMapParams {
    float sdrWhiteNits = 280.0f;       // 系统 SDR 参考白（系统读取或实测值，如 280 nits）
    float sourcePeakNits = 1000.0f;    // 源高光峰值（nits，如 1000 或 1500）
    float exposure = 0.0f;             // 曝光补偿 (EV)
    uint32_t toneMapper = static_cast<uint32_t>(ToneMapperType::BT2390);
    float highlightRollOff = 1.0f;     // 高光滚降调节系数
    float sdrTargetNits = 80.0f;       // 目标 SDR 白（标称 80 nits）
    uint32_t oetfType = static_cast<uint32_t>(OetfType::sRGB);
    float pad = 0.0f;
};

// RAII 状态隔离守卫：保存并恢复受 Execute 影响的 D3D11 Immediate Context 状态
class D3D11StateGuard {
public:
    explicit D3D11StateGuard(ID3D11DeviceContext* context);
    ~D3D11StateGuard();

    D3D11StateGuard(const D3D11StateGuard&) = delete;
    D3D11StateGuard& operator=(const D3D11StateGuard&) = delete;

private:
    ID3D11DeviceContext* m_context = nullptr;

    // Viewports
    D3D11_VIEWPORT m_savedViewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    UINT m_numSavedViewports = 0;

    // IA
    Microsoft::WRL::ComPtr<ID3D11InputLayout> m_savedInputLayout;
    D3D11_PRIMITIVE_TOPOLOGY m_savedTopology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;

    // VS / PS
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_savedVS;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_savedPS;

    // PS Resources & Sampler (Slot 0)
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_savedPSCB;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_savedPSSRV;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_savedPSSampler;

    // RS
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_savedRasterizerState;

    // Blend
    Microsoft::WRL::ComPtr<ID3D11BlendState> m_savedBlendState;
    FLOAT m_savedBlendFactor[4]{};
    UINT m_savedSampleMask = 0xffffffff;

    // OM (RTVs + DSV)
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_savedRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> m_savedDSV;
};

class ToneMapCore {
public:
    ToneMapCore() = default;
    ~ToneMapCore() = default;

    // 禁止拷贝，允许移动
    ToneMapCore(const ToneMapCore&) = delete;
    ToneMapCore& operator=(const ToneMapCore&) = delete;
    ToneMapCore(ToneMapCore&&) noexcept = default;
    ToneMapCore& operator=(ToneMapCore&&) noexcept = default;

    // 初始化 Shader、Sampler、ConstantBuffer 等 GPU 常驻资源
    bool Initialize(ID3D11Device* device, const std::wstring& shaderPath = L"shaders/tonemap_scrgb.hlsl");

    // 更新色调映射参数
    void SetParams(const ToneMapParams& params);
    const ToneMapParams& GetParams() const { return m_params; }

    // 执行单次色调映射 Pass（全屏三角形绘制，内部严格 RAII 状态隔离）
    bool Execute(ID3D11DeviceContext* context,
                 ID3D11ShaderResourceView* inputSRV,
                 ID3D11RenderTargetView* outputRTV,
                 UINT width,
                 UINT height);

    // 针对 Texture2D 的便捷执行封装（若传入已创建好的纹理，内部维护兼容的 SRV/RTV 缓存）
    bool Execute(ID3D11DeviceContext* context,
                 ID3D11Texture2D* inputTexture,
                 ID3D11Texture2D* outputTexture);

    bool IsInitialized() const { return m_initialized; }

private:
    bool CreatePipelineStates(ID3D11Device* device);
    bool UpdateConstantBuffer(ID3D11DeviceContext* context);

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_ps;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_cbuffer;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_sampler;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_rasterizerState;
    Microsoft::WRL::ComPtr<ID3D11BlendState> m_blendState;

    ToneMapParams m_params{};
    bool m_paramsDirty = true;
    bool m_initialized = false;
};

} // namespace hdrfix
