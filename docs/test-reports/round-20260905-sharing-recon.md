# 开发回合证据记录：共享态模块确认（Gate P1 → P2 交割）

| 字段 | 填写内容 |
| --- | --- |
| 日期 | 2026-09-05 06:25 |
| 客户端版本 | HeyboxChat 1.56.0（EXE FileVersion 1.31.0） |
| Git HEAD | 8e40cf1 之后的本回合 |
| 本回合问题 | 真实屏幕共享时，客户端实际加载哪个 RTC SDK、捕获走什么 API、编码走什么实现 |
| 观察方法 | module_diff 快照（idle 05:48 / sharing 06:25，两次快照间客户端进程 pid 全部重建，按模块名 diff）+ DDA 单会话冲突探针（capture_format_spy） |
| 证据 | docs/recon/module-snapshots/{idle-20260905-054838,sharing-20260905-062507}.csv、diff-idle-vs-sharing-20260905-062628.md、spy 探针输出 |
| 结论 | **GO** |
| 下一步 | 开发客户端内只读 CaptureProbe（P2）：确认 WGC FramePool 请求的纹理格式与编码器输入格式 |

## 证据明细

共享态新增 41 个模块、移除 0 个。关键项全部集中在 **pid 16428（131 模块，idle 时其前身进程仅 ~50 模块）**：

| 新增模块 | 归属进程 | 含义 |
| --- | --- | --- |
| VolcEngineRTC.dll v3.58.1.63260 (ByteDance) | 16428 | **活跃 SDK = 火山引擎 VeRTC**（TRTC/liteav 未出现，候选 #2 排除为本场景通道） |
| GraphicsCapture.dll（WinRT WGC 组件） | 16428 | **捕获 = Windows.Graphics.Capture** |
| d3d11.dll / dxgi.dll / d3d9.dll / dxva2.dll | 16428 | SDK 的 D3D11/D3D9/DXVA 栈随共享加载 |
| nvEncodeAPI64.dll (NVIDIA) | 16428 | **编码 = NVENC 硬编活跃**（RTCFFmpeg/openh264 为软编后备） |
| RTCFFmpeg.dll / openh264-4.dll | 16428 | ffmpeg 编码包装 + 软编 |
| libEGL/libGLESv2/electron-sdk.node | 16428 等 | SDK Electron 绑定层 |

**DDA 冲突探针**：共享进行中，独立进程 `DuplicateOutput` 仍能成功创建复制会话
（每输出仅允许一个 DDA 会话）→ **客户端未占用 DDA**，与 GraphicsCapture.dll 相互印证捕获走 WGC。

## 对帧路径图的更新（Gate P2 五项进度）

| # | 项目 | 当前结论 | 状态 |
| --- | --- | --- | --- |
| 1 | 捕获 API | WGC（VolcEngineRTC 内部） | ✅ 动态确认 |
| 2 | 捕获纹理格式 | 未知（B8G8R8A8 vs FP16 待 Probe；SDK 有 PQ/nits 字符串，可能按 HDR 状态切换） | ☐ P2 |
| 3 | 颜色/格式转换节点 | SDK 内部（NV12/I420 候选格式已知） | ☐ P2 |
| 4 | 编码器输入格式 | 编码器=NVENC 已确认；输入格式待 Probe | 半确认 |
| 5 | HDR 信息丢失点 | 未定位 | ☐ P2 |

## 风险与注意

- 两次快照间客户端 6 个进程 pid 全部更换（Electron 进程重建）——后续比较一律按模块名，不按 pid
- 共享房间类型/业务线可能影响 SDK 选择；换房间复验时重跑 sharing 快照
