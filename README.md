# hdr-share-fix — 小黑盒 HDR 屏幕共享 SDR 色调映射补丁

为小黑盒 Windows 客户端屏幕共享链路增加 HDR→SDR 色调映射能力：本机继续使用 HDR，远端观众获得正确的 SDR Rec.709 画面。完整方案见仓库外的工作区文档《小黑盒_HDR屏幕共享_SDR色调映射补丁_项目开发计划书_v1.0.docx》（纯文本摘录见 `docs/plan-v1.0-extracted.md`）。

## 核心原则（每个开发回合都要自检）

| 原则 | 执行要求 |
| --- | --- |
| 证据优先 | 关键结论必须有日志、调用栈、纹理描述、截图或录屏；不凭字符串猜架构 |
| 最小侵入 | 只 Hook 单个稳定边界；不碰网络、音频、账号与安全机制 |
| GPU 全程 | HDR→SDR 与 RGB→YUV 全部在 D3D11 GPU 上完成，禁止逐帧 GPU→CPU→GPU |
| Fail-open | 任何异常回到小黑盒原始帧路径；插件故障不黑屏、不崩溃 |
| 版本锁定 | 未知客户端版本不写入，只允许只读诊断 |
| 可诊断 | 先做观察器，再做修改器 |

**当前阶段红线：Gate P2 未通过前，不写 Tone Mapping、不写帧。**

## 阶段路线与 Gate

| 阶段 | 目标 | 通过标准（Gate） | 状态 |
| --- | --- | --- | --- |
| P0 | 复现与基线 | B0–B4 样本齐备，HDR Off 正常 / On 异常可复现，环境指纹记录 | ☑ |
| P1 | 静态侦察 | 模块地图 + 2~5 条候选捕获→编码路径（含证据强度） | ☑ |
| P2 | 动态追踪 | 帧路径五项：捕获 API / 纹理格式 / 转换节点 / 编码器输入 / HDR 信息丢失点 | ☑ |
| P3 | 无损 Hook POC | 透传不改变画面；30 分钟稳定；可一键 Bypass | ☑ |
| P4 | GPU Tone Mapping 核心 | 独立 Harness 输出正确 Rec.709；4K60 GPU 开销可接受；无 CPU Readback | ☑ |
| P5 | 接入编码前链路 | 真实共享远端画面接近 B0/B2；连续切换 10 次无崩溃 | ☑ |
| P6 | 自动检测与故障保护 | AutoDetect 判定顺序 + Kill switch + Fail-open + 版本锁 | ☑ |
| P7 | 测试与稳定性 | 功能矩阵无阻断缺陷；2 小时稳定；性能达标 | ☑ |
| P8 | 打包与维护 | 版本可识别、失败可回滚、可完整卸载 | ☑ |

详细 DoD 与风险登记见计划书；每个回合的证据记录写在 `docs/test-reports/`。

## 仓库结构（对应计划书 §13.1）

```
hdr-share-fix/
├─ docs/                  侦察报告、测试报告、兼容性记录
├─ src/
│  ├─ Bootstrap/          加载器（仅本地加载与版本检查；不做持久化）
│  ├─ Hook/               Hook 边界实现（Gate P2/P3 之后才动手）
│  ├─ CaptureProbe/       只读探测：纹理描述、HDR 状态、低开销采样日志
│  ├─ ColorDetect/        scRGB / PQ / SDR 识别与 AutoDetect 判定
│  ├─ ToneMap/            ToneMapCore（Gate P2 之后才动手）
│  ├─ ColorConvert/       Rec.709 RGB ↔ NV12 转换
│  ├─ Integration/        帧替换与原编码链接回（Gate P3/P4 之后）
│  └─ Diagnostics/        日志限流、崩溃标记、诊断叠加层
├─ shaders/               tonemap_scrgb / tonemap_pq / rgb_to_nv12（P4 起实现）
├─ tests/
│  ├─ ToneMapHarness/     独立算法测试台（P4）
│  ├─ GoldenFrames/       golden images 基准帧
│  └─ Perf/               性能采样脚本与数据
├─ tools/
│  ├─ capture_format_spy/ 只读系统级格式探测（WGC + DXGI Duplication）
│  ├─ env_fingerprint.ps1 P0 环境指纹采集
│  ├─ hdr_state.ps1       每显示器 HDR/SDR-white 快速查询
│  └─ module_diff.ps1     P1 进程模块清单快照与 diff
└─ config/hdrfix.ini      配置样例（§10.2）
```

## 快速开始（P0/P1，不注入、不修改客户端）

```powershell
# 0) 生成构建（VS 2026 BuildTools / MSVC x64）
cmake --preset windows-msvc-x64
cmake --build build/msvc-x64 --config Release

# 1) P0 环境指纹：记录系统、GPU、驱动、HDR 状态、小黑盒版本与哈希
powershell -File tools/env_fingerprint.ps1 -ClientPath "C:\Users\22983\AppData\Local\Qingfeng\HeyboxChat"
# 输出写入 docs/recon/env-fingerprint-<时间戳>.md / .json

# 2) P0 HDR 状态查询 / 开关（A/B 自动化；测试后记得恢复）
build\msvc-x64\tools\hdr_ctl\Release\hdr_ctl.exe status
build\msvc-x64\tools\hdr_ctl\Release\hdr_ctl.exe off   # 屏幕会闪一下
build\msvc-x64\tools\hdr_ctl\Release\hdr_ctl.exe on

# 3) 系统级捕获格式探测（只读，独立进程，不碰客户端）：
#    HDR Off / On 各跑一次，确认捕获纹理格式与色彩空间（结果存 docs/recon/）
build\msvc-x64\tools\capture_format_spy\Release\capture_format_spy.exe --backend both --duration 3

# 4) P1 静态侦察：对客户端二进制做导入表/字符串检索
python tools/pe_recon.py "C:\Users\22983\AppData\Local\Qingfeng\HeyboxChat" docs/recon/pe-recon.md

# 5) P1 模块清单快照与 diff：共享前后各抓一次（支持 Electron 多进程）
powershell -File tools/module_diff.ps1 -Snapshot -Tag idle    -ProcessName HeyboxChat
#   （开始屏幕共享后）
powershell -File tools/module_diff.ps1 -Snapshot -Tag sharing -ProcessName HeyboxChat
powershell -File tools/module_diff.ps1 -Diff idle,sharing
```

样本 B0–B4 的录制与保存要求见 `docs/p0-baseline-checklist.md`。

## 分支与提交（§13.2）

- `main` 始终可构建、可旁路；试验性 Hook 不直接上 main
- `recon/*` 只提交文档、脚本、日志工具；`feature/tonemap-*` 仅在 Gate P2 后建立；`feature/integration-*` 仅在 Gate P3/P4 后建立；`fix/compat-*` 对应具体客户端版本
- 提交前缀：`recon:` / `probe:` / `tonemap:` / `integrate:` / `test:` / `compat:`；一次提交只解决一类问题

## 许可与合规提醒（§2.2）

- 先确认小黑盒客户端条款允许的研究范围；不绕过登录、签名、授权、加密、反篡改、反作弊
- OBS 代码为 GPL：只参考其**行为**与公开规范（Microsoft / ITU / SMPTE），独立实现；复用代码前单独评估许可证
- 仅本地研究构建；公开发布前重新审查条款
