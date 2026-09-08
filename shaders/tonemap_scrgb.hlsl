// tonemap_scrgb.hlsl — FP16 scRGB → SDR Rec.709/sRGB
//
// 集成 ITU-R BT.2390 EETF（OBS Studio 28+ 对齐）与 Rec.2020 宽色域感知空间映射。
//
// 物理模型与要点：
//  1. scRGB 是线性光、Rec.709/sRGB 原色；标称 (1,1,1) = 80 nits D65 白。
//  2. Windows Advanced Color 下，SDR 参考白通过常量缓冲注入（sdrWhiteNits，如 280 nits 对应 scRGB 白点 3.5）。
//  3. 采用 ITU-R BT.2390 EETF（Hermite 三次样条）：在拐点 KS 以下 100% 严格 1:1 无损透传 SDR 内容；
//     拐点以上在 SMPTE ST 2084 (PQ) 感知空间中平滑压缩，保持一阶连续，零爆白裁切。
//  4. 在 Rec.2020 宽色域中执行感知色调映射，避免高饱和色彩在 Rec.709 边界剪切与色相畸变。
//  5. 采用等比例 RGB 缩放，保持 0 色相漂移，彩色高光不泛白。
//  6. 默认输出 IEC 61966-2-1 sRGB OETF（与 OBS 对齐），提供扎实深沉的暗部与鲜明通透的对比度。

Texture2D<float4> g_InputTexture : register(t0);
SamplerState      g_LinearSampler : register(s0);

cbuffer ToneMapConstants : register(b0)
{
    float sdrWhiteNits;      // 系统 SDR 参考白（默认 280.0f）
    float sourcePeakNits;    // 源高光峰值（默认 1000.0f）
    float exposure;          // 曝光 EV 补偿（默认 0.0f）
    uint  toneMapper;        // 0: Clamp, 1: Reinhard, 2: Hable, 3: ACES, 4: Luma-HuePreserve, 5: BT2390, 6: OBS-Reinhard
    float highlightRollOff;  // 高光滚降调节系数（默认 1.0f）
    float sdrTargetNits;     // SDR 目标白（默认 80.0f）
    uint  oetfType;          // 0: Rec.709, 1: sRGB, 2: Linear, 3: Gamma2.4
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
// 色彩空间转换矩阵（Rec.709 <-> Rec.2020 D65）
// -------------------------------------------------------------
float3 rec709_to_rec2020(float3 v)
{
    float r = dot(v, float3(0.62740389593469903, 0.32928303837788370, 0.043313065687417225));
    float g = dot(v, float3(0.069097289358232075, 0.91954039507545871, 0.011362315566309178));
    float b = dot(v, float3(0.016391438875150280, 0.088013307877225749, 0.89559525324762401));
    return float3(r, g, b);
}

float3 rec2020_to_rec709(float3 v)
{
    float r = dot(v, float3(1.6604910021084345, -0.58764113878854951, -0.072849863319884883));
    float g = dot(v, float3(-0.12455047452159074, 1.1328998971259603, -0.0083494226043694768));
    float b = dot(v, float3(-0.018150763354905303, -0.10057889800800739, 1.1187296613629127));
    return float3(r, g, b);
}

// -------------------------------------------------------------
// SMPTE ST 2084 (PQ) 传递函数 (1.0 = 10000 nits)
// -------------------------------------------------------------
float linear_to_st2084_channel(float x)
{
    if (x <= 1e-8f) return 0.0f;
    float c = pow(x, 0.1593017578f);
    return pow((0.8359375f + 18.8515625f * c) / (1.0f + 18.6875f * c), 78.84375f);
}

float st2084_to_linear_channel(float u)
{
    if (u <= 1e-8f) return 0.0f;
    float c = pow(u, 1.0f / 78.84375f);
    float num = max(c - 0.8359375f, 0.0f);
    float den = max(1e-6f, 18.8515625f - 18.6875f * c);
    return pow(num / den, 1.0f / 0.1593017578f);
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

// 1. ITU-R BT.2390 EETF (OBS Studio 工业级对齐算法，推荐默认)
// 在 Rec.2020 宽色域中转 + SMPTE ST 2084 感知空间中执行 Hermite 三次样条压缩
// 特性：
//   - 拐点 KS 以下：100% 严格无损 1:1 透传，保持原有对比度、中灰与暗部细节
//   - 拐点 KS 以上：一阶导数连续 C1 平滑滚降至最大白点，无硬裁切
//   - 等比例缩放 RGB：色相 0 偏移，彩色高光不泛白
float3 ToneMap_BT2390(float3 linearScrgb, float sourcePeak, float sdrWhite, float rollOff)
{
    // 将 scRGB 转换为以 10000 nits 为基准的绝对线性光 (scRGB 1.0 = 80 nits)
    float3 rgb10k = max(0.0f, linearScrgb) * (80.0f / 10000.0f);

    // 转入 Rec.2020 宽色域感知空间，防止高光在 Rec.709 边缘发生色相畸变与通道剪切
    float3 rgb2020 = max(0.0f, rec709_to_rec2020(rgb10k));

    float Lw = max(sdrWhite + 10.0f, sourcePeak);
    float Lmax = max(10.0f, sdrWhite);

    float Lw_pq = linear_to_st2084_channel(Lw / 10000.0f);
    float Lmax_pq = linear_to_st2084_channel(Lmax / 10000.0f);

    float maxRGB1_linear = max(rgb2020.r, max(rgb2020.g, rgb2020.b));
    float maxRGB1_pq = linear_to_st2084_channel(maxRGB1_linear);

    float E1 = saturate(maxRGB1_pq / max(1e-6f, Lw_pq));
    float maxLum = Lmax_pq / max(1e-6f, Lw_pq);
    float KS = clamp(((1.5f * maxLum) - 0.5f) * rollOff, 0.0f, 0.99f);

    if (E1 <= KS)
    {
        return saturate(linearScrgb * (80.0f / Lmax));
    }

    float T = (E1 - KS) / max(1e-6f, 1.0f - KS);
    float Tsquared = T * T;
    float Tcubed = Tsquared * T;
    float P = (2.0f * Tcubed - 3.0f * Tsquared + 1.0f) * KS
            + (Tcubed - 2.0f * Tsquared + T) * (1.0f - KS)
            + (-2.0f * Tcubed + 3.0f * Tsquared) * maxLum;

    float maxRGB2_pq = P * Lw_pq;
    float maxRGB2_linear = st2084_to_linear_channel(maxRGB2_pq);

    // 严格保持各通道等比例缩放（0 色相漂移）
    float scale = maxRGB2_linear / max(6.10352e-5f, maxRGB1_linear);
    float3 mapped2020 = rgb2020 * scale;

    // 转回 Rec.709 色域
    float3 mapped709 = rec2020_to_rec709(mapped2020);

    // 归一化到 [0, 1] SDR 范围 (Lmax 映射至 1.0)
    float3 sdrLinear = mapped709 * (10000.0f / Lmax);
    return saturate(sdrLinear);
}

// 2. OBS Studio 风格 Reinhard (Rec.2020 宽色域中转)
float3 ToneMap_OBS_Reinhard(float3 linearScrgb, float sdrWhite)
{
    float safeSdrWhite = max(10.0f, sdrWhite);
    float multiplier = 80.0f / safeSdrWhite;
    float3 normRgb = max(0.0f, linearScrgb) * multiplier;

    float3 rgb2020 = max(0.0f, rec709_to_rec2020(normRgb));
    float3 mapped2020 = rgb2020 / (rgb2020 + 1.0f);
    float3 mapped709 = rec2020_to_rec709(mapped2020);
    return saturate(mapped709 * 2.0f);
}

// 3. Hable / Uncharted 2 曲线
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

// 4. ACES Fitted (Stephen Hill / Narkowicz 拟合改进)
float3 ToneMap_ACES(float3 color)
{
    float3 x = color * 0.6f;
    const float a = 2.51f;
    const float b = 0.03f;
    const float c = 2.43f;
    const float d = 0.59f;
    const float e = 0.14f;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e)) / 1.033f;
}

