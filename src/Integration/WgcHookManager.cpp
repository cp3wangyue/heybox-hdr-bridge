// Integration/WgcHookManager.cpp — WGC 帧池拦截管理器实现
#include "Integration/WgcHookManager.h"

#include <winstring.h>
#include <cstdio>
#include <mutex>

#include "Hook/inline_hook.h"
#include "Integration/FramePoolProxy.h"
#include "Diagnostics/SafetyGuard.h"
#include "Diagnostics/BridgeLog.h"

#pragma comment(lib, "onecore.lib")

using namespace ABI::Windows::Graphics::Capture;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Make;

namespace hdrfix {

namespace {

using RoGetActivationFactoryFn = HRESULT(WINAPI*)(HSTRING, REFIID, void**);
static InlineHook g_roHook;
static RoGetActivationFactoryFn g_origRoGetActivationFactory = nullptr;
static std::mutex g_managerMutex;

HRESULT WINAPI Hooked_RoGetActivationFactory(HSTRING activatableClassId, REFIID iid, void** factory)
{
    if (!g_origRoGetActivationFactory) {
        BRIDGE_LOG("WgcHook", "Hooked_RoGetActivationFactory called with nullptr g_origRoGetActivationFactory!");
        return E_UNEXPECTED;
    }

    static thread_local bool t_inHook = false;
    if (t_inHook) {
        return g_origRoGetActivationFactory(activatableClassId, iid, factory);
    }
    struct ReentrancyGuard {
        bool& flag;
        ReentrancyGuard(bool& f) : flag(f) { flag = true; }
        ~ReentrancyGuard() { flag = false; }
    } guard(t_inHook);

    // 先调用原系统工厂
    HRESULT hr = g_origRoGetActivationFactory(activatableClassId, iid, factory);
    if (FAILED(hr) || !factory || !*factory) {
        return hr;
    }

    UINT32 len = 0;
    PCWSTR rawStr = WindowsGetStringRawBuffer(activatableClassId, &len);
    if (!rawStr) return hr;

    // 检查是否为目标类: Windows.Graphics.Capture.Direct3D11CaptureFramePool
    if (wcscmp(rawStr, L"Windows.Graphics.Capture.Direct3D11CaptureFramePool") == 0) {
        BRIDGE_LOG("WgcHook", "Target class detected: Windows.Graphics.Capture.Direct3D11CaptureFramePool");

        // 检查 SafetyGuard 与手动 Bypass
        if (!SafetyGuard::Instance().CanIntercept() || WgcHookManager::Instance().IsBypassed()) {
            BRIDGE_LOG("WgcHook", "SafetyGuard CanIntercept=false or Bypassed -> Passthrough native factory");
            return hr; // 安全降级/Bypass 模式直接原生透传
        }

        WgcHookManager::Instance().IncrementInterceptedPools();

        ComPtr<IInspectable> originalInspectable;
        originalInspectable.Attach(reinterpret_cast<IInspectable*>(*factory));

        ComPtr<IDirect3D11CaptureFramePoolStatics> s1;
        ComPtr<IDirect3D11CaptureFramePoolStatics2> s2;

        originalInspectable.As(&s1);
        originalInspectable.As(&s2);

        if (s1 || s2) {
            auto proxyStatics = Make<ProxyFramePoolStatics>(s1, s2);
            *factory = nullptr;
            hr = proxyStatics.CopyTo(iid, factory);
            BRIDGE_LOG("WgcHook", "ProxyFramePoolStatics successfully installed into *factory (hr=0x%08lX)", hr);
            return hr;
        }

        BRIDGE_LOG("WgcHook", "Failed to query Statics/Statics2 from native factory! Detaching original.");
        // 无法转换则归还原指针
        *factory = originalInspectable.Detach();
    }

    return hr;
}

} // namespace

WgcHookManager& WgcHookManager::Instance()
{
    static WgcHookManager s_inst;
    return s_inst;
}

WgcHookManager::~WgcHookManager()
{
    Remove();
}

bool WgcHookManager::Install()
{
    std::lock_guard<std::mutex> lock(g_managerMutex);
    if (m_installed.load()) return true;

    HMODULE combase = ::GetModuleHandleW(L"combase.dll");
    if (!combase) {
        combase = ::LoadLibraryW(L"combase.dll");
    }
    if (!combase) return false;

    void* targetFn = reinterpret_cast<void*>(::GetProcAddress(combase, "RoGetActivationFactory"));
    if (!targetFn) return false;

    if (!g_roHook.Install(targetFn, reinterpret_cast<void*>(Hooked_RoGetActivationFactory))) {
        return false;
    }

    g_origRoGetActivationFactory = g_roHook.GetOriginal<RoGetActivationFactoryFn>();
    m_installed.store(true);
    return true;
}

bool WgcHookManager::Remove()
{
    std::lock_guard<std::mutex> lock(g_managerMutex);
    if (!m_installed.load()) return true;

    g_roHook.Remove();
    g_origRoGetActivationFactory = nullptr;
    m_installed.store(false);
    return true;
}

} // namespace hdrfix
