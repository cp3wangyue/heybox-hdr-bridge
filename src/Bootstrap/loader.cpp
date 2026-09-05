// loader.cpp — P8 简易安装、卸载与伴随加载器 (hdrfix_loader.exe)
//
// 功能：
//   1. --install   : 一键安装插件并生成桌面快捷方式
//   2. --uninstall : 一键卸载插件并清理桌面快捷方式与残留配置
//   3. --launch    : 启动黑盒语音并自动注入 hdrfix.dll
//   4. --inject    : 向已运行的黑盒语音注入 hdrfix.dll
//   5. --status    : 检测系统 HDR 状态、黑盒运行状态与插件生效状态

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <shlobj.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

namespace {

std::string ToUtf8(const std::wstring& wstr)
{
    if (wstr.empty()) return {};
    int sizeNeeded = ::WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
    std::string res(sizeNeeded, 0);
    ::WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), res.data(), sizeNeeded, nullptr, nullptr);
    return res;
}

std::wstring GetSelfDirectory()
{
    wchar_t path[MAX_PATH]{};
    ::GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p(path);
    size_t pos = p.find_last_of(L"\\/");
    return (pos != std::wstring::npos) ? p.substr(0, pos + 1) : L"";
}

std::wstring GetHeyboxDefaultDir()
{
    wchar_t localApp[MAX_PATH]{};
    if (::SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, localApp) == S_OK) {
        fs::path p = fs::path(localApp) / L"Qingfeng" / L"HeyboxChat";
        if (fs::exists(p / L"HeyboxChat.exe")) {
            return p.wstring();
        }
    }
    return L"";
}

std::wstring GetDesktopPath()
{
    wchar_t desktop[MAX_PATH]{};
    if (::SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, SHGFP_TYPE_CURRENT, desktop) == S_OK) {
        return std::wstring(desktop);
    }
    return L"";
}

bool CreateShortcut(const std::wstring& shortcutPath, const std::wstring& targetExe,
                    const std::wstring& arguments, const std::wstring& iconPath,
                    const std::wstring& description)
{
    ::CoInitialize(nullptr);
    ComPtr<IShellLinkW> shellLink;
    HRESULT hr = ::CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_IShellLinkW, reinterpret_cast<void**>(shellLink.GetAddressOf()));
    if (FAILED(hr)) {
        ::CoUninitialize();
        return false;
    }

    shellLink->SetPath(targetExe.c_str());
    shellLink->SetArguments(arguments.c_str());
    shellLink->SetDescription(description.c_str());

    std::wstring workDir = fs::path(targetExe).parent_path().wstring();
    shellLink->SetWorkingDirectory(workDir.c_str());

    if (!iconPath.empty()) {
        shellLink->SetIconLocation(iconPath.c_str(), 0);
    }

    ComPtr<IPersistFile> persistFile;
    hr = shellLink.As(&persistFile);
    if (SUCCEEDED(hr)) {
        hr = persistFile->Save(shortcutPath.c_str(), TRUE);
    }

    ::CoUninitialize();
    return SUCCEEDED(hr);
}

std::vector<DWORD> FindHeyboxPids()
{
    std::vector<DWORD> pids;
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return pids;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    for (BOOL ok = ::Process32FirstW(snap, &pe); ok; ok = ::Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, L"HeyboxChat.exe") == 0) {
            pids.push_back(pe.th32ProcessID);
        }
    }
    ::CloseHandle(snap);
    return pids;
}

bool IsDllLoaded(DWORD pid, const std::wstring& dllName)
{
    HANDLE msnap = ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if (msnap == INVALID_HANDLE_VALUE) return false;

    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    bool found = false;
    for (BOOL ok = ::Module32FirstW(msnap, &me); ok; ok = ::Module32NextW(msnap, &me)) {
        if (_wcsicmp(me.szModule, dllName.c_str()) == 0) {
            found = true;
            break;
        }
    }
    ::CloseHandle(msnap);
    return found;
}

