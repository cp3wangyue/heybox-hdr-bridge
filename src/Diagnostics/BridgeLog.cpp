#include "Diagnostics/BridgeLog.h"

#include <cstdio>
#include <cstdarg>
#include <vector>

namespace hdrfix {

BridgeLogger& BridgeLogger::Instance()
{
    static BridgeLogger s_inst;
    return s_inst;
}

BridgeLogger::BridgeLogger()
{
    ::InitializeCriticalSection(&m_cs);

    wchar_t tempDir[MAX_PATH]{};
    ::GetTempPathW(MAX_PATH, tempDir);
    m_logPath = std::wstring(tempDir) + L"hdrfix.log";

    // 打开或创建日志文件 (共享读写)
    m_file = ::CreateFileW(
        m_logPath.c_str(),
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
}

BridgeLogger::~BridgeLogger()
{
    if (m_file != INVALID_HANDLE_VALUE) {
        ::CloseHandle(m_file);
        m_file = INVALID_HANDLE_VALUE;
    }
    ::DeleteCriticalSection(&m_cs);
}

void BridgeLogger::WriteEntry(const char* tag, const char* message)
{
    SYSTEMTIME st{};
    ::GetLocalTime(&st);

    DWORD pid = ::GetCurrentProcessId();
    DWORD tid = ::GetCurrentThreadId();

    char line[2048];
    int len = snprintf(
        line, sizeof(line),
        "[%02u:%02u:%02u.%03u] [PID:%5lu|TID:%5lu] [%s] %s\r\n",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
        pid, tid, tag ? tag : "Info", message
    );

    if (len <= 0) return;
    if (static_cast<size_t>(len) >= sizeof(line)) {
        len = static_cast<int>(sizeof(line) - 1);
        line[len - 2] = '\r';
        line[len - 1] = '\n';
    }

    ::EnterCriticalSection(&m_cs);

    // 输出到 DebugView
    ::OutputDebugStringA(line);

    // 写入文件并立即刷盘
    if (m_file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        ::WriteFile(m_file, line, static_cast<DWORD>(len), &written, nullptr);
        ::FlushFileBuffers(m_file);
    }

    ::LeaveCriticalSection(&m_cs);
}

void BridgeLogger::Log(const char* tag, const char* fmt, ...)
{
    if (!fmt) return;

    char buf[1536];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    WriteEntry(tag, buf);
}

void BridgeLogger::LogW(const char* tag, const wchar_t* fmt, ...)
{
    if (!fmt) return;

    wchar_t wbuf[1536];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf(wbuf, sizeof(wbuf) / sizeof(wchar_t), fmt, args);
    va_end(args);

    int sizeNeeded = ::WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, nullptr, 0, nullptr, nullptr);
    if (sizeNeeded <= 0) return;

    std::vector<char> utf8(sizeNeeded);
    ::WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, utf8.data(), sizeNeeded, nullptr, nullptr);

    WriteEntry(tag, utf8.data());
}

} // namespace hdrfix
