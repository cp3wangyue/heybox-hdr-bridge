# ColorConvert

Rec.709 RGB ↔ NV12（Limited Range）GPU 转换（§8.5/§9.1）：

- 优先 D3D11 Video Processor（驱动路径）；Compute Shader 作为可控回退
- 分两步：HDR→SDR 输出 Rec.709 RGB → 再转 NV12；编码器要求 BGRA 输入时保留原 RGB→YUV 路径
- P010/10-bit：远端只支持 SDR 时不保留 PQ，进编码器前转 SDR 路径
- 禁止逐帧 Map/Staging/Flush；资源随尺寸重建（Resize/切屏/重共享，§9.2）
