#pragma once
// Hook/inline_hook.h — 轻量级 x64 Inline Hook 与 Trampoline 管理（计划书 §2.3 最小侵入）

#include <windows.h>
#include <cstddef>
#include <cstdint>

namespace hdrfix {

class InlineHook {
public:
    InlineHook() = default;
    ~InlineHook();

    InlineHook(const InlineHook&) = delete;
    InlineHook& operator=(const InlineHook&) = delete;

    // 安装 Hook：将 targetFunction 的入口重定向至 hookFunction，并通过 Trampoline 保存原函数调用能力
    bool Install(void* targetFunction, void* hookFunction);

    // 卸载 Hook：恢复原函数头部指令
    bool Remove();

    bool IsInstalled() const { return m_installed; }

    // 获取可直接调用的原始函数 Trampoline 指针
    template <typename Fn>
    Fn GetOriginal() const {
        return reinterpret_cast<Fn>(m_trampoline);
    }

private:
    void* m_target = nullptr;
    void* m_trampoline = nullptr;
    uint8_t m_originalBytes[32]{};
    size_t m_stolenBytes = 0;
    bool m_installed = false;
};

} // namespace hdrfix
