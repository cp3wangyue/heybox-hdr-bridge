# 开发回合证据记录：P2 帧路径五项确认（Gate P2 GO）

| 字段 | 填写内容 |
| --- | --- |
| 日期 | 2026-09-05 07:17–07:28 |
| 客户端版本 | HeyboxChat 1.56.0 / VolcEngineRTC.dll v3.58.1.63260 |
| Git HEAD | 5b96c94（probe3） |
| 本回合问题 | 真实共享的帧路径：捕获 API/格式、转换节点、编码器输入、HDR 信息丢失点 |
| 观察方法 | 注入式只读探针（IAT hook D3D11CreateDevice → 每实例 vtable 克隆观察 CreateTexture2D + 节流像素采样），SDR 窗口会话与 HDR 全屏会话各一，含 HDR On/Off 切换对照 |
| 证据 | docs/recon/probe-{1492,34416}-*.log、frame-path.md、capture-format-ab-analysis.md |
| 结论 | **GO** —— 五项齐备，插入边界确定为"捕获池格式" |
| 下一步 | P3：Hook WGC 帧池创建（Direct3D11CaptureFramePool），以 FP16 池替换 BGRA8 请求，无损透传验证 |

## 帧路径（最终确认版）

```
[系统] WGC 帧池（BGRA8, SHARED_NTHANDLE）        ← HDR 数据在此被 DWM 压平（丢失点）
   ↓ DWM 8-bit 下转换（SDR 内容归一化 ✓ / HDR 高光裁切 ✗）
[VeRTC] BGRA 中间纹理（目标分辨率, SHARED）
   ↓ SDK 内部 Compute（NV12 bind=SRV|RTV|UAV）
[VeRTC] NV12 limited-range（SHARED）
   ↓ NVENC D3D11 注册（nvEncodeAPI64.dll）
[NVENC] H.264/H.265 → 网络
```

## 关键数字

| 指标 | SDR 窗口 | HDR 全屏 |
| --- | --- | --- |
| BGRA meanLuma | 112.6 | 191.0 |
| BGRA bright(≥250) | 11.8% | **44.5%** |
| BGRA max | 255（恒） | 255（恒） |
| NV12 Y max | 235（恒） | 235（恒） |
| NV12 Y meanLuma | 112.2 | 178.6 |
| HDR 开关对捕获的影响 | ±1.2%（无增益） | — |
