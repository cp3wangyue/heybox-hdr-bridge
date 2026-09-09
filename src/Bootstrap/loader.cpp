// loader.cpp — HEYBOX HDR Bridge 安装器与会话伴随加载器 (hdrfix_loader.exe)
//
// 设计目标：
//   1. 不再注册开机自启动，也不常驻全局 daemon；
//   2. 通过 --launch 启动小黑盒时，加载器仅在本次小黑盒会话期间作为伴随进程运行；
//   3. 伴随进程负责覆盖 Electron 后续派生的 HeyboxChat.exe 子进程，并在小黑盒完全退出后自动退出；
//   4. --inject 保留为一次性手动附加模式；
//   5. 卸载时兼容清理旧版本曾使用的注册表自启动项。

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <shlobj.h>
#include <shellapi.h>
#include <wrl/client.h>

#include <cstdio>
#include <clocale>
#include <io.h>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <vector>

#pragma comment(lib, "advapi32.lib")

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kSessionMutexName[] = L"Local\\heybox_hdr_bridge_session";
constexpr wchar_t kSessionStopEventName[] = L"Local\\heybox_hdr_bridge_stop";
constexpr wchar_t kLegacyAutoRunValue[] = L"HeyboxHDRFix";
constexpr wchar_t kShortcutName[] = L"\u5C0F\u9ED1\u76D2 (HDR Bridge).lnk";
constexpr wchar_t kLegacyShortcutName[] = L"\u5C0F\u9ED1\u76D2\u8BED\u97F3 (\u5E26HDR\u4FEE\u590D).lnk";

std::string ToUtf8(const std::wstring& wstr)
{
    if (wstr.empty()) return {};
    int sizeNeeded = ::WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
    std::string res(sizeNeeded, 0);
    ::WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), res.data(), sizeNeeded, nullptr, nullptr);
    return res;
}

void LogLoader(const char* fmt, ...)
{
    wchar_t tempDir[MAX_PATH]{};
    ::GetTempPathW(MAX_PATH, tempDir);
    std::wstring logPath = std::wstring(tempDir) + L"hdrfix_loader.log";

    SYSTEMTIME st{};
    ::GetLocalTime(&st);

    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    char line[1200];
    int len = snprintf(line, sizeof(line), "[%02u:%02u:%02u.%03u] [PID:%5lu] %s\r\n",
                       st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                       ::GetCurrentProcessId(), buf);

    ::OutputDebugStringA(line);

    FILE* fp = _wfopen(logPath.c_str(), L"a");
    if (fp) {
        fwrite(line, 1, len, fp);
        fclose(fp);
    }
}

std::wstring GetSelfDirectory()
{
    wchar_t path[MAX_PATH]{};
    ::GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p(path);
    size_t pos = p.find_last_of(L"\\/");
    return (pos != std::wstring::npos) ? p.substr(0, pos + 1) : L"";
}

