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

仓库目前保留 `dist/` 作为便携测试包。长期发布建议迁移到 GitHub Releases，避免把编译产物长期放在源码历史中。

### 安装

```text
dist/install.bat
```

安装器会把必要文件复制到：

```text
%LOCALAPPDATA%\Qingfeng\HeyboxChat\plugins\hdrfix
```

并启动当前 Windows 会话中的后台守护进程。**当前代码中的开机自启动注册逻辑仍有待修正，重启 Windows 后不要默认认为守护已经自动恢复。**

### 状态检查

```powershell
dist\hdrfix_loader.exe --status
```

### 手动注入

先正常启动小黑盒，再执行：

```powershell
dist\hdrfix_loader.exe --inject
```

### 卸载

```text
dist/uninstall.bat
```

卸载流程会发送旁路/停止信号并清理插件目录。

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

- 当前“严格版本检查”并没有真正校验 `HeyboxChat.exe` / `VolcEngineRTC.dll` 的文件版本或哈希；`compat.json` 也尚未接入运行时白名单逻辑。
- 当前 x64 inline hook 为自研轻量实现，尚未完整处理被搬移指令中的 RIP-relative 寻址和相对跳转，客户端/系统 DLL 更新后存在兼容风险。
- DLL 初始化阶段仍有较多工作发生在 `DllMain` 中，应迁移到独立初始化线程以避免 Loader Lock 风险。
- Tone Mapping 使用宿主 D3D11 immediate context，并会修改渲染管线状态；后续需要做完整状态保存/恢复或隔离上下文。
- 目前主要验证的是 FP16 scRGB 路径，R10G10B10A2 / PQ 仍是诊断模式。
- 当前没有 CI、自动化 Release、签名和跨版本回归矩阵。

## 开发原则

1. **Fail-open**：任何异常优先返回小黑盒原始帧。
2. **GPU-only hot path**：逐帧路径避免 GPU → CPU → GPU readback。
3. **最小 Hook 面**：只处理本地屏幕捕获/颜色转换，不碰账号、网络协议、登录、加密或服务端逻辑。
4. **版本保守**：未知客户端版本默认应旁路，而不是强行介入。
5. **可回滚**：安装、守护、注入和配置都必须能够完整停用与卸载。

## 安全与兼容性说明

DLL 注入和 API Hook 属于高权限本地运行时行为，部分安全软件可能产生告警。不要把本项目用于绕过登录、授权、签名、反作弊或其他安全机制；本项目的目标仅是修复本地屏幕共享的 HDR → SDR 颜色处理链路。

## License

仓库目前**尚未声明开源许可证**。在正式添加 `LICENSE` 前，公开可见源码并不等同于获得复制、修改、再发布许可；如计划开放贡献或分发二进制，请先选择并补充合适的许可证。
