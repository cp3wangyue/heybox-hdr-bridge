#pragma once

#include <windows.h>
#include <string>

namespace hdrfix {

class BridgeLogger {
public:
    static BridgeLogger& Instance();

    void Log(const char* tag, const char* fmt, ...);
    void LogW(const char* tag, const wchar_t* fmt, ...);

    std::wstring GetLogFilePath() const { return m_logPath; }

private:
    BridgeLogger();
    ~BridgeLogger();

    void WriteEntry(const char* tag, const char* message);

    CRITICAL_SECTION m_cs;
    std::wstring m_logPath;
    HANDLE m_file = INVALID_HANDLE_VALUE;
};

} // namespace hdrfix

#define BRIDGE_LOG(tag, fmt, ...) ::hdrfix::BridgeLogger::Instance().Log(tag, fmt, ##__VA_ARGS__)
#define BRIDGE_LOGW(tag, fmt, ...) ::hdrfix::BridgeLogger::Instance().LogW(tag, fmt, ##__VA_ARGS__)
