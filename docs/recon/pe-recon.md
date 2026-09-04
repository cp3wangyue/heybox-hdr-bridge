# PE 静态侦察（P1，§5.2）

- 采集时间：2026-09-05 05:40:30
- 目标目录：`C:\Users\22983\AppData\Local\Qingfeng\HeyboxChat`
- 二进制数量：39（深度扫描命中如下）

## `1.56.0\HeyboxChat.exe`（179.9 MB，仅导入表）

- **编码**
  - 导入函数:ffmpeg.dll: `avcodec_align_dimensions, avcodec_alloc_context3, avcodec_descriptor_get, avcodec_find_decoder, avcodec_flush_buffers, avcodec_free_context, avcodec_open2, avcodec_parameters_to_context, avcodec_receive_frame, avcodec_send_packet`
  - 导入函数:mf.dll: `MFCreateDeviceSource`
  - 导入函数:mfplat.dll（共14项，示例）: `MFCreateAlignedMemoryBuffer, MFCreateAsyncResult, MFCreateAttributes, MFCreateDXGIDeviceManager, MFCreateDXGISurfaceBuffer, MFCreateEventQueue, MFCreateMediaBufferWrapper, MFCreateMediaEvent, MFCreateMediaType, MFCreateMemoryBuffer, MFCreatePresentationDescriptor, MFCreateSample`
  - 导入函数:mfreadwrite.dll: `MFCreateSourceReaderFromMediaSource`
- **WinRT/捕获**
  - 导入函数:api-ms-win-core-winrt-l1-1-0.dll: `RoGetActivationFactory`
  - 导入函数:api-ms-win-core-winrt-string-l1-1-0.dll: `WindowsCreateString, WindowsCreateStringReference`
- **捕获/D3D11 初始化**
  - 导入函数:d3d11.dll: `D3D11CreateDevice`
  - 导入函数:dxgi.dll: `CreateDXGIFactory1`

## `1.56.0\resources\versions\1.56.0\app\node_modules\@volcengine\vertc-electron-sdk\build\Release\VolcEngineRTC.dll`（46.5 MB，深度扫描）

- **捕获/D3D11 初始化**
  - 导入函数:d3d11.dll: `D3D11CreateDevice`
  - 导入函数:dxgi.dll: `CreateDXGIFactory, CreateDXGIFactory1`
- **编码**
  - 导入DLL:openh264-4.dll: `openh264`
  - 导入函数:rtcffmpeg.dll（共14项，示例）: `avcodec_align_dimensions, avcodec_alloc_context3, avcodec_close, avcodec_decode_video2, avcodec_find_decoder, avcodec_find_decoder_by_name, avcodec_find_encoder_by_name, avcodec_free_context, avcodec_open2, avcodec_receive_frame, avcodec_receive_packet, avcodec_register_all`
- **捕获 API**
  - 字符串: `BitBlt, CreateDirect3D11DeviceFromDXGIDevice, Direct3D11CaptureFramePool, DuplicateOutput, GetDC, GetWindowDC, GraphicsCaptureItem, GraphicsCaptureSession, IDXGIOutputDuplication, PrintWindow, ReleaseFrame, Windows.Graphics.Capture`
- **像素格式**
  - 字符串: `ABGR, ARGB, I420, NV12, P010, YUV2`
- **色彩/HDR**
  - 字符串: `ColorSpace, HDR, PQ, color_space, colorspace, nits`
- **编码器**
  - 字符串: `H265, NVENC, VideoEncoder, amf, eEncoder, h264, hevc, nvEncodeAPI`
- **WebRTC/RTC**
  - 字符串: `DesktopCapturer (ci), ScreenCapture, VideoFrame, VideoTrack, bytertc, screen_capture, screen_share, vertc, webrtc`

## `1.56.0\resources\versions\1.56.0\app\node_modules\trtc-electron-sdk\build\Release\liteav.dll`（15.4 MB，深度扫描）

