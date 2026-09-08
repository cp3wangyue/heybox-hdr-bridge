#pragma once
// CaptureProbe/hdr_control.h — 系统 HDR 开关（自动化测试与稳定性验证）
// 只改显示器高级色彩状态，不做任何帧/客户端操作；调用方负责在测试后恢复原状态。

#include <string>

namespace hdrfix {

// 打开/关闭指定显示器的 HDR。
// 优先 Win11 SET_HDR_STATE(16)，失败回退旧版 SET_ADVANCED_COLOR_STATE(10)；
// 成功后轮询 DXGI 色彩空间验证实际生效（PQ 切换可能需要 1~2 秒）。
// 返回 0=成功（已验证生效）；非 0=失败（原状态不变或未验证）。
int SetOutputHdr(const std::wstring& gdiDeviceName, bool enable, int timeoutMs = 5000);

} // namespace hdrfix