// 5. Extended Reinhard (白点控制)
float3 ToneMap_Reinhard(float3 color, float peakWhite, float rollOff)
{
    float maxCh = max(color.r, max(color.g, color.b));
    if (maxCh < 1e-6f) return color;

    float white = max(1.0f, peakWhite * rollOff);
    float white2 = white * white;
    float mapped = (maxCh * (1.0f + (maxCh / white2))) / (1.0f + maxCh);
    return color * (mapped / maxCh);
}

// 6. 基于亮度的色度保持滚降
float3 ToneMap_LumaHuePreserving(float3 color, float peakWhite, float rollOff)
{
    float maxCh = max(color.r, max(color.g, color.b));
    if (maxCh < 1e-6f) return color;

    const float k = 0.70f;
    if (maxCh <= k) {
        return color;
    }

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

    // 3. 归一化参数准备
    float safeSdrWhite = max(10.0f, sdrWhiteNits);
    float sdrScale = 80.0f / safeSdrWhite;
    float3 normRgb = linearRgb * sdrScale;
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

    case 4: // Luminance Hue-Preserving
        mappedRgb = ToneMap_LumaHuePreserving(normRgb, peakScale, highlightRollOff);
        break;

    case 6: // OBS Reinhard
        mappedRgb = ToneMap_OBS_Reinhard(linearRgb, safeSdrWhite);
        break;

    case 5: // ITU-R BT.2390 EETF (OBS 工业级参考对齐，默认)
    default:
        mappedRgb = ToneMap_BT2390(linearRgb, sourcePeakNits, safeSdrWhite, highlightRollOff);
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
    } else if (oetfType == 3) {
        finalSdr = pow(mappedRgb, 1.0f / 2.4f);
    } else {
        finalSdr = mappedRgb; // Linear Passthrough
    }

    return float4(finalSdr, raw.a);
}
