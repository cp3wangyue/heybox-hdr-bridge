# 真实帧路径图（Gate P2 输出，§6）

> **Gate P2：GO**（五项齐备）。客户端 HeyboxChat 1.56.0，会话 2026-09-05 07:17–07:28（HDR 开启，
> \\.\DISPLAY1 `RGB_FULL_G2084_NONE_P2020`，SDR white 280 nits）。
> 证据：`probe-34416-hdr-share.log`（全屏 HDR 内容会话）、`probe-1492-window-share.log`（SDR 窗口会话 +
> HDR 开关对照实验）、`capture-format-ab-analysis.md`（系统级 A/B）。

## 五项必答

| # | 项目 | 结论 | 证据 |
| --- | --- | --- | --- |
| 1 | 捕获 API | **Windows.Graphics.Capture**（VeRTC 内部；窗口/全屏两种模式） | GraphicsCapture.dll 随共享加载；帧池纹理带 `misc=0x802`(SHARED\|SHARED_NTHANDLE) 签名；独立进程 DuplicateOutput 成功（DDA 未被占用） |
| 2 | 捕获纹理格式 | **B8G8R8A8_UNORM（8-bit SDR）**，HDR 开启下也如此；窗口模式池=窗口物理尺寸，全屏模式池=3840x2160 | 三次会话全部只出现 BGRA 池纹理；没有任何 FP16/RGB10A2 纹理被创建 |
| 3 | 颜色/格式转换节点 | **VeRTC 内部 BGRA→NV12**（Compute Shader 特征：NV12 bind=SRV\|RTV\|UAV），先缩放（3840x2160→1920x1080）再转换 | 同尺寸 BGRA(misc=0x2) 与 NV12(misc=0x2) 成对创建，两代共享内容均如此 |
| 4 | 编码器输入格式 | **NV12（limited range）**，SHARED 纹理 → NVENC D3D11 注册路径 | NV12 Y 平面实测 max=235.000（标准 limited 映射）；nvEncodeAPI64.dll 随共享加载 |
| 5 | HDR 信息丢失点 | **捕获环节（WGC BGRA8 池）**。8-bit UNORM 值域 0~1 结构上无法承载 >1.0 scRGB 高光；HDR 内容进入 SDK 代码之前已被 DWM 压平 | HDR 全屏内容实测：BGRA bright(≥250) 占比 **44.5%**（SDR 窗口仅 11.8%），max 恒 255；NV12 Y max 恒 235 |

## 对照追踪数据（§6.3）

| 测试 | HDR | 内容 | 捕获（BGRA） | NV12 Y | 观察 |
| --- | --- | --- | --- | --- | --- |
| T2-1 | 关 | （系统级） | FP16/BGRA 均可得（跟随请求） | — | 格式不随 HDR 变化（Win11 25H2） |
| T2-2 | 开 | SDR 窗口 | meanLuma 239.5（HDR On）vs 236.5（HDR Off） | max=235 | **SDR 内容不随 HDR 增益**——DWM 8-bit 路径做了归一化 |
| T2-3 | 开 | SDR 桌面 | 同上 | max=235 | 正常 |
| T2-4 | 开 | **HDR 全屏内容** | **bright 44.5%@≥250，max 恒 255** | max=235, mean 178 | 高光堆在 8-bit 天花板 = 捕获层裁切 |

补充判定树执行（§6.4）：capture_format == B8G8R8A8_UNORM 且 HDR 输出开启 → 比较 pre/post：
高光在**捕获纹理本身**已饱和 → 缺陷在探针观察点之前（WGC/DWM 8-bit 下转换），不在 VeRTC 内部。

## 关键定量记录

- NV12 编码侧范围：**limited range 正确**（Y max=235，SDR/窗口内容 underBlack≈0.07%）
  → **排除"发白=full/limited 范围错配"假设**；远端过亮的根源是高光在捕获层被压平
- VeRTC 共享分辨率管线：池（源尺寸）→ BGRA 中间（目标分辨率）→ NV12（目标分辨率）→ NVENC
- 两会话纹理世代：窗口共享 2020x1200/2400x1600/3072x1808 池；全屏 3840x2160 池

## 修复架构推论（进入 P3 的依据）

1. **插入边界必须在捕获池格式上**：只要 VeRTC 继续请求 BGRA8 池，任何下游 Tone Mapping
   都是在已被 DWM 压平的数据上工作——无高光可救。Hook 点 = WGC 帧池创建路径
   （§3.1 优先级 A 的具体化：让池以 R16G16B16A16_FLOAT 创建，再由我们完成 FP16→SDR）
2. WGC 在本机支持 FP16 池（系统级 A/B 已证），窗口与全屏模式均需验证
3. NV12 limited-range 输出符合远端惯例，P5 输出侧按 NV12 limited 对接即可

## 遗留与风险

- DWM 对 HDR→BGRA8 的映射是"归一化 SDR 内容"已证；对 HDR 高光是"裁切"还是"某固定 tone map"
  的定量曲线未测（需要 FP16 同内容对照，P4 Harness 内完成即可，不阻塞 P3）
- VeRTC 每次共享会话新建宿主进程（两个会话 pid 均不同）——P3 Hook 注入需跟随进程创建
- 签名/版本锁：VolcEngineRTC.dll v3.58.1.63260（本次验证版本）
