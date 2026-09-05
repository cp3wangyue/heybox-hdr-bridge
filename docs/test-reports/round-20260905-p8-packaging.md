# 开发回合证据记录：P8 打包、简易安装与维护（Gate P8 GO）

| 字段 | 填写内容 |
| --- | --- |
| 日期 | 2026-09-05 12:45 |
| 客户端版本 | HeyboxChat 1.56.0 / VolcEngineRTC.dll v3.58.1.63260 |
| 显卡与驱动 | NVIDIA GeForce RTX 5070 Ti (15.6 GB VRAM) / Feature Level 0xB000 |
| Git HEAD | 936614d |
| 本回合问题 | 按照计划书 §12 及主人要求，将系统打包为小黑盒语音的专属外置插件，提供极简一键安装与一键卸载机制，实现零系统侵入、零文件修改与完整回滚，并准备推送到 GitHub |
| 观察方法 | 构建核心插件 `hdrfix.dll` 与管理加载器 `hdrfix_loader.exe`；内置 HLSL Shader 实现 100% 零依赖单文件自治；打包 `dist/` 交付包（含一键安装/卸载/启动 bat）；实测 `--install` 自动识别客户端目录与桌面快捷方式生成；实测 `--uninstall` 彻底清理与零残留 |
| 证据 | `dist/` 完整打包、`hdrfix_loader --status` 状态检测通过、`--install` 与 `--uninstall` 实测全绿 |
| 结论 | **GO** —— Gate P8 全部指标达成 |
| 下一步 | 提交全阶段 Git Commit，并按照用户要求上传至 GitHub 仓库 |

---

## 1. 架构与交付设计（计划书 §12）

### 1.1 核心插件模块 (`hdrfix.dll`)
- 整合 Hook 拦截、色彩空间判定、Tone Mapping 着色器与故障保护网；
- 在 `ToneMapCore` 中直接内置编译 HLSL 源码备份，摆脱外部着色器文件的路径依赖，支持任意目录即插即用；
- `DllMain` 自动读取同目录下的 `hdrfix.ini` 与 `compat.json`，完成安全守门校验后透明挂载。

### 1.2 简易安装与加载管理程序 (`hdrfix_loader.exe`)
- **自动寻径**：自动定位 `%LOCALAPPDATA%\Qingfeng\HeyboxChat` 小黑盒根目录；
- **一键安装 (`--install`)**：
  - 建立 `%LOCALAPPDATA%\Qingfeng\HeyboxChat\plugins\hdrfix` 插件专属目录；
  - 复制必要 DLL、配置与加载工具；
  - 在当前用户桌面生成快捷方式 **【小黑盒语音 (带HDR修复).lnk】**；
- **一键卸载 (`--uninstall`)**：
  - 触发 `Local\hdrfix_kill` 事件热熔断运行中的 Hook；
  - 彻底删除桌面快捷方式；
  - 递归清理插件目录，100% 恢复原生状态，零垃圾残留；
- **伴随拉起 (`--launch`)**：
  - 支持一键拉起小黑盒客户端，并自动完成伴随注入；
- **状态诊断 (`--status`)**：
  - 命令行清晰呈现客户端各进程 PID、`VolcEngineRTC.dll` 模块状态与补丁生效状态。

---

## 2. 交付包结构 (`dist/`)

```text
dist/
├─ hdrfix.dll                   # 核心色调映射插件
├─ hdrfix_loader.exe            # 简易安装/卸载/伴随加载器
├─ hdrfix.ini                   # 用户配置文件（算法选择、曝光补偿等）
├─ compat.json                  # 版本白名单数据库
├─ install.bat                  # 一键安装向导（生成桌面快捷方式）
├─ uninstall.bat                # 一键卸载向导（一键清理还原）
├─ 启动小黑盒(带HDR修复).bat    # 便携免安装启动脚本
└─ README.md                    # 简明用户使用手册
```

---

## 3. 验证证据记录

### 3.1 状态检测与注入验证 (`hdrfix_loader.exe --status / --inject`)
```text
===================================================
  小黑盒 HDR 屏幕共享修复补丁 — 运行状态诊断
===================================================

[客户端路径] : C:\Users\22983\AppData\Local\Qingfeng\HeyboxChat
[客户端进程] : 运行中 (发现 7 个进程)
  - PID 39672 | VolcEngineRTC: YES | HDRFix 补丁: 【已生效】
```

### 3.2 一键安装验证 (`hdrfix_loader.exe --install`)
```text
===================================================
  小黑盒 HDR 屏幕共享修复补丁 — 一键安装向导
===================================================

[1/3] 找到小黑盒目录: C:\Users\22983\AppData\Local\Qingfeng\HeyboxChat
[2/3] 正在复制补丁核心与配置文件到插件目录...
  - 已安装: hdrfix.dll
  - 已安装: hdrfix_loader.exe
  - 已安装: hdrfix.ini
  - 已安装: compat.json
[3/3] 正在创建桌面快捷方式...
  - [成功] 已在桌面生成快捷方式: 小黑盒语音 (带HDR修复).lnk
```
- 桌面生成 `小黑盒语音 (带HDR修复).lnk` 验证结果: `True`。

### 3.3 一键卸载验证 (`hdrfix_loader.exe --uninstall`)
```text
===================================================
  小黑盒 HDR 屏幕共享修复补丁 — 一键卸载向导
===================================================

[1/3] 未检测到活跃的 Hook 事件。
[2/3] 正在移除桌面快捷方式...
  - 已删除桌面快捷方式。
[3/3] 正在清理插件文件...
  - 已彻底移除插件目录: C:\Users\22983\AppData\Local\Qingfeng\HeyboxChat\plugins\hdrfix

---------------------------------------------------
  卸载完成！系统已恢复到原生状态，零任何残留文件。
---------------------------------------------------
```
- 文件残留检查: `Test-Path Desktop\*.lnk` -> `False`，`Test-Path plugins\hdrfix` -> `False`。零残留达标。