std::wstring GetBridgeInstallDir()
{
    wchar_t localApp[MAX_PATH]{};
    if (::SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, localApp) == S_OK) {
        return (fs::path(localApp) / L"HeyboxHDRBridge").wstring();
    }
    return L"";
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

    // 备用检索：注册表卸载项
    const wchar_t* subKeys[] = {
        L"Software\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\HeyboxChat",
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\HeyboxChat"
    };
    for (const auto* subKey : subKeys) {
        HKEY hKey = nullptr;
        if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, subKey, 0, KEY_READ, &hKey) == ERROR_SUCCESS ||
            ::RegOpenKeyExW(HKEY_CURRENT_USER, subKey, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            wchar_t val[MAX_PATH]{};
            DWORD valSize = sizeof(val);
            if (::RegQueryValueExW(hKey, L"DisplayIcon", nullptr, nullptr, reinterpret_cast<LPBYTE>(val), &valSize) == ERROR_SUCCESS) {
                fs::path p(val);
                if (fs::exists(p)) {
                    ::RegCloseKey(hKey);
                    return p.parent_path().wstring();
                }
            }
            valSize = sizeof(val);
            if (::RegQueryValueExW(hKey, L"InstallLocation", nullptr, nullptr, reinterpret_cast<LPBYTE>(val), &valSize) == ERROR_SUCCESS) {
                fs::path p(val);
                if (fs::exists(p / L"HeyboxChat.exe")) {
                    ::RegCloseKey(hKey);
                    return p.wstring();
                }
            }
            valSize = sizeof(val);
            if (::RegQueryValueExW(hKey, L"UninstallString", nullptr, nullptr, reinterpret_cast<LPBYTE>(val), &valSize) == ERROR_SUCCESS) {
                fs::path p(val);
                if (fs::exists(p.parent_path() / L"HeyboxChat.exe")) {
                    ::RegCloseKey(hKey);
                    return p.parent_path().wstring();
                }
            }
            ::RegCloseKey(hKey);
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
    LogLoader("CreateShortcut: target=%s, lnk=%s", ToUtf8(targetExe).c_str(), ToUtf8(shortcutPath).c_str());
    HRESULT initHr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool needUninit = SUCCEEDED(initHr);

    ComPtr<IShellLinkW> shellLink;
    HRESULT hr = ::CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_IShellLinkW, reinterpret_cast<void**>(shellLink.GetAddressOf()));
    if (FAILED(hr)) {
        LogLoader("CreateShortcut: CoCreateInstance failed hr=0x%08lX", hr);
        if (needUninit) ::CoUninitialize();
        return false;
    }

    std::wstring workDir = fs::path(targetExe).parent_path().wstring();
    shellLink->SetPath(targetExe.c_str());
    shellLink->SetArguments(arguments.c_str());
    shellLink->SetDescription(description.c_str());
    shellLink->SetWorkingDirectory(workDir.c_str());
    shellLink->SetShowCmd(SW_SHOWNORMAL);

    if (!iconPath.empty()) {
        shellLink->SetIconLocation(iconPath.c_str(), 0);
    }

    ComPtr<IPersistFile> persistFile;
    hr = shellLink.As(&persistFile);
    if (SUCCEEDED(hr)) {
        std::error_code ec;
        fs::remove(shortcutPath, ec);
        hr = persistFile->Save(shortcutPath.c_str(), TRUE);
        LogLoader("CreateShortcut: persistFile->Save hr=0x%08lX (remove ec=%d)", hr, ec.value());
    } else {
        LogLoader("CreateShortcut: shellLink.As(&persistFile) failed hr=0x%08lX", hr);
    }

    if (needUninit) ::CoUninitialize();
    return SUCCEEDED(hr);
}

void RemoveLegacyAutoRun()
{
    HKEY hKey = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                        0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        ::RegDeleteValueW(hKey, kLegacyAutoRunValue);
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
    HANDLE msnap = ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
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
    if (IsDllLoaded(pid, L"hdrfix.dll")) {
        return true;
    }

    HANDLE hProc = ::OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                 PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                                 FALSE, pid);
    if (!hProc) {
        LogLoader("InjectDll: OpenProcess(PID %lu) failed err=%lu", pid, ::GetLastError());
        return false;
    }

    size_t sizeBytes = (dllFullPath.length() + 1) * sizeof(wchar_t);
    LPVOID remoteMem = ::VirtualAllocEx(hProc, nullptr, sizeBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteMem) {
        LogLoader("InjectDll: VirtualAllocEx(PID %lu) failed err=%lu", pid, ::GetLastError());
        ::CloseHandle(hProc);
        return false;
    }

    if (!::WriteProcessMemory(hProc, remoteMem, dllFullPath.c_str(), sizeBytes, nullptr)) {
        LogLoader("InjectDll: WriteProcessMemory(PID %lu) failed err=%lu", pid, ::GetLastError());
        ::VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
        ::CloseHandle(hProc);
        return false;
    }

    HMODULE hKernel = ::GetModuleHandleW(L"kernel32.dll");
    auto pLoadLib = reinterpret_cast<LPTHREAD_START_ROUTINE>(::GetProcAddress(hKernel, "LoadLibraryW"));
    if (!pLoadLib) {
        ::VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
        ::CloseHandle(hProc);
        return false;
    }

    HANDLE hThread = ::CreateRemoteThread(hProc, nullptr, 0, pLoadLib, remoteMem, 0, nullptr);
    if (!hThread) {
        LogLoader("InjectDll: CreateRemoteThread(PID %lu) failed err=%lu", pid, ::GetLastError());
        ::VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
        ::CloseHandle(hProc);
        return false;
    }

    DWORD waitResult = ::WaitForSingleObject(hThread, 5000);
    DWORD exitCode = 0;
    if (waitResult == WAIT_OBJECT_0) {
        ::GetExitCodeThread(hThread, &exitCode);
    } else {
        LogLoader("InjectDll: WaitForSingleObject(PID %lu) timed out/failed (result=%lu)", pid, waitResult);
    }

    ::CloseHandle(hThread);
    ::VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
    ::CloseHandle(hProc);

    bool ok = (waitResult == WAIT_OBJECT_0 && exitCode != 0);
    LogLoader("InjectDll: PID %lu LoadLibrary result: exitCode=0x%08lX -> %s", pid, exitCode, ok ? "SUCCESS" : "FAIL");
    return ok;
}

