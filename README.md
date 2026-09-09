# HEYBOX HDR Bridge

> 非官方的 Windows HDR → SDR 屏幕共享兼容层，面向小黑盒（HEYBOX）PC 客户端。

HEYBOX HDR Bridge 用于解决 **Windows 开启 HDR 后，小黑盒屏幕共享在远端出现过亮、发白、高光裁切或颜色异常** 的问题。它不会关闭本机 HDR，而是在小黑盒的 Windows Graphics Capture（WGC）捕获链路中保留 HDR 动态范围，并在 GPU 上将画面映射为标准 SDR Rec.709 后交回原有编码流程。

> [!WARNING]
> 这是一个**非官方、实验性**兼容项目，与小黑盒/HEYBOX 官方无隶属、合作或授权关系。项目会向小黑盒进程加载 DLL 并 Hook 本地 WGC 接口；客户端更新后兼容性可能变化，使用前请阅读“已知限制”。

## 为什么需要它

Windows HDR 桌面通常以 FP16 scRGB 参与合成。若屏幕共享程序仍请求 8-bit BGRA 捕获表面，HDR 高光会在进入后续编码链前就被压缩或裁切，之后再做普通 SDR 编码已经无法恢复正确亮度关系。

本项目当前的处理路径是：

```text
HEYBOX 请求 WGC BGRA8 帧池
        ↓
拦截 Direct3D11CaptureFramePool 创建
        ↓
Windows HDR 开启时提升为 FP16 scRGB
        ↓
D3D11 GPU Tone Mapping (ITU-R BT.2390 EETF)
        ↓
sRGB / Rec.709 (BGRA8 SDR)
        ↓
代理帧返回给小黑盒原有编码链
```

HDR 关闭、格式未知或安全检查失败时，目标是直接走原生帧路径（Fail-open），避免因补丁异常导致黑屏。

## 当前状态

当前代码已经具备 WGC 帧池代理、基于 ITU-R BT.2390 的 scRGB → SDR GPU 色调映射、配置读取、安全熔断守卫以及会话级伴随加载器等核心模块。

目前仓库中的兼容与色彩信息对应：

| 项目 | 已验证环境 |
| --- | --- |
| 小黑盒客户端 | HeyboxChat 1.56.0 / 1.57.0 |
| RTC 模块 | VolcEngineRTC.dll 3.58.1.63260 |
| 捕获路径 | Windows Graphics Capture / D3D11 |
| HDR 输入 | FP16 scRGB (80~10000 nits) |
| SDR 输出 | sRGB / Rec.709 (BGRA8) |
| 色调映射 | ITU-R BT.2390 EETF (Hermite 样条 / Rec.2020 宽色域中转 / 对标 OBS Studio 28+) |
| 10-bit PQ 输入 | 目前仅诊断，不主动转换 |

## 使用方式

### 安装

运行：

```text
dist/install.bat
```

安装器会把必要文件复制到独立持久化目录（不受小黑盒自身热更新或重装影响）：

```text
%LOCALAPPDATA%\HeyboxHDRBridge
```

并在桌面创建：

```text
小黑盒 (HDR Bridge)
```

以后从这个快捷方式启动小黑盒即可。

### 会话级伴随模式

新版不再注册 Windows 开机自启动，也不会让注入守护器全天常驻。

启动流程是：

```text
桌面“小黑盒 (HDR Bridge)”
        ↓
hdrfix_loader.exe --launch
        ↓
启动 HeyboxChat.exe
        ↓
伴随器仅在当前小黑盒会话存在
        ↓
为当前及后续 Electron 子进程加载 hdrfix.dll
        ↓
小黑盒完全退出
        ↓
伴随器自动退出
```

这意味着正常状态下，**小黑盒没运行时不会有 HDR Bridge 伴随进程**。

> 注意：如果绕过该快捷方式，直接点击原版小黑盒图标，Windows 不会凭空知道需要启动 HDR Bridge；此时可在小黑盒启动后手动执行 `hdrfix_loader.exe --inject`。要做到“无论从任何入口启动原版小黑盒都自动伴随、但平时又完全没有常驻监听”，需要更侵入的系统级启动拦截机制，本项目目前不采用这种方案。

### 状态检查

```powershell
dist\hdrfix_loader.exe --status
```

### 一次性手动注入

先正常启动小黑盒，再执行：

```powershell
dist\hdrfix_loader.exe --inject
```

### 卸载

```text
dist/uninstall.bat
```

卸载流程会通知当前伴随器退出、清理新版快捷方式，同时兼容删除旧版本可能残留的 `HKCU\...\Run\HeyboxHDRFix` 自启动项。

## 配置

默认配置位于 `config/hdrfix.ini`，发布包中也包含同名配置。

```ini
[General]
Enable=true
FailOpen=true
Input=Auto
Output=sRGB

[HDR]
ToneMapper=BT2390
SourcePeakNits=Auto
SDRReferenceWhite=System
Exposure=0.0
HighlightRollOff=1.0

[Compatibility]
# 四级版本兼容体系策略：
# Verified: compat.json 官方验证版本，完全信任
# UntestedCompatible: 未知版本但在运行时通过能力检测 (WGC/RTC/PixelFormat/FP16)
# Incompatible / DiagnoseOnly: 关键接口缺失或结构异常，强制 Fail-open 原生透传
AllowUntestedCompatible=true
StrictVersionCheck=false
AllowUnknownBuild=true
```

