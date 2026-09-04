// injector — 本地研究加载器（计划书 §12.1：仅本地加载；不做持久化、不做隐藏）
//
// 用法:
//   injector.exe [--name HeyboxChat.exe] [--module VolcEngineRTC.dll] [--dll <路径>]   注入
//   injector.exe --stop                                                                停止探针（named event）
// 注入目标：所有加载了 --module 的 --name 进程；未找到时列出候选。

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

struct Options {
    std::wstring processName = L"HeyboxChat.exe";
    std::wstring moduleName = L"VolcEngineRTC.dll";
    std::wstring dllPath;
    bool stop = false;
    bool watch = false;   // 持续轮询：对所有 name 匹配且未注入的进程注入（追赶新进程）
    int watchSeconds = 3600;
};

DWORD FindPidWithModule(const std::wstring& processName, const std::wstring& moduleName,
                        std::vector<DWORD>* candidatesWithoutModule)
{
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    DWORD found = 0;
    for (BOOL ok = ::Process32FirstW(snap, &pe); ok; ok = ::Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, processName.c_str()) != 0) continue;
        HANDLE msnap = ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pe.th32ProcessID);
        if (msnap == INVALID_HANDLE_VALUE) {
            if (candidatesWithoutModule) candidatesWithoutModule->push_back(pe.th32ProcessID);
            continue;
        }
        MODULEENTRY32W me{};
        me.dwSize = sizeof(me);
        bool has = false;
        for (BOOL mok = ::Module32FirstW(msnap, &me); mok; mok = ::Module32NextW(msnap, &me)) {
            if (_wcsicmp(me.szModule, moduleName.c_str()) == 0) { has = true; break; }
        }
        ::CloseHandle(msnap);
        if (has) { found = pe.th32ProcessID; break; }
        if (candidatesWithoutModule) candidatesWithoutModule->push_back(pe.th32ProcessID);
    }
    ::CloseHandle(snap);
    return found;
}

bool IsProbeLoaded(DWORD pid)
{
    HANDLE msnap = ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if (msnap == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    bool has = false;
    for (BOOL ok = ::Module32FirstW(msnap, &me); ok; ok = ::Module32NextW(msnap, &me)) {
        if (_wcsicmp(me.szModule, L"hdrfix_probe.dll") == 0 ||
            _wcsicmp(me.szModule, L"hdrfix_probe2.dll") == 0) { has = true; break; }
    }
    ::CloseHandle(msnap);
    return has;
}

// watch 模式：轮询所有 name 匹配且未注入的进程并注入（每 pid 只注入一次）
bool InjectDll(DWORD pid, const std::wstring& dllPath);

int RunWatch(const Options& opt)
{
    std::vector<DWORD> injected;
    auto already = [&](DWORD pid) {
        for (DWORD p : injected) if (p == pid) return true;
        return false;
    };
    auto deadline = ::GetTickCount64() + static_cast<ULONGLONG>(opt.watchSeconds) * 1000;
    wprintf(L"watching '%s' processes, injecting probe (max %ds)...\n", opt.processName.c_str(),
            opt.watchSeconds);
    while (::GetTickCount64() < deadline) {
        HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe{};
            pe.dwSize = sizeof(pe);
            for (BOOL ok = ::Process32FirstW(snap, &pe); ok; ok = ::Process32NextW(snap, &pe)) {
                if (_wcsicmp(pe.szExeFile, opt.processName.c_str()) != 0) continue;
                DWORD pid = pe.th32ProcessID;
                if (already(pid) || IsProbeLoaded(pid)) continue;
                // 稳定性：跳过刚创建 <1s 的进程（模块快照可能不可用）
                Sleep(0);
                wprintf(L"[watch] injecting pid=%lu\n", pid);
                if (InjectDll(pid, opt.dllPath)) {
                    injected.push_back(pid);
                } else {
                    wprintf(L"[watch] inject pid=%lu failed (will retry)\n", pid);
                }
            }
            ::CloseHandle(snap);
        }
        ::Sleep(300);
    }
    wprintf(L"watch done, injected %zu processes\n", injected.size());
    return 0;
}

