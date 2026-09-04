// CaptureProbe/probe_logger.cpp

#include "CaptureProbe/probe_logger.h"

#include <windows.h>

#include <cstdio>
#include <vector>

namespace hdrfix {

ProbeLogger& ProbeLogger::Instance()
{
    static ProbeLogger logger;
    return logger;
}

bool ProbeLogger::Start(const std::wstring& filePath, std::chrono::milliseconds minInterval)
{
    std::lock_guard<std::mutex> lock(s_->mutex);
    if (s_->file) {
        return true;
    }
    errno_t err = _wfopen_s(&s_->file, filePath.c_str(), L"w");
    if (err != 0 || !s_->file) {
        return false;
    }
    s_->minInterval = minInterval;
    s_->lastWrite = (std::chrono::steady_clock::time_point::min)();
    s_->dropped = 0;
    std::fwprintf(s_->file, L"# probe logger started, minInterval=%lld ms\n",
                  static_cast<long long>(minInterval.count()));
    fflush(s_->file);
    return true;
}

void ProbeLogger::Stop()
{
    std::lock_guard<std::mutex> lock(s_->mutex);
    if (!s_->file) {
        return;
    }
    std::fwprintf(s_->file, L"# probe logger stopped, dropped=%llu\n",
                  static_cast<unsigned long long>(s_->dropped));
    fclose(s_->file);
    s_->file = nullptr;
}

bool ProbeLogger::IsRunning() const
{
    std::lock_guard<std::mutex> lock(s_->mutex);
    return s_->file != nullptr;
}

uint64_t ProbeLogger::DroppedCount() const
{
    std::lock_guard<std::mutex> lock(s_->mutex);
    return s_->dropped;
}

namespace {

std::string NowHmsMs()
{
    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    char buf[32]{};
    sprintf_s(buf, "%02u:%02u:%02u.%03u", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return buf;
}

std::string PtrHex(const void* p)
{
    char buf[24]{};
    sprintf_s(buf, "0x%012llX", static_cast<unsigned long long>(
                                    reinterpret_cast<uintptr_t>(p) & 0xFFFFFFFFFFFFull));
    return buf;
}

} // namespace

void ProbeLogger::LogFrame(const FrameLogRecord& r)
{
    std::lock_guard<std::mutex> lock(s_->mutex);
    if (!s_->file) {
        return;
    }
    auto now = std::chrono::steady_clock::now();
    if (!r.force && now - s_->lastWrite < s_->minInterval) {
        ++s_->dropped;
        return;
    }
    s_->lastWrite = now;

    const char* fmtName = FormatName(r.tex.format);
    char fmtBuf[32]{};
    if (fmtName) {
        strncpy_s(fmtBuf, fmtName, _TRUNCATE);
    } else {
        sprintf_s(fmtBuf, "0x%08X", static_cast<unsigned>(r.tex.format));
    }

    fprintf(s_->file,
            "[%s] frame=%llu tid=%u qpc=%lld "
            "device=%s texture=%s "
            "size=%ux%u format=%s bind=%s misc=0x%X "
            "windows_hdr=%d output_color_space=%s sdr_white_level=%.1f "
            "path=%s shader_ms=%.2f convert_ms=%.2f total_ms=%.2f "
            "encoder_input_format=%s bypass=%d dropped=%llu\n",
            NowHmsMs().c_str(),
            static_cast<unsigned long long>(r.frame),
            static_cast<unsigned>(r.tid),
            static_cast<long long>(r.qpc),
            PtrHex(r.sourceDevice).c_str(),
            PtrHex(r.sourceTexture).c_str(),
            static_cast<unsigned>(r.tex.width),
            static_cast<unsigned>(r.tex.height),
            fmtBuf,
            BindFlagsToString(r.tex.bindFlags).c_str(),
            static_cast<unsigned>(r.tex.miscFlags),
            r.windowsHdr ? 1 : 0,
            r.outputColorSpace.c_str(),
            static_cast<double>(r.sdrWhiteNits),
            r.path.c_str(),
            static_cast<double>(r.shaderMs),
            static_cast<double>(r.convertMs),
            static_cast<double>(r.totalMs),
            r.encoderInputFormat.c_str(),
            r.bypass ? 1 : 0,
            static_cast<unsigned long long>(s_->dropped));
    fflush(s_->file);
}

} // namespace hdrfix