- **Output**：支持 `sRGB`（默认推荐，对标 OBS Studio 28+，暗部扎实色彩通透）、`Rec709`（传统广播曲线）、`Gamma24` 与 `Linear`。
- **ToneMapper**：支持 `BT2390`（默认推荐，国际广播标准 ITU-R BT.2390 EETF，基于 Rec.2020 宽色域中转与 Hermite 三次样条，实现 1:1 SDR 无损透传与自然高光压缩）、`OBSReinhard`（OBS 宽色域 Reinhard）、`ExtendedReinhard`、`Hable`、`ACES`、`LumaHuePreserving` 与 `Clamp`。`Auto` 默认启用 `BT2390`。

## 关于版本兼容体系

本项目采用**四级动态兼容模型**与**能力检测优先**策略，拒绝“版本号一变就机械判死”的死版本锁：

1. **Verified（已验证）**：
   - 客户端与 RTC 模块版本完全命中 `config/compat.json` 官方验证白名单，直接启用 HDR 桥接。
2. **UntestedCompatible（未测兼容）**：
   - 客户端或 RTC 发生小版本更新，但在运行时通过全部关键能力检测：
     - `VolcEngineRTC.dll` 模块已加载；
     - WGC `Direct3D11CaptureFramePool` 工厂可用；
     - 请求的捕获格式受支持（如 BGRA8）；
     - GPU 支持创建 FP16 纹理与 RTV 渲染目标；
   - 此时自动评定为 `UntestedCompatible` 并启用 HDR 桥接，保证客户端平滑更新可用。
3. **Incompatible（不兼容）**：
   - 必需接口缺失、格式不支持或链路结构发生重大不兼容断裂，强制 **Fail-open**，走原生透传帧，杜绝黑屏崩溃。
4. **DiagnoseOnly（仅诊断降级）**：
   - 检测到异常、上次未清理的 Crash Marker 或手动热旁路，仅记录诊断日志，不介入修改任何帧。

运行时会输出清晰的标准诊断块，例如：

```text
Compatibility:
  HeyboxChat: 1.56.0 [VERIFIED]
  VolcEngineRTC: 3.58.1.63260 [VERIFIED]
  WGC FramePool: compatible
  Requested format: BGRA8
  Decision: Verified
  Action: HDR bridge enabled
```

## 从源码构建

### 环境

- Windows 11
- Visual Studio / MSVC x64
- CMake 3.24+
- Windows SDK

### 构建

```powershell
cmake --preset windows-msvc-x64
cmake --build --preset msvc-x64-release
```

生成物位于：

```text
build/msvc-x64/
```

仓库使用 CMake 作为唯一构建源；Visual Studio 的 `.vcxproj`、`.slnx`、`CMakeFiles/`、`cmake_install.cmake` 等均属于生成文件，不应提交到 Git。

## 代码结构

```text
src/
├─ Bootstrap/      DLL 入口与加载器
├─ CaptureProbe/   WGC / D3D11 / HDR 状态探测
├─ ColorDetect/    HDR / SDR 判定
├─ Diagnostics/    配置与安全旁路
├─ Hook/           本地 Hook 实现
├─ Integration/    WGC 帧池代理与帧替换
└─ ToneMap/        D3D11 色调映射核心

shaders/           HLSL 参考实现
tools/             捕获、HDR 状态与诊断工具
tests/             Tone Mapping 独立测试台
config/            默认配置
dist/              当前便携发布包
```

## 已知限制

- 目前主要验证的是 FP16 scRGB 路径，R10G10B10A2 / PQ 仍是诊断模式。
- 当前尚未配置远端 CI 自动化流水线与代码签名证书。


## 开发原则

1. **Fail-open**：任何异常优先返回小黑盒原始帧。
2. **GPU-only hot path**：逐帧路径避免 GPU → CPU → GPU readback。
3. **最小 Hook 面**：只处理本地屏幕捕获/颜色转换，不碰账号、网络协议、登录、加密或服务端逻辑。
4. **能力优先于版本号**：更新后先验证捕获链能力；版本号用于标记“已验证/未验证”，而不是唯一判断依据。
5. **可回滚**：安装、注入、配置和会话伴随都必须能够完整停用与卸载。

## 安全与兼容性说明

DLL 注入和 API Hook 属于高权限本地运行时行为，部分安全软件可能产生告警。不要把本项目用于绕过登录、授权、签名、反作弊或其他安全机制；本项目的目标仅是修复本地屏幕共享的 HDR → SDR 颜色处理链路。

## License

仓库目前**尚未声明开源许可证**。在正式添加 `LICENSE` 前，公开可见源码并不等同于获得复制、修改、再发布许可；如计划开放贡献或分发二进制，请先选择并补充合适的许可证。
