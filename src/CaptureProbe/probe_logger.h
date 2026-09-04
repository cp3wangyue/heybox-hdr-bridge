#pragma once
// CaptureProbe/probe_logger.h — 低开销采样日志（附录 A 字段 / §3.2 诊断要求）
// 默认每秒抽样落盘；force=true 的记录无视限流（Debug 帧级采样）。
// 线程安全；文件为逐行 flush，崩溃时最多丢一行。

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

#include "CaptureProbe/texture_info.h"

namespace hdrfix {

struct FrameLogRecord {
    uint64_t frame = 0;
    uint32_t tid = 0;
    int64_t qpc = 0;                       // QueryPerformanceCounter 原始值
    const void* sourceDevice = nullptr;    // ID3D11Device*
    const void* sourceTexture = nullptr;   // ID3D11Texture2D*
    TextureInfo tex{};
    bool windowsHdr = false;               // 系统 HDR 开关（主输出）
    std::string outputColorSpace;          // 如 RGB_FULL_G2084_NONE_P2020
    float sdrWhiteNits = 0.f;
    std::string path;                      // 如 ToneMapScRGB->Rec709RGB->NV12Limited
    float shaderMs = 0.f;
    float convertMs = 0.f;
    float totalMs = 0.f;
    std::string encoderInputFormat;        // 如 NV12
    bool bypass = true;
    bool force = false;                    // true 时无视采样限流
};

class ProbeLogger {
public:
    static ProbeLogger& Instance();

    // minInterval：两条落盘记录之间的最小间隔（默认 1s 采样）；force 记录不受限
    bool Start(const std::wstring& filePath,
               std::chrono::milliseconds minInterval = std::chrono::milliseconds(1000));
    void Stop();
    bool IsRunning() const;

    void LogFrame(const FrameLogRecord& r);

    // 限流窗口内被丢弃的记录数（下一条落盘记录会带上）
    uint64_t DroppedCount() const;

private:
    ProbeLogger() = default;
    ~ProbeLogger() = default;
    ProbeLogger(const ProbeLogger&) = delete;
    ProbeLogger& operator=(const ProbeLogger&) = delete;

    struct State {
        std::mutex mutex;
        FILE* file = nullptr;
        std::chrono::steady_clock::time_point lastWrite{};
        std::chrono::milliseconds minInterval{1000};
        uint64_t dropped = 0;
    };
    // 单例生命周期，常驻不释放（避免析构顺序问题）
    State* s_ = new State();
};

} // namespace hdrfix
