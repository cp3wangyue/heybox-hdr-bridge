// CaptureProbe/hdr_state.cpp

#include "CaptureProbe/hdr_state.h"

#include <dxgi.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>

namespace hdrfix {

using Microsoft::WRL::ComPtr;

namespace {

// wingdi.h 的 DISPLAYCONFIG 设备信息请求类型
constexpr UINT32 kGetSourceName = 1;
constexpr UINT32 kGetSdrWhiteLevel = 11;
constexpr UINT32 kGetAdvancedColorInfo = 12;
constexpr UINT32 kQdcOnlyActivePaths = 2;

struct DisplayConfigSourceName {
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    WCHAR viewGdiDeviceName[32]; // CCHDEVICENAME
};

bool GetSdrWhiteLevelFor(const WCHAR* gdiDeviceName, float* outNits)
{
    *outNits = 0.f;

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
        if (wcscmp(src.viewGdiDeviceName, gdiDeviceName) != 0) {
            continue;
        }
        DISPLAYCONFIG_SDR_WHITE_LEVEL sdr{};
        sdr.header.type = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(kGetSdrWhiteLevel);
        sdr.header.size = sizeof(sdr);
        sdr.header.adapterId = paths[i].targetInfo.adapterId;
        sdr.header.id = paths[i].targetInfo.id;
        if (::DisplayConfigGetDeviceInfo(&sdr.header) != ERROR_SUCCESS) {
            return false;
        }
        // SDRWhiteLevel 单位为 1/1000 × 80 nits：nits = value * 80 / 1000
        *outNits = sdr.SDRWhiteLevel * 80.0f / 1000.0f;
        return true;
    }
    return false;
}

bool GetAdvancedColorFor(const WCHAR* gdiDeviceName, bool* supported, bool* enabled)
{
    *supported = false;
    *enabled = false;

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

    constexpr UINT32 kAcSupported = 0x1;
    constexpr UINT32 kAcEnabled = 0x2;

    for (UINT32 i = 0; i < numPaths; ++i) {
        DisplayConfigSourceName src{};
        src.header.type = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(kGetSourceName);
        src.header.size = sizeof(src);
        src.header.adapterId = paths[i].sourceInfo.adapterId;
        src.header.id = paths[i].sourceInfo.id;
        if (::DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS) {
            continue;
        }
        if (wcscmp(src.viewGdiDeviceName, gdiDeviceName) != 0) {
            continue;
        }
        DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO aci{};
        aci.header.type = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(kGetAdvancedColorInfo);
        aci.header.size = sizeof(aci);
        aci.header.adapterId = paths[i].targetInfo.adapterId;
        aci.header.id = paths[i].targetInfo.id;
        if (::DisplayConfigGetDeviceInfo(&aci.header) != ERROR_SUCCESS) {
            return false;
        }
        *supported = (aci.value & kAcSupported) != 0;
        *enabled = (aci.value & kAcEnabled) != 0;
        // 部分驱动在开启时只置 ENABLED 位
        *supported = *supported || *enabled;
        return true;
    }
    return false;
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

            float nits = 0.f;
            if (GetSdrWhiteLevelFor(state.gdiDeviceName.c_str(), &nits) && nits > 0.f) {
                state.sdrWhiteNits = nits;
            }
            GetAdvancedColorFor(state.gdiDeviceName.c_str(), &state.hdrSupported, &state.hdrEnabled);
            // HDR 开启时 DXGI 输出颜色空间会切换到 PQ；以此兜底
            if (!state.hdrEnabled &&
                state.colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) {
                state.hdrEnabled = true;
            }

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
