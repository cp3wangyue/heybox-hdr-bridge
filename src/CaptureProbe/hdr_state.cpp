// CaptureProbe/hdr_state.cpp

#include "CaptureProbe/hdr_state.h"

#include <dxgi.h>
#include <wrl/client.h>
#include <cstring>
#include <vector>

namespace hdrfix {

using Microsoft::WRL::ComPtr;

namespace {

// wingdi.h 的 DISPLAYCONFIG 设备信息请求类型（务必与 SDK 头一致！）
constexpr UINT32 kGetSourceName = 1;
constexpr UINT32 kGetAdvancedColorInfo = 9;   // 旧版查询（注意不是 12）
constexpr UINT32 kSetAdvancedColorState = 10; // 旧版 HDR/高级色彩开关
constexpr UINT32 kGetSdrWhiteLevel = 11;
constexpr UINT32 kGetMonitorSpecialization = 12;
constexpr UINT32 kGetAdvancedColorInfo2 = 15; // Win11：含 highDynamicRangeUserEnabled / activeColorMode
constexpr UINT32 kSetHdrState = 16;           // Win11：HDR 开关
constexpr UINT32 kQdcOnlyActivePaths = 2;

struct DisplayConfigSourceName {
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    WCHAR viewGdiDeviceName[32]; // CCHDEVICENAME
};

struct AdvancedColorInfo2 {
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    union {
        struct {
            UINT32 advancedColorSupported : 1;
            UINT32 advancedColorActive : 1;
            UINT32 reserved1 : 1;
            UINT32 advancedColorLimitedByPolicy : 1;
            UINT32 highDynamicRangeSupported : 1;
            UINT32 highDynamicRangeUserEnabled : 1;
            UINT32 wideColorSupported : 1;
            UINT32 wideColorUserEnabled : 1;
            UINT32 reserved : 24;
        } bits;
        UINT32 value;
    } u;
    DISPLAYCONFIG_ADVANCED_COLOR_MODE activeColorMode;
};

enum class PathKind { Source, Target };

bool FindPathForGdiDevice(const WCHAR* gdiDeviceName, DISPLAYCONFIG_PATH_INFO* outPath)
{
    UINT32 numPaths = 0, numModes = 0;
    if (::GetDisplayConfigBufferSizes(kQdcOnlyActivePaths, &numPaths, &numModes) != ERROR_SUCCESS) {
        return false;
    }
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(numPaths);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(numModes);
    if (::QueryDisplayConfig(kQdcOnlyActivePaths, &numPaths, paths.data(), &numModes,
                             modes.data(), nullptr) != ERROR_SUCCESS) {
        return false;
    }
    for (UINT32 i = 0; i < numPaths; ++i) {
        DisplayConfigSourceName src{};
        src.header.type = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(kGetSourceName);
        src.header.size = sizeof(src);
        src.header.adapterId = paths[i].sourceInfo.adapterId;
        src.header.id = paths[i].sourceInfo.id;
        if (::DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS) {
            continue;
        }
        if (wcscmp(src.viewGdiDeviceName, gdiDeviceName) == 0) {
            *outPath = paths[i];
            return true;
        }
    }
    return false;
}

bool GetSdrWhiteLevelFor(const DISPLAYCONFIG_PATH_INFO& path, float* outNits)
{
    *outNits = 0.f;
    DISPLAYCONFIG_SDR_WHITE_LEVEL sdr{};
    sdr.header.type = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(kGetSdrWhiteLevel);
    sdr.header.size = sizeof(sdr);
    sdr.header.adapterId = path.targetInfo.adapterId;
    sdr.header.id = path.targetInfo.id;
    if (::DisplayConfigGetDeviceInfo(&sdr.header) != ERROR_SUCCESS) {
        return false;
    }
    // SDRWhiteLevel 单位为 1/1000 × 80 nits：nits = value * 80 / 1000
    *outNits = sdr.SDRWhiteLevel * 80.0f / 1000.0f;
    return true;
}

bool GetAdvancedColor2For(const DISPLAYCONFIG_PATH_INFO& path, AdvancedColorInfo2* out)
{
    *out = {};
    out->header.type = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(kGetAdvancedColorInfo2);
    out->header.size = sizeof(AdvancedColorInfo2);
    out->header.adapterId = path.targetInfo.adapterId;
    out->header.id = path.targetInfo.id;
    return ::DisplayConfigGetDeviceInfo(&out->header) == ERROR_SUCCESS;
}

} // namespace

std::vector<OutputHdrState> QueryOutputHdrStates()
{
    std::vector<OutputHdrState> states;

    ComPtr<IDXGIFactory1> factory;
    if (FAILED(::CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf())))) {
        return states;
    }

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, adapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++a) {
        ComPtr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, output.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++o) {
            DXGI_OUTPUT_DESC desc{};
            if (FAILED(output->GetDesc(&desc)) || !desc.AttachedToDesktop) {
                continue;
            }

            OutputHdrState state;
            state.gdiDeviceName = desc.DeviceName;
            state.colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
            state.bitsPerColor = 8;

            ComPtr<IDXGIOutput6> output6;
            if (SUCCEEDED(output.As(&output6))) {
                DXGI_OUTPUT_DESC1 desc1{};
                if (SUCCEEDED(output6->GetDesc1(&desc1))) {
                    state.gdiDeviceName = desc1.DeviceName;
                    state.colorSpace = desc1.ColorSpace;
                    state.bitsPerColor = desc1.BitsPerColor;
                }
            }

            DISPLAYCONFIG_PATH_INFO path{};
            if (FindPathForGdiDevice(state.gdiDeviceName.c_str(), &path)) {
                float nits = 0.f;
                if (GetSdrWhiteLevelFor(path, &nits) && nits > 0.f) {
                    state.sdrWhiteNits = nits;
                }
                AdvancedColorInfo2 aci2{};
                if (GetAdvancedColor2For(path, &aci2)) {
                    state.hdrSupported = aci2.u.bits.advancedColorSupported != 0;
                    state.hdrUserEnabled = aci2.u.bits.highDynamicRangeUserEnabled != 0;
                    state.wideColorUser = aci2.u.bits.wideColorUserEnabled != 0;
                }
            }

            // 权威判定：输出色彩空间切换到 PQ 即 HDR 生效（用户开关与实际生效可能短暂不一致）
            state.hdrEnabled =
                state.colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;

            states.push_back(std::move(state));
        }
    }
    return states;
}