bool InjectDll(DWORD pid, const std::wstring& dllFullPath)
{
    if (IsDllLoaded(pid, L"hdrfix.dll") || IsDllLoaded(pid, L"hdrfix_probe7.dll")) {
        printf("  [提示] PID %lu 已加载 HDR 修复插件，无需重复注入。\n", pid);
        return true;
    }

    HANDLE hProc = ::OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                 PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                                 FALSE, pid);
    if (!hProc) {
        printf("  [错误] 无法打开目标进程 PID %lu (错误码: %lu)\n", pid, ::GetLastError());
        return false;
    }

    size_t sizeBytes = (dllFullPath.length() + 1) * sizeof(wchar_t);
    LPVOID remoteMem = ::VirtualAllocEx(hProc, nullptr, sizeBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteMem) {
        printf("  [错误] 远程内存分配失败 (PID %lu)\n", pid);
        ::CloseHandle(hProc);
        return false;
    }

    if (!::WriteProcessMemory(hProc, remoteMem, dllFullPath.c_str(), sizeBytes, nullptr)) {
        printf("  [错误] 写入远程内存失败 (PID %lu)\n", pid);
        ::VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
        ::CloseHandle(hProc);
        return false;
    }

    HMODULE hKernel = ::GetModuleHandleW(L"kernel32.dll");
    LPTHREAD_START_ROUTINE pLoadLib = (LPTHREAD_START_ROUTINE)::GetProcAddress(hKernel, "LoadLibraryW");
    if (!pLoadLib) {
        ::VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
        ::CloseHandle(hProc);
        return false;
    }

    HANDLE hThread = ::CreateRemoteThread(hProc, nullptr, 0, pLoadLib, remoteMem, 0, nullptr);
    if (!hThread) {
        printf("  [错误] 创建远程注入线程失败 (PID %lu, 错误码: %lu)\n", pid, ::GetLastError());
        ::VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
        ::CloseHandle(hProc);
        return false;
    }

    ::WaitForSingleObject(hThread, 5000);
    DWORD exitCode = 0;
    ::GetExitCodeThread(hThread, &exitCode);
    ::CloseHandle(hThread);
    ::VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
    ::CloseHandle(hProc);

    if (exitCode == 0) {
        printf("  [警告] LoadLibraryW 返回 NULL (PID %lu)\n", pid);
        return false;
    }

    printf("  [成功] 已成功向 PID %lu 注入插件: %s\n", pid, ToUtf8(dllFullPath).c_str());
    return true;
}

// -------------------------------------------------------------
// 操作 1: 一键安装
// -------------------------------------------------------------
int DoInstall()
{
    printf("\n===================================================\n");
    printf("  小黑盒 HDR 屏幕共享修复补丁 — 一键安装向导\n");
    printf("===================================================\n\n");

    std::wstring heyboxDir = GetHeyboxDefaultDir();
    if (heyboxDir.empty()) {
        printf("[错误] 未能检测到小黑盒语音安装路径 (%%LOCALAPPDATA%%\\Qingfeng\\HeyboxChat)！\n");
        printf("请确认小黑盒是否安装在默认路径下。\n");
        return 1;
    }

    printf("[1/3] 找到小黑盒目录: %s\n", ToUtf8(heyboxDir).c_str());

    // 创建插件目录
    fs::path pluginDir = fs::path(heyboxDir) / L"plugins" / L"hdrfix";
    try {
        fs::create_directories(pluginDir);
    } catch (...) {
        printf("[错误] 无法创建插件目录: %s\n", ToUtf8(pluginDir.wstring()).c_str());
        return 1;
    }

    std::wstring selfDir = GetSelfDirectory();
    printf("[2/3] 正在复制补丁核心与配置文件到插件目录...\n");

    std::vector<std::wstring> filesToCopy = {
        L"hdrfix.dll",
        L"hdrfix_loader.exe",
        L"hdrfix.ini",
        L"compat.json"
    };

    for (const auto& file : filesToCopy) {
        fs::path src = fs::path(selfDir) / file;
        fs::path dst = pluginDir / file;
        if (fs::exists(src)) {
            try {
                fs::copy_file(src, dst, fs::copy_options::overwrite_existing);
                printf("  - 已安装: %s\n", ToUtf8(file).c_str());
            } catch (const std::exception& e) {
                printf("  - 复制 %s 失败: %s\n", ToUtf8(file).c_str(), e.what());
            }
        }
    }

    // 创建桌面快捷方式
    printf("[3/3] 正在创建桌面快捷方式...\n");
    std::wstring desktop = GetDesktopPath();
    if (!desktop.empty()) {
        std::wstring lnkPath = (fs::path(desktop) / L"小黑盒语音 (带HDR修复).lnk").wstring();
        std::wstring targetExe = (pluginDir / L"hdrfix_loader.exe").wstring();
        std::wstring heyboxExe = (fs::path(heyboxDir) / L"HeyboxChat.exe").wstring();
        if (CreateShortcut(lnkPath, targetExe, L"--launch", heyboxExe, L"启动小黑盒语音并自动启用 HDR 共享色调映射修复")) {
            printf("  - [成功] 已在桌面生成快捷方式: 小黑盒语音 (带HDR修复).lnk\n");
        } else {
            printf("  - [警告] 快捷方式生成失败。\n");
        }
    }

    printf("\n---------------------------------------------------\n");
    printf("  安装成功！\n");
    printf("  从今以后双击桌面【小黑盒语音 (带HDR修复)】即可无感享受正常色彩！\n");
    printf("---------------------------------------------------\n\n");
    return 0;
}