int InjectAllRunningProcesses(const std::wstring& dllPath, bool verbose)
{
    int okCount = 0;
    for (DWORD pid : FindHeyboxPids()) {
        const bool alreadyLoaded = IsDllLoaded(pid, L"hdrfix.dll");
        if (alreadyLoaded || InjectDll(pid, dllPath)) {
            ++okCount;
            if (verbose && !alreadyLoaded) {
                printf("  - PID %lu 注入成功。\n", pid);
            }
        }
    }
    return okCount;
}

bool StartHeybox()
{
    std::wstring heyboxDir = GetHeyboxDefaultDir();
    if (heyboxDir.empty()) return false;

    fs::path exe = fs::path(heyboxDir) / L"HeyboxChat.exe";
    std::wstring command = L"\"" + exe.wstring() + L"\"";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    BOOL ok = ::CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0,
                               nullptr, heyboxDir.c_str(), &si, &pi);
    if (!ok) return false;

    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return true;
}

int RunSessionCompanion(bool launchIfNeeded)
{
    LogLoader("=== RunSessionCompanion started (launchIfNeeded=%d) ===", launchIfNeeded ? 1 : 0);

    HANDLE hMutex = ::CreateMutexW(nullptr, TRUE, kSessionMutexName);
    if (!hMutex) {
        LogLoader("Failed to create session mutex: err=%lu", ::GetLastError());
        return 1;
    }

    if (::GetLastError() == ERROR_ALREADY_EXISTS) {
        ::CloseHandle(hMutex);
        LogLoader("Session companion already running. Exiting redundant instance.");
        if (launchIfNeeded && FindHeyboxPids().empty()) {
            StartHeybox();
        }
        return 0;
    }

    if (launchIfNeeded && FindHeyboxPids().empty()) {
        LogLoader("Heybox not running. Calling StartHeybox()...");
        if (!StartHeybox()) {
            LogLoader("StartHeybox() failed!");
            ::ReleaseMutex(hMutex);
            ::CloseHandle(hMutex);
            return 1;
        }
        LogLoader("StartHeybox() succeeded.");
    }

    // --launch 通常由桌面快捷方式调用，伴随器整个会话静默运行。
    if (HWND hwnd = ::GetConsoleWindow()) {
        ::ShowWindow(hwnd, SW_HIDE);
    }

    std::wstring dllPath = (fs::path(GetSelfDirectory()) / L"hdrfix.dll").wstring();
    if (!fs::exists(dllPath)) {
        std::wstring fallback = (fs::path(GetBridgeInstallDir()) / L"hdrfix.dll").wstring();
        if (fs::exists(fallback)) {
            dllPath = fallback;
        }
    }
    if (!fs::exists(dllPath)) {
        LogLoader("hdrfix.dll not found at: %ls", dllPath.c_str());
        ::ReleaseMutex(hMutex);
        ::CloseHandle(hMutex);
        return 1;
    }
    LogLoader("Target DLL path: %ls", dllPath.c_str());

    HANDLE hStop = ::CreateEventW(nullptr, TRUE, FALSE, kSessionStopEventName);
    if (!hStop) {
        LogLoader("Failed to create stop event: err=%lu", ::GetLastError());
        ::ReleaseMutex(hMutex);
        ::CloseHandle(hMutex);
        return 1;
    }
    ::ResetEvent(hStop);

    bool sawHeybox = false;
    int emptySeconds = 0;
    int startupSeconds = 0;
    int heartbeatSeconds = 0;

    LogLoader("Entering companion monitoring loop...");

    while (::WaitForSingleObject(hStop, 0) != WAIT_OBJECT_0) {
        auto pids = FindHeyboxPids();
        if (!pids.empty()) {
            sawHeybox = true;
            emptySeconds = 0;
            for (DWORD pid : pids) {
                if (!IsDllLoaded(pid, L"hdrfix.dll")) {
                    bool ok = InjectDll(pid, dllPath);
                    LogLoader("Injected into PID %lu -> %s", pid, ok ? "OK" : "FAIL");
                }
            }
        } else if (sawHeybox) {
            // Electron 退出时多个进程不是同时消失，留 20 秒宽限期。
            if (++emptySeconds >= 20) {
                LogLoader("Heybox processes disappeared for 20 seconds. Companion exiting.");
                break;
            }
        } else if (++startupSeconds >= 60) {
            LogLoader("Heybox failed to start within 60 seconds. Companion exiting.");
            break;
        }

        if (++heartbeatSeconds >= 10) {
            heartbeatSeconds = 0;
            LogLoader("Heartbeat: sawHeybox=%d, activePids=%zu", sawHeybox ? 1 : 0, pids.size());
        }

        if (::WaitForSingleObject(hStop, 1000) == WAIT_OBJECT_0) {
            LogLoader("Stop event signaled. Companion exiting.");
            break;
        }
    }

    ::CloseHandle(hStop);
    ::ReleaseMutex(hMutex);
    ::CloseHandle(hMutex);
    LogLoader("=== Companion exited cleanly ===");
    return 0;
}

