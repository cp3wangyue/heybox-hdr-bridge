// dllmain.cpp — HEYBOX HDR Bridge 核心插件入口
//
// 原则：DllMain 中只做最小工作，避免在 Loader Lock 下读取配置、初始化复杂对象或安装 Hook。
// 真正初始化由后台线程完成；进程正常卸载时只做轻量清理。

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string>

#include "Diagnostics/ConfigManager.h"
#include "Diagnostics/SafetyGuard.h"
#include "Integration/WgcHookManager.h"

using namespace hdrfix;

namespace {

HMODULE g_module = nullptr;
HANDLE g_initThread = nullptr;

DWORD WINAPI InitializeBridge(LPVOID param)
{
    HMODULE module = static_cast<HMODULE>(param);

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
    ConfigManager::Instance().Load(dir + L"hdrfix.ini");

    // 2. 初始化安全防护与 Crash Marker。
    SafetyGuard::Instance().Initialize();

    // 3. 守门通过后再安装 WGC Hook。
    if (SafetyGuard::Instance().CanIntercept()) {
        WgcHookManager::Instance().Install();
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

        // 仅创建初始化线程并立即返回；不在 Loader Lock 下执行 Hook/配置/COM 逻辑。
        g_initThread = ::CreateThread(nullptr, 0, InitializeBridge, hModule, 0, nullptr);
        if (g_initThread) {
            ::CloseHandle(g_initThread);
            g_initThread = nullptr;
        }
        break;

    case DLL_PROCESS_DETACH:
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
