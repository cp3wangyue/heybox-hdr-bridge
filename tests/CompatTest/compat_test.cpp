// tests/CompatTest/compat_test.cpp
// 版本兼容体系与能力检测自动化测试

#include <windows.h>
#include <cstdio>
#include <cassert>
#include <string>

#include "Diagnostics/CompatibilityManager.h"
#include "Diagnostics/ConfigManager.h"
#include "Diagnostics/SafetyGuard.h"

int main()
{
    printf("====================================================\n");
    printf("  HEYBOX HDR Bridge — 版本兼容与能力检测测试\n");
    printf("====================================================\n\n");

    ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    auto& mgr = hdrfix::CompatibilityManager::Instance();

    // 1. 验证 compat.json 数据库加载
    bool loaded = mgr.LoadCompatDatabase();
    printf("[1] compat.json 数据库加载: %s\n", loaded ? "PASS (成功解析白名单)" : "FAIL");
    if (!loaded) {
        printf("[ERROR] 未能加载或解析 compat.json\n");
        return 1;
    }

    // 2. 运行时能力与环境评估
    auto d1 = mgr.Evaluate(nullptr, ABI::Windows::Graphics::DirectX::DirectXPixelFormat_B8G8R8A8UIntNormalized);
    printf("[2] 当前环境四级决策评估:\n");
    printf("%s\n", mgr.FormatReport(d1).c_str());

    // 必须属于 Verified 或 UntestedCompatible
    if (d1.tier != hdrfix::CompatibilityTier::Verified &&
        d1.tier != hdrfix::CompatibilityTier::UntestedCompatible) {
        printf("[FAIL] 当前合法支持环境判定为: %d\n", static_cast<int>(d1.tier));
        return 1;
    }
    printf("  -> [PASS] 决策允许介入，符合预期。\n\n");

    // 3. 验证异常格式判定（非 BGRA8/FP16，例如未知格式 999）
    auto dUnsupported = mgr.Evaluate(nullptr, static_cast<ABI::Windows::Graphics::DirectX::DirectXPixelFormat>(999));
    printf("[3] 异常格式请求能力检测 (Format=999):\n");
    printf("  Decision: %s | Action: %s\n",
           dUnsupported.tier == hdrfix::CompatibilityTier::Incompatible ? "Incompatible" : "Other",
           dUnsupported.bridgeEnabled ? "Enabled" : "Fail-open");
    if (dUnsupported.tier != hdrfix::CompatibilityTier::Incompatible || dUnsupported.bridgeEnabled) {
        printf("[FAIL] 异常格式未正确拦截与 Fail-open！\n");
        return 1;
    }
    printf("  -> [PASS] 异常格式成功触发 Incompatible 与 Fail-open 原生透传！\n\n");

    // 4. 验证 Crash Marker / SafetyGuard 旁路联动
    printf("[4] 安全降级测试 (SafetyGuard 旁路联动与 Fail-open):\n");
    hdrfix::SafetyGuard::Instance().TriggerManualBypass(true);
    bool canIntercept = hdrfix::SafetyGuard::Instance().CanIntercept();
    printf("  SafetyGuard::CanIntercept: %s\n", canIntercept ? "true" : "false");
    if (canIntercept) {
        printf("[FAIL] 降级旁路未生效！\n");
        return 1;
    }
    printf("  -> [PASS] 安全旁路成功阻断介入并保障 Fail-open！\n\n");
    hdrfix::SafetyGuard::Instance().TriggerManualBypass(false);


    ::CoUninitialize();

    printf("----------------------------------------------------\n");
    printf("  [PASS] 四级版本兼容模型与能力检测全部通过！\n");
    printf("----------------------------------------------------\n\n");

    return 0;
}
