// CaptureProbe/hdr_control.cpp

#include "CaptureProbe/hdr_control.h"

#include <windows.h>

#include <chrono>
#include <thread>

#include "CaptureProbe/hdr_state.h"

namespace hdrfix {

namespace {

// 与 wingdi.h 一致的 Win11 结构（部分 SDK 配置下需要自带定义）
struct SetHdrState {
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    union {
        struct {
            UINT32 enableHdr : 1;
            UINT32 reserved : 31;
        } bits;
        UINT32 value;
    } u;
};

struct SetAdvancedColorState {
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    union {
        struct {
            UINT32 enableAdvancedColor : 1;
            UINT32 reserved : 31;
        } bits;
        UINT32 value;
    } u;
};

bool FindPathForGdiDevice(const WCHAR* gdiDeviceName, DISPLAYCONFIG_PATH_INFO* outPath)
{
    UINT32 numPaths = 0, numModes = 0;
    if (::GetDisplayConfigBufferSizes(2 /*QDC_ONLY_ACTIVE_PATHS*/, &numPaths, &numModes) != ERROR_SUCCESS) {
        return false;
    }
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(numPaths);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(numModes);
    if (::QueryDisplayConfig(2, &numPaths, paths.data(), &numModes, modes.data(), nullptr) != ERROR_SUCCESS) {
        return false;
    }
    struct SourceName {
        DISPLAYCONFIG_DEVICE_INFO_HEADER header;
        WCHAR name[32];
    };
    for (UINT32 i = 0; i < numPaths; ++i) {
        SourceName src{};
        src.header.type = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(1 /*GET_SOURCE_NAME*/);
        src.header.size = sizeof(src);
        src.header.adapterId = paths[i].sourceInfo.adapterId;
        src.header.id = paths[i].sourceInfo.id;
        if (::DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS) {
            continue;
        }
        if (wcscmp(src.name, gdiDeviceName) == 0) {
            *outPath = paths[i];
            return true;
        }
    }
    return false;
}

bool HdrActiveFor(const std::wstring& gdiDeviceName)
{
    for (const auto& st : QueryOutputHdrStates()) {
        if (st.gdiDeviceName == gdiDeviceName) {
            return st.hdrEnabled;
        }
    }
    return false;
}

} // namespace

int SetOutputHdr(const std::wstring& gdiDeviceName, bool enable, int timeoutMs)
{
    DISPLAYCONFIG_PATH_INFO path{};
    if (!FindPathForGdiDevice(gdiDeviceName.c_str(), &path)) {
        return 1; // 找不到该显示器的活动路径
    }

    LONG hr = ERROR_NOT_SUPPORTED;
    {
        SetHdrState pkt{};
        pkt.header.type = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(16 /*SET_HDR_STATE*/);
        pkt.header.size = sizeof(pkt);
        pkt.header.adapterId = path.targetInfo.adapterId;
        pkt.header.id = path.targetInfo.id;
        pkt.u.bits.enableHdr = enable ? 1u : 0u;
        hr = ::DisplayConfigSetDeviceInfo(&pkt.header);
    }
    if (hr != ERROR_SUCCESS) {
        SetAdvancedColorState legacy{};
        legacy.header.type = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(10 /*SET_ADVANCED_COLOR_STATE*/);
        legacy.header.size = sizeof(legacy);
        legacy.header.adapterId = path.targetInfo.adapterId;
        legacy.header.id = path.targetInfo.id;
        legacy.u.bits.enableAdvancedColor = enable ? 1u : 0u;
        hr = ::DisplayConfigSetDeviceInfo(&legacy.header);
        if (hr != ERROR_SUCCESS) {
            return 2; // 两种开关都被拒绝
        }
    }

    // 轮询验证：PQ 色彩空间出现/消失
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        if (HdrActiveFor(gdiDeviceName) == enable) {
            return 0;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            return 3; // 已提交但未在时限内生效
        }
    }
}

} // namespace hdrfix
