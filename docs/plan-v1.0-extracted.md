项目开发计划书
小黑盒 HDR 屏幕共享
SDR 色调映射补丁
Windows HDR 捕获链路识别、GPU Tone Mapping 与屏幕共享客户端集成
=== TABLE ===
版本 | v1.0
日期 | 2026-09-05
阶段 | 方案设计 / 待建仓
目标平台 | Windows 11 x64 / D3D11 优先
=== END TABLE ===
核心目标：本机继续使用 HDR，远端观众获得正确的 SDR Rec.709 画面；不依赖 OBS 中转。
[Heading 1] 执行摘要
本项目拟为小黑盒 Windows 客户端的屏幕共享链路增加 HDR→SDR 色调映射能力。当前问题表现为：当 Windows HDR 开启时，本地游戏与桌面显示正常，但远端观看屏幕共享时出现整体过亮、发白、高光裁切或色彩异常。项目将优先定位客户端实际使用的捕获 API、D3D11 纹理格式和编码器输入格式，然后在“捕获之后、编码之前”的 GPU 路径中插入色调映射与色彩空间转换。
最终版本应做到：HDR 开启时自动识别输入是 scRGB、PQ/HDR10 还是已经是 SDR；仅对确需转换的帧执行 GPU Tone Mapping；输出小黑盒原有 SDR 编码链可接受的 Rec.709/NV12（或等价格式）；HDR 关闭或识别失败时安全旁路。整个方案以最小侵入、可回滚、可诊断和不改变网络协议为基本原则。
=== TABLE ===
首要技术判断 / 不要先写 Tone Mapping，也不要先猜小黑盒使用哪种捕获 API。第一阶段必须把真实的进程、模块、捕获纹理格式、色彩空间和编码器输入格式查清楚。只要找到“捕获帧 → 编码器”的边界，后续工作就从“逆向整个客户端”降为“在一个 D3D11 视频节点中插入转换”。
=== END TABLE ===
=== TABLE ===
成功指标 | 目标值 / 验收方式
远端 SDR 观感 | 与 Windows 关闭 HDR 后的小黑盒共享画面在曝光、灰阶和主要颜色上基本一致；HDR 高光不再大面积裁白
本地体验 | 游戏继续 HDR 输出，不要求切换 Windows HDR，不改变游戏渲染设置
性能 | 4K60 场景下新增 GPU 开销目标 ≤ 2%，额外延迟目标 ≤ 1 帧；无逐帧 CPU Readback
稳定性 | 连续 2 小时共享无持续显存增长、无设备丢失死锁、无新增高频掉帧
安全性 | 识别失败、Hook 失败或 Shader 初始化失败时自动旁路原链路；提供一键禁用与完整卸载
维护性 | 客户端版本不匹配时拒绝加载或仅运行诊断模式；版本签名/哈希可追踪
=== END TABLE ===
[Heading 2] 建议交付物
Recon 报告：进程树、关键模块、捕获 API、D3D11 Device/Context、帧纹理格式、编码器入口与调用时序。
CaptureProbe：只观察不改帧的动态诊断模块，可输出每秒采样日志。
ToneMapCore：独立 GPU 转换模块，覆盖 scRGB→SDR 与 PQ→SDR 两条路径。
Integration：将转换后的帧无缝接回小黑盒原有编码链，保持尺寸、时间戳和帧率。
配置与故障保护：自动检测、旁路、日志、版本锁、崩溃转储和紧急禁用。
测试包：HDR/SDR 测试图、A/B 录屏、性能采样、稳定性报告与版本兼容记录。
[Heading 1] 目录与阶段路线
本计划按“先证据、后改造；先旁路验证、后替换帧；先独立算法、后客户端集成”的顺序推进。任何阶段未达到退出条件，都不进入下一阶段。
=== TABLE ===
章节 | 内容
1 | 背景与问题定义
2 | 项目范围、边界与原则
3 | 目标架构与技术路线
4 | P0：复现与基线建立
5 | P1：静态侦察
6 | P2：动态追踪与格式确认
7 | P3：无损 Hook POC
8 | P4：GPU Tone Mapping 核心
9 | P5：接入编码前链路
10 | P6：自动检测、配置与故障保护
11 | P7：测试、性能与稳定性
12 | P8：打包、发布与版本维护
13 | 工程结构、Git 与开发规范
14 | 里程碑、工期与资源估算
15 | 风险登记与应对策略
16 | 完成定义与最终验收
附录 | 日志字段、配置样例、参考资料
=== END TABLE ===
图 1  项目阶段路线
[Heading 1] 1. 背景与问题定义
[Heading 2] 1.1 现象
Windows HDR 开启后，本机显示器通过 Windows Advanced Color / HDR 合成链路正确显示游戏与桌面；但小黑盒屏幕共享的远端画面出现明显过亮、发白或高光裁切。OBS 通过正确识别 HDR 输入并向 SDR Rec.709 输出时能够缓解或解决同类问题，说明问题很可能位于“捕获帧的格式/色彩空间解释”或“编码前缺失 HDR→SDR Tone Mapping”这两类环节。
[Heading 2] 1.2 技术根因假设
Windows.Graphics.Capture 在 HDR/Advanced Color 场景下可能需要使用 DXGI_FORMAT_R16G16B16A16_FLOAT 贯穿捕获管线；若过早压到 8-bit BGRA，可能产生 overclipping / washed-out。
Windows DWM 的 HDR 桌面常以 FP16 scRGB 线性空间工作。scRGB 数值可以大于 1.0；若直接 clamp 到 0~1 再当 SDR 使用，高光会大面积变白。
如果输入是 R10G10B10A2 + Rec.2100 PQ，必须先按 PQ EOTF 还原亮度，再执行动态范围压缩和色域映射；不能只做位深转换。
如果小黑盒已经在内部将 HDR 转为某种中间格式，但色彩元数据丢失，则后续编码器仍可能按 Rec.709 解释错误。
[Heading 2] 1.3 当前链路与目标链路
图 2  当前异常链路与目标链路
=== TABLE ===
项目核心原则 / 不修改服务端、不重写直播协议、不接管音频、不绕过账号或安全机制。目标只限于本机视频帧在编码前的颜色管理与格式转换。
=== END TABLE ===
[Heading 1] 2. 项目范围、边界与开发原则
[Heading 2] 2.1 范围
=== TABLE ===
类别 | 包含 | 不包含
捕获 | 识别 WGC / DXGI Desktop Duplication / D3D11 Shared Texture / 自研捕获路径 | 重新实现整个桌面捕获系统，除非作为最终兜底
颜色 | scRGB/PQ 识别、HDR→SDR Tone Mapping、Rec.709 输出、必要的 RGB↔YUV 转换 | 改变游戏自身 HDR 渲染、改变显示器 HDR 模式
集成 | 在捕获后或编码前替换视频帧；保持时间戳与尺寸 | 修改小黑盒网络协议、加密、认证或服务器逻辑
工程 | 日志、配置、版本锁、旁路、回滚、性能与稳定性测试 | 持久化驻留、隐藏进程、规避安全软件
发布 | 本地研究构建、明确版本兼容、可卸载 | 未经许可的大规模分发或商业化
=== END TABLE ===
[Heading 2] 2.2 研发边界与许可注意
客户端逆向和二进制 Hook 可能受软件许可协议、当地法律或企业政策约束。开发前应确认小黑盒客户端条款允许的研究与修改范围；公开发布前需要重新审查。项目应避免绕过登录、签名、授权、加密、反篡改、反作弊等安全控制。
OBS 可以作为行为与颜色管理参考，但 OBS Studio 代码采用 GPL 许可证。若直接复制或链接其实现，可能触发相应许可证义务。建议优先依据 Microsoft / ITU / SMPTE 公开规范独立实现 Tone Mapping 与格式转换；如确需复用代码，再单独做许可证评估。
[Heading 2] 2.3 开发原则
=== TABLE ===
原则 | 执行要求
证据优先 | 每一个关键结论都由日志、调用栈、纹理描述、截图或录屏证明，不凭字符串猜架构。
最小侵入 | 优先 Hook 单个稳定边界，不修改无关模块，不接管网络和音频。
GPU 全程 | HDR→SDR 与 RGB→YUV 均尽量在 D3D11 GPU 上完成，禁止逐帧 GPU→CPU→GPU。
Fail-open | 任何异常都回到小黑盒原始帧路径；插件故障不应导致客户端无法启动或持续黑屏。
版本锁定 | 每个兼容版本记录目标模块的版本号、文件哈希、关键签名；未知版本默认不写入。
可诊断 | 先做观察器，再做修改器。Debug 构建可看到输入格式、HDR 状态、帧率、耗时和当前 Tone Mapper。
=== END TABLE ===
[Heading 1] 3. 目标架构与技术路线
[Heading 2] 3.1 Hook 点优先级
=== TABLE ===
优先级 | 位置 | 优点 | 主要风险 | 结论
A | 捕获完成 → 颜色转换之前 | 最接近原始 HDR；可保留 FP16/PQ 信息；改动最小 | 需要准确定位 capture texture 生命周期 | 首选
B | 颜色转换之后 → 编码器之前 | 离编码器最近，容易验证最终格式 | 若前面已裁高光，信息不可恢复 | 第二选择
C | 直接替换编码器输入纹理 | 边界清晰，容易做帧替换 | 可能涉及 NVENC / Media Foundation / WebRTC 多种实现 | 候选
D | 外部桥接 / 虚拟视频源 | 无需深入修改客户端 | 多一次拷贝/合成，复杂度与延迟更高 | 兜底
=== END TABLE ===
[Heading 2] 3.2 推荐技术栈
=== TABLE ===
模块 | 建议
语言 / 构建 | C++20 + CMake；x64 Release/RelWithDebInfo；MSVC 优先
图形 | D3D11 + HLSL；与小黑盒现有 D3D11 Device 共享资源优先
Hook | 优先选择成熟、许可证清晰的最小 Hook 库；仅 Hook 必要函数
诊断 | WinDbg/x64dbg、Process Explorer、Process Monitor、ETW/GPUView；必要时 PIX/RenderDoc 做本地纹理验证
静态分析 | Ghidra / IDA（任选）；先模块/导入/字符串，再函数级追踪
日志 | 异步或低开销日志；默认每秒抽样而非逐帧刷盘；支持帧级 Debug 采样
测试 | 独立 ToneMapHarness + HDR 测试图 + 可重复的帧 dump/录制
=== END TABLE ===
[Heading 2] 3.3 帧数据路径设计
=== TABLE ===
Capture Surface (HDR or SDR) /         | /         +--> Probe: format / size / device / color-state / timestamp /         | /         +--> AutoDetect /                |-- SDR ------------------------------> Bypass /                |-- FP16 scRGB --> ToneMapScRGB -----+ /                |-- RGB10A2/PQ --> ToneMapPQ --------+--> Rec.709 RGB /                                                      | /                                                      +--> NV12 Limited / native SDR input /                                                      | /                                                      +--> Original encoder path
=== END TABLE ===
[Heading 1] 4. P0：复现与基线建立
目标：建立一个以后每次修改都能重复的“问题基线”。此阶段不对小黑盒进程做任何修改。
[Heading 2] 4.1 环境固定
记录 Windows 版本与 Build、GPU 型号、驱动版本、显示器 HDR 状态、分辨率、刷新率、缩放比例。
记录小黑盒 PC 客户端版本、安装路径、主 EXE 和关键 DLL 文件版本与 SHA-256。
固定一个可稳定复现 HDR 高光的游戏场景，并准备一个 SDR 对照场景。
准备至少一个远端 SDR 观看端（手机或另一台 SDR 显示器），不要只看本机 HDR 屏幕。
[Heading 2] 4.2 必须保存的基线样本
=== TABLE ===
样本 | 条件 | 用途
B0 | Windows HDR 关闭，小黑盒直接共享 | 正确 SDR 参照
B1 | Windows HDR 开启，小黑盒直接共享 | 问题样本
B2 | Windows HDR 开启，OBS 正确 HDR→SDR 录制 | 颜色管理正向参照
B3 | Windows HDR 开启，OBS 预览再被小黑盒抓取 | 验证二次桌面合成是否仍影响客户端
B4 | HDR 开启但共享纯 SDR 桌面/浏览器 | 判断问题是否只针对 HDR 游戏还是整个 HDR 桌面
=== END TABLE ===
[Heading 2] 4.3 P0 退出条件
=== TABLE ===
Gate P0 / 至少有一组“关闭 HDR 正常 / 开启 HDR 异常”的可重复 A/B 样本；客户端版本和系统环境全部记录；远端观看结果可稳定复现。否则禁止进入逆向阶段。
=== END TABLE ===
[Heading 1] 5. P1：静态侦察
目标：在不注入代码的情况下，把“小黑盒屏幕共享由哪些模块负责”缩小到可控范围。先找模块，再找函数，不要从主 EXE 全量反编译开始。
[Heading 2] 5.1 进程与模块盘点
启动小黑盒但不共享，记录进程树、子进程、GPU 进程与已加载模块。
开始屏幕共享后再次抓取模块清单，比较新增 DLL。
重点标记 dxgi.dll、d3d11.dll、Windows.Graphics.Capture 相关 WinRT 组件、mf*.dll、nvEncodeAPI64.dll、avcodec/ffmpeg、webrtc/libwebrtc 等候选。
对新增或高相关模块记录基址、路径、版本、签名状态和哈希。
[Heading 2] 5.2 静态线索检索
=== TABLE ===
线索 | 检索目标 | 可能含义
API / 导入 | CreateDXGIFactory*、D3D11CreateDevice、DuplicateOutput、CreateDirect3D11CaptureFramePool | 捕获或 D3D11 初始化
编码 | NvEnc*、MFCreate*、IMFTransform、avcodec_*、VideoEncoder | 编码器实现
像素格式 | NV12、P010、R16G16B16A16_FLOAT、R10G10B10A2、B8G8R8A8 | 中间帧格式
色彩 | 709、2020、PQ、HLG、HDR、scRGB、ColorSpace | 颜色管理路径
WebRTC | DesktopCapturer、VideoFrame、I420、NV12、webrtc | 共享可能基于 WebRTC
=== END TABLE ===
[Heading 2] 5.3 静态阶段输出
《模块地图.md》：按“捕获 / 转换 / 编码 / 网络 / UI”对模块归类。
《候选调用链.md》：列出 2~5 条最可能的捕获→编码路径，给每条路径标注证据强度。
禁止在本阶段修改二进制或打补丁；目标是为动态追踪选入口。
=== TABLE ===
Gate P1 / 能够明确回答：屏幕共享时新增/活跃的关键模块有哪些；至少找到一类候选捕获 API 和一类候选编码器。若完全无法定位，转向 ETW/调用跟踪扩大证据，而不是猜。
=== END TABLE ===
[Heading 1] 6. P2：动态追踪与格式确认
目标：确认每帧视频从哪里来、是什么格式、在哪里被转换、最终以什么格式进入编码器。这是整个项目最关键的阶段。
[Heading 2] 6.1 先做“观察器”
开发一个只记录、不替换帧的 CaptureProbe。无论采用 API Hook、调试器断点还是 ETW，第一版都必须保证帧内容完全透传。任何画面变化都视为 Probe 本身有副作用。
[Heading 2] 6.2 每个关键帧节点要记录的字段
=== TABLE ===
字段 | 说明
Thread / Timestamp | 线程 ID、QPC 时间、相邻帧间隔
Device / Context | ID3D11Device 与 Context 指针，确认是否跨设备
Texture | 地址、Width/Height、Format、MipLevels、ArraySize、SampleDesc、BindFlags、Usage、MiscFlags
HDR State | 系统 HDR 开关、输出 ColorSpace、输出最大亮度/SDR white level（可获取时）
Lifecycle | 创建、重建、Resize、Alt-Tab、停止共享、切换显示器时的资源变化
Encoder Input | 进入编码器前纹理格式、尺寸、色彩矩阵/范围元数据（若有）
=== END TABLE ===
[Heading 2] 6.3 HDR/SDR 对照追踪
=== TABLE ===
测试 | HDR | 重点观察
T2-1 | 关闭 | 捕获纹理与编码器输入的“正常路径”格式
T2-2 | 开启 | 格式是否从 BGRA8 变为 FP16 / RGB10A2；是否出现数值裁切
T2-3 | 开启 + 纯 SDR 桌面 | Windows SDR-on-HDR 的缩放行为
T2-4 | 开启 + HDR 游戏 | 游戏高光是否在某一节点前正常、节点后被裁白
=== END TABLE ===
[Heading 2] 6.4 关键判定树
=== TABLE ===
if capture_format == R16G16B16A16_FLOAT: /     hypothesis = "Windows HDR/scRGB or FP16 intermediate" / elif capture_format == R10G10B10A2_UNORM: /     inspect DXGI color space / PQ metadata / elif capture_format in {B8G8R8A8_UNORM, R8G8B8A8_UNORM}: /     compare pre-capture source and post-capture values /     if HDR highlights already clipped: /         defect is earlier than current hook point /  / Then follow the same frame until encoder input. / The first node where highlight information is lost = primary insertion boundary candidate.
=== END TABLE ===
=== TABLE ===
Gate P2 / 必须得到一张“真实帧路径图”，至少确认：① 捕获 API；② 捕获纹理格式；③ 颜色/格式转换节点；④ 编码器输入格式；⑤ HDR 信息在哪一步首次丢失。没有这五项，不进入写帧阶段。
=== END TABLE ===
[Heading 1] 7. P3：无损 Hook POC
目标：证明我们可以稳定地站在目标边界上观察和替换帧，但暂时不改变任何像素。
[Heading 2] 7.1 POC 步骤
在目标函数或接口处安装最小 Hook，只保存函数参数、纹理描述和调用时序。
确认 Hook 在开始共享、停止共享、重新共享、Alt-Tab、窗口 Resize 后仍只出现一个有效生命周期。
如果目标对象是 COM vtable，记录对象创建与销毁，避免对已释放实例继续调用。
实现“原样帧替换”：创建等尺寸兼容纹理，GPU CopyResource 后再送回原链路；远端画面必须与未加载插件一致。
增加一键 Bypass 开关，运行时可切回原始帧，以便 A/B 对比。
连续运行至少 30 分钟，验证无死锁、无 device removed、无明显掉帧。
[Heading 2] 7.2 POC 失败时的退路
Hook 点不稳定：向上游移动到“纹理创建/获取帧”边界，或向下游移动到“编码器 Submit/Input”边界。
目标线程对延迟极敏感：只在 Hook 内 enqueue 轻量任务，将 Shader 执行放到现有 D3D11 Context 合适位置。
存在多 D3D11 Device：为每个 Device 建独立资源池，禁止跨设备直接使用纹理。
客户端完整性检测导致无法加载：停止尝试规避安全机制，改走外部桥接/虚拟视频源方案。
=== TABLE ===
Gate P3 / 开启插件但不做颜色修改时，远端画面、帧率、延迟和客户端稳定性与基线基本一致；能够稳定拿到每帧目标纹理并安全旁路。
=== END TABLE ===
[Heading 1] 8. P4：GPU Tone Mapping 核心
目标：先在独立 Harness 中完成正确的 HDR→SDR，再接入小黑盒。算法模块必须与逆向/Hook 解耦，方便单独测试。
[Heading 2] 8.1 输入路径 A：FP16 scRGB
Windows Advanced Color / DWM 常使用 FP16 scRGB。其特点是线性光、sRGB/Rec.709 色度体系、数值可以超出 0~1。Microsoft 文档给出的典型关系是 scRGB (1,1,1) 代表约 80 nits 的 D65 白，(12.5,12.5,12.5) 可代表约 1000 nits。实现时不能先 clamp 到 1.0。
读取 FP16 线性 RGB，保留负值和 >1 的高光数值到 Tone Mapping 之前。
根据 Windows SDR reference white（若该帧包含经 DWM 提亮的 SDR 内容）恢复合理的 SDR/UI 亮度关系。
将线性 RGB 计算亮度或最大通道，执行曝光与高光 roll-off。
在 Tone Mapping 后进行 gamut compression / hue-preserving clipping，确保输出落入 Rec.709 可显示范围。
应用 Rec.709/sRGB 风格 OETF，生成 8-bit SDR RGB 中间纹理。
[Heading 2] 8.2 输入路径 B：Rec.2100 PQ / HDR10
根据输入元数据或已确认的色彩空间，将 RGB10A2 数值按 PQ EOTF 还原为线性亮度。
从 Rec.2020/实际容器色度映射到工作空间；避免直接把 PQ 数字当线性 RGB。
使用 peak_nits / mastering metadata（若可用）决定高光压缩范围；无法获取时使用可配置峰值并保守 roll-off。
Tone Mapping 到 SDR 动态范围后，转换到 Rec.709 色度与传递函数。
[Heading 2] 8.3 Tone Mapper 策略
=== TABLE ===
阶段 | 算法建议 | 说明
POC | Hable / ACES fitted / 简单可控曲线 | 实现快，便于观察是否解决“爆亮”根因；参数少
Alpha | 基于亮度的高光 roll-off + 色度保持 | 重点解决彩色高光变白与肤色漂移
Beta | PQ 场景可评估 BT.2390 思路 | 更规范地处理 PQ 高光与峰值显示映射
最终 | 保留 2~3 个模式 + Auto | 默认稳定模式；调试时可切换以比较画面
=== END TABLE ===
[Heading 2] 8.4 SDR 白点 / 峰值不要硬编码
不要把“203 nits”或“300 nits”写死为唯一真值。Windows 的 SDR-on-HDR reference white 可被用户调节；编码后的 SDR Rec.709 本身又是相对信号。建议把“输入解释所需的 SDR reference white”和“Tone Mapping 目标强度”分成两个参数：前者尽量从系统读取，后者提供合理默认值与校准范围。
[Heading 2] 8.5 GPU 实现要求
HLSL Pixel Shader 或 Compute Shader；优先与原 Device 共用，避免创建第二套 GPU 设备。
初始化时创建 Shader、Sampler、常量缓冲和纹理池；帧循环内禁止频繁 CreateTexture2D。
按 2~3 帧建立循环资源池，配合原编码器节奏，避免 GPU/CPU 强制 Flush。
HDR→SDR 与 NV12 转换可分两步：先输出 Rec.709 RGB，再用 D3D11 Video Processor 或 Compute Shader 转 NV12。
所有 Shader 参数可记录到日志，便于复现某一帧的 Tone Mapping 配置。
[Heading 2] 8.6 独立算法测试
=== TABLE ===
测试图 | 检查点
0~1000 nit 灰阶 ramp | 无大段截断；白点后平滑 roll-off；黑位不抬灰
彩色高光 | 红/绿/蓝高亮不应同时快速趋向纯白；尽量保留色相
SDR 白色 UI + HDR 高光同屏 | SDR UI 亮度自然，高光比 UI 更亮但不过曝
暗场 + 小高光 | 暗部层次不被整体提亮；小高光仍有存在感
肤色 / 游戏 HUD | 肤色与 UI 不出现明显过饱和或去饱和
=== END TABLE ===
=== TABLE ===
Gate P4 / 独立 Harness 可以把已知 HDR 测试输入稳定转换成 Rec.709 SDR；A/B 对比不再出现大面积过曝；4K60 单独跑转换时 GPU 开销可接受；没有 CPU Readback。
=== END TABLE ===
[Heading 1] 9. P5：接入小黑盒编码前链路
目标：把 P4 的独立 ToneMapCore 接到 P3 已验证的帧边界，保持小黑盒原有编码与网络逻辑不变。
[Heading 2] 9.1 按编码器输入格式分支
=== TABLE ===
编码器输入 | 集成方式
NV12 | ToneMap → Rec.709 RGB → GPU RGB-to-NV12 Limited；把 NV12 纹理送回原编码器
BGRA/RGBA 8-bit | ToneMap 直接输出 Rec.709 8-bit RGB；保留原后续 RGB→YUV 路径
P010 / 10-bit HDR | 若远端链路实际只支持 SDR，则不要保留 PQ；应在进入编码器前改为 SDR 路径；若客户端对格式写死，需定位其 SDR 编码分支
I420 CPU buffer | 优先向上游找 GPU 边界，避免把 HDR GPU 帧拉回 CPU；只有无法找到 GPU 输入时才评估 CPU 兜底
=== END TABLE ===
[Heading 2] 9.2 集成顺序
先只处理 1080p60，验证功能正确；再扩展到 1440p/4K。
先使用固定 Tone Mapping 参数；稳定后再加入 Auto HDR 与峰值检测。
替换帧时保持原 Width/Height、帧率、时间戳、裁剪矩形和旋转信息。
每次 Resize / 切换显示器 / 重新共享时销毁并重建尺寸相关资源。
如果编码器对共享纹理有特定 BindFlags/MiscFlags/KeyedMutex 要求，按原纹理描述建立兼容输出资源，不擅自简化。
确认 GPU 同步只发生在必要边界；禁止每帧 Map/Staging/Flush。
[Heading 2] 9.3 集成阶段诊断叠加层（Debug only）
=== TABLE ===
HDRFix: ON | Input: scRGB FP16 | Output: NV12 Rec.709 Limited / Capture: 3840x2160 @ 60.0 fps / ToneMapper: Auto/Hable | SourcePeak: 1000 nits | SDRRef: system / GPU: 0.42 ms | Convert: 0.18 ms | Total: 0.60 ms / Bypass: F8 | Dump next frame: F9
=== END TABLE ===
=== TABLE ===
Gate P5 / 在小黑盒真实共享中，Windows HDR 保持开启，远端 SDR 画面接近 B0/B2 参照；HDR 高光不再爆白；连续切换共享 10 次无崩溃；新增延迟和 GPU 占用达到预设目标。
=== END TABLE ===
[Heading 1] 10. P6：自动检测、配置与故障保护
[Heading 2] 10.1 AutoDetect 判定顺序
确认系统/目标输出是否处于 Advanced Color / HDR 状态。
读取捕获纹理 Format；FP16 优先按已验证的 scRGB 路径处理，RGB10A2 再结合 ColorSpace/上下文判断 PQ。
若捕获纹理已是 8-bit SDR 且 A/B 证明画面正常，则完全旁路。
若颜色空间无法确定，不自动强制转换；进入诊断模式并记录候选信息。
[Heading 2] 10.2 建议配置文件
=== TABLE ===
[General] / Enable=true / FailOpen=true / Input=Auto / Output=Rec709 / DebugOverlay=false / LogLevel=info /  / [HDR] / ToneMapper=Auto / SourcePeakNits=Auto / SDRReferenceWhite=System / Exposure=0.0 / HighlightRollOff=1.0 /  / [Compatibility] / StrictVersionCheck=true / AllowUnknownBuild=false
=== END TABLE ===
[Heading 2] 10.3 必须具备的故障保护
Kill switch：启动前可通过配置禁用；运行中 Debug 版本可热旁路。
Fail-open：Shader 编译失败、资源创建失败、颜色空间未知、Device Removed 时恢复原帧。
Crash marker：若上次运行在插件模块内异常退出，下次默认诊断/旁路启动。
版本不匹配：未知客户端哈希不执行写操作，只允许收集只读诊断。
日志限流：正式版默认不记录逐帧地址与大块数据，避免 I/O 造成卡顿。
[Heading 1] 11. P7：测试、性能与稳定性
[Heading 2] 11.1 功能测试矩阵
=== TABLE ===
维度 | 覆盖项
HDR 状态 | Windows HDR Off / On；游戏原生 HDR / Auto HDR / SDR 游戏
捕获类型 | 全屏显示器 / 游戏窗口 / OBS 投影等小黑盒支持的实际入口
分辨率 | 1920×1080、2560×1440、3840×2160
帧率 | 30、60；若客户端支持则加入 120
桌面缩放 | 100%、125%、150%
显示器 | 单 HDR；HDR+SDR 双屏；切换主屏；将共享目标拖到另一屏
生命周期 | 开始/停止、反复共享、Alt-Tab、最小化、睡眠恢复、显示器热插拔
远端 | 手机、普通 SDR 显示器、不同浏览器/客户端版本
=== END TABLE ===
[Heading 2] 11.2 颜色验收
以 B0（HDR Off 直接共享）和 B2（OBS 正确 HDR→SDR 录制）作为两类参照，而不是仅凭本机 HDR 预览判断。
测试灰阶、白色 UI、彩色高光、暗场、小高光和常见游戏 HUD。
允许 Tone Mapping 与 B0 在高光风格上略有差异，但不能出现整体曝光错误、大片纯白、明显黑位漂移或严重偏色。
对固定测试图保存输出帧，形成 golden images；后续版本自动做像素级/统计级回归。
[Heading 2] 11.3 性能与稳定性目标
=== TABLE ===
指标 | 目标 | 测试方法
Tone Mapping GPU 时间 | 4K60 单帧目标 < 1.0 ms | GPU timestamp query / PIX
RGB→NV12 | 目标 < 0.5 ms | GPU timestamp query
总额外 GPU 占用 | 目标 ≤ 2%（场景依赖） | 同一场景 A/B 统计
额外端到端延迟 | 目标 ≤ 1 帧 | 高帧率拍摄计时或共享端时间码
内存 / 显存增长 | 2 小时无持续线性增长 | 周期采样 working set / dedicated memory
掉帧 | 相对基线无显著增加 | 客户端/编码器计数 + 远端观察
=== END TABLE ===
[Heading 2] 11.4 稳定性压力场景
连续开始/停止共享 50 次。
共享中切换游戏全屏/无边框/窗口模式。
共享中切换 HDR Off→On→Off（用于验证资源重建与旁路，不作为用户常规操作）。
游戏崩溃或退出后重新启动，确保旧纹理/COM 对象不会继续被引用。
GPU 驱动重启或 Device Removed 情况下记录错误并安全停止转换。
=== TABLE ===
Gate P7 / 功能矩阵无阻断级缺陷；核心测试场景通过；2 小时稳定性达标；性能指标不显著影响游戏与共享体验；所有失败场景都能旁路或明确报错。
=== END TABLE ===
[Heading 1] 12. P8：打包、发布与版本维护
[Heading 2] 12.1 构建产物
=== TABLE ===
产物 | 用途
hdrfix.dll / 核心模块 | Hook、颜色检测、Tone Mapping 与帧替换
bootstrap / loader | 仅负责本地加载与版本检查；不做持久化和隐藏
hdrfix.ini | 用户配置
compat.json | 已验证客户端版本、文件哈希、关键模块签名
logs/ | 诊断日志、崩溃标记
uninstall / rollback | 完整撤销加载方式和配置
=== END TABLE ===
[Heading 2] 12.2 版本兼容策略
每次小黑盒更新后先比较主 EXE / 关键视频 DLL 哈希。
哈希变化但符号/签名仍命中时，只运行 Probe 验证，不直接开启帧替换。
重新确认关键调用链、纹理格式与对象生命周期后再把新版本加入 allowlist。
所有签名都应有语义验证，例如“目标函数参数中确实出现预期尺寸的 D3D11 Texture”，避免纯字节签名误命中。
[Heading 2] 12.3 发布级别
=== TABLE ===
级别 | 内容 | 用途
Dev | 全日志、帧 dump、Debug Overlay、符号 | 本机逆向和算法开发
Alpha | 限制版本、可旁路、保留较多诊断 | 少量测试机
Beta | 默认 AutoDetect、日志限流、稳定性保护 | 日常使用验证
Release | 仅已验证版本、最少 UI、完整卸载与回滚 | 长期自用或在许可允许时发布
=== END TABLE ===
[Heading 1] 13. 工程结构、Git 与开发规范
[Heading 2] 13.1 推荐仓库结构
=== TABLE ===
hdr-share-fix/ / ├─ docs/ / │  ├─ recon/ / │  ├─ test-reports/ / │  └─ compatibility/ / ├─ src/ / │  ├─ Bootstrap/ / │  ├─ Hook/ / │  ├─ CaptureProbe/ / │  ├─ ColorDetect/ / │  ├─ ToneMap/ / │  ├─ ColorConvert/ / │  ├─ Integration/ / │  └─ Diagnostics/ / ├─ shaders/ / │  ├─ tonemap_scrgb.hlsl / │  ├─ tonemap_pq.hlsl / │  └─ rgb_to_nv12.hlsl / ├─ tests/ / │  ├─ ToneMapHarness/ / │  ├─ GoldenFrames/ / │  └─ Perf/ / ├─ tools/ / ├─ config/ / └─ CMakeLists.txt
=== END TABLE ===
[Heading 2] 13.2 分支与提交建议
=== TABLE ===
分支 / 提交 | 规则
main | 始终保持可构建、可旁路；不直接做试验性 Hook
recon/* | 静态/动态侦察，允许只提交文档、脚本和日志工具
feature/tonemap-* | 独立算法与 Shader
feature/integration-* | 仅在 Gate P3/P4 通过后建立
fix/compat-* | 针对某个小黑盒版本的兼容修复
提交信息 | recon: / probe: / tonemap: / integrate: / test: / compat: 前缀；一次提交只解决一类问题
=== END TABLE ===
[Heading 2] 13.3 每个开发回合的固定流程
开始前：确认目标客户端版本、Git HEAD、工作区状态、上一阶段 Gate。
观察：先保存日志/截图/调用栈，明确本回合要证明的问题。
改动：只修改能验证该问题的最小代码。
本地验证：先 Bypass，再启用单一功能；避免一次打开多个新变量。
回归：跑 HDR Off、HDR On、开始/停止共享三组最小回归。
记录：在 docs/test-reports 写结论，标记 GO / PARTIAL / NO-GO。
提交：只有证据与代码一致后才 commit；重要里程碑打 tag。
[Heading 1] 14. 里程碑、工期与资源估算
逆向部分的不确定性远高于 Tone Mapping 算法本身。以下时间按 1 名熟悉 C++/Windows 图形接口的开发者估算，并预留客户端架构差异带来的波动；不应把它理解为固定承诺。
=== TABLE ===
里程碑 | 主要成果 | 预估
M0 / P0 | 问题基线、环境指纹、参照样本 | 0.5–1 天
M1 / P1 | 模块地图、候选捕获/编码路径 | 1–3 天
M2 / P2 | 真实帧路径与 HDR 信息丢失点 | 1–4 天
M3 / P3 | 稳定无损 Hook / 透传 POC | 1–3 天
M4 / P4 | 独立 GPU HDR→SDR 核心 | 2–4 天
M5 / P5 | 小黑盒集成 Alpha | 2–6 天
M6 / P6-P7 | 自动检测、故障保护、测试与稳定性 | 3–6 天
M7 / P8 | 打包、回滚、兼容文档 | 1–2 天
=== END TABLE ===
=== TABLE ===
总体预估 / 顺利情况下，功能 POC 约 5–10 个工作日；可日常使用的稳定 Alpha 约 2–4 周。若客户端存在自研捕获管线、强完整性保护、跨进程 GPU 共享或频繁版本更新，逆向阶段可能明显延长。
=== END TABLE ===
[Heading 2] 14.1 里程碑判定
=== TABLE ===
里程碑 | 必须证明
M1 | “谁在捕获、谁在编码”基本确定
M2 | “HDR 信息在哪一步丢失”有证据
M3 | “可以站在边界上不破坏原链路”
M4 | “算法在独立环境里确实能把 HDR 正确变成 SDR”
M5 | “小黑盒真实远端观看已修复”
M6 | “性能、生命周期和异常处理足以日常使用”
M7 | “更新可识别、失败可回滚、用户可卸载”
=== END TABLE ===
[Heading 1] 15. 风险登记与应对策略
=== TABLE ===
风险 | 概率 | 影响 | 应对
客户端使用非标准/自研捕获管线 | 中 | 高 | 以动态帧纹理追踪为主，不依赖 API 名称；从编码器输入反向追踪
HDR 信息在 Hook 点之前已经裁切 | 中 | 高 | 向更上游移动；优先保留 FP16/RGB10A2 原始帧
客户端更新导致签名失效 | 高 | 中 | 版本哈希 allowlist + 语义校验 + Probe 先行
Hook 引入死锁 / 生命周期错误 | 中 | 高 | 最小 Hook、COM 生命周期记录、Fail-open、停止共享时统一释放
多 D3D11 Device / 跨进程共享 | 中 | 中~高 | 按 Device 建资源池；必要时通过 shared handle 或定位同设备的更下游节点
Tone Mapping 观感偏灰/偏色 | 中 | 中 | 多测试图 + B0/B2 参照 + 可切换算法 + 参数记录
4K60 性能不足 | 低~中 | 中 | GPU 全程、资源复用、减少 pass、优先 Video Processor/Compute
客户端完整性或安全机制阻止加载 | 未知 | 高 | 不绕过安全控制；转外部桥接/虚拟视频方案
OBS 代码复用带来许可证义务 | 低 | 中 | 独立实现；仅参考公开规范与行为，复用前单独评估许可证
许可条款不允许逆向/分发 | 未知 | 高 | 仅本地研究；公开前确认条款与法律意见
=== END TABLE ===
[Heading 1] 16. 完成定义与最终验收
项目只有同时满足功能、性能、稳定性、可回滚和可维护性五类条件，才算“完成”，而不是只要远端看起来不亮就结束。
=== TABLE ===
类别 | 最终 DoD
功能 | HDR On 时自动转换；HDR Off 时旁路；远端 SDR 曝光、灰阶和主要颜色正常；高光无大面积裁白
性能 | 4K60 场景新增开销符合目标；无逐帧 CPU Readback；无明显共享延迟增加
稳定 | 至少 2 小时稳定；50 次开始/停止共享；Alt-Tab/Resize/退出游戏不会挂住客户端
故障 | 未知格式、未知版本、Shader 失败、Device Removed 都能安全旁路或停止转换
维护 | 有兼容清单、版本哈希、Recon 文档、测试报告；客户端更新有明确复验流程
卸载 | 删除/禁用补丁后客户端完全回到原状态，不留下持久化注入项
=== END TABLE ===
=== TABLE ===
最终判断标准 / 如果必须依赖“每次手动调亮度”才能工作，或者只能在某一个游戏/某一个场景里看起来正常，就还不能算完成。真正的目标是：颜色空间识别正确、转换路径稳定、异常可旁路。
=== END TABLE ===
[Heading 1] 附录 A：诊断日志字段建议
=== TABLE ===
[12:34:56.789] frame=1842 tid=14320 / capture_api=WGC / source_device=0x000001F2... source_texture=0x000001F4... / size=3840x2160 format=R16G16B16A16_FLOAT / bind=SRV|RTV misc=0x... / windows_hdr=1 output_color_space=RGB_FULL_G2084_NONE_P2020 / sdr_white_level=... / path=ToneMapScRGB -> Rec709RGB -> NV12Limited / shader_ms=0.43 convert_ms=0.16 total_ms=0.59 / encoder_input_format=NV12 bypass=0
=== END TABLE ===
[Heading 1] 附录 B：阶段证据模板
=== TABLE ===
字段 | 填写内容
日期 / 客户端版本 | 
Git HEAD | 
本回合问题 | 要证明或否定的单一假设
观察方法 | 日志 / 调试器 / ETW / 帧 dump / A/B 录屏
证据 | 关键调用栈、纹理描述、截图/录屏时间点
结论 | GO / PARTIAL / NO-GO
下一步 | 只写下一条最小验证动作
=== END TABLE ===
[Heading 1] 附录 C：参考资料
Microsoft Learn — Screen capture（HDR 捕获与 FP16 建议）
https://learn.microsoft.com/en-us/windows/apps/develop/media-authoring-processing/screen-capture
Microsoft Learn — Use DirectX with Advanced Color on HDR/SDR displays（scRGB、80 nit 参考白等）
https://learn.microsoft.com/en-us/windows/win32/direct3darticles/high-dynamic-range
OBS libobs Media I/O API Reference（Rec.709 / Rec.2100 PQ / range 定义）
https://docs.obsproject.com/reference-libobs-media-io
OBS NVIDIA NVENC Guide（HDR 工作流的行为参考）
https://obsproject.com/forum/resources/nvidia-nvenc-guide.740/
[Heading 1] 附录 D：第一轮实际开发的最小任务清单
建立仓库与 docs/recon 目录；保存客户端版本、文件哈希和 P0 A/B 样本。
抓取“未共享 / 正在共享”两份进程模块清单并做 diff。
确认是否出现 WGC / DXGI Duplication / WebRTC / NVENC / Media Foundation 线索。
写一个只读 CaptureProbe 原型，第一目标只打印捕获纹理 Width/Height/Format。
HDR Off / On 各跑一次，确认纹理格式是否变化。
沿同一帧追到编码器输入，标记第一个发生信息丢失的节点。
只在确认 Gate P2 后，再开始 ToneMapHarness 和 Shader。
=== TABLE ===
推荐开工顺序 / 真正动手时，先做 P0→P2。只要这三步证据扎实，后面的算法和集成几乎都属于常规 D3D11 工程；反过来先写 Shader，最容易在错误的 Hook 点上浪费时间。
=== END TABLE ===