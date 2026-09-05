// tonemap_scrgb.hlsl — 输入路径 A：FP16 scRGB → SDR Rec.709（P4，计划书 §8.1 / §8.3 / §8.5）
//
// 物理模型与要点：
//  1. scRGB 是线性光、Rec.709/sRGB 原色；标称 (1,1,1) = 80 nits D65 白。
//  2. Windows Advanced Color 下，SDR 参考白通过常量缓冲注入（sdrWhiteNits，如 280 nits 对应 scRGB 白点 3.5）。
//  3. 绝对禁止在色调映射前直接 clamp 到 1.0（否则高光大面积爆白截断）。
//  4. 归一化到 SDR 参考白基准，使 SDR UI 处于 1.0 附近，HDR 高光平滑 roll-off。
//  5. 支持 Clamp(对照)、Extended Reinhard、Hable、ACES Fitted、Luminance Hue-Preserving 5 种算法。
//  6. 输出 Rec.709 OETF（可切 sRGB），落入 [0, 1] 供 8-bit SDR 编码使用。

Texture2D<float4> g_InputTexture : register(t0);
SamplerState      g_LinearSampler : register(s0);

cbuffer ToneMapConstants : register(b0)
{
    float sdrWhiteNits;      // 系统 SDR 参考白（默认 280.0f）
    float sourcePeakNits;    // 源高光峰值（默认 1000.0f）
    float exposure;          // 曝光 EV 补偿（默认 0.0f）
    uint  toneMapper;        // 0: Clamp, 1: Reinhard, 2: Hable, 3: ACES, 4: Luma-HuePreserve
    float highlightRollOff;  // 高光滚降调节系数（默认 1.0f）
    float sdrTargetNits;     // SDR 目标白（默认 80.0f）
    uint  oetfType;          // 0: Rec.709, 1: sRGB, 2: Linear
    float pad;
};

struct VSOutput
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

// 全屏三角形顶点着色器（无须 VBO，根据 SV_VertexID 生成全屏覆盖）
VSOutput ToneMapVS(uint id : SV_VertexID)
{
    VSOutput output;
    output.uv = float2((id << 1) & 2, id & 2);
    output.pos = float4(output.uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return output;
}

// -------------------------------------------------------------
// OETF (Opto-Electronic Transfer Function)
// -------------------------------------------------------------
float LinearToRec709_Scalar(float L)
{
    L = saturate(L);
    return (L < 0.018f) ? (4.5f * L) : (1.099f * pow(L, 0.45f) - 0.099f);
}

float3 LinearToRec709(float3 rgb)
{
    return float3(
        LinearToRec709_Scalar(rgb.r),
        LinearToRec709_Scalar(rgb.g),
        LinearToRec709_Scalar(rgb.b)
    );
}

float LinearToSRGB_Scalar(float L)
{
    L = saturate(L);
    return (L <= 0.0031308f) ? (12.92f * L) : (1.055f * pow(L, 1.0f / 2.4f) - 0.055f);
}

float3 LinearToSRGB(float3 rgb)
{
    return float3(
        LinearToSRGB_Scalar(rgb.r),
        LinearToSRGB_Scalar(rgb.g),
        LinearToSRGB_Scalar(rgb.b)
    );
}

// -------------------------------------------------------------
// 色调映射算法实现
// -------------------------------------------------------------

// Hable / Uncharted 2 曲线
float3 HableCurve(float3 x)
{
    const float A = 0.15f; // Shoulder Strength
    const float B = 0.50f; // Linear Strength
    const float C = 0.10f; // Linear Angle
    const float D = 0.20f; // Toe Strength
    const float E = 0.02f; // Toe Numerator
    const float F = 0.30f; // Toe Denominator
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

float3 ToneMap_Hable(float3 linearCol, float peakWhite)
{
    const float exposureBias = 2.0f;
    float3 curr = HableCurve(linearCol * exposureBias);
    float3 whiteScale = 1.0f / HableCurve(float3(peakWhite * exposureBias, peakWhite * exposureBias, peakWhite * exposureBias));
    return curr * whiteScale;
}

// ACES Fitted (Stephen Hill / Narkowicz 拟合改进)
float3 ToneMap_ACES(float3 color)
{
    float3 x = color * 0.6f;
    const float a = 2.51f;
    const float b = 0.03f;
    const float c = 2.43f;
    const float d = 0.59f;
    const float e = 0.14f;
    // 渐近极限为 a/c = 2.51/2.43 ≈ 1.0329
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e)) / 1.033f;
}

