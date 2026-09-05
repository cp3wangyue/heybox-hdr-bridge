// Diagnostics/ConfigManager.cpp — 配置文件解析实现
#include "Diagnostics/ConfigManager.h"

#include <windows.h>
#include <algorithm>
#include <vector>

namespace hdrfix {

namespace {

std::wstring ReadIniString(const wchar_t* section, const wchar_t* key, const wchar_t* def, const std::wstring& path)
{
    wchar_t buf[256]{};
    GetPrivateProfileStringW(section, key, def, buf, 256, path.c_str());
    return buf;
}

bool ReadIniBool(const wchar_t* section, const wchar_t* key, bool def, const std::wstring& path)
{
    std::wstring s = ReadIniString(section, key, def ? L"true" : L"false", path);
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(::towlower(c)); });
    return (s == L"true" || s == L"1" || s == L"yes" || s == L"on");
}

float ReadIniFloat(const wchar_t* section, const wchar_t* key, float def, const std::wstring& path)
{
    std::wstring s = ReadIniString(section, key, L"", path);
    if (s.empty()) return def;
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(::towlower(c)); });
    if (s == L"auto" || s == L"system") return 0.0f;
    try {
        return std::stof(s);
    } catch (...) {
        return def;
    }
}

std::string ToString(const std::wstring& ws)
{
    if (ws.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return {};
    std::string s(size - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, s.data(), size, nullptr, nullptr);
    return s;
}

} // namespace

ConfigManager& ConfigManager::Instance()
{
    static ConfigManager s_inst;
    return s_inst;
}

ConfigManager::ConfigManager()
{
    Load();
}

std::wstring ConfigManager::ResolveConfigPath(const std::wstring& customPath)
{
    if (!customPath.empty() && (GetFileAttributesW(customPath.c_str()) != INVALID_FILE_ATTRIBUTES)) {
        return customPath;
    }

    const wchar_t* candidates[] = {
        L"config/hdrfix.ini",
        L"../config/hdrfix.ini",
        L"../../config/hdrfix.ini",
        L"hdr-share-fix/config/hdrfix.ini",
        L"hdrfix.ini"
    };

    for (const auto* p : candidates) {
        if (GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES) {
            wchar_t fullPath[MAX_PATH]{};
            GetFullPathNameW(p, MAX_PATH, fullPath, nullptr);
            return fullPath;
        }
    }
    return L"";
}

bool ConfigManager::Load(const std::wstring& iniPath)
{
    std::wstring resolved = ResolveConfigPath(iniPath);
    m_loadedPath = resolved;

    if (resolved.empty()) {
        // 使用安全默认值
        m_config = AppConfig{};
        return false;
    }

    // [General]
    m_config.general.enable = ReadIniBool(L"General", L"Enable", true, resolved);
    m_config.general.failOpen = ReadIniBool(L"General", L"FailOpen", true, resolved);
    m_config.general.input = ToString(ReadIniString(L"General", L"Input", L"Auto", resolved));
    m_config.general.output = ToString(ReadIniString(L"General", L"Output", L"Rec709", resolved));
    m_config.general.debugOverlay = ReadIniBool(L"General", L"DebugOverlay", false, resolved);
    m_config.general.logLevel = ToString(ReadIniString(L"General", L"LogLevel", L"info", resolved));

    // [HDR]
    m_config.hdr.toneMapper = ToString(ReadIniString(L"HDR", L"ToneMapper", L"Auto", resolved));
    m_config.hdr.sourcePeakNits = ReadIniFloat(L"HDR", L"SourcePeakNits", 0.0f, resolved);
    m_config.hdr.sdrReferenceWhite = ReadIniFloat(L"HDR", L"SDRReferenceWhite", 0.0f, resolved);
    m_config.hdr.exposure = ReadIniFloat(L"HDR", L"Exposure", 0.0f, resolved);
    m_config.hdr.highlightRollOff = ReadIniFloat(L"HDR", L"HighlightRollOff", 1.0f, resolved);

    // [Compatibility]
    m_config.compat.strictVersionCheck = ReadIniBool(L"Compatibility", L"StrictVersionCheck", true, resolved);
    m_config.compat.allowUnknownBuild = ReadIniBool(L"Compatibility", L"AllowUnknownBuild", false, resolved);

    return true;
}

} // namespace hdrfix
