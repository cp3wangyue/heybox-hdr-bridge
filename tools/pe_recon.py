#!/usr/bin/env python3
# pe_recon.py — 静态侦察：对客户端二进制做导入表与字符串检索（只读）
# 用法: python pe_recon.py <客户端根目录> <输出md>
# 输出: 候选捕获 API / 编码器 / 像素格式 / 色彩关键词的命中表（每条带证据来源）

import os
import re
import sys
import datetime

import pefile

# 检索目标（Electron/RTC 场景补充）
IMPORT_HINTS = {
    "捕获/D3D11 初始化": [
        "CreateDXGIFactory", "D3D11CreateDevice", "DuplicateOutput",
        "D3D11CreateDeviceAndSwapChain", "CreateSwapChainForComposition",
    ],
    "编码": ["NvEncode", "NvEnc", "MFCreate", "IMFTransform", "avcodec", "avutil", "openh264"],
    "WinRT/捕获": ["RoGetActivationFactory", "WindowsCreateString"],
}
STRING_HINTS = {
    "捕获 API": [
        "Windows.Graphics.Capture", "CreateDirect3D11CaptureFramePool",
        "Direct3D11CaptureFramePool", "GraphicsCaptureItem", "GraphicsCaptureSession",
        "CreateForMonitor", "DuplicateOutput", "DuplicateOutput1",
        "IDXGIOutputDuplication", "AcquireNextFrame", "ReleaseFrame",
        "DXGI_OUTDUPL", "DXGI_ERROR_ACCESS_LOST", "CreateDirect3D11DeviceFromDXGIDevice",
        "BitBlt", "GetDC", "GetWindowDC", "PrintWindow", "CopyFromScreen",
    ],
    "像素格式": [
        "NV12", "P010", "R16G16B16A16", "DXGI_FORMAT_R16G16B16A16_FLOAT",
        "R10G10B10A2", "B8G8R8A8", "I420", "YUV2", "ABGR", "ARGB",
    ],
    "色彩/HDR": [
        "2084", "PQ", "HLG", "HDR", "scRGB", "BT709", "BT.709", "BT2020", "BT.2020",
        "Rec709", "Rec2020", "colorspace", "ColorSpace", "color_space", "G2084",
        "nits", "maxCLL", "MaxCLL", "tone_map", "ToneMap", "tonemap",
    ],
    "编码器": [
        "nvEncodeAPI", "NVENC", "amf", "VAAPI", "libx264", "h264", "hevc", "H265",
        "MFCreateVideoEncoder", "eEncoder", "VideoEncoder",
    ],
    "WebRTC/RTC": [
        "DesktopCapturer", "DesktopAndCursorComposer", "webrtc", "libwebrtc",
        "VideoFrame", "VideoTrack", "screen_capture", "ScreenCapture", "screen_share",
        "liteav", "bytertc", "vertc",
    ],
}

# 只深度扫描这些二进制（大文件全扫太慢，其余仅列基本信息）
KEY_PATTERNS = ("VolcEngineRTC", "liteav", "txffmpeg", "ffmpeg", "RTCFFmpeg", "openh264",
                "heybox-overlay", "steam-hook", "dinput8", "MonsterHunterWilds", "live_kit")
MAX_DEEP_SCAN_BYTES = 60 * 1024 * 1024


def human(n: int) -> str:
    return f"{n / (1024*1024):.1f} MB" if n >= 1024 * 1024 else f"{n / 1024:.0f} KB"


def scan_file(path: str, deep: bool):
    """返回 (size, imports: set, hint_hits: {类别: {关键词: [原始串样本]}})"""
    size = os.path.getsize(path)
    imports, hits = set(), {}
    try:
        pe = pefile.PE(path, fast_load=True)
        pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_IMPORT"],
                                               pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT"]])
        for entry in (getattr(pe, "DIRECTORY_ENTRY_IMPORT", []) +
                      getattr(pe, "DIRECTORY_ENTRY_DELAY_IMPORT", [])):
            dll = entry.dll.decode(errors="ignore").lower()
            imports.add(dll)
            for cat, kws in IMPORT_HINTS.items():
                for kw in kws:
                    if kw.lower() in dll:
                        hits.setdefault(cat, {}).setdefault(f"导入DLL:{dll}", []).append(kw)
            for imp in getattr(entry, "imports", []):
                name = imp.name.decode(errors="ignore") if imp.name else ""
                for cat, kws in IMPORT_HINTS.items():
                    for kw in kws:
                        if kw.lower() in name.lower():
                            hits.setdefault(cat, {}).setdefault(f"导入函数:{dll}", []).append(name)
        pe.close()
    except Exception as e:  # 不是 PE 或解析失败，继续做字符串扫描
        if deep:
            print(f"  [warn] PE parse failed: {os.path.basename(path)}: {e}")

    if deep:
        with open(path, "rb") as f:
            data = f.read(MAX_DEEP_SCAN_BYTES)
        for cat, kws in STRING_HINTS.items():
            for kw in kws:
                # 精确大小写匹配（误报风险低的直接报）；
                # 另做大小写不敏感匹配，仅用于长度 >= 8 的长词（避免 HDR/PQ/NV12 短词误报）
                for label, flags in (("精确", 0), ("忽略大小写", re.IGNORECASE)):
                    if label == "忽略大小写" and len(kw) < 8:
                        continue
                    pat = re.compile(re.escape(kw.encode()) + b"|"
                                     + re.escape(kw.encode("utf-16-le")), flags)
                    if pat.search(data):
                        hits.setdefault(cat, {}).setdefault("字符串", []).append(
                            kw if label == "精确" else kw + " (ci)")
                        break
    return size, imports, hits


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "."
    out_path = sys.argv[2] if len(sys.argv) > 2 else "docs/recon/pe-recon.md"
    binaries = []
    for dirpath, _, filenames in os.walk(root):
        for fn in filenames:
            if fn.lower().endswith((".dll", ".exe")):
                binaries.append(os.path.join(dirpath, fn))
    binaries.sort(key=lambda p: -os.path.getsize(p))
    print(f"scanning {len(binaries)} binaries under {root}")

    lines = [f"# PE 静态侦察", "",
             f"- 采集时间：{datetime.datetime.now():%Y-%m-%d %H:%M:%S}",
             f"- 目标目录：`{root}`",
             f"- 二进制数量：{len(binaries)}（深度扫描命中如下）", ""]

    total_hits = 0
    for path in binaries:
        base = os.path.basename(path)
        rel = os.path.relpath(path, root)
        deep = any(p.lower() in base.lower() for p in KEY_PATTERNS)
        size, imports, hits = scan_file(path, deep)
        if not hits:
            continue
        total_hits += 1
        lines.append(f"## `{rel}`（{human(size)}，{'深度扫描' if deep else '仅导入表'}）")
        lines.append("")
        for cat, srcs in hits.items():
            lines.append(f"- **{cat}**")
            for src, names in sorted(srcs.items()):
                uniq = sorted(set(names))[:12]
                more = f"（共{len(set(names))}项，示例）" if len(set(names)) > 12 else ""
                lines.append(f"  - {src}{more}: `{', '.join(uniq)}`")
        lines.append("")

    lines.append("## 结论待填")
    lines.append("")
    lines.append("- 候选捕获路径（按证据强度）：")
    lines.append("- 候选编码路径：")
    lines.append("- 下一步动态验证入口：")
    lines.append("")

    with open(out_path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print(f"wrote {out_path}; {total_hits} binaries with hints")


if __name__ == "__main__":
    main()
