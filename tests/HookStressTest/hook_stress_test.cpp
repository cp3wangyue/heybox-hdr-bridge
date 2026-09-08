// tests/HookStressTest/hook_stress_test.cpp
// Hook 30 次启动/退出循环压力测试

#include <windows.h>
#include <roapi.h>
#include <winstring.h>
#include <cstdio>
#include <cstdint>
#include <atomic>

#include "Hook/inline_hook.h"

#pragma comment(lib, "onecore.lib")

namespace {

using RoGetActivationFactoryFn = HRESULT(WINAPI*)(HSTRING, REFIID, void**);

std::atomic<int> g_hookCallCount{0};
RoGetActivationFactoryFn g_originalRoGetActivationFactory = nullptr;

HRESULT WINAPI TestHook_RoGetActivationFactory(HSTRING activatableClassId, REFIID iid, void** factory)
{
    g_hookCallCount++;
    if (g_originalRoGetActivationFactory) {
        return g_originalRoGetActivationFactory(activatableClassId, iid, factory);
    }
    return E_UNEXPECTED;
}

} // namespace

int main()
{
    printf("====================================================\n");
    printf("  HEYBOX HDR Bridge — Hook 30次挂载/卸载压力测试\n");
    printf("====================================================\n\n");

    ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    HMODULE hCombase = ::GetModuleHandleW(L"combase.dll");
    if (!hCombase) {
        hCombase = ::LoadLibraryW(L"combase.dll");
    }
    if (!hCombase) {
        printf("[FATAL] 无法加载 combase.dll\n");
        return 1;
    }

    void* targetProc = reinterpret_cast<void*>(::GetProcAddress(hCombase, "RoGetActivationFactory"));
    if (!targetProc) {
        printf("[FATAL] 未找到 combase.dll!RoGetActivationFactory 符号\n");
        return 1;
    }

    printf("[Target] combase.dll!RoGetActivationFactory 地址: %p\n", targetProc);

    HSTRING testHString = nullptr;
    const wchar_t testClassName[] = L"Windows.Foundation.Uri";
    HRESULT hr = ::WindowsCreateString(testClassName, static_cast<UINT32>(wcslen(testClassName)), &testHString);
    if (FAILED(hr)) {
        printf("[FATAL] WindowsCreateString 失败 (hr=0x%08lX)\n", hr);
        return 1;
    }

    constexpr int kTotalCycles = 35;
    int successCycles = 0;
    int failedCycles = 0;

    printf("[测试] 开始执行 %d 次 Hook 挂载 -> 调用拦截 -> 卸载恢复 循环...\n", kTotalCycles);

    for (int i = 1; i <= kTotalCycles; ++i) {
        hdrfix::InlineHook hook;
        g_hookCallCount = 0;
        g_originalRoGetActivationFactory = nullptr;

        // 1. Install Hook
        bool installOk = hook.Install(targetProc, reinterpret_cast<void*>(TestHook_RoGetActivationFactory));
        if (!installOk || !hook.IsInstalled()) {
            printf("  [Cycle %2d/%d] 失败: Hook Install 失败！\n", i, kTotalCycles);
            failedCycles++;
            continue;
        }

        g_originalRoGetActivationFactory = hook.GetOriginal<RoGetActivationFactoryFn>();
        if (!g_originalRoGetActivationFactory) {
            printf("  [Cycle %2d/%d] 失败: Trampoline 指针为空！\n", i, kTotalCycles);
            hook.Remove();
            failedCycles++;
            continue;
        }

        // 2. 调用目标函数（触发 Hook）
        auto callTarget = reinterpret_cast<RoGetActivationFactoryFn>(targetProc);
        void* pFactory = nullptr;
        hr = callTarget(testHString, __uuidof(IInspectable), &pFactory);
        if (pFactory) {
            reinterpret_cast<IUnknown*>(pFactory)->Release();
            pFactory = nullptr;
        }

        if (g_hookCallCount.load() != 1) {
            printf("  [Cycle %2d/%d] 失败: Hook 未被命中 (count=%d)\n", i, kTotalCycles, g_hookCallCount.load());
            hook.Remove();
            failedCycles++;
            continue;
        }

        // 3. Remove Hook
        bool removeOk = hook.Remove();
        if (!removeOk || hook.IsInstalled()) {
            printf("  [Cycle %2d/%d] 失败: Hook Remove 失败！\n", i, kTotalCycles);
            failedCycles++;
            continue;
        }

        // 4. 再次调用目标函数（验证已卸载，不再命中 Hook 函数）
        g_hookCallCount = 0;
        pFactory = nullptr;
        hr = callTarget(testHString, __uuidof(IInspectable), &pFactory);
        if (pFactory) {
            reinterpret_cast<IUnknown*>(pFactory)->Release();
            pFactory = nullptr;
        }

        if (g_hookCallCount.load() != 0) {
            printf("  [Cycle %2d/%d] 失败: Remove 后依然被 Hook 拦截！\n", i, kTotalCycles);
            failedCycles++;
            continue;
        }

        successCycles++;
        if (i % 5 == 0 || i == kTotalCycles) {
            printf("  - 已完成 %2d/%d 轮: Install [OK] -> Intercept [OK] -> Trampoline [OK] -> Remove [OK] -> Restore [OK]\n",
                   i, kTotalCycles);
        }
    }

    ::WindowsDeleteString(testHString);
    ::CoUninitialize();

    printf("\n----------------------------------------------------\n");
    printf("  Hook 压力测试结果: 总轮数 %d, 成功 %d, 失败 %d\n", kTotalCycles, successCycles, failedCycles);
    if (failedCycles == 0) {
        printf("  [PASS] 质量指标达成：MinHook 30+ 轮挂载/卸载无故障、原函数正常恢复！\n");
    } else {
        printf("  [FAIL] 存在失败轮次！\n");
    }
    printf("----------------------------------------------------\n\n");

    return (failedCycles == 0) ? 0 : 1;
}
