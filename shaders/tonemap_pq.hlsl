// tonemap_pq.hlsl — 输入路径 B：Rec.2100 PQ / HDR10 → SDR Rec.709（P4，计划书 §8.2）
//
// 状态：PASSTHROUGH 占位。Gate P2 未通过前不实现、不调参（计划书红线）。
//
// 实现要点（P4 时展开）：
//  * PQ 是绝对亮度编码：先按 SMPTE ST 2084 EOTF 还原线性亮度（nits），
//    不能把 PQ 数值当线性 RGB 用。
//    PQ inverse EOTF（线性亮度 nits → PQ 信号）常量：
//      m1 = 2610/16384 = 0.1593017578125
//      m2 = 2523/4096*128 = 78.84375
//      c1 = 3424/4096 = 0.8359375
//      c2 = 2413/4096*32 = 18.8515625
//      c3 = 2392/4096*32 = 18.6875
//  * 色域：Rec.2020 容器 → 工作空间 → Rec.709；避免直接丢位深。
//  * peak_nits / mastering metadata 可用时用于决定压缩范围；
//    不可用时用可配置峰值并保守 roll-off（SourcePeakNits=Auto）。
//  * Beta 阶段评估 BT.2390 思路处理高光（§8.3）。

float4 ToneMapPS(float4 color : COLOR) : SV_Target
{
    // TODO(P4): PQ EOTF → 亮度 tone map → Rec.709 色度/传递函数
    return color; // passthrough —— 仅供编译链验证
}