int DoInstall()
{
    LogLoader("=== DoInstall started ===");
    printf("\n===================================================\n");
    printf("  HEYBOX HDR Bridge — 一键安装\n");
    printf("===================================================\n\n");

    std::wstring heyboxDir = GetHeyboxDefaultDir();
    LogLoader("DoInstall: heyboxDir=%ls", heyboxDir.c_str());
    if (heyboxDir.empty()) {
        printf("[错误] 未检测到小黑盒客户端安装目录 (HeyboxChat.exe)\n");
        return 1;
    }

    std::wstring installDir = GetBridgeInstallDir();
    LogLoader("DoInstall: installDir=%ls", installDir.c_str());
    if (installDir.empty()) {
        printf("[错误] 无法获取 %%LOCALAPPDATA%% 目录\n");
        return 1;
    }

    fs::path targetDir(installDir);
    std::error_code ec;
    fs::create_directories(targetDir, ec);
    if (ec) {
        printf("[错误] 无法创建独立安装目录: %s\n", ec.message().c_str());
        LogLoader("DoInstall: create_directories failed ec=%d", ec.value());
        return 1;
    }

    std::wstring selfDir = GetSelfDirectory();
    LogLoader("DoInstall: selfDir=%ls", selfDir.c_str());
    const std::vector<std::wstring> filesToCopy = {
        L"hdrfix.dll", L"hdrfix_loader.exe", L"hdrfix.ini", L"compat.json"
    };

    printf("[1/3] 复制运行文件到独立持久化目录...\n");
    printf("  安装路径: %s\n", ToUtf8(installDir).c_str());

    bool runningFromTarget = false;
    try {
        if (!selfDir.empty() && fs::exists(selfDir) && fs::equivalent(selfDir, targetDir)) {
            runningFromTarget = true;
        }
    } catch (...) {}

    for (const auto& file : filesToCopy) {
        fs::path src = fs::path(selfDir) / file;
        fs::path dst = targetDir / file;
        if (runningFromTarget && file == L"hdrfix_loader.exe") {
            continue;
        }
        if (!fs::exists(src)) {
            if (fs::exists(dst)) {
                continue;
            }
            printf("  - [缺失] %s\n", ToUtf8(file).c_str());
            LogLoader("DoInstall: file missing: %ls", file.c_str());
            return 1;
        }
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            printf("  - [失败] %s: %s\n", ToUtf8(file).c_str(), ec.message().c_str());
            LogLoader("DoInstall: copy_file failed for %ls: %s", file.c_str(), ec.message().c_str());
            return 1;
        }
    }

    // 迁移旧版：明确清理小黑盒内部旧插件目录以及开机自启动配置
    printf("[2/3] 清理旧版历史配置与旧插件目录...\n");
    RemoveLegacyAutoRun();
    fs::path oldPluginDir = fs::path(heyboxDir) / L"plugins" / L"hdrfix";
    if (fs::exists(oldPluginDir)) {
        ec.clear();
        fs::remove_all(oldPluginDir, ec);
        LogLoader("DoInstall: removed oldPluginDir (ec=%d)", ec.value());
    }

    printf("[3/3] 创建“随小黑盒会话运行”的启动快捷方式...\n");
    std::wstring desktop = GetDesktopPath();
    LogLoader("DoInstall: desktop=%s", ToUtf8(desktop).c_str());
    if (desktop.empty()) {
        printf("[错误] 获取桌面路径失败。\n");
        return 1;
    }

    fs::path installedLoader = targetDir / L"hdrfix_loader.exe";
    fs::path heyboxExe = fs::path(heyboxDir) / L"HeyboxChat.exe";
    fs::path shortcut = fs::path(desktop) / kShortcutName;

    LogLoader("DoInstall: Creating shortcut %s -> %s", ToUtf8(shortcut.wstring()).c_str(), ToUtf8(installedLoader.wstring()).c_str());
    if (!CreateShortcut(shortcut.wstring(), installedLoader.wstring(), L"--launch", heyboxExe.wstring(),
                        L"启动小黑盒，并仅在本次会话期间启用 HDR 屏幕共享修复")) {
        printf("[错误] 创建桌面快捷方式失败。\n");
        LogLoader("DoInstall: CreateShortcut returned false!");
        return 1;
    }

    // 清理旧版快捷方式名称，避免用户误用旧行为。
    fs::remove(fs::path(desktop) / kLegacyShortcutName, ec);

    printf("\n安装完成。以后请从桌面的【小黑盒 (HDR Bridge)】启动。\n");
    printf("补丁已安装至独立目录，小黑盒客户端更新不会导致快捷方式或修复补丁失效。\n");
    printf("不会注册开机常驻进程；伴随器只在小黑盒运行期间存在，并在小黑盒退出后自动结束。\n\n");
    LogLoader("=== DoInstall completed successfully ===");
    return 0;
}