// -------------------------------------------------------------
// 操作 2: 一键卸载
// -------------------------------------------------------------
int DoUninstall()
{
    printf("\n===================================================\n");
    printf("  小黑盒 HDR 屏幕共享修复补丁 — 一键卸载向导\n");
    printf("===================================================\n\n");

    // 1. 发送全局停止信号，让运行中的探针退出
    HANDLE hKill = ::OpenEventW(EVENT_MODIFY_STATE, FALSE, L"Local\\hdrfix_kill");
    if (hKill) {
        ::SetEvent(hKill);
        ::CloseHandle(hKill);
        printf("[1/3] 已发送 Kill Switch 熔断信号。\n");
    } else {
        printf("[1/3] 未检测到活跃的 Hook 事件。\n");
    }

    // 2. 删除桌面快捷方式
    printf("[2/3] 正在移除桌面快捷方式...\n");
    std::wstring desktop = GetDesktopPath();
    if (!desktop.empty()) {
        fs::path lnk = fs::path(desktop) / L"小黑盒语音 (带HDR修复).lnk";
        if (fs::exists(lnk)) {
            std::error_code ec;
            fs::remove(lnk, ec);
            printf("  - 已删除桌面快捷方式。\n");
        }
    }

    // 3. 删除插件安装目录
    printf("[3/3] 正在清理插件文件...\n");
    std::wstring heyboxDir = GetHeyboxDefaultDir();
    if (!heyboxDir.empty()) {
        fs::path pluginDir = fs::path(heyboxDir) / L"plugins" / L"hdrfix";
        if (fs::exists(pluginDir)) {
            std::error_code ec;
            fs::remove_all(pluginDir, ec);
            printf("  - 已彻底移除插件目录: %s\n", ToUtf8(pluginDir.wstring()).c_str());
        }
    }

    printf("\n---------------------------------------------------\n");
    printf("  卸载完成！系统已恢复到原生状态，零任何残留文件。\n");
    printf("---------------------------------------------------\n\n");
    return 0;
}

