#pragma once
// CaptureProbe/iat_hook.h — 最小 IAT Hook（只改目标模块自己的导入表）

#include <windows.h>
#include <string>

namespace hdrfix {

// 把 targetModule 导入表里 importDll!funcName 的条目替换为 hookFn，原值写入 *originalFnOut。
// 成功返回 true；模块未导入该函数时返回 false（不视为错误）。
bool InstallIATHook(HMODULE targetModule, const char* importDll, const char* funcName,
                    void* hookFn, void** originalFnOut);

// 恢复原始条目（须传 InstallIATHook 保存的 originalFn）
bool RemoveIATHook(HMODULE targetModule, const char* importDll, const char* funcName,
                   void* originalFn);

} // namespace hdrfix