int DoUninstall()
{
    printf("\n===================================================\n");
    printf("  HEYBOX HDR Bridge — 一键卸载\n");
    printf("===================================================\n\n");

    // 通知本次会话伴随器退出。
    HANDLE hStop = ::OpenEventW(EVENT_MODIFY_STATE, FALSE, kSessionStopEventName);
    if (hStop) {
        ::SetEvent(hStop);
        ::CloseHandle(hStop);
    }

    // 兼容清理旧版本可能留下的开机启动项。
    RemoveLegacyAutoRun();

    std::wstring desktop = GetDesktopPath();
    std::error_code ec;
    if (!desktop.empty()) {
        fs::remove(fs::path(desktop) / kShortcutName, ec);
        ec.clear();
        fs::remove(fs::path(desktop) / kLegacyShortcutName, ec);
    }

    // 清理旧版插件目录（如果在小黑盒内部）
    std::wstring heyboxDir = GetHeyboxDefaultDir();
    if (!heyboxDir.empty()) {
        fs::path oldPluginDir = fs::path(heyboxDir) / L"plugins" / L"hdrfix";
        ec.clear();
        fs::remove_all(oldPluginDir, ec);
    }

    // 清理独立持久化安装目录
    std::wstring installDir = GetBridgeInstallDir();
    if (!installDir.empty() && fs::exists(installDir)) {
        ec.clear();
        fs::remove_all(installDir, ec);
        if (ec) {
            printf("[提示] 部分文件可能仍被占用。请完全退出小黑盒后重新执行卸载。\n");
            return 2;
        }
    }

    printf("卸载完成；已清理快捷方式与安装目录，没有保留开机自启动项或后台常驻守护。\n\n");
    return 0;
}

int DoLaunch()
{
    return RunSessionCompanion(true);
}

int DoInject()
{
    std::wstring dllPath = (fs::path(GetSelfDirectory()) / L"hdrfix.dll").wstring();
    if (!fs::exists(dllPath)) {
        std::wstring fallback = (fs::path(GetBridgeInstallDir()) / L"hdrfix.dll").wstring();
        if (fs::exists(fallback)) {
            dllPath = fallback;
        }
    }
    if (!fs::exists(dllPath)) {
        printf("[错误] 未找到 hdrfix.dll。\n");
        return 1;
    }

    auto pids = FindHeyboxPids();
    if (pids.empty()) {
        printf("[提示] 当前未检测到 HeyboxChat.exe。\n");
        return 1;
    }

    return InjectAllRunningProcesses(dllPath, true) > 0 ? 0 : 1;
}

