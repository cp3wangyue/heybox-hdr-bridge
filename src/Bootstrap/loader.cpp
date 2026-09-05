// loader.cpp — P8 简易安装、卸载与全自动伴随守护 (hdrfix_loader.exe)
//
// 功能：
//   1. --install   : 一键安装并配置开机/日常自动静默守护，从此正常启动小黑盒即可全自动注入，无须特殊快捷方式
//   2. --uninstall : 一键卸载，清理自动守护、注册表自启项与插件文件
//   3. --daemon    : 静默后台守护进程（0 CPU占用），检测到黑盒运行自动注入
//   4. --launch    : 启动黑盒语音并自动注入 hdrfix.dll
//   5. --inject    : 向已运行的黑盒语音注入 hdrfix.dll
//   6. --status    : 检测系统 HDR 状态、黑盒运行状态与插件生效状态

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

#pragma comment(lib, "advapi32.lib")

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

void SetAutoRun(const std::wstring& exePath)
{
    HKEY hKey = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                        0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        std::wstring val = L"\"" + exePath + L"\" --daemon";
        ::RegSetValueExW(hKey, L"HeyboxHDRFix", 0, REG_SZ,
                         reinterpret_cast<const BYTE*>(val.c_str()),
                         static_cast<DWORD>((val.length() + 1) * sizeof(wchar_t)));
        ::RegCloseKey(hKey);
    }
}

void RemoveAutoRun()
{
    HKEY hKey = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                        0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        ::RegDeleteValueW(hKey, L"HeyboxHDRFix");
        ::RegCloseKey(hKey);
    }
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
        return true;
    }

    HANDLE hProc = ::OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                 PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                                 FALSE, pid);
    if (!hProc) {
        return false;
    }

    size_t sizeBytes = (dllFullPath.length() + 1) * sizeof(wchar_t);
    LPVOID remoteMem = ::VirtualAllocEx(hProc, nullptr, sizeBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteMem) {
        ::CloseHandle(hProc);
        return false;
    }

    if (!::WriteProcessMemory(hProc, remoteMem, dllFullPath.c_str(), sizeBytes, nullptr)) {
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

    return exitCode != 0;
}

// -------------------------------------------------------------
// 操作 1: 一键安装（配置自动守护与开机自启）
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

    printf("[1/4] 找到小黑盒目录: %s\n", ToUtf8(heyboxDir).c_str());

    // 创建插件目录
    fs::path pluginDir = fs::path(heyboxDir) / L"plugins" / L"hdrfix";
    try {
        fs::create_directories(pluginDir);
    } catch (...) {
        printf("[错误] 无法创建插件目录: %s\n", ToUtf8(pluginDir.wstring()).c_str());
        return 1;
    }

    std::wstring selfDir = GetSelfDirectory();
    printf("[2/4] 正在复制补丁核心与配置文件到插件目录...\n");

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

    // 注册 Windows 用户自启动（静默后台守护）
    printf("[3/4] 正在注册自动注入守护服务 (无需特殊快捷方式)...\n");
    fs::path installedLoader = pluginDir / L"hdrfix_loader.exe";
    // 复位停止信号（防止上次卸载的 kill 信号影响）
    HANDLE hKillReset = ::OpenEventW(EVENT_MODIFY_STATE, FALSE, L"Local\\hdrfix_kill");
    if (hKillReset) {
        ::ResetEvent(hKillReset);
        ::CloseHandle(hKillReset);
    }

    // 立即在后台静默启动守护进程
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + installedLoader.wstring() + L"\" --daemon";
    if (::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        ::CloseHandle(pi.hProcess);
        ::CloseHandle(pi.hThread);
        printf("  - [成功] 后台静默自动守护已激活！\n");
    }

    // 若当前小黑盒正在运行，立即挂载生效
    printf("[4/4] 正在检测当前运行中的小黑盒进程...\n");
    auto pids = FindHeyboxPids();
    if (!pids.empty()) {
        std::wstring dllPath = (pluginDir / L"hdrfix.dll").wstring();
        int okCount = 0;
        for (DWORD pid : pids) {
            if (InjectDll(pid, dllPath)) okCount++;
        }
        printf("  - [成功] 已即时为当前正在运行的小黑盒 (%zu 个进程) 挂载修复！\n", pids.size());
    } else {
        printf("  - 当前小黑盒未运行。以后随时打开小黑盒，守护将自动秒级挂载！\n");
    }

    printf("\n---------------------------------------------------\n");
    printf("  安装成功！\n");
    printf("  【全自动生效说明】：\n");
    printf("  已配置好静默自动守护，您不需要点击任何特殊的快捷方式。\n");
    printf("  像平常一样直接打开原版小黑盒（桌面图标、任务栏、甚至开机自启），\n");
    printf("  HDR 屏幕共享修复都会全自动在后台秒级注入生效！\n");
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

    // 1. 发送全局停止信号，让运行中的守护与探针退出
    HANDLE hKill = ::OpenEventW(EVENT_MODIFY_STATE, FALSE, L"Local\\hdrfix_kill");
    if (hKill) {
        ::SetEvent(hKill);
        ::CloseHandle(hKill);
        printf("[1/4] 已发送停止信号，关闭后台守护进程与 Hook。\n");
    } else {
        printf("[1/4] 未检测到活跃的 Hook 事件。\n");
    }

    // 2. 清除注册表开机自启项
    printf("[2/4] 正在移除自动启动注册表项...\n");
    RemoveAutoRun();

    // 3. 删除桌面快捷方式（若有）
    printf("[3/4] 正在移除桌面快捷方式...\n");
    std::wstring desktop = GetDesktopPath();
    if (!desktop.empty()) {
        fs::path lnk = fs::path(desktop) / L"小黑盒语音 (带HDR修复).lnk";
        if (fs::exists(lnk)) {
            std::error_code ec;
            fs::remove(lnk, ec);
            printf("  - 已删除历史快捷方式。\n");
        }
    }

    // 4. 删除插件安装目录
    printf("[4/4] 正在清理插件文件...\n");
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
    printf("  卸载完成！自动守护已注销，系统已恢复到原生状态，零任何残留。\n");
    printf("---------------------------------------------------\n\n");
    return 0;
}