const char* ColorSpaceName(DXGI_COLOR_SPACE_TYPE cs)
{
    switch (cs) {
    case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709: return "RGB_FULL_G22_NONE_P709";
    case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709: return "RGB_FULL_G10_NONE_P709";
    case DXGI_COLOR_SPACE_RGB_STUDIO_G22_NONE_P709: return "RGB_STUDIO_G22_NONE_P709";
    case DXGI_COLOR_SPACE_YCBCR_FULL_G22_NONE_P709_X601: return "YCBCR_FULL_G22_NONE_P709_X601";
    case DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601: return "YCBCR_STUDIO_G22_LEFT_P601";
    case DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P601: return "YCBCR_FULL_G22_LEFT_P601";
    case DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709: return "YCBCR_STUDIO_G22_LEFT_P709";
    case DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709: return "YCBCR_FULL_G22_LEFT_P709";
    case DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P2020: return "YCBCR_STUDIO_G22_LEFT_P2020";
    case DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P2020: return "YCBCR_FULL_G22_LEFT_P2020";
    case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020: return "RGB_FULL_G2084_NONE_P2020";
    case DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020: return "YCBCR_STUDIO_G2084_LEFT_P2020";
    case DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020: return "RGB_STUDIO_G2084_NONE_P2020";
    case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P2020: return "RGB_FULL_G22_NONE_P2020";
    case DXGI_COLOR_SPACE_YCBCR_STUDIO_GHLG_TOPLEFT_P2020: return "YCBCR_STUDIO_GHLG_TOPLEFT_P2020";
    case DXGI_COLOR_SPACE_YCBCR_FULL_GHLG_TOPLEFT_P2020: return "YCBCR_FULL_GHLG_TOPLEFT_P2020";
    default: return nullptr;
    }
}

} // namespace hdrfix
