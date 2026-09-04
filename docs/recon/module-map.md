# 模块地图（P1 输出，§5.3）

> 填写说明：按"捕获 / 转换 / 编码 / 网络 / UI"归类屏幕共享相关模块。数据来源：
> `tools/module_diff.ps1 -Snapshot`（共享前后各一次）+ 静态导入表/字符串检索。
> 客户端版本：_______　采集日期：_______

## 快照 diff 摘要

- 快照 idle：`docs/recon/module-snapshots/<tag>-<时间>.csv`
- 快照 sharing：`docs/recon/module-snapshots/<tag>-<时间>.csv`
- 新增模块数：____

## 捕获

| 模块 | 路径 | 版本 | 签名 | 基址 | 证据（导入/字符串/调用栈） | 强度 |
| --- | --- | --- | --- | --- | --- | --- |
| | | | | | | 高/中/低 |

## 转换 / 颜色处理

| 模块 | 路径 | 版本 | 签名 | 基址 | 证据 | 强度 |
| --- | --- | --- | --- | --- | --- | --- |

## 编码

| 模块 | 路径 | 版本 | 签名 | 基址 | 证据 | 强度 |
| --- | --- | --- | --- | --- | --- | --- |

重点候选：dxgi.dll / d3d11.dll / Windows.Graphics.Capture 相关 WinRT 组件 / mf*.dll / nvEncodeAPI64.dll / avcodec* / webrtc*

## 网络 / 传输

| 模块 | 路径 | 版本 | 签名 | 基址 | 证据 | 强度 |
| --- | --- | --- | --- | --- | --- | --- |

## UI / 宿主

| 模块 | 路径 | 版本 | 签名 | 基址 | 证据 | 强度 |
| --- | --- | --- | --- | --- | --- | --- |

## 静态线索检索结果（§5.2）

| 线索 | 命中模块 | 命中内容 | 含义 |
| --- | --- | --- | --- |
| CreateDXGIFactory* / D3D11CreateDevice / DuplicateOutput / CreateDirect3D11CaptureFramePool | | | |
| NvEnc* / MFCreate* / IMFTransform / avcodec_* | | | |
| NV12 / P010 / R16G16B16A16_FLOAT / R10G10B10A2 / B8G8R8A8 | | | |
| 709 / 2020 / PQ / HLG / HDR / scRGB / ColorSpace | | | |
| DesktopCapturer / VideoFrame / I420 / webrtc | | | |

## Gate P1 自检

- [ ] 能明确回答：共享时新增/活跃的关键模块有哪些
- [ ] 至少找到一类候选捕获 API 和一类候选编码器
- 若无法定位 → 转向 ETW/调用跟踪扩大证据，不猜
