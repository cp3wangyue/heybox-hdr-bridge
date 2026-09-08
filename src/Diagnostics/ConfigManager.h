#pragma once
// Diagnostics/ConfigManager.h — 配置文件读取与管理
//
// 负责读取并解析 config/hdrfix.ini，提供类型安全的运行期配置访问与默认值保证。

#include <string>

namespace hdrfix {

struct GeneralConfig {
    bool enable = true;
    bool failOpen = true;
    std::string input = "Auto";
    std::string output = "sRGB";
    bool debugOverlay = false;
    std::string logLevel = "info";
};

struct HdrConfig {
    std::string toneMapper = "Auto"; // Auto, BT2390, OBSReinhard, Reinhard, Hable, ACES, LumaHuePreserve, Clamp
    float sourcePeakNits = 0.0f;     // <= 0 表示 Auto
    float sdrReferenceWhite = 0.0f;  // <= 0 表示 System
    float exposure = 0.0f;
    float highlightRollOff = 1.0f;
};

struct CompatibilityConfig {
    bool strictVersionCheck = false;
    bool allowUntestedCompatible = true;
    bool allowUnknownBuild = true;
};

struct AppConfig {
    GeneralConfig general;
    HdrConfig hdr;
    CompatibilityConfig compat;
};

class ConfigManager {
public:
    static ConfigManager& Instance();

    // 从指定路径或默认候选路径加载配置
    bool Load(const std::wstring& iniPath = L"");

    const AppConfig& GetConfig() const { return m_config; }
    void SetConfig(const AppConfig& cfg) { m_config = cfg; }

    std::wstring GetLoadedPath() const { return m_loadedPath; }

private:
    ConfigManager();
    std::wstring ResolveConfigPath(const std::wstring& customPath);

    AppConfig m_config{};
    std::wstring m_loadedPath;
};

} // namespace hdrfix
