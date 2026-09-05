#pragma once
// Integration/WgcHookManager.h — WGC 帧池拦截管理器（计划书 §9.1 / §9.2）
//
// 负责拦截 RoGetActivationFactory 并将 Direct3D11CaptureFramePool 工厂重定向至 ProxyFramePoolStatics

#include <windows.h>
#include <roapi.h>
#include <atomic>
#include <string>

namespace hdrfix {

class WgcHookManager {
public:
    static WgcHookManager& Instance();

    bool Install();
    bool Remove();
    bool IsInstalled() const { return m_installed.load(); }

    void SetBypass(bool bypass) { m_bypass.store(bypass); }
    bool IsBypassed() const { return m_bypass.load(); }

    UINT64 GetInterceptedPoolCount() const { return m_interceptedPools.load(); }
    void IncrementInterceptedPools() { m_interceptedPools++; }

private:
    WgcHookManager() = default;
    ~WgcHookManager();

    std::atomic<bool> m_installed{false};
    std::atomic<bool> m_bypass{false};
    std::atomic<UINT64> m_interceptedPools{0};
};

} // namespace hdrfix
