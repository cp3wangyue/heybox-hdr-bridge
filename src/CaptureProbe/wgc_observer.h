#pragma once
// CaptureProbe/wgc_observer.h — 并行 FP16 观察池（无损：不接触 SDK 路径的任何对象）
//
// 原理：自有 D3D11 设备 → WGC CreateFreeThreaded(FP16) 池 → 对主显示器开自己的
//       CaptureSession → 轮询每帧：SystemRelativeTime(fps) + CopyResource
//       等尺寸纹理机制 + 节流 FP16 像素统计
//       （overWhite = max(R,G,B)>1.0 占比，量化 BGRA 路径丢失的 HDR 高光）。
//
// 与 BGRA 路径采样对照：FP16 overWhite% vs 客户端 BGRA bright%@250。
// 分辨率变化时按 item 尺寸 Recreate；Stop 停止会话并释放。

#include <string>

namespace hdrfix {

class WgcObserver {
public:
    static WgcObserver& Instance();

    // 启动观察管线（幂等）。monitor 为空 = 主显示器。
    bool Start();
    void Stop(); // 线程安全；结束后资源全部释放

private:
    WgcObserver() = default;
    struct Impl;
    Impl* impl_ = nullptr;
};

} // namespace hdrfix
