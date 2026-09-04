#pragma once
// CaptureProbe/d3d11_observer.h — 只读 D3D11 观察器（P2，计划书 §6.1：先做观察器，再做修改器）
//
// 原理：IAT Hook 目标模块对 D3D11CreateDevice 的静态导入 → 拿到 SDK 的设备 →
//       对该设备实例做 vtable 克隆，只挂这一个实例的 CreateTexture2D(vtable[5]) 与
//       Release(vtable[2]，仅用于自动回收注册表，不改行为)。
//       不修改任何帧数据；其他设备实例（Chromium/ANGLE）不受影响。
//
// 观察输出：
//   [DEVICE] 设备创建参数（FeatureLevel、Flags——含 SINGLETHREADED 检查）
//   [CREATE] 每种 (尺寸, 格式, bind, misc) 组合首次出现时记录完整纹理描述
// 像素采样（回答"HDR 信息在哪一步丢失"）经 TakePixelSample 由 pixel_sampler 驱动。

#include <d3d11.h>
#include <memory>
#include <string>

namespace hdrfix {

struct ObserverConfig {
    std::wstring targetModule = L"VolcEngineRTC.dll"; // 被观察模块（IAT 归属）
    UINT minTextureWidth = 640;                       // 采样候选过滤
    UINT minTextureHeight = 360;
};

// 一次像素采样的统计结果
struct PixelSampleResult {
    bool ok = false;
    ID3D11Texture2D* source = nullptr;
    UINT width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    float maxChannel = 0.f;      // 全帧最大单通道值（FP16 有效；BGRA/NV12-Y 为原值域）
    float overWhiteFrac = 0.f;   // FP16：max(R,G,B) > 1.0 的像素占比（HDR 高光存在性）
    float brightFrac = 0.f;      // BGRA：亮度 >= 250/255 占比；NV12：Y >= 250 占比
    float underBlackFrac = 0.f;  // NV12：Y <= 16 占比（limited-range 黑位检查）
    float meanLuma = 0.f;        // 平均亮度（Rec.709 系数；FP16 线性域 / BGRA 0-255 / NV12 Y 0-255）
    UINT64 sampleCount = 0;      // 本会话累计采样次数
};

class D3D11Observer {
public:
    static D3D11Observer& Instance();

    // 安装 IAT hook；目标模块未加载时返回 false（调用方稍后重试）
    bool Start(const ObserverConfig& cfg);
    // 恢复原状（vtable 指针与 IAT 条目），线程安全
    void Stop();

    // 从注册表挑一个候选纹理做节流像素采样（内部 CopyResource→staging→Map，只读）。
    // 设备为 SINGLETHREADED 或无候选时返回 false。用 SEH 包裹，绝不影响宿主。
    bool TakePixelSample(PixelSampleResult* out);

private:
    D3D11Observer() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hdrfix