// Extended Reinhard (白点控制)
float3 ToneMap_Reinhard(float3 color, float peakWhite, float rollOff)
{
    float maxCh = max(color.r, max(color.g, color.b));
    if (maxCh < 1e-6f) return color;

    float white = max(1.0f, peakWhite * rollOff);
    float white2 = white * white;
    float mapped = (maxCh * (1.0f + (maxCh / white2))) / (1.0f + maxCh);
    return color * (mapped / maxCh);
}

// 基于亮度/最大分量的高光平滑 roll-off + 色相与饱和度保持（Alpha 策略）
// 特性：
//   1. 在膝点 knee (默认 0.70) 以下，100% 严格线性保真 (f(x) = x)，SDR 内容、暗部与中间调无任何畸变；
//   2. 在膝点之上，采用一阶连续 C1 有理曲线平滑压缩高光，使超亮 HDR 内容渐近收敛于 1.0，绝不爆白截断；
//   3. 使用 max(R,G,B) 驱动同比例缩放，严格保持色相不变，彩色高光不泛白、肤色不漂移。
float3 ToneMap_LumaHuePreserving(float3 color, float peakWhite, float rollOff)
{
    float maxCh = max(color.r, max(color.g, color.b));
    if (maxCh < 1e-6f) return color;

    // 线性保留膝点 (0.7 对应 SDR 中间灰到高光过渡区域)
    const float k = 0.70f;
    if (maxCh <= k) {
        return color; // SDR 基础区域 1:1 无损透传
    }

    // 平滑压缩函数：在 x=k 处 f(k)=k, f'(k)=1；当 x->peakWhite 时平滑渐近于 1.0
    // 公式: f(x) = k + (1 - k) * (x - k) / [ (x - k) + S ]
    // 其中 S 控制压缩陡峭程度，S = (1 - k) * rollOff
    float delta = maxCh - k;
    float S = (1.0f - k) * max(0.2f, rollOff);
    float compressed = k + ((1.0f - k) * delta) / (delta + S);

    float scale = compressed / maxCh;
    return color * scale;
}

// -------------------------------------------------------------
// Pixel Shader 主入口
// -------------------------------------------------------------
float4 ToneMapPS(VSOutput input) : SV_Target
{
    float4 raw = g_InputTexture.Sample(g_LinearSampler, input.uv);

    // 1. 保留原始正数值，防止非法 NaN / inf
    float3 linearRgb = max(0.0f, raw.rgb);

    // 2. 曝光调节 (EV)
    linearRgb *= exp2(exposure);

    // 3. 归一化到 SDR 参考白基准
    // scRGB 中 (1,1,1) 对应 80 nits；Windows SDR 参考白为 sdrWhiteNits (默认 280)
    // 纯白 SDR UI 在 scRGB 中的值为 sdrWhiteNits / 80.0
    float safeSdrWhite = max(10.0f, sdrWhiteNits);
    float sdrScale = 80.0f / safeSdrWhite;
    float3 normRgb = linearRgb * sdrScale;

    // 相对 SDR 白的源高光峰值比例 (例如 1500 nits / 280 nits ≈ 5.36)
    float peakScale = max(1.0001f, sourcePeakNits / safeSdrWhite);

    // 4. 选择色调映射算法
    float3 mappedRgb = normRgb;
    switch (toneMapper)
    {
    case 0: // Clamp (基线对照组：复现直接裁切的爆白现象)
        mappedRgb = saturate(normRgb);
        break;

    case 1: // Extended Reinhard
        mappedRgb = ToneMap_Reinhard(normRgb, peakScale, highlightRollOff);
        break;

    case 2: // Hable
        mappedRgb = ToneMap_Hable(normRgb, peakScale);
        break;

    case 3: // ACES Fitted
        mappedRgb = ToneMap_ACES(normRgb);
        break;

    case 4: // Luminance Hue-Preserving (推荐默认)
    default:
        mappedRgb = ToneMap_LumaHuePreserving(normRgb, peakScale, highlightRollOff);
        break;
    }

    // 5. 色域与边界保护
    mappedRgb = saturate(mappedRgb);

    // 6. 传递函数 / OETF 转换为 SDR 显示空间
    float3 finalSdr;
    if (oetfType == 0) {
        finalSdr = LinearToRec709(mappedRgb);
    } else if (oetfType == 1) {
        finalSdr = LinearToSRGB(mappedRgb);
    } else {
        finalSdr = mappedRgb; // Linear Passthrough
    }

    return float4(finalSdr, raw.a);
}
