#pragma once
// Diagnostics/CompatibilityManager.h — 四级运行时兼容性与能力检测体系
//
// 目标模型：
//   Verified           = compat.json 中明确验证过的版本
//   UntestedCompatible = 版本未知，但运行时能力检测全部通过
//   Incompatible       = 必需接口、模块、捕获格式或调用链不符合要求
//   DiagnoseOnly       = 无法确定或存在安全异常，默认不修改帧，只记录诊断
//
// 原则：
//   1. 不因版本号变动武断拦截；
//   2. 能力检测优先于版本号；
//   3. 所有异常必须 Fail-open（原生透传）；
//   4. 提供标准化诊断输出。

#include <windows.h>
#include <d3d11.h>
#include <windows.graphics.directx.h>
#include <mutex>
#include <string>
#include <vector>

namespace hdrfix {

enum class CompatibilityTier {
    Verified = 0,           // compat.json 明确验证过的版本
    UntestedCompatible = 1, // 版本未知，但能力检测通过
    Incompatible = 2,       // 关键接口或格式不兼容
    DiagnoseOnly = 3        // 处于降级或仅诊断模式
};

struct ClientCompatRule {
    std::string name;
    std::string version;
    std::string status;
    std::vector<std::pair<std::string, std::string>> verifiedModules; // moduleName -> version
};

struct CompatibilityDecision {
    CompatibilityTier tier = CompatibilityTier::DiagnoseOnly;
    std::string heyboxVersion = "Unknown";
    bool heyboxVerified = false;
    std::string rtcVersion = "Unknown";
    bool rtcVerified = false;
    bool rtcLoaded = false;
    bool wgcFramePoolAvailable = false;
    std::string requestedFormat = "None";
    bool formatSupported = false;
    bool fp16Constructible = false;
    std::string reason;
    bool bridgeEnabled = false;
};

class CompatibilityManager {
public:
    static CompatibilityManager& Instance();

    // 加载并解析 compat.json
    bool LoadCompatDatabase(const std::wstring& jsonPath = L"");

    // 探测当前宿主可执行程序版本
    std::string DetectHeyboxVersion(const std::wstring& exePath = L"");

    // 探测当前 RTC 模块版本
    std::string DetectVolcEngineRtcVersion(HMODULE rtcModule = nullptr);

    // 综合判定兼容性决策
    CompatibilityDecision Evaluate(
        ID3D11Device* d3dDevice = nullptr,
        ABI::Windows::Graphics::DirectX::DirectXPixelFormat requestedPixelFormat =
            ABI::Windows::Graphics::DirectX::DirectXPixelFormat_B8G8R8A8UIntNormalized);

    const CompatibilityDecision& GetLastDecision() const { return m_lastDecision; }

    // 格式化输出标准化诊断报告
    std::string FormatReport(const CompatibilityDecision& d) const;

    // 输出报告至控制台/日志
    void LogDecisionIfNeeded(const CompatibilityDecision& d);

    void Reset();

private:
    CompatibilityManager();

    std::wstring ResolveDatabasePath(const std::wstring& customPath);
    bool CheckWgcAvailability();
    bool CheckFp16Capability(ID3D11Device* device);

    std::recursive_mutex m_mutex;
    std::vector<ClientCompatRule> m_rules;
    std::wstring m_loadedPath;
    CompatibilityDecision m_lastDecision;
    bool m_hasLogged = false;
    std::string m_lastLoggedReport;
};

} // namespace hdrfix