- **编码**
  - 导入函数:txffmpeg.dll: `liteav_avcodec_alloc_context3, liteav_avcodec_close, liteav_avcodec_decode_audio4, liteav_avcodec_default_get_format, liteav_avcodec_find_decoder, liteav_avcodec_flush_buffers, liteav_avcodec_free_context, liteav_avcodec_get_name, liteav_avcodec_open2, liteav_avcodec_parameters_from_context, liteav_avcodec_receive_frame, liteav_avcodec_send_packet`
- **捕获/D3D11 初始化**
  - 导入函数:dxgi.dll: `CreateDXGIFactory, CreateDXGIFactory1`
- **捕获 API**
  - 字符串: `BitBlt, DuplicateOutput, GetDC, IDXGIOutputDuplication, PrintWindow, ReleaseFrame`
- **像素格式**
  - 字符串: `ARGB, B8G8R8A8, I420, NV12, P010`
- **色彩/HDR**
  - 字符串: `BT2020, BT709, ColorSpace, HDR, HLG, PQ, color_space, colorspace, nits`
- **编码器**
  - 字符串: `H265, NVENC, VAAPI, VideoEncoder, amf, eEncoder, h264, hevc, nvEncodeAPI`
- **WebRTC/RTC**
  - 字符串: `DesktopCapturer (ci), ScreenCapture, VideoFrame, VideoTrack, liteav, screen_capture, screen_share, webrtc`

## `1.56.0\resources\versions\1.56.0\app\addon\mhwilds\dinput8.dll`（12.3 MB，深度扫描）

- **捕获/D3D11 初始化**
  - 导入函数:d3d11.dll: `D3D11CreateDeviceAndSwapChain`
- **像素格式**
  - 字符串: `NV12`
- **色彩/HDR**
  - 字符串: `ColorSpace, HDR, PQ, ToneMap, colorspace (ci), nits, tonemap`

## `1.56.0\libGLESv2.dll`（8.0 MB，仅导入表）

- **捕获/D3D11 初始化**
  - 导入函数:dxgi.dll: `CreateDXGIFactory, CreateDXGIFactory1`

## `1.56.0\resources\versions\1.56.0\app\node_modules\trtc-electron-sdk\build\Release\txffmpeg.dll`（7.7 MB，深度扫描）

- **像素格式**
  - 字符串: `ABGR, ARGB, I420, NV12, P010`
- **色彩/HDR**
  - 字符串: `2084, BT.2020, BT.709, ColorSpace, MaxCLL, PQ, colorspace, nits`
- **编码器**
  - 字符串: `amf, h264, hevc`
- **WebRTC/RTC**
  - 字符串: `liteav`

## `1.56.0\resources\versions\1.56.0\app\node_modules\@volcengine\vertc-electron-sdk\build\Release\bytertc_ffmpeg_audio_extension.dll`（7.5 MB，深度扫描）

- **像素格式**
  - 字符串: `ABGR, ARGB, I420, NV12`
- **色彩/HDR**
  - 字符串: `2084, BT.2020, BT.709, ColorSpace (ci), HDR, MaxCLL, PQ, color_space, colorspace, nits`
- **编码器**
  - 字符串: `H265, VideoEncoder, amf, h264, hevc`
- **WebRTC/RTC**
  - 字符串: `bytertc`

## `1.56.0\resources\versions\1.56.0\app\node_modules\@volcengine\vertc-electron-sdk\build\Release\RTCFFmpeg.dll`（6.8 MB，深度扫描）

- **像素格式**
  - 字符串: `ABGR, ARGB, I420, NV12, P010`
- **色彩/HDR**
  - 字符串: `2084, BT.2020, BT.709, ColorSpace, HDR, MaxCLL, PQ, color_space, colorspace, nits`
- **编码器**
  - 字符串: `H265, NVENC, VideoEncoder, amf, eEncoder, h264, hevc, nvEncodeAPI`
- **WebRTC/RTC**
  - 字符串: `VideoFrame, bytertc`

