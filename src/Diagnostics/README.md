# Diagnostics

诊断基础设施：

- 日志限流（§10.3）：正式版默认不逐帧落盘；帧级采样仅 Debug（见 CaptureProbe/probe_logger）
- Crash marker：上次运行在插件内异常退出 → 下次默认诊断/旁路启动
- 帧参数记录：所有 Shader 参数可记录到日志，便于复现单帧配置（§8.5）
- 故障保护：Kill switch / Fail-open / 版本锁的运行时行为在此统一实现
