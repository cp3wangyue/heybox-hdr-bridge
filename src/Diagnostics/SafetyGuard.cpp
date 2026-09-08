// Diagnostics/SafetyGuard.cpp — 故障保护实现
#include "Diagnostics/SafetyGuard.h"

#include <cstdio>
#include <fstream>
#include <vector>

#include "Diagnostics/ConfigManager.h"
#include "Diagnostics/CompatibilityManager.h"

namespace hdrfix {

namespace {

std::wstring GetTempMarkerPath(DWORD pid)
{
    wchar_t tempDir[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tempDir);
    wchar_t filename[128];
    swprintf_s(filename, L"hdrfix_active_%lu.marker", pid);
    return std::wstring(tempDir) + filename;
}

} // namespace

SafetyGuard& SafetyGuard::Instance()
{
    static SafetyGuard s_inst;
    return s_inst;
}

SafetyGuard::SafetyGuard()
{
    m_markerPath = GetTempMarkerPath(::GetCurrentProcessId());
}

SafetyGuard::~SafetyGuard()
{
    Shutdown();
}

bool SafetyGuard::Initialize()
{
    if (m_initialized.exchange(true)) return true;

    // 1. 检查是否存在上次异常遗留的 Marker
    CheckCrashMarker();

    // 2. 建立本次会话的 Marker 文件
    FILE* fp = _wfopen(m_markerPath.c_str(), L"w");
    if (fp) {
        fprintf(fp, "pid=%lu\ntimestamp=%llu\n", ::GetCurrentProcessId(), ::GetTickCount64());
        fclose(fp);
    }

    return true;
}

void SafetyGuard::Shutdown()
{
    if (!m_initialized.load()) return;

    // 正常退出：清理 Marker 文件与重置内部降级标记
    if (!m_markerPath.empty()) {
        ::DeleteFileW(m_markerPath.c_str());
    }
    m_safeFallbackMode.store(false);
    m_versionChecked.store(false);
    m_versionPassed.store(false);
    m_lastStatus = SafetyStatus::Safe;
    m_initialized.store(false);
}

bool SafetyGuard::CheckKillSwitch()
{
    // a. 配置文件全局开关
    if (!ConfigManager::Instance().GetConfig().general.enable) {
        m_lastStatus = SafetyStatus::KilledByConfig;
        return false;
    }

    // b. 环境变量快速开关 HDRFIX_DISABLE=1
    wchar_t envBuf[16]{};
    if (::GetEnvironmentVariableW(L"HDRFIX_DISABLE", envBuf, 16) > 0) {
        if (envBuf[0] == L'1' || _wcsicmp(envBuf, L"true") == 0) {
            m_lastStatus = SafetyStatus::KilledByEnv;
            return false;
        }
    }

    // c. 命名事件热旁路开关 Local\hdrfix_kill
    HANDLE killEvent = ::OpenEventW(SYNCHRONIZE, FALSE, L"Local\\hdrfix_kill");
    if (killEvent) {
        DWORD waitRes = ::WaitForSingleObject(killEvent, 0);
        ::CloseHandle(killEvent);
        if (waitRes == WAIT_OBJECT_0) {
            m_lastStatus = SafetyStatus::KilledByEvent;
            return false;
        }
    }

    // d. 手动动态旁路
    if (m_manualBypass.load()) {
        m_lastStatus = SafetyStatus::KilledByEvent;
        return false;
    }

    return true;
}

bool SafetyGuard::CheckCrashMarker()
{
    // 若当前 marker 文件已经存在且不为空，说明可能发生过未清理的非正常退出
    DWORD attr = ::GetFileAttributesW(m_markerPath.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        m_safeFallbackMode.store(true);
        m_lastStatus = SafetyStatus::CrashMarkerDetected;
        return false;
    }
    return true;
}

bool SafetyGuard::CheckVersionLock()
{
    if (m_versionChecked.load()) {
        if (!m_versionPassed.load()) {
            m_lastStatus = SafetyStatus::VersionRejected;
            return false;
        }
        return true;
    }

    // 获取当前进程主可执行文件名
    wchar_t exePath[MAX_PATH]{};
    ::GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring exeName = exePath;
    size_t lastSlash = exeName.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos) {
        exeName = exeName.substr(lastSlash + 1);
    }

    // 允许的宿主进程
    if (_wcsicmp(exeName.c_str(), L"HeyboxChat.exe") != 0 &&
        _wcsicmp(exeName.c_str(), L"probe_testhost.exe") != 0 &&
        _wcsicmp(exeName.c_str(), L"tone_map_harness.exe") != 0 &&
        _wcsicmp(exeName.c_str(), L"hook_stress_test.exe") != 0 &&
        _wcsicmp(exeName.c_str(), L"compat_test.exe") != 0) {
        m_lastStatus = SafetyStatus::VersionRejected;
        m_versionPassed.store(false);
        m_versionChecked.store(true);
        return false;
    }

    const auto& compat = ConfigManager::Instance().GetConfig().compat;
    auto decision = CompatibilityManager::Instance().Evaluate();

    if (decision.tier == CompatibilityTier::Incompatible) {
        m_lastStatus = SafetyStatus::VersionRejected;
        m_versionPassed.store(false);
        m_versionChecked.store(true);
        return false;
    }

    if (decision.tier == CompatibilityTier::DiagnoseOnly) {
        m_lastStatus = SafetyStatus::CrashMarkerDetected;
        m_versionPassed.store(false);
        m_versionChecked.store(true);
        return false;
    }

    if (decision.tier == CompatibilityTier::UntestedCompatible) {
        if (!compat.allowUntestedCompatible) {
            m_lastStatus = SafetyStatus::VersionRejected;
            m_versionPassed.store(false);
            m_versionChecked.store(true);
            return false;
        }
    }

    m_versionPassed.store(true);
    m_versionChecked.store(true);
    return true;
}


bool SafetyGuard::CanIntercept()
{
    // 1. 若处于崩溃降级模式，绝不修改帧，强制 Fail-open 旁路
    if (m_safeFallbackMode.load()) {
        m_lastStatus = SafetyStatus::CrashMarkerDetected;
        return false;
    }

    // 2. 检查各项 Kill Switch
    if (!CheckKillSwitch()) {
        return false;
    }

    // 3. 检查版本兼容性锁
    if (!CheckVersionLock()) {
        return false;
    }

    m_lastStatus = SafetyStatus::Safe;
    return true;
}

const char* SafetyGuard::GetStatusString(SafetyStatus status) const
{
    switch (status) {
    case SafetyStatus::Safe: return "Safe (正常介入)";
    case SafetyStatus::KilledByConfig: return "Killed by config (Enable=false)";
    case SafetyStatus::KilledByEvent: return "Killed by event (Local\\hdrfix_kill or Manual)";
    case SafetyStatus::KilledByEnv: return "Killed by environment (HDRFIX_DISABLE=1)";
    case SafetyStatus::CrashMarkerDetected: return "Crash Marker detected (安全降级旁路模式)";
    case SafetyStatus::VersionRejected: return "Version rejected (未知客户端版本拒绝介入)";
    default: return "Unknown";
    }
}

} // namespace hdrfix
