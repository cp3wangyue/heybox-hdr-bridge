# HEYBOX HDR Bridge — 便携测试包

此目录暂时保存当前预编译测试包，方便本机验证。项目成熟后建议改用 **GitHub Releases** 分发二进制，源码仓库只保留源代码与构建脚本。

## 安装

双击：

```text
install.bat
```

或命令行运行：

```powershell
hdrfix_loader.exe --install
```

安装后可用：

```powershell
hdrfix_loader.exe --status
```

安装后会在桌面生成【小黑盒 (HDR Bridge)】快捷方式，双击该快捷方式即可启动小黑盒并自动加载 HDR 修复补丁。伴随器仅在当前小黑盒会话期间运行，小黑盒完全退出后自动结束，不残留后台进程，不注册开机常驻。

## 便携模式

正常启动小黑盒后执行：

```powershell
hdrfix_loader.exe --inject
```

## 卸载

双击：

```text
uninstall.bat
```

或：

```powershell
hdrfix_loader.exe --uninstall
```

完整原理、支持范围、配置项与已知限制请阅读仓库根目录的 `README.md`。
