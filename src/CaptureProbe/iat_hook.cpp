// CaptureProbe/iat_hook.cpp

#include "CaptureProbe/iat_hook.h"

namespace hdrfix {

namespace {

PIMAGE_IMPORT_DESCRIPTOR FindImportDescriptor(HMODULE module, const char* dllNameLower)
{
    auto base = reinterpret_cast<BYTE*>(module);
    auto dos = reinterpret_cast<PIMAGE_DOS_HEADER>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    auto nt = reinterpret_cast<PIMAGE_NT_HEADERS>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;

    auto dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress == 0) return nullptr;
    auto desc = reinterpret_cast<PIMAGE_IMPORT_DESCRIPTOR>(base + dir.VirtualAddress);
    for (; desc->Name != 0; ++desc) {
        auto name = reinterpret_cast<const char*>(base + desc->Name);
        if (_stricmp(name, dllNameLower) == 0) return desc;
    }
    return nullptr;
}

template <typename Fn>
bool PatchThunk(BYTE* base, PIMAGE_IMPORT_DESCRIPTOR desc, const char* funcName, Fn patch)
{
    auto lookup = reinterpret_cast<PIMAGE_THUNK_DATA>(base + desc->OriginalFirstThunk);
    auto iat = reinterpret_cast<PIMAGE_THUNK_DATA>(base + desc->FirstThunk);
    for (; lookup->u1.AddressOfData != 0; ++lookup, ++iat) {
        if (lookup->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
        auto name = reinterpret_cast<PIMAGE_IMPORT_BY_NAME>(base + lookup->u1.AddressOfData);
        if (strcmp(name->Name, funcName) != 0) continue;
        // x64 上 IMAGE_THUNK_DATA::u1.Function 是 ULONGLONG，与 void* 同宽
        return patch(reinterpret_cast<void**>(&iat->u1.Function));
    }
    return false;
}

bool ProtectAndWrite(void** slot, void* newValue, void** oldValueOut)
{
    DWORD oldProtect = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) return false;
    if (oldValueOut) *oldValueOut = *slot;
    *slot = newValue;
    DWORD tmp = 0;
    VirtualProtect(slot, sizeof(void*), oldProtect, &tmp);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
    return true;
}

} // namespace

bool InstallIATHook(HMODULE targetModule, const char* importDll, const char* funcName,
                    void* hookFn, void** originalFnOut)
{
    if (!targetModule) return false;
    auto base = reinterpret_cast<BYTE*>(targetModule);
    auto desc = FindImportDescriptor(targetModule, importDll);
    if (!desc) return false;
    struct Ctx { void* hook; void** orig; bool ok; } ctx{ hookFn, originalFnOut, false };
    return PatchThunk(base, desc, funcName, [&ctx](void** slot) {
        if (*slot == ctx.hook) { ctx.ok = true; return true; } // 已装过
        ctx.ok = ProtectAndWrite(slot, ctx.hook, ctx.orig);
        return ctx.ok;
    }) && ctx.ok;
}

bool RemoveIATHook(HMODULE targetModule, const char* importDll, const char* funcName,
                   void* originalFn)
{
    if (!targetModule || !originalFn) return false;
    auto base = reinterpret_cast<BYTE*>(targetModule);
    auto desc = FindImportDescriptor(targetModule, importDll);
    if (!desc) return false;
    return PatchThunk(base, desc, funcName, [originalFn](void** slot) {
        if (*slot == originalFn) return true;
        return ProtectAndWrite(slot, originalFn, nullptr);
    });
}

} // namespace hdrfix