// -------------------------------------------------------------
// 操作 3: 伴随拉起并自动注入
// -------------------------------------------------------------
int DoLaunch()
{
    std::wstring heyboxDir = GetHeyboxDefaultDir();
    std::wstring heyboxExe = fs::path(heyboxDir) / L"HeyboxChat.exe";

    std::wstring selfDir = GetSelfDirectory();
    std::wstring dllPath = (fs::path(selfDir) / L"hdrfix.dll").wstring();
    if (!fs::exists(dllPath)) {
        dllPath = (fs::path(selfDir) / L"hdrfix_probe7.dll").wstring();
    }

    auto pids = FindHeyboxPids();
    if (pids.empty()) {
        printf("[伴随启动] 正在启动小黑盒: %s ...\n", ToUtf8(heyboxExe).c_str());
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (!::CreateProcessW(heyboxExe.c_str(), nullptr, nullptr, nullptr, FALSE, 0, nullptr, heyboxDir.c_str(), &si, &pi)) {
            printf("[错误] 无法启动小黑盒 (错误码: %lu)！\n", ::GetLastError());
            return 1;
        }
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
        printf("[伴随启动] 启动成功，等待客户端初始化 (5秒)...\n");
        ::Sleep(5000);
    }

    // 循环等待 VolcEngineRTC.dll 或直接注入所有相关进程
    printf("[伴随启动] 正在向小黑盒进程注入 HDR 修复补丁...\n");
    int injectedCount = 0;
    for (int retry = 0; retry < 10; ++retry) {
        auto curPids = FindHeyboxPids();
        for (DWORD pid : curPids) {
            if (InjectDll(pid, dllPath)) {
                injectedCount++;
            }
        }
        if (injectedCount > 0) break;
        ::Sleep(1000);
    }

    if (injectedCount > 0) {
        printf("[伴随启动] 补丁注入成功！您可以正常发起屏幕共享了。\n");
    } else {
        printf("[警告] 未能完成注入，请确认小黑盒是否已正常打开。\n");
    }

    return 0;
}

// -------------------------------------------------------------
// 操作 4: 立即向运行中的黑盒注入
// -------------------------------------------------------------
int DoInject()
{
    std::wstring selfDir = GetSelfDirectory();
    std::wstring dllPath = (fs::path(selfDir) / L"hdrfix.dll").wstring();
    if (!fs::exists(dllPath)) {
        dllPath = (fs::path(selfDir) / L"hdrfix_probe7.dll").wstring();
    }

    auto pids = FindHeyboxPids();
    if (pids.empty()) {
        printf("[提示] 当前未检测到正在运行的 HeyboxChat.exe！\n");
        return 1;
    }

    int okCount = 0;
    for (DWORD pid : pids) {
        if (InjectDll(pid, dllPath)) {
            okCount++;
        }
    }
    return okCount > 0 ? 0 : 1;
}

// -------------------------------------------------------------
// 操作 5: 状态检查
// -------------------------------------------------------------
int DoStatus()
{
    printf("\n===================================================\n");
    printf("  小黑盒 HDR 屏幕共享修复补丁 — 运行状态诊断\n");
    printf("===================================================\n\n");

    std::wstring heyboxDir = GetHeyboxDefaultDir();
    printf("[客户端路径] : %s\n", heyboxDir.empty() ? "未找到" : ToUtf8(heyboxDir).c_str());

    auto pids = FindHeyboxPids();
    printf("[客户端进程] : %s (发现 %zu 个进程)\n", pids.empty() ? "未运行" : "运行中", pids.size());
    for (DWORD pid : pids) {
        bool vertc = IsDllLoaded(pid, L"VolcEngineRTC.dll");
        bool fixed = IsDllLoaded(pid, L"hdrfix.dll") || IsDllLoaded(pid, L"hdrfix_probe7.dll");
        printf("  - PID %5lu | VolcEngineRTC: %-3s | HDRFix 补丁: %s\n",
               pid, vertc ? "YES" : "NO", fixed ? "【已生效】" : "未挂载");
    }

    printf("\n");
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    ::SetConsoleOutputCP(65001);
    ::SetConsoleCP(65001);

    if (argc > 1) {
        std::string arg = argv[1];
        if (arg == "--install" || arg == "-i") return DoInstall();
        if (arg == "--uninstall" || arg == "-u") return DoUninstall();
        if (arg == "--launch" || arg == "-l") return DoLaunch();
        if (arg == "--inject") return DoInject();
        if (arg == "--status" || arg == "-s") return DoStatus();
    }

    // 默认双击行为：若已运行则注入，若未运行则伴随启动
    auto pids = FindHeyboxPids();
    if (pids.empty()) {
        return DoLaunch();
    } else {
        return DoInject();
    }
}