## `1.56.0\resources\versions\1.56.0\app\node_modules\@volcengine\vertc-electron-sdk\build\Release\bytertc_vp8codec_extension.dll`（3.8 MB，仅导入表）

- **捕获/D3D11 初始化**
  - 导入函数:d3d11.dll: `D3D11CreateDevice`
  - 导入函数:dxgi.dll: `CreateDXGIFactory`

## `1.56.0\ffmpeg.dll`（2.8 MB，深度扫描）

- **像素格式**
  - 字符串: `ABGR, ARGB, I420, NV12`
- **色彩/HDR**
  - 字符串: `2084, BT.2020, BT.709, ColorSpace (ci), HDR, PQ, colorspace`
- **编码器**
  - 字符串: `h264, hevc`

## `1.56.0\resources\versions\1.56.0\app\addon\heybox-overlay-x64.dll`（1.4 MB，深度扫描）

- **色彩/HDR**
  - 字符串: `PQ`

## `1.56.0\resources\versions\1.56.0\app\addon\steam-hook-x64.dll`（1.3 MB，深度扫描）

- **色彩/HDR**
  - 字符串: `PQ`

## `1.56.0\resources\versions\1.56.0\app\node_modules\@volcengine\vertc-electron-sdk\build\Release\openh264-4.dll`（1.3 MB，深度扫描）

- **色彩/HDR**
  - 字符串: `PQ, nits`
- **编码器**
  - 字符串: `h264`
- **WebRTC/RTC**
  - 字符串: `VideoFrame (ci)`

## `1.56.0\resources\versions\1.56.0\app\addon\steam-hook-x86.dll`（1.2 MB，深度扫描）

- **色彩/HDR**
  - 字符串: `PQ`

## `1.56.0\resources\versions\1.56.0\app\addon\mhwilds\reframework\plugins\MonsterHunterWilds.dll`（912 KB，深度扫描）

- **色彩/HDR**
  - 字符串: `PQ`

## `1.56.0\resources\versions\1.56.0\app\addon\heybox-overlay-x86.dll`（874 KB，深度扫描）

- **色彩/HDR**
  - 字符串: `PQ`

## `1.56.0\resources\versions\1.56.0\app\node_modules\trtc-electron-sdk\build\Release\live_kit_engine.dll`（700 KB，深度扫描）

- **色彩/HDR**
  - 字符串: `PQ`
- **编码器**
  - 字符串: `VideoEncoder`
- **WebRTC/RTC**
  - 字符串: `ScreenCapture, VideoFrame, liteav, screen_capture`

## `1.56.0\resources\versions\1.56.0\app\node_modules\trtc-electron-sdk\build\Release\live_kit_server.dll`（668 KB，深度扫描）

- **色彩/HDR**
  - 字符串: `PQ`
- **编码器**
  - 字符串: `VideoEncoder`
- **WebRTC/RTC**
  - 字符串: `ScreenCapture, VideoFrame, liteav`

## `1.56.0\resources\versions\1.56.0\app\node_modules\trtc-electron-sdk\build\Release\liteav_media_server.exe`（374 KB，深度扫描）

- **色彩/HDR**
  - 字符串: `PQ`
- **WebRTC/RTC**
  - 字符串: `liteav`

## `1.56.0\resources\versions\1.56.0\app\node_modules\trtc-electron-sdk\build\Release\liteav_screen.dll`（177 KB，深度扫描）

- **捕获/D3D11 初始化**
  - 导入函数:d3d11.dll: `D3D11CreateDevice`
- **捕获 API**
  - 字符串: `CreateDirect3D11DeviceFromDXGIDevice, Direct3D11CaptureFramePool, GraphicsCaptureItem, GraphicsCaptureSession, Windows.Graphics.Capture`
- **色彩/HDR**
  - 字符串: `PQ`
- **WebRTC/RTC**
  - 字符串: `liteav`

## 结论待填

- 候选捕获路径（按证据强度）：
- 候选编码路径：
- 下一步动态验证入口：
