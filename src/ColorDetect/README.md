# ColorDetect

输入帧格式与色彩空间识别（AutoDetect，§10.1 判定顺序）：

1. 系统/目标输出是否 Advanced Color / HDR（复用 CaptureProbe/hdr_state）
2. 捕获纹理 Format：FP16 → scRGB 路径；RGB10A2 → 结合 ColorSpace/上下文判 PQ
3. 已是 8-bit SDR 且 A/B 正常 → 完全旁路
4. 无法确定 → 不自动转换，进诊断模式并记录候选信息

失败安全：识别不确定时永远倾向旁路（fail-open）。
