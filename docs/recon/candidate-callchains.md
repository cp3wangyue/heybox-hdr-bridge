# 候选调用链（P1 输出，§5.3）

> 客户端：HeyboxChat 1.56.0（Electron + 内嵌 RTC SDK）。日期：2026-09-05。
> 证据来源：`pe-recon.md`（静态）、`capture-format-ab-analysis.md`（系统级 A/B）、`module-map.md`、
> `../test-reports/round-20260905-sharing-recon.md`（**动态确认：共享态快照 + DDA 冲突探针**）。

## 候选 #1：VeRTC 屏幕共享 —— ✅ 已动态确认为本客户端实际通道

- 路径：Electron 单进程（pid 16428）→ **VolcEngineRTC.dll v3.58.1.63260 内部 WGC 捕获**
  （`Windows.Graphics.Capture` / `Direct3D11CaptureFramePool`；DDA 仅作代码内备选未占用）
  → SDK 内部转换（NV12/I420；色彩串含 PQ/color_space/nits）
  → **NVENC 硬编**（nvEncodeAPI64.dll）+ RTCFFmpeg/openh264 软编后备
- 动态证据：共享态新增 41 模块全部集中于该进程；GraphicsCapture.dll 加载；
  独立进程 DuplicateOutput 成功（DDA 单会话未被占用）→ WGC
- 强度：**高（已确认）**
- P2 剩余问题：FramePool 请求的纹理格式（B8G8R8A8 vs FP16）、转换节点输入输出、NVENC 输入格式

## 候选 #2：TRTC 屏幕共享 —— 本场景排除

- 静态存在（liteav.dll / liteav_screen.dll 纯 WGC 采集模块），但共享态未加载。
  保留为备选：不同业务线/房间类型可能分流，换房间时复验 sharing 快照。
- 强度：存在=高；本场景活跃=已排除

## 候选 #3：游戏内捕获（游戏共享场景）

- 路径：游戏进程内注入（addon/mhwilds/dinput8.dll 代理注入 + heybox-overlay-x64.dll）
- 证据：mhwilds addon 的 dinput8.dll 有 D3D11CreateDeviceAndSwapChain 导入与像素格式字符串
- 强度：低~中（对"共享游戏窗口"路径可能关键；桌面共享无关）
- 验证方式：启动游戏 + 共享后，检查游戏进程加载的 heybox/mhwilds 模块

## 候选 #4：Chromium desktopCapturer —— 未观察到

- idle/sharing 快照均未见额外 Chromium 捕获栈加载迹象；VeRTC 自带完整捕获，此路径可能性进一步降低。
- 强度：低

## P2 入口选择（已按动态证据修订）

- 首选：**WGC FramePool 帧消费边界** —— VeRTC 每帧从 `Direct3D11CaptureFramePool` 取
  `IDirect3D11CaptureFrame`（内部经 `IDirect3DDxgiInterfaceAccess` 拿到 ID3D11Texture2D）。
  Probe 观察点：FramePool 创建（记录请求格式 → 回答"BGRA8 还是 FP16"）与每帧 surface 的
  Texture2D 描述、以及送入 NVENC 前的中间纹理。
- 备选：编码器输入侧反向追踪（nvEncodeAPI64 的 NvEncCreateInputBuffer/NvEncLockInputBuffer
  调用参数可确认编码器输入格式与尺寸）
- 风险：VeRTC 内部多 Device（采集/编码可能不同 Device，需按 Device 建资源池）；
  COM 生命周期由 SDK 管理；FramePool 重建（Alt-Tab/分辨率切换）时机需记录（§7.1）

