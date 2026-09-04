# 系统级捕获格式 A/B 分析（P0/P2 前置证据）

- 采集时间：2026-09-05 05:45–05:52
- 环境：Windows 11 专业版 25H2 build 26200.9168 / RTX 5070 Ti / 4K@160Hz 单屏
- 工具：`capture_format_spy.exe`（只读，独立进程）+ `hdr_ctl.exe`（状态切换，已恢复原状）
- 原始日志：`capture-format-spy-20260905-HDR-ON.log` / `capture-format-spy-20260905-HDR-OFF.log`

## A/B 对照结果

| 项 | HDR ON | HDR OFF |
| --- | --- | --- |
| 输出色彩空间（DXGI GetDesc1） | `RGB_FULL_G2084_NONE_P2020` | `RGB_FULL_G22_NONE_P709` |
| 输出位深 | 10 bit | 10 bit |
| SDR 参考白 | 280 nits（用户调节值） | 80 nits |
| WGC 池 B8G8R8A8 | 请求格式 → BGRA8 | 请求格式 → BGRA8 |
| WGC 池 FP16 | 请求格式 → FP16 | 请求格式 → FP16 |
| WGC 池 RGB10A2 | 建池被拒 `0x80070057` | 同左 |
| DDA 默认 DuplicateOutput | `R16G16B16A16_FLOAT`（偶发 BGRA8） | `R16G16B16A16_FLOAT`（偶发 BGRA8） |
| DDA DuplicateOutput1(FP16) | 可用，全部 FP16 | 可用，全部 FP16 |

## 关键结论（对计划书的修正）

1. **纹理格式在本机（Win11 25H2）不随 HDR 状态变化。** WGC 严格跟随请求格式；DDA 默认即 FP16。
   §6.4 判定树"capture_format == FP16 → HDR 假设"在本机不成立：FP16 可以只是格式请求的结果。
   **AutoDetect 必须以输出色彩空间（`IDXGIOutput6::GetDesc1().ColorSpace == G2084`）为 HDR 依据**，
   辅以 `DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO_2`（注意 type=9/15 的正确取值）。
2. **SDR 参考白随 HDR 开关变化（80 ↔ 280 nits），且是用户可调值** —— 再次验证 §8.4：
   输入解释所需的 SDR reference white 必须运行时读取，禁止硬编码。
3. WGC 不支持用 RGB10A2 建捕获池；客户端若捕获 PQ 内容只可能经 FP16 或 BGRA。
4. DDA 默认 FP16 是 Win11 新行为（计划书成文时的"BGRA8 默认"认知已过时）。

## 对 P2 的指导

- 客户端捕获格式探测结论不能作为 HDR 状态依据；探测要同时记录输出色彩空间。
- 若客户端用 WGC 请求 BGRA：HDR 桌面下 DWM 会把 HDR 内容压进 BGRA —— 具体是"过亮/裁切"还是
  "自动 tone map"，需要在 P2 用客户端内 CaptureProbe 对比 pre/post 像素值（T2-4）。
- 若客户端请求 FP16：拿到的是 scRGB 线性光，数值可 >1.0，直接 clamp 会裁高光（计划书 §1.2 假设②）。

## 环境注意事项

- 本机存在 3 个 NVIDIA LUID + 1 个 Basic Render Driver + 2 个虚拟显示适配器（MuMu / GameViewer）。
  捕获/共享测试时要明确目标显示器挂在哪个适配器上；虚拟适配器可作为"纯 SDR 输出"对照屏。
