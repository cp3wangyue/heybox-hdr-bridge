// dllmain.cpp — HEYBOX HDR Bridge 核心插件入口
//
// 原则：DllMain 中只做最小工作，避免在 Loader Lock 下读取配置、初始化复杂对象或安装 Hook。
// 真正初始化由后台线程完成；进程正常卸载时只做轻量清理。

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string>

#include "Diagnostics/BridgeLog.h"
#include "Diagnostics/ConfigManager.h"
#include "Diagnostics/SafetyGuard.h"
#include "Integration/WgcHookManager.h"

using namespace hdrfix;

namespace {

HMODULE g_module = nullptr;
HANDLE g_initThread = nullptr;

DWORD WINAPI InitializeBridge(LPVOID param)
{
    ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    HMODULE module = static_cast<HMODULE>(param);

    BRIDGE_LOG("Bridge", "=== InitializeBridge started in PID=%lu ===", ::GetCurrentProcessId());

    // 1. 先读取 DLL 同目录配置。
    wchar_t dllPath[MAX_PATH]{};
    ::GetModuleFileNameW(module, dllPath, MAX_PATH);
    std::wstring dir(dllPath);
    const size_t pos = dir.find_last_of(L"\\/");
    if (pos != std::wstring::npos) {
        dir.resize(pos + 1);
    } else {
        dir.clear();
    }
    std::wstring iniPath = dir + L"hdrfix.ini";
    bool cfgLoaded = ConfigManager::Instance().Load(iniPath);
    BRIDGE_LOGW("Bridge", L"ConfigManager::Load(%ls) -> %s", iniPath.c_str(), cfgLoaded ? L"OK" : L"FAIL (using defaults)");

    // 2. 初始化安全防护与 Crash Marker。
    SafetyGuard::Instance().Initialize();

    // 3. 守门通过后再安装 WGC Hook。
    bool canIntercept = SafetyGuard::Instance().CanIntercept();
    SafetyStatus status = SafetyGuard::Instance().GetLastStatus();
    BRIDGE_LOG("Bridge", "SafetyGuard::CanIntercept() -> %s (status=%s)",
               canIntercept ? "TRUE" : "FALSE", SafetyGuard::Instance().GetStatusString(status));

    if (canIntercept) {
        bool hookOk = WgcHookManager::Instance().Install();
        BRIDGE_LOG("Bridge", "WgcHookManager::Install() -> %s", hookOk ? "SUCCESS" : "FAILED");
    } else {
        BRIDGE_LOG("Bridge", "WgcHookManager NOT installed because CanIntercept is FALSE");
    }

    return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        g_module = hModule;
        ::DisableThreadLibraryCalls(hModule);
        BRIDGE_LOG("DllMain", "DLL_PROCESS_ATTACH in PID=%lu", ::GetCurrentProcessId());

        // 仅创建初始化线程并立即返回；不在 Loader Lock 下执行 Hook/配置/COM 逻辑。
        g_initThread = ::CreateThread(nullptr, 0, InitializeBridge, hModule, 0, nullptr);
        if (g_initThread) {
            ::CloseHandle(g_initThread);
            g_initThread = nullptr;
        }
        break;

    case DLL_PROCESS_DETACH:
        BRIDGE_LOG("DllMain", "DLL_PROCESS_DETACH in PID=%lu (reserved=%p)", ::GetCurrentProcessId(), reserved);
        // 进程终止时 reserved != nullptr，不做复杂清理，交给 OS 回收。
        if (reserved == nullptr) {
            WgcHookManager::Instance().Remove();
            SafetyGuard::Instance().Shutdown();
        }
        g_module = nullptr;
        break;

    default:
        break;
    }

    return TRUE;
}
