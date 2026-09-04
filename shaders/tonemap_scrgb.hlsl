// tonemap_scrgb.hlsl — 输入路径 A：FP16 scRGB → SDR Rec.709（P4，计划书 §8.1）
//
// 状态：PASSTHROUGH 占位。Gate P2 未通过前不实现、不调参（计划书红线）。
//
// 实现要点（P4 时展开）：
//  * scRGB 是线性光，sRGB/Rec.709 色度；数值可超出 0~1，也可为负。
//    Microsoft 参考关系：scRGB (1,1,1) ≈ 80 nits D65 白，(12.5,12.5,12.5) ≈ 1000 nits。
//  * 禁止先 clamp 到 1.0 —— 必须带着原始数值计算亮度：
//      Y = dot(rgb, (0.2126, 0.7152, 0.0722))，或用 max(R,G,B) 保色相。
//  * SDR 参考白不要硬编码：从系统读（SDRReferenceWhite=System，§8.4），
//    scRGB 数值 × SDR white nits 得到绝对亮度后再做 roll-off。
//  * 亮度映射：曝光 + 高光 roll-off（Hable / ACES fitted 起步，§8.3 表）。
//  * Tone map 之后做 gamut compression / hue-preserving clipping，落到 Rec.709 色域。
//  * 最后套 Rec.709/sRGB OETF 输出 8-bit SDR RGB。

static const float kScrgbRefWhiteNits = 80.0f; // scRGB (1,1,1) 的物理亮度（暂定，P4 由常量缓冲注入）

float4 ToneMapPS(float4 color : COLOR) : SV_Target
{
    // TODO(P4): 按上述要点实现；参数全部走常量缓冲并记录到日志（§8.5）
    return color; // passthrough —— 仅供编译链验证
}
