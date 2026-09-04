// hdrfix_probe.dll — P2 只读探针（计划书 §6.1：只记录、不替换帧）
//
// 注入到 HeyboxChat 进程后：
//   1. 等待 VolcEngineRTC.dll 加载（共享会话建立时出现）
//   2. IAT hook 其 D3D11CreateDevice → 每实例 vtable 克隆观察 CreateTexture2D
//   3. 节流像素采样（默认 3s 一次）：FP16 帧统计 >1.0 像素占比（HDR 高光存在性），
//      BGRA 帧统计亮度 >=250 占比（裁白程度）
//   4. 日志： %TEMP%\hdrfix_probe_<pid>.log（附录 A 字段风格）
// 停止：named event "Local\hdrfix_probe_stop" 或运行满 HDRFIX_PROBE_MAXSEC（默认 900s）。
// 环境变量：HDRFIX_PROBE_MODULE（默认 VolcEngineRTC.dll）、HDRFIX_PROBE_LOG、HDRFIX_PROBE_MAXSEC。

#include <windows.h>

#include <cstdio>
#include <string>
#include <thread>

#include "CaptureProbe/d3d11_observer.h"
#include "CaptureProbe/hdr_state.h"
#include "CaptureProbe/probe_logger.h"
#include "CaptureProbe/wgc_observer.h"

using namespace hdrfix;

static HANDLE g_stopEvent = nullptr;
static HANDLE g_worker = nullptr;

static std::wstring ReadEnvStr(const wchar_t* name, const std::wstring& def)
{
    wchar_t buf[512]{};
    DWORD n = ::GetEnvironmentVariableW(name, buf, 512);
    return n > 0 ? std::wstring(buf) : def;
}

static int ReadEnvInt(const wchar_t* name, int def)
{
    wchar_t buf[32]{};
    DWORD n = ::GetEnvironmentVariableW(name, buf, 32);
    return n > 0 ? _wtoi(buf) : def;
}

static std::wstring DefaultLogPath()
{
    wchar_t temp[MAX_PATH]{};
    ::GetTempPathW(MAX_PATH, temp);
    wchar_t name[128];
    swprintf_s(name, L"hdrfix_probe_%lu.log", ::GetCurrentProcessId());
    return std::wstring(temp) + name;
}

static DWORD WINAPI Worker(LPVOID)
{
    const std::wstring module = ReadEnvStr(L"HDRFIX_PROBE_MODULE", L"VolcEngineRTC.dll");
    const std::wstring logPath = ReadEnvStr(L"HDRFIX_PROBE_LOG", DefaultLogPath());
    const int maxSeconds = ReadEnvInt(L"HDRFIX_PROBE_MAXSEC", 900);

    if (!ProbeLogger::Instance().Start(logPath, std::chrono::milliseconds(500))) {
        return 1;
    }
    {
        FrameLogRecord r;
        r.path = "ProbeLoaded pid=" + std::to_string(::GetCurrentProcessId()) +
                 " targetModule=" + std::string(module.begin(), module.end());
        r.force = true;
        ProbeLogger::Instance().LogFrame(r);
    }

    // 记录当前 HDR 状态（AutoDetect 依据：输出色彩空间）
    for (const auto& st : QueryOutputHdrStates()) {
        FrameLogRecord r;
        r.tid = ::GetCurrentThreadId();
        r.windowsHdr = st.hdrEnabled;
        char path[256];
        sprintf_s(path, "DisplayState %ls hdr=%d supported=%d bits=%u sdrWhite=%.1f colorspace=%s",
                  st.gdiDeviceName.c_str(), st.hdrEnabled ? 1 : 0, st.hdrSupported ? 1 : 0,
                  st.bitsPerColor, st.sdrWhiteNits,
                  ColorSpaceName(st.colorSpace) ? ColorSpaceName(st.colorSpace) : "unknown");
        r.path = path;
        r.force = true;
        ProbeLogger::Instance().LogFrame(r);
    }

    // P3：并行 FP16 观察池（独立设备 + 独立会话，不接触 SDK 路径）
    WgcObserver& wgc = WgcObserver::Instance();
    if (wgc.Start()) {
        FrameLogRecord r;
        r.path = "WGC:observer launched";
        r.force = true;
        ProbeLogger::Instance().LogFrame(r);
    }

    // 等待目标模块出现（共享会话建立时 VeRTC 才加载）
    D3D11Observer& observer = D3D11Observer::Instance();
    ObserverConfig cfg;
    cfg.targetModule = module;
    bool observerOn = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(maxSeconds);
    while (std::chrono::steady_clock::now() < deadline) {
        if (::WaitForSingleObject(g_stopEvent, 0) == WAIT_OBJECT_0) break;
        if (!observerOn) {
            observerOn = observer.Start(cfg);
            if (observerOn) {
                FrameLogRecord r;
                r.path = "ObserverInstalled module=" + std::string(module.begin(), module.end());
                r.force = true;
                ProbeLogger::Instance().LogFrame(r);
            }
        }
        ::Sleep(2000);
        if (!observerOn) continue;

        // 节流像素采样：回答"HDR 高光在哪一步丢失"
        static int sampleTick = 0;
        if (++sampleTick % 2 == 0) { // 观察器等待循环 2s/次 → 每 4s 采样
            PixelSampleResult sr;
            if (observer.TakePixelSample(&sr)) {
                FrameLogRecord r;
                r.tid = ::GetCurrentThreadId();
                r.sourceTexture = sr.source;
                r.tex.width = sr.width;
                r.tex.height = sr.height;
                r.tex.format = sr.format;
                char path[256];
                sprintf_s(path,
                          "PixelSample #%llu fmt=%d max=%.3f overWhite=%.4f bright=%.4f underBlack=%.4f meanLuma=%.2f",
                          static_cast<unsigned long long>(sr.sampleCount),
                          static_cast<int>(sr.format), sr.maxChannel,
                          sr.overWhiteFrac, sr.brightFrac, sr.underBlackFrac, sr.meanLuma);
                r.path = path;
                r.force = true;
                ProbeLogger::Instance().LogFrame(r);
            }
        }
    }

    observer.Stop();
    wgc.Stop();
    FrameLogRecord r;
    r.path = "ProbeStop";
    r.force = true;
    ProbeLogger::Instance().LogFrame(r);
    ProbeLogger::Instance().Stop();
    return 0;
}

static void StartWorker()
{
    // 事件名带 pid：避免全局停止事件的残留信号量让新探针立即退出
    wchar_t name[64];
    swprintf_s(name, L"Local\\hdrfix_probe_stop_%lu", ::GetCurrentProcessId());
    g_stopEvent = ::CreateEventW(nullptr, TRUE, FALSE, name);
    g_worker = ::CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved)
{
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        ::DisableThreadLibraryCalls(module);
        StartWorker(); // 工作线程会阻塞在 loader lock 上，直到 ATTACH 完成，天然安全
        break;
    case DLL_PROCESS_DETACH:
        if (reserved == nullptr) { // FreeLibrary 调用（而非进程退出）
            if (g_stopEvent) ::SetEvent(g_stopEvent);
            // 不等待线程退出：避免 loader lock 死锁；线程会在下一个 tick 自行结束
        }
        break;
    }
    return TRUE;
}
