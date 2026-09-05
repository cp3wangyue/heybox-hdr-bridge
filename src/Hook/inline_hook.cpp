// Hook/inline_hook.cpp — 轻量级 x64 Inline Hook 实现
#include "Hook/inline_hook.h"

#include <cstring>
#include <vector>

namespace hdrfix {

namespace {

// 微型 x64 指令长度解码器（覆盖 MSVC/系统 DLL 函数序言常见指令）
size_t DecodeInstructionLength(const uint8_t* p)
{
    size_t len = 0;
    bool hasRex = false;
    uint8_t rex = 0;

    // 1. 跳过前缀与 REX 前缀
    while (true) {
        uint8_t b = p[len];
        if (b == 0x66 || b == 0x67 || b == 0xF0 || b == 0xF2 || b == 0xF3 ||
            b == 0x2E || b == 0x36 || b == 0x3E || b == 0x26 || b == 0x64 || b == 0x65) {
            len++;
        } else if ((b & 0xF0) == 0x40) { // REX 前缀
            hasRex = true;
            rex = b;
            len++;
        } else {
            break;
        }
    }

    uint8_t op = p[len++];
    bool hasModRm = false;
    size_t immSize = 0;

    // 2. 判断两字节 Opcode (0x0F ...)
    if (op == 0x0F) {
        uint8_t op2 = p[len++];
        if (op2 >= 0x80 && op2 <= 0x8F) { // jcc rel32
            return len + 4;
        }
        if (op2 == 0x1F) { // nop with modrm
            hasModRm = true;
        } else if (op2 >= 0x90 && op2 <= 0x9F) { // setcc
            hasModRm = true;
        } else if (op2 == 0xB6 || op2 == 0xB7 || op2 == 0xBE || op2 == 0xBF) { // movzx / movsx
            hasModRm = true;
        } else {
            hasModRm = true;
        }
    } else {
        // 单字节 Opcode
        if ((op >= 0x50 && op <= 0x5F) || op == 0x90) { // push/pop reg, nop
            return len;
        }
        if (op == 0x68) return len + 4; // push imm32
        if (op == 0x6A) return len + 1; // push imm8
        if (op == 0xE8 || op == 0xE9) return len + 4; // call/jmp rel32
        if (op == 0xEB) return len + 1; // jmp rel8
        if (op >= 0x70 && op <= 0x7F) return len + 1; // jcc rel8
        if (op == 0xC3 || op == 0xCB) return len; // ret

        if ((op & 0xF8) == 0xB8) { // mov reg, imm
            return len + ((hasRex && (rex & 0x08)) ? 8 : 4);
        }
        if ((op & 0xF8) == 0xB0) return len + 1; // mov reg8, imm8

        if (op == 0x81) { hasModRm = true; immSize = 4; }
        else if (op == 0x83) { hasModRm = true; immSize = 1; }
        else if (op == 0xC7) { hasModRm = true; immSize = 4; }
        else if (op == 0xC6) { hasModRm = true; immSize = 1; }
        else if (op >= 0x88 && op <= 0x8C) { hasModRm = true; }
        else if (op == 0x8D) { hasModRm = true; } // lea
        else if (op == 0x8F) { hasModRm = true; } // pop rm
        else if ((op >= 0x00 && op <= 0x03) || (op >= 0x08 && op <= 0x0B) ||
                 (op >= 0x10 && op <= 0x13) || (op >= 0x18 && op <= 0x1B) ||
                 (op >= 0x20 && op <= 0x23) || (op >= 0x28 && op <= 0x2B) ||
                 (op >= 0x30 && op <= 0x33) || (op >= 0x38 && op <= 0x3B) ||
                 (op >= 0x84 && op <= 0x85)) { // test/add/sub/cmp/xor...
            hasModRm = true;
        } else if (op == 0xFF) {
            hasModRm = true; // inc/dec/call/jmp rm
        } else {
            // 保守默认
            hasModRm = true;
        }
    }

    if (hasModRm) {
        uint8_t modrm = p[len++];
        uint8_t mod = (modrm >> 6) & 3;
        uint8_t rm = modrm & 7;

        if (mod != 3 && rm == 4) { // SIB 字节
            uint8_t sib = p[len++];
            if (mod == 0 && (sib & 7) == 5) {
                len += 4; // disp32
            }
        }

        if (mod == 1) {
            len += 1; // disp8
        } else if (mod == 2) {
            len += 4; // disp32
        } else if (mod == 0 && rm == 5) {
            len += 4; // RIP + disp32
        }
    }

    return len + immSize;
}

// 获取至少 minBytes 字节的整条指令总长
size_t GetStolenLength(const uint8_t* code, size_t minBytes)
{
    size_t total = 0;
    while (total < minBytes) {
        size_t l = DecodeInstructionLength(code + total);
        if (l == 0 || l > 15) return 0; // 解码异常防死循环
        total += l;
    }
    return total;
}

// 写入 14 字节 x64 绝对跳转: jmp [rip+0]; dq TargetAddress
void WriteAbsoluteJmp(void* location, void* targetAddress)
{
    uint8_t* p = reinterpret_cast<uint8_t*>(location);
    p[0] = 0xFF;
    p[1] = 0x25;
    p[2] = 0x00;
    p[3] = 0x00;
    p[4] = 0x00;
    p[5] = 0x00;
    uint64_t addr = reinterpret_cast<uint64_t>(targetAddress);
    memcpy(p + 6, &addr, sizeof(addr));
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

    m_target = targetFunction;

    // 1. 计算覆盖指令长度（至少 14 字节）
    constexpr size_t kJmpSize = 14;
    m_stolenBytes = GetStolenLength(reinterpret_cast<const uint8_t*>(m_target), kJmpSize);
    if (m_stolenBytes < kJmpSize || m_stolenBytes > sizeof(m_originalBytes)) {
        return false;
    }

    // 2. 备份原指令
    memcpy(m_originalBytes, m_target, m_stolenBytes);

    // 3. 分配 Trampoline 执行页 (Stolen Bytes + 14 字节跳回指令)
    size_t trampSize = m_stolenBytes + kJmpSize;
    m_trampoline = ::VirtualAlloc(nullptr, trampSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!m_trampoline) return false;

    // 写入原指令并附加跳回原函数的剩余位置 (target + m_stolenBytes)
    memcpy(m_trampoline, m_originalBytes, m_stolenBytes);
    void* returnAddress = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(m_target) + m_stolenBytes);
    WriteAbsoluteJmp(reinterpret_cast<uint8_t*>(m_trampoline) + m_stolenBytes, returnAddress);

