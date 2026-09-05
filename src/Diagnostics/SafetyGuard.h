#pragma once
// Diagnostics/SafetyGuard.h — 四重故障保护核心（计划书 §10.3 / §12.1）
//
// 负责实现：
//   1. Kill Switch（配置文件、命名事件 Local\hdrfix_kill、环境变量 HDRFIX_DISABLE）；
//   2. Crash Marker 崩溃标记自检与安全降级保护；
//   3. 版本锁与兼容性白名单检查；
//   4. 全局 Fail-Open 决策守门。

#include <windows.h>
#include <atomic>
#include <string>

namespace hdrfix {

enum class SafetyStatus {
    Safe = 0,                // 状态健康，允许介入
    KilledByConfig = 1,      // 配置禁用 (Enable=false)
    KilledByEvent = 2,       // 命名事件触发热旁路 (Local\hdrfix_kill)
    KilledByEnv = 3,         // 环境变量禁用 (HDRFIX_DISABLE=1)
    CrashMarkerDetected = 4, // 检测到上次崩溃遗留标记，已触发安全降级模式
    VersionRejected = 5      // 客户端版本未经验证且版本锁生效
};

class SafetyGuard {
public:
    static SafetyGuard& Instance();

    // 在插件加载初始化时调用：建立 Crash Marker，检查旧标记与版本锁
    bool Initialize();

    // 在插件正常退出前调用：清理 Crash Marker
    void Shutdown();

    // 综合判断当前是否允许执行帧拦截与色调映射接入
    bool CanIntercept();

    SafetyStatus GetLastStatus() const { return m_lastStatus; }
    const char* GetStatusString(SafetyStatus status) const;

    // 手动/动态触发旁路
    void TriggerManualBypass(bool bypass) { m_manualBypass.store(bypass); }
    bool IsManualBypassed() const { return m_manualBypass.load(); }

    // Crash marker 路径
    std::wstring GetMarkerPath() const { return m_markerPath; }

private:
    SafetyGuard();
    ~SafetyGuard();

    bool CheckKillSwitch();
    bool CheckCrashMarker();
    bool CheckVersionLock();

    std::atomic<bool> m_initialized{false};
    std::atomic<bool> m_manualBypass{false};
    std::atomic<bool> m_safeFallbackMode{false}; // 崩溃降级模式
    SafetyStatus m_lastStatus = SafetyStatus::Safe;
    std::wstring m_markerPath;
};

} // namespace hdrfix
