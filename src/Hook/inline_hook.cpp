// Hook/inline_hook.cpp — x64 Inline Hook 实现 (基于成熟工业级 MinHook 引擎)
#include "Hook/inline_hook.h"
#include "MinHook.h"

#include <mutex>
#include <atomic>

namespace hdrfix {

namespace {

std::mutex g_minhookMutex;
int g_activeHookCount = 0;
bool g_minhookInitialized = false;

bool EnsureMinHookInit()
{
    std::lock_guard<std::mutex> lock(g_minhookMutex);
    if (!g_minhookInitialized) {
        MH_STATUS status = MH_Initialize();
        if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
            return false;
        }
        g_minhookInitialized = true;
    }
    return true;
}

void ReleaseMinHookHook()
{
    std::lock_guard<std::mutex> lock(g_minhookMutex);
    if (g_activeHookCount > 0) {
        --g_activeHookCount;
    }
}

void AddMinHookHook()
{
    std::lock_guard<std::mutex> lock(g_minhookMutex);
    ++g_activeHookCount;
}

} // namespace

InlineHook::~InlineHook()
{
    Remove();
}

bool InlineHook::Install(void* targetFunction, void* hookFunction)
{
    if (!targetFunction || !hookFunction) return false;
    if (m_installed) return false;

    if (!EnsureMinHookInit()) {
        return false;
    }

    void* original = nullptr;
    MH_STATUS status = MH_CreateHook(targetFunction, hookFunction, &original);
    if (status != MH_OK) {
        return false;
    }

    status = MH_EnableHook(targetFunction);
    if (status != MH_OK) {
        MH_RemoveHook(targetFunction);
        return false;
    }

    m_target = targetFunction;
    m_trampoline = original;
    m_installed = true;
    AddMinHookHook();
    return true;
}

bool InlineHook::Remove()
{
    if (!m_installed || !m_target) return false;

    MH_STATUS disableStatus = MH_DisableHook(m_target);
    MH_STATUS removeStatus = MH_RemoveHook(m_target);

    m_target = nullptr;
    m_trampoline = nullptr;
    m_installed = false;
    ReleaseMinHookHook();

    return (disableStatus == MH_OK || disableStatus == MH_ERROR_DISABLED) &&
           (removeStatus == MH_OK || removeStatus == MH_ERROR_NOT_CREATED);
}

} // namespace hdrfix
