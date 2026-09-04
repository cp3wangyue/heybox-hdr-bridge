#pragma once
// CaptureProbe/hdr_state.h — 系统 HDR 状态与 SDR 参考白查询（§6.2 HDR State 字段、§10.1 AutoDetect 输入）
// 只读。实现方式与 tools/DisplayConfig.psm1 相同：DXGI IDXGIOutput6 + DisplayConfigGetDeviceInfo。

#include <dxgi1_6.h>
#include <string>
#include <vector>

namespace hdrfix {

struct OutputHdrState {
    std::wstring gdiDeviceName;       // 如 \\.\DISPLAY1
    DXGI_COLOR_SPACE_TYPE colorSpace; // HDR 输出通常为 DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
    UINT bitsPerColor = 0;
    float sdrWhiteNits = 0.f;         // 用户可调节的 SDR 参考白（§8.4：不要硬编码 203/300 nits）
    bool hdrEnabled = false;
    bool hdrSupported = false;
};

// 枚举所有活动的桌面输出；失败逐项留空而不是整体失败（Fail-open 原则）
std::vector<OutputHdrState> QueryOutputHdrStates();

// DXGI_COLOR_SPACE_TYPE 的可读名；未收录返回 nullptr
const char* ColorSpaceName(DXGI_COLOR_SPACE_TYPE cs);

} // namespace hdrfix
