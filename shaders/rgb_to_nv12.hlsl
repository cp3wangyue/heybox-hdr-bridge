// rgb_to_nv12.hlsl — Rec.709 RGB → NV12（Limited Range）转换参考实现
//
// 实现要点（P4/P5 时展开）：
//  * 优先评估 D3D11 Video Processor 是否满足需求（驱动路径，代价最低）；
//    Compute Shader 方案作为可控回退。
//  * BT.709 矩阵（Y 在 [16,235]，UV 在 [16,240]，limited range）：
//      Y  =  0.2126*R + 0.7152*G + 0.0722*B
//      U  = -0.2126*R - 0.7152*G + 0.8878*B   （再映射到 128±0.5*224/255）
//      V  =  0.8786*R - 0.7152*G - 0.1634*B
//  * NV12 布局：Y 平面全分辨率 + UV 交织半分辨率；CS 按 2x2 线程组处理。
//  * 与编码器确认色彩矩阵/范围元数据（若编码器只认 BT.601 需换矩阵并记录）。

[numthreads(8, 8, 1)]
void RGBToNV12CS(uint3 dtid : SV_DispatchThreadID)
{
    // TODO(P4/P5): 采样 2x2 邻域 → Y/UV 平面输出
}