bool InjectDll(DWORD pid, const std::wstring& dllPath)
{
    HANDLE proc = ::OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
            PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE, pid);
    if (!proc) {
        wprintf(L"OpenProcess(%lu) failed gle=%lu\n", pid, ::GetLastError());
        return false;
    }
    SIZE_T bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remote = ::VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) {
        wprintf(L"VirtualAllocEx failed gle=%lu\n", ::GetLastError());
        ::CloseHandle(proc);
        return false;
    }
    if (!::WriteProcessMemory(proc, remote, dllPath.c_str(), bytes, nullptr)) {
        wprintf(L"WriteProcessMemory failed gle=%lu\n", ::GetLastError());
        ::VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        ::CloseHandle(proc);
        return false;
    }
    auto loadLibraryW = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        ::GetProcAddress(::GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
    HANDLE thread = ::CreateRemoteThread(proc, nullptr, 0, loadLibraryW, remote, 0, nullptr);
    if (!thread) {
        wprintf(L"CreateRemoteThread failed gle=%lu\n", ::GetLastError());
        ::VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        ::CloseHandle(proc);
        return false;
    }
    ::WaitForSingleObject(thread, 10000);
    DWORD exitCode = 0;
    ::GetExitCodeThread(thread, &exitCode);
    ::CloseHandle(thread);
    ::VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    ::CloseHandle(proc);
    wprintf(L"LoadLibraryW thread exit=0x%08lX (module base on x64 截断显示)\n", exitCode);
    return exitCode != 0;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    Options opt;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        auto next = [&]() -> std::wstring { return (i + 1 < argc) ? argv[++i] : L""; };
        if (a == L"--name") opt.processName = next();
        else if (a == L"--module") opt.moduleName = next();
        else if (a == L"--dll") opt.dllPath = next();
        else if (a == L"--stop") opt.stop = true;
        else if (a == L"--watch") opt.watch = true;
        else if (a == L"--watch-seconds") opt.watchSeconds = _wtoi(next().c_str());
    }

    if (opt.stop) {
        // 对每个 name 匹配进程设置独立的停止事件（探针按 pid 命名事件）
        HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return 1;
        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);
        int n = 0;
        for (BOOL ok = ::Process32FirstW(snap, &pe); ok; ok = ::Process32NextW(snap, &pe)) {
            if (_wcsicmp(pe.szExeFile, opt.processName.c_str()) != 0) continue;
            wchar_t name[64];
            swprintf_s(name, L"Local\\hdrfix_probe_stop_%lu", pe.th32ProcessID);
            HANDLE ev = ::CreateEventW(nullptr, TRUE, FALSE, name);
            if (ev) {
                ::SetEvent(ev);
                ::CloseHandle(ev);
                ++n;
            }
        }
        ::CloseHandle(snap);
        wprintf(L"stop event set for %d processes (per-pid events)\n", n);
        return 0;
    }

    if (opt.dllPath.empty()) {
        wprintf(L"--dll <probe.dll 路径> required\n");
        return 1;
    }
    wchar_t full[MAX_PATH]{};
    if (!::GetFullPathNameW(opt.dllPath.c_str(), MAX_PATH, full, nullptr)) {
        wprintf(L"GetFullPathName failed\n");
        return 1;
    }

    if (opt.watch) {
        return RunWatch(opt);
    }

    std::vector<DWORD> candidates;
    DWORD pid = FindPidWithModule(opt.processName, opt.moduleName, &candidates);
    if (!pid) {
        wprintf(L"no %s process with %s loaded (client idle?). candidates:", opt.processName.c_str(),
                opt.moduleName.c_str());
        for (DWORD c : candidates) wprintf(L" %lu", c);
        wprintf(L"\n");
        return 1;
    }
    wprintf(L"target pid=%lu, injecting %s\n", pid, full);
    if (!InjectDll(pid, full)) return 1;

    // 确认模块已加载
    Sleep(500);
    HANDLE msnap = ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if (msnap != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W me{};
        me.dwSize = sizeof(me);
        for (BOOL ok = ::Module32FirstW(msnap, &me); ok; ok = ::Module32NextW(msnap, &me)) {
            if (_wcsicmp(me.szModule, L"hdrfix_probe.dll") == 0) {
                wprintf(L"probe loaded at base=0x%p size=%lu\n", me.modBaseAddr, me.modBaseSize);
                break;
            }
        }
        ::CloseHandle(msnap);
    }
    return 0;
}
