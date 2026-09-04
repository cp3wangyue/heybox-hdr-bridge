// hdr_ctl — 系统 HDR 查询/开关控制台工具（P0 A/B 自动化）
// 用法:
//   hdr_ctl.exe status          打印每个输出的 HDR 状态
//   hdr_ctl.exe on  [displayN]  打开 HDR（默认第一个输出）
//   hdr_ctl.exe off [displayN]  关闭 HDR
// 退出码: 0 成功；非 0 失败

#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "CaptureProbe/hdr_control.h"
#include "CaptureProbe/hdr_state.h"

using namespace hdrfix;

static const OutputHdrState* Find(const std::vector<OutputHdrState>& states, const std::wstring& name)
{
    for (const auto& s : states) {
        if (s.gdiDeviceName == name) return &s;
    }
    return nullptr;
}

static int PrintStatus()
{
    auto states = QueryOutputHdrStates();
    for (const auto& st : states) {
        printf("%ls hdr_active=%d supported=%d user_enabled=%d bits=%u sdr_white=%.1f nits colorspace=%s\n",
               st.gdiDeviceName.c_str(), st.hdrEnabled ? 1 : 0, st.hdrSupported ? 1 : 0,
               st.hdrUserEnabled ? 1 : 0, st.bitsPerColor, st.sdrWhiteNits,
               ColorSpaceName(st.colorSpace) ? ColorSpaceName(st.colorSpace) : "unknown");
    }
    return 0;
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        printf("usage: hdr_ctl.exe status | on | off [displayN]\n");
        return 1;
    }
    std::string cmd = argv[1];

    if (cmd == "status") {
        return PrintStatus();
    }

    bool enable;
    if (cmd == "on") enable = true;
    else if (cmd == "off") enable = false;
    else {
        printf("unknown command: %s\n", cmd.c_str());
        return 1;
    }

    auto states = QueryOutputHdrStates();
    if (states.empty()) {
        printf("no active desktop output found\n");
        return 1;
    }
    std::wstring target;
    if (argc >= 3) {
        std::string d = argv[2];
        if (d.rfind("\\\\", 0) != 0) d = "\\\\.\\DISPLAY" + d;
        std::wstring wide(d.begin(), d.end());
        if (!Find(states, wide)) {
            printf("display %s not found\n", d.c_str());
            return 1;
        }
        target = wide;
    } else {
        target = states.front().gdiDeviceName;
    }

    const OutputHdrState* before = Find(states, target);
    printf("before: %ls hdr_active=%d colorspace=%s\n", before->gdiDeviceName.c_str(),
           before->hdrEnabled ? 1 : 0,
           ColorSpaceName(before->colorSpace) ? ColorSpaceName(before->colorSpace) : "unknown");

    int rc = SetOutputHdr(target, enable);
    if (rc != 0) {
        printf("SetOutputHdr failed rc=%d (state may be unchanged)\n", rc);
        return rc;
    }

    auto after = QueryOutputHdrStates();
    const OutputHdrState* st = Find(after, target);
    printf("after : %ls hdr_active=%d colorspace=%s sdr_white=%.1f nits\n", st->gdiDeviceName.c_str(),
           st->hdrEnabled ? 1 : 0,
           ColorSpaceName(st->colorSpace) ? ColorSpaceName(st->colorSpace) : "unknown",
           st->sdrWhiteNits);
    return 0;
}
