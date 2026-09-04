// CaptureProbe/texture_info.cpp

#include "CaptureProbe/texture_info.h"

#include <cstdio>

namespace hdrfix {

TextureInfo TextureInfo::FromTexture2D(ID3D11Texture2D* tex)
{
    TextureInfo info;
    if (!tex) {
        return info;
    }
    D3D11_TEXTURE2D_DESC desc{};
    tex->GetDesc(&desc);
    info.width = desc.Width;
    info.height = desc.Height;
    info.mipLevels = desc.MipLevels;
    info.arraySize = desc.ArraySize;
    info.format = desc.Format;
    info.sampleDesc = desc.SampleDesc;
    info.usage = desc.Usage;
    info.bindFlags = desc.BindFlags;
    info.cpuAccessFlags = desc.CPUAccessFlags;
    info.miscFlags = desc.MiscFlags;
    return info;
}

std::string TextureInfo::ToString() const
{
    char buf[256]{};
    const char* name = FormatName(format);
    char fmtBuf[32];
    if (name) {
        strncpy_s(fmtBuf, name, _TRUNCATE);
    } else {
        sprintf_s(fmtBuf, "0x%08X", static_cast<unsigned>(format));
    }
    sprintf_s(buf, "%ux%u format=%s mip=%u array=%u msaa=%u/%u usage=%d bind=%s misc=0x%X",
              static_cast<unsigned>(width), static_cast<unsigned>(height), fmtBuf,
              static_cast<unsigned>(mipLevels), static_cast<unsigned>(arraySize),
              static_cast<unsigned>(sampleDesc.Count), static_cast<unsigned>(sampleDesc.Quality),
              static_cast<int>(usage), BindFlagsToString(bindFlags).c_str(),
              static_cast<unsigned>(miscFlags));
    return buf;
}

const char* FormatName(DXGI_FORMAT fmt)
{
    switch (fmt) {
    case DXGI_FORMAT_UNKNOWN: return "UNKNOWN";
    case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
    case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB";
    case DXGI_FORMAT_B8G8R8X8_UNORM: return "B8G8R8X8_UNORM";
    case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
    case DXGI_FORMAT_R16G16B16A16_UNORM: return "R16G16B16A16_UNORM";
    case DXGI_FORMAT_NV12: return "NV12";
    case DXGI_FORMAT_P010: return "P010";
    case DXGI_FORMAT_P016: return "P016";
    case DXGI_FORMAT_Y410: return "Y410";
    case DXGI_FORMAT_Y210: return "Y210";
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return "R32G32B32A32_FLOAT";
    default: return nullptr;
    }
}

std::string BindFlagsToString(UINT flags)
{
    std::string s;
    auto add = [&](const char* name) {
        if (!s.empty()) s += "|";
        s += name;
    };
    if (flags & D3D11_BIND_VERTEX_BUFFER) add("VB");
    if (flags & D3D11_BIND_INDEX_BUFFER) add("IB");
    if (flags & D3D11_BIND_CONSTANT_BUFFER) add("CB");
    if (flags & D3D11_BIND_SHADER_RESOURCE) add("SRV");
    if (flags & D3D11_BIND_STREAM_OUTPUT) add("SO");
    if (flags & D3D11_BIND_RENDER_TARGET) add("RTV");
    if (flags & D3D11_BIND_DEPTH_STENCIL) add("DSV");
    if (flags & D3D11_BIND_UNORDERED_ACCESS) add("UAV");
    if (s.empty()) s = "0";
    return s;
}

std::string MiscFlagsToString(UINT flags)
{
    std::string s;
    auto add = [&](const char* name) {
        if (!s.empty()) s += "|";
        s += name;
    };
    if (flags & D3D11_RESOURCE_MISC_GENERATE_MIPS) add("GENMIPS");
    if (flags & D3D11_RESOURCE_MISC_SHARED) add("SHARED");
    if (flags & D3D11_RESOURCE_MISC_TEXTURECUBE) add("CUBE");
    if (flags & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX) add("KEYEDMUTEX");
    if (flags & D3D11_RESOURCE_MISC_GDI_COMPATIBLE) add("GDI");
    if (flags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE) add("NTHANDLE");
    if (s.empty()) s = "0";
    return s;
}

} // namespace hdrfix
