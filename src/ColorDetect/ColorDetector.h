#pragma once
// ColorDetect/ColorDetector.h — 输入格式与色彩空间 AutoDetect 判定树（计划书 §10.1）
//
// 按照计划书 §10.1 严格执行判定顺序：
//   1. 确认系统/目标输出是否 Advanced Color / HDR；
//   2. 评估捕获纹理请求格式（FP16 scRGB vs RGB10A2 PQ vs BGRA8 SDR）；
//   3. 给出精准决策：ElevateAndConvert、Passthrough 或 DiagnoseOnly；
//   4. 失败安全：无法确定时永远倾向旁路 (Fail-Open)。

#include <dxgiformat.h>
#include <string>

namespace hdrfix {

enum class DecisionAction {
    Passthrough,         // 原生透传，不执行修改
    ElevateAndConvert,   // 升级为 FP16 并在消费点执行色调映射
    DiagnoseOnly         // 格式未决，仅收集诊断并旁路
};

enum class DetectedColorSpace {
    Unknown,
    SDR_Rec709,
    HDR_scRGB,
    HDR_PQ
};

struct DecisionResult {
    DecisionAction action = DecisionAction::Passthrough;
    DetectedColorSpace colorSpace = DetectedColorSpace::Unknown;
    float sdrWhiteNits = 280.0f;
    std::string reason;
};

class ColorDetector {
public:
    // 依据当前系统 HDR 状态与客户端请求格式做出介入决策
    static DecisionResult Evaluate(DXGI_FORMAT requestedFormat);

    // 针对显式参数的无副作用评估函数（供单元测试与模拟）
    static DecisionResult EvaluateExplicit(bool systemHdrEnabled, float sdrWhiteNits, DXGI_FORMAT requestedFormat);
};

} // namespace hdrfix