int DoStatus()
{
    printf("\n===================================================\n");
    printf("  HEYBOX HDR Bridge — 状态\n");
    printf("===================================================\n\n");

    std::wstring heyboxDir = GetHeyboxDefaultDir();
    printf("[小黑盒路径]   : %s\n", heyboxDir.empty() ? "未找到" : ToUtf8(heyboxDir).c_str());

    std::wstring installDir = GetBridgeInstallDir();
    bool installed = !installDir.empty() && fs::exists(fs::path(installDir) / L"hdrfix_loader.exe");
    printf("[补丁安装路径] : %s%s\n",
           installDir.empty() ? "未配置" : ToUtf8(installDir).c_str(),
           installed ? " (已安装)" : " (未安装/未找到)");

    HANDLE hSession = ::OpenMutexW(SYNCHRONIZE, FALSE, kSessionMutexName);
    printf("[会话伴随器]   : %s\n", hSession ? "运行中（仅随当前小黑盒会话）" : "未运行");
    if (hSession) ::CloseHandle(hSession);

    auto pids = FindHeyboxPids();
    printf("[小黑盒进程]   : %s（%zu 个）\n", pids.empty() ? "未运行" : "运行中", pids.size());
    for (DWORD pid : pids) {
        bool vertc = IsDllLoaded(pid, L"VolcEngineRTC.dll");
        bool fixed = IsDllLoaded(pid, L"hdrfix.dll");
        printf("  - PID %5lu | VolcEngineRTC: %-3s | HDR Bridge: %s\n",
               pid, vertc ? "YES" : "NO", fixed ? "已加载" : "未加载");
    }
    printf("\n");
    return 0;
}

} // namespace

int RunLoader(int argc, wchar_t** argv)
{
    setlocale(LC_ALL, ".utf8");
    bool isConsoleCommand = false;
    if (argc > 1) {
        std::wstring a = argv[1];
        if (a == L"--status" || a == L"-s" ||
            a == L"--install" || a == L"-i" ||
            a == L"--uninstall" || a == L"-u" ||
            a == L"--inject" ||
            a == L"--help" || a == L"-h") {
            isConsoleCommand = true;
        }
    }

    if (isConsoleCommand) {
        HANDLE hStdOut = ::GetStdHandle(STD_OUTPUT_HANDLE);
        if (hStdOut && hStdOut != INVALID_HANDLE_VALUE && ::GetFileType(hStdOut) != FILE_TYPE_UNKNOWN) {
            int fd = _open_osfhandle(reinterpret_cast<intptr_t>(hStdOut), _O_TEXT);
            if (fd >= 0) {
                FILE* fp = _fdopen(fd, "w");
                if (fp) {
                    *stdout = *fp;
                    setvbuf(stdout, nullptr, _IONBF, 0);
                }
            }
        } else if (::AttachConsole(ATTACH_PARENT_PROCESS)) {
            FILE* fp;
            freopen_s(&fp, "CONOUT$", "w", stdout);
            freopen_s(&fp, "CONOUT$", "w", stderr);
            freopen_s(&fp, "CONIN$", "r", stdin);
            if (stdout) setvbuf(stdout, nullptr, _IONBF, 0);
            if (stderr) setvbuf(stderr, nullptr, _IONBF, 0);
        }
        ::SetConsoleOutputCP(65001);
        ::SetConsoleCP(65001);
    }

    std::string argStr;
    for (int i = 1; i < argc; ++i) {
        if (!argStr.empty()) argStr += " ";
        argStr += ToUtf8(argv[i]);
    }
    LogLoader("hdrfix_loader main invoked with args: '%s'", argStr.c_str());

    if (argc > 1) {
        std::wstring arg = argv[1];
        if (arg == L"--install" || arg == L"-i") return DoInstall();
        if (arg == L"--uninstall" || arg == L"-u") return DoUninstall();
        if (arg == L"--launch" || arg == L"-l") return DoLaunch();
        if (arg == L"--inject") return DoInject();
        if (arg == L"--status" || arg == L"-s") return DoStatus();
    }

    // 默认双击即“启动小黑盒 + 本次会话伴随”，不再隐式安装或启动全局守护。
    return DoLaunch();
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    int argc = 0;
    wchar_t** argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    int res = RunLoader(argc, argv);
    if (argv) ::LocalFree(argv);
    return res;
}

int main(int, char**)
{
    int argc = 0;
    wchar_t** argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    int res = RunLoader(argc, argv);
    if (argv) ::LocalFree(argv);
    return res;
}
