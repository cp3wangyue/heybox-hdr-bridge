# 候选调用链（P1 输出，§5.3）

> 客户端：HeyboxChat 1.56.0（Electron + 内嵌 RTC SDK）。日期：2026-09-05。
> 证据来源：`pe-recon.md`（静态）、`capture-format-ab-analysis.md`（系统级 A/B）、`module-map.md`。

## 候选 #1（首选怀疑对象）：VeRTC 屏幕共享

- 路径：Electron 主进程/工具进程 → **VolcEngineRTC.dll 内部捕获**
  （WGC：`CreateDirect3D11CaptureFramePool`（可能请求 B8G8R8A8 或 FP16）；
  兜底 DDA `DuplicateOutput` / GDI `PrintWindow`）
  → SDK 内部格式转换（NV12/I420，色彩字符串含 PQ/color_space/nits）
  → 编码（NVENC 硬编 / RTCFFmpeg(avcodec) 软编 / openh264）
- 证据：SDK 内 WGC+DDA+GDI 三套捕获字符串齐全；NVENC/avcodec 编码字符串齐全；
  `vertc-electron-sdk` 目录结构与 node 集成方式证明为业务主通道之一
- 强度：高
- 动态验证方式：进入共享后 `module_diff.ps1 -Snapshot -Tag sharing` 看 VolcEngineRTC.dll 是否加载；
  是 → 在其线程上做纹理追踪（P2 Probe 挂 D3D11 纹理创建/复制边界）

## 候选 #2：TRTC 屏幕共享

- 路径：Electron → **liteav.dll + liteav_screen.dll**（liteav_screen 为纯 WGC 采集模块）
  → txffmpeg/liteav_avcodec 编码（NVENC/VAAPI/amf）
- 证据：liteav_screen.dll 独立存在且只含 WGC 相关；liteav.dll 含 DDA+GDI+色彩串
- 强度：中（存在性确凿，何时被业务调用未知——可能与 VeRTC 按业务线分流）
- 动态验证方式：同上，看 liteav.dll/liteav_screen.dll 是否随共享加载

## 候选 #3：游戏内捕获（游戏共享场景）

- 路径：游戏进程内注入（addon/mhwilds/dinput8.dll 代理注入 + heybox-overlay-x64.dll）
  → overlay 内部捕获游戏帧（12MB dinput8 内含捕获/D3D11 字符串）
- 证据：mhwilds addon 的 dinput8.dll 有 D3D11CreateDeviceAndSwapChain 导入与像素格式字符串
- 强度：低~中（对"共享游戏窗口"路径可能是关键；对桌面共享无关）
- 动态验证方式：启动游戏 + 共享后，检查游戏进程加载的 heybox/mhwilds 模块

## 候选 #4：Chromium desktopCapturer（Electron 原生 getDisplayMedia）

- 路径：renderer `navigator.mediaDevices.getDisplayMedia` → Chromium desktopCapturer（内部走 WGC）
  → 交给 RTC SDK 或 WebRTC 直接发送
- 证据：Electron 宿主存在；ffmpeg.dll 已在 idle 加载；`DesktopCapturer (ci)` 字符串命中 VeRTC/liteav
  （也可能是 SDK 内嵌的 webrtc 代码，需区分）
- 强度：低~中
- 动态验证方式：sharing 快照关注 windows.graphics 相关 WinRT DLL 是否在 renderer/utility 进程出现

## P2 入口选择

- 首选追踪入口：**D3D11 纹理生命周期边界**（CreateTexture2D / CopyResource / IID_ID3D11Texture2D 查询）
  而不是具体 API 名——因为四条候选里三家都可能用 WGC，WGC 的帧纹理来自系统 FramePool，
  不经客户端"创建"，Hook 纹理创建函数看不到它；应 Hook 帧消费点（帧池 TryGetNextFrame 返回的
  surface 使用处、编码器输入拷贝）或直接枚举每帧提交给编码器的纹理。
- 备选入口：DXGI 层的 Present/FramePool 事件 + 编码器输入侧反向追踪（§15 风险登记的推荐做法）
- 风险：RTC SDK 自带多 Device（采集 Device 与编码 Device 可能不同）、内部线程池帧转发、
  COM 生命周期由 SDK 管理——Probe 必须 按 Device 建资源池（§7.2）
