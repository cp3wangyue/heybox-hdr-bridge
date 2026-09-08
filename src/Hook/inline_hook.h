#pragma once
// Hook/inline_hook.h — x64 Inline Hook 封装（基于工业级成熟 MinHook 引擎）
//
// 保证：
//   1. 完整解码与重定位 x64 指令（包括 RIP-relative、call rel32、jmp rel32、jcc rel8/32 等）；
//   2. 线程安全的挂载与恢复；
//   3. 安装失败 Fail-open；
//   4. 支持随时热卸载 (Remove)。

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

    // 安装 Hook：将 targetFunction 入口重定向至 hookFunction，并通过 Trampoline 保存原函数调用能力
    bool Install(void* targetFunction, void* hookFunction);

    // 卸载 Hook：恢复原函数头部指令与线程状态
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
    bool m_installed = false;
};

} // namespace hdrfix