    // 4. 修改目标函数属性并写入跳转至 Hook
    DWORD oldProtect = 0;
    if (!::VirtualProtect(m_target, m_stolenBytes, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        ::VirtualFree(m_trampoline, 0, MEM_RELEASE);
        m_trampoline = nullptr;
        return false;
    }

    WriteAbsoluteJmp(m_target, hookFunction);
    // 用 NOP 填充剩余被挪用的字节
    for (size_t i = kJmpSize; i < m_stolenBytes; ++i) {
        reinterpret_cast<uint8_t*>(m_target)[i] = 0x90;
    }

    DWORD tmpProtect = 0;
    ::VirtualProtect(m_target, m_stolenBytes, oldProtect, &tmpProtect);
    ::FlushInstructionCache(::GetCurrentProcess(), m_target, m_stolenBytes);
    ::FlushInstructionCache(::GetCurrentProcess(), m_trampoline, trampSize);

    m_installed = true;
    return true;
}

bool InlineHook::Remove()
{
    if (!m_installed || !m_target) return false;

    DWORD oldProtect = 0;
    if (::VirtualProtect(m_target, m_stolenBytes, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        memcpy(m_target, m_originalBytes, m_stolenBytes);
        DWORD tmpProtect = 0;
        ::VirtualProtect(m_target, m_stolenBytes, oldProtect, &tmpProtect);
        ::FlushInstructionCache(::GetCurrentProcess(), m_target, m_stolenBytes);
    }

    if (m_trampoline) {
        ::VirtualFree(m_trampoline, 0, MEM_RELEASE);
        m_trampoline = nullptr;
    }

    m_installed = false;
    return true;
}

} // namespace hdrfix
