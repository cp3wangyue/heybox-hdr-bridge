# 模块地图（P1 输出，§5.3）

> 客户端：小黑盒语音 HeyboxChat（Electron），安装目录 `C:\Users\22983\AppData\Local\Qingfeng\HeyboxChat`，
> 安装版本 1.56.0（目录名）/ EXE FileVersion 1.31.0。
> 数据来源：`tools/pe_recon.py` 静态扫描（`pe-recon.md`）+ `tools/module_diff.ps1` idle 快照
> （`module-snapshots/idle-20260905-054838.csv`，374 模块 / 6 进程）。采集日期：2026-09-05。

## 关键结构判定

**客户端是 Electron 多进程应用，屏幕共享由内嵌的第三方 RTC SDK 实现（非自研捕获管线）。**
静态证据：完整 RTC SDK 以独立 DLL 随客户端分发；idle 快照证明这些 SDK **按需加载**——
6 个空闲进程均未加载 VolcEngineRTC/liteav/d3d11/dxgi，共享会话建立时才会出现。

## 捕获

| 模块 | 路径 | 版本 | 签名 | 证据 | 强度 |
| --- | --- | --- | --- | --- | --- |
| VolcEngineRTC.dll | `1.56.0\resources\versions\1.56.0\app\node_modules\@volcengine\vertc-electron-sdk\build\Release\` | - | - | WGC 全套字符串（Windows.Graphics.Capture / Direct3D11CaptureFramePool / GraphicsCaptureItem/Session / CreateDirect3D11DeviceFromDXGIDevice）+ DDA（DuplicateOutput / IDXGIOutputDuplication / ReleaseFrame）+ GDI 兜底（BitBlt / GetDC / GetWindowDC / PrintWindow）；导入 d3d11!D3D11CreateDevice、dxgi!CreateDXGIFactory1 | 高（字符串+导入） |
| liteav.dll（TRTC） | `...\node_modules\trtc-electron-sdk\build\Release\` | - | - | DDA（DuplicateOutput / IDXGIOutputDuplication）+ GDI（BitBlt / PrintWindow）+ D3D11CreateDevice 导入 | 高 |
| liteav_screen.dll | 同上 | - | - | 纯 WGC 屏幕采集模块（Windows.Graphics.Capture / GraphicsCaptureItem / CreateDirect3D11DeviceFromDXGIDevice）+ D3D11CreateDevice 导入 | 高 |
| HeyboxChat.exe（Electron/Chromium） | 安装根 | 1.31.0 | - | 180MB Electron 宿主；Chromium desktopCapturer 走 WGC（待动态确认由哪一侧捕获） | 中 |

## 转换 / 颜色处理

| 模块 | 证据 | 强度 |
| --- | --- | --- |
| VolcEngineRTC.dll | 色彩字符串 `HDR, PQ, color_space, colorspace, nits`；像素格式 `ABGR, ARGB, I420, NV12, P010, YUV2` | 高（能力存在，行为待验证） |
| liteav.dll | `BT2020, BT709, HDR, HLG, PQ, color_space, nits`；像素格式 `ARGB, B8G8R8A8, I420, NV12, P010` | 高 |

## 编码

| 模块 | 证据 | 强度 |
| --- | --- | --- |
| VolcEngineRTC.dll | `nvEncodeAPI / NVENC / amf / H265 / hevc / h264`；导入 RTCFFmpeg.dll 14 项 avcodec 函数（编码器经 ffmpeg 包装）；导入 openh264-4.dll | 高 |
| liteav.dll + txffmpeg.dll | `liteav_avcodec_*` 导入；`NVENC, VAAPI, amf` 字符串 | 高 |
| openh264-4.dll | VeRTC 软编后备 | 高 |

## 网络 / 传输

| 模块 | 证据 |
| --- | --- |
| VolcEngineRTC.dll | bytertc/vertc/webrtc 字符串（火山引擎 VeRTC 协议栈） |
| liteav.dll / live_kit_engine.dll / liteav_media_server.exe | liteav/webrtc（腾讯 TRTC 协议栈） |

## UI / 宿主

| 模块 | 说明 |
| --- | --- |
| HeyboxChat.exe ×6 进程 | Electron 主进程 + GPU 进程 + renderer/utility；idle 中 GPU 进程（pid 37724）模块最多 |
| heybox-overlay-server.node + addon/heybox-overlay-x64.dll | 游戏内 overlay 服务 |
| addon/mhwilds/dinput8.dll（12MB）+ reframework 插件 | 针对怪物猎人荒野的进程内注入（dinput8 代理 + REFramework 插件），游戏内捕获/overlay 候选 |
| addon/steam-hook-x64.dll | Steam 注入辅助 |

## 静态线索检索结果（§5.2）

| 线索 | 命中模块 | 含义 |
| --- | --- | --- |
| Windows.Graphics.Capture 全套 | VolcEngineRTC.dll、liteav_screen.dll | WGC 捕获路径存在 |
| DuplicateOutput / IDXGIOutputDuplication | VolcEngineRTC.dll、liteav.dll | DDA 捕获路径存在 |
| BitBlt / PrintWindow / GetDC | VolcEngineRTC.dll、liteav.dll | GDI 兜底路径存在 |
| NvEnc*/nvEncodeAPI | VolcEngineRTC.dll、liteav.dll | NVENC 硬编存在 |
| avcodec_* | RTCFFmpeg.dll（经 VolcEngineRTC 导入）、txffmpeg.dll（经 liteav 导入） | 编码走 ffmpeg 包装 |
| NV12/P010/I420/BGRA | 两家 SDK | 中间帧格式候选 |
| PQ/HLG/BT2020/BT709/color_space/nits | 两家 SDK | 色彩元数据能力存在（实际是否用于 HDR 共享待验证） |
| 桌面 Duplication 常量/字符串 | - | 与 idle 快照一致：全部按需加载 |

## Gate P1 自检

- [x] 能明确回答：共享时新增/活跃的关键模块有哪些
- [x] 至少找到一类候选捕获 API 和一类候选编码器
- 动态确认（2026-09-05 共享态快照 + DDA 冲突探针，见 test-reports/round-20260905-sharing-recon.md）：
  **VeRTC 通道实锤**。共享时新增 41 模块，全部集中在单个进程（pid 16428）：
  `VolcEngineRTC.dll v3.58.1.63260` + `GraphicsCapture.dll`(WGC) + `d3d11/dxgi/d3d9/dxva2` +
  `nvEncodeAPI64.dll`(NVENC 硬编) + `RTCFFmpeg.dll`/`openh264-4.dll`(软编后备)。
  TRTC(liteav) 未加载。独立进程 DuplicateOutput 成功 → 客户端未占用 DDA → **捕获 = WGC**。

## 关键悬念解答进度

1. ~~共享走 VeRTC 还是 TRTC~~ → **VeRTC**（TRTC 静态存在但本场景未加载；换业务线需复验）
2. ~~捕获 API 实际选型~~ → **WGC**（GraphicsCapture.dll 加载 + DDA 未被占用；请求的纹理格式待 Probe）
3. SDK 的 HDR/PQ 字符串是否意味着已有色彩处理 → 待 P2 帧级证据（捕获格式 + 编码器输入格式 + 远端表现对照）
