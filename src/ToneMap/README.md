# ToneMap

ToneMapCore：独立 GPU 转换模块，覆盖 scRGB→SDR 与 PQ→SDR 两条路径（P4，§8）。

**Gate P2 未确认前不写实现。** 算法必须与逆向/Hook 解耦，先在 tests/ToneMapHarness 中验证。

- POC：Hable / ACES fitted / 简单可控曲线（§8.3）
- Alpha：亮度高光 roll-off + 色度保持（彩色高光不变白、肤色不漂移）
- Beta：PQ 场景评估 BT.2390
- 最终：保留 2~3 个模式 + Auto
- GPU 要求：与原 Device 共用；帧循环内禁止 CreateTexture2D；2~3 帧循环资源池；无逐帧 CPU Readback
