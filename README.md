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
D3D11 GPU Tone Mapping
        ↓
Rec.709 / BGRA8 SDR
        ↓
代理帧返回给小黑盒原有编码链
```

HDR 关闭、格式未知或安全检查失败时，目标是直接走原生帧路径（Fail-open），避免因补丁异常导致黑屏。

## 当前状态

当前代码已经具备 WGC 帧池代理、scRGB → SDR GPU 色调映射、配置读取、旁路/熔断以及加载器等主要模块，但仍处于 **Experimental** 阶段，不建议把“在一台机器上验证通过”理解为对所有客户端版本、GPU 和 Windows 构建都已稳定支持。

目前仓库中的兼容信息对应：

| 项目 | 已验证环境 |
| --- | --- |
| 小黑盒客户端 | HeyboxChat 1.56.0 |
| RTC 模块 | VolcEngineRTC.dll 3.58.1.63260 |
| 捕获路径 | Windows Graphics Capture / D3D11 |
| HDR 输入 | FP16 scRGB |
| SDR 输出 | Rec.709 / BGRA8 |
| 10-bit PQ 输入 | 目前仅诊断，不主动转换 |

## 使用方式

### 安装

运行：

```text
dist/install.bat
```

安装器会把必要文件复制到：

```text
%LOCALAPPDATA%\Qingfeng\HeyboxChat\plugins\hdrfix
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
Output=Rec709

[HDR]
ToneMapper=Auto
SourcePeakNits=Auto
SDRReferenceWhite=System
Exposure=0.0
HighlightRollOff=1.0

[Compatibility]
StrictVersionCheck=true
AllowUnknownBuild=false
```

当前可用的 Tone Mapper 包括 `LumaHuePreserving`、`ACES`、`Hable`、`Reinhard` 和 `Clamp`。`Auto` 当前会回落到默认的亮度/色相保持路径。

## 关于版本兼容

“版本锁”不应该被理解成“只要小黑盒更新，补丁必然失效”。真正合理的设计应分成两层：

1. **能力检测**：先判断目标进程是否仍使用 WGC、目标 FramePool 接口是否存在、请求格式和 RTC 捕获路径是否仍符合预期；这些条件没变时，新版本有机会继续工作。
2. **已验证版本记录**：`compat.json` 记录我们实际测试过的客户端/RTC 版本。未知版本可进入“兼容性未验证”状态，而不是仅凭版本号机械判死刑。

当前代码里的 `StrictVersionCheck` 仍是一个未完成项：它还没有真正解析 `compat.json` 并校验文件版本/哈希，所以现阶段不能把它视作完整的版本兼容系统。

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
dist/              当前便携测试包
```

## 已知限制

- `compat.json` 尚未真正接入运行时版本/哈希校验；当前版本兼容系统仍需完善。
- 当前 x64 inline hook 为自研轻量实现，尚未完整处理被搬移指令中的 RIP-relative 寻址和相对跳转，客户端/系统 DLL 更新后存在兼容风险。
- DLL 的重初始化工作已经移出 `DllMain`，但 Hook 本身的生命周期与卸载并发仍需要继续做压力测试。
- Tone Mapping 使用宿主 D3D11 immediate context，并会修改渲染管线状态；后续需要做完整状态保存/恢复或隔离上下文。
- 目前主要验证的是 FP16 scRGB 路径，R10G10B10A2 / PQ 仍是诊断模式。
- 当前没有 CI、自动化 Release、签名和跨版本回归矩阵。

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