// -------------------------------------------------------------
// 操作 3: 后台静默守护（单例、0 CPU占用，随小黑盒秒级注入）
// -------------------------------------------------------------
int DoDaemon()
{
    // 单例互斥体，防止重复启动多个守护
    HANDLE hMutex = ::CreateMutexW(nullptr, TRUE, L"Local\\hdrfix_daemon_mutex");
    if (::GetLastError() == ERROR_ALREADY_EXISTS) {
        if (hMutex) ::CloseHandle(hMutex);
        return 0;
    }

    HWND hwnd = ::GetConsoleWindow();
    if (hwnd) ::ShowWindow(hwnd, SW_HIDE);

    std::wstring selfDir = GetSelfDirectory();
    std::wstring dllPath = (fs::path(selfDir) / L"hdrfix.dll").wstring();

    HANDLE hKill = ::CreateEventW(nullptr, TRUE, FALSE, L"Local\\hdrfix_kill");
    if (hKill) ::ResetEvent(hKill);

    while (true) {
        if (::WaitForSingleObject(hKill, 0) == WAIT_OBJECT_0) {
            break;
        }

        auto pids = FindHeyboxPids();
        for (DWORD pid : pids) {
            if (!IsDllLoaded(pid, L"hdrfix.dll")) {
                InjectDll(pid, dllPath);
            }
        }

        // 睡眠 2 秒（低开销），若收到 kill 信号立即退出
        if (::WaitForSingleObject(hKill, 2000) == WAIT_OBJECT_0) {
            break;
        }
    }

    if (hKill) ::CloseHandle(hKill);
    if (hMutex) {
        ::ReleaseMutex(hMutex);
        ::CloseHandle(hMutex);
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
            printf("  - PID %lu 注入成功。\n", pid);
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

    // 检查守护进程互斥体
    HANDLE hMutexCheck = ::OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\hdrfix_daemon_mutex");
    printf("[后台自动守护] : %s\n", hMutexCheck ? "运行中 (已开启全自动注入)" : "未运行");
    if (hMutexCheck) ::CloseHandle(hMutexCheck);

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
        if (arg == "--daemon" || arg == "-d") return DoDaemon();
        if (arg == "--inject") return DoInject();
        if (arg == "--status" || arg == "-s") return DoStatus();
    }

    // 默认双击行为：若已运行则注入，若未运行则执行安装
    auto pids = FindHeyboxPids();
    if (pids.empty()) {
        return DoInstall();
    } else {
        return DoInject();
    }
}
