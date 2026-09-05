// dllmain.cpp — P8 交付核心插件模块 (hdrfix.dll)
//
// 负责在注入或加载到宿主进程（HeyboxChat.exe / VolcEngineRTC.dll）后自动执行：
//   1. SafetyGuard 初始化与自检（版本锁、Crash Marker、Kill Switch 状态）；
//   2. 读取与应用 config/hdrfix.ini；
//   3. 挂载 WgcHookManager 拦截 RoGetActivationFactory；
//   4. 进程卸载时安全回退与清理。

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "CaptureProbe/hdr_state.h"
#include "Diagnostics/ConfigManager.h"
#include "Diagnostics/SafetyGuard.h"
#include "Integration/WgcHookManager.h"

using namespace hdrfix;

static HMODULE g_module = nullptr;

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    (void)lpReserved;
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        g_module = hModule;
        ::DisableThreadLibraryCalls(hModule);

        // 1. 初始化安全防护网 (Crash Marker, Kill Switch)
        SafetyGuard::Instance().Initialize();

        // 2. 加载配置文件 (优先加载同目录下的 hdrfix.ini)
        {
            wchar_t dllPath[MAX_PATH]{};
            ::GetModuleFileNameW(hModule, dllPath, MAX_PATH);
            std::wstring dir(dllPath);
            size_t pos = dir.find_last_of(L"\\/");
            if (pos != std::wstring::npos) {
                dir = dir.substr(0, pos + 1);
            }
            ConfigManager::Instance().Load(dir + L"hdrfix.ini");
        }

        // 3. 守门检查：通过后才安装 Hook
        if (SafetyGuard::Instance().CanIntercept()) {
            WgcHookManager::Instance().Install();
        }
        break;

    case DLL_PROCESS_DETACH:
        // 安全卸载 Hook 并清理资源与 Marker
        WgcHookManager::Instance().Remove();
        SafetyGuard::Instance().Shutdown();
        break;

    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
        break;
    }
    return TRUE;
}
