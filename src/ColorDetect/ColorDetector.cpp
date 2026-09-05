// ColorDetect/ColorDetector.cpp — AutoDetect 判定树实现
#include "ColorDetect/ColorDetector.h"

#include "CaptureProbe/hdr_state.h"

namespace hdrfix {

DecisionResult ColorDetector::Evaluate(DXGI_FORMAT requestedFormat)
{
    bool systemHdr = false;
    float sdrWhite = 280.0f;

    auto states = QueryOutputHdrStates();
    for (const auto& s : states) {
        if (s.hdrEnabled) {
            systemHdr = true;
            if (s.sdrWhiteNits > 10.0f) sdrWhite = s.sdrWhiteNits;
            break;
        }
    }

    return EvaluateExplicit(systemHdr, sdrWhite, requestedFormat);
}

DecisionResult ColorDetector::EvaluateExplicit(bool systemHdrEnabled, float sdrWhiteNits, DXGI_FORMAT requestedFormat)
{
    DecisionResult r;
    r.sdrWhiteNits = sdrWhiteNits;

    // 1. 确认系统/目标输出是否处于 Advanced Color / HDR 状态
    if (!systemHdrEnabled) {
        r.action = DecisionAction::Passthrough;
        r.colorSpace = DetectedColorSpace::SDR_Rec709;
        r.reason = "Windows HDR 未开启，系统处于标准 SDR 空间，完全旁路透传";
        return r;
    }

    // 2. 读取捕获纹理请求 Format 判定
    switch (requestedFormat) {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
        // HDR 开启下客户端请求 8-bit SDR 池：这是导致 DWM 压平高光的根因节点
        r.action = DecisionAction::ElevateAndConvert;
        r.colorSpace = DetectedColorSpace::HDR_scRGB;
        r.reason = "Windows HDR 开启但捕获请求为 8-bit SDR，提升为 FP16 scRGB 池以保留高光并执行色调映射";
        return r;

    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        r.action = DecisionAction::ElevateAndConvert;
        r.colorSpace = DetectedColorSpace::HDR_scRGB;
        r.reason = "捕获请求为原生 FP16，按 scRGB 物理模型执行色调映射";
        return r;

    case DXGI_FORMAT_R10G10B10A2_UNORM:
        r.action = DecisionAction::DiagnoseOnly;
        r.colorSpace = DetectedColorSpace::HDR_PQ;
        r.reason = "检测到 10-bit HDR (PQ/Rec.2100) 格式，当前进入只读诊断记录";
        return r;

    default:
        // 无法确定格式，绝不强行修改
        r.action = DecisionAction::DiagnoseOnly;
        r.colorSpace = DetectedColorSpace::Unknown;
        r.reason = "捕获格式未知，进入安全诊断模式并旁路原生链路 (Fail-Open)";
        return r;
    }
}

} // namespace hdrfix
