#pragma once
// CaptureProbe/texture_info.h — D3D11 纹理描述快照
// 只读：不做任何资源修改，供 Probe 与后续 ColorDetect 使用。

#include <d3d11.h>
#include <cstdint>
#include <string>

namespace hdrfix {

struct TextureInfo {
    UINT width = 0;
    UINT height = 0;
    UINT mipLevels = 0;
    UINT arraySize = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    DXGI_SAMPLE_DESC sampleDesc{1, 0};
    D3D11_USAGE usage = D3D11_USAGE_DEFAULT;
    UINT bindFlags = 0;
    UINT cpuAccessFlags = 0;
    UINT miscFlags = 0;

    // 读取失败返回 format == DXGI_FORMAT_UNKNOWN 且 width == 0
    static TextureInfo FromTexture2D(ID3D11Texture2D* tex);

    // 单行摘要，用于采样日志：
    // "3840x2160 format=R16G16B16A16_FLOAT bind=SRV|RTV misc=0x0 usage=DEFAULT"
    std::string ToString() const;
};

// 常见 DXGI_FORMAT 的可读名；未收录的返回 nullptr，由调用方按数值打印
const char* FormatName(DXGI_FORMAT fmt);
std::string BindFlagsToString(UINT flags);
std::string MiscFlagsToString(UINT flags);

} // namespace hdrfix
