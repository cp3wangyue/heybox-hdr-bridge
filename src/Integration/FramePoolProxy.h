#pragma once
// Integration/FramePoolProxy.h — WGC 帧池与帧代理（计划书 §9.1 / §9.2）
//
// 核心职责：
//   1. ProxyFramePoolStatics：拦截 CreateFreeThreaded / Create，透明将请求提升为 FP16 scRGB 池；
//   2. ProxyFramePool：实现 IDirect3D11CaptureFramePool，内部管理真实的 FP16 池与 3 缓冲轮转 BGRA8 目标池；
//   3. 在 TryGetNextFrame 消费点调度 ToneMapCore 进行 GPU 全屏 Pass，将转换后的 BGRA8 帧交回客户端；
//   4. Fail-open：非 HDR 模式或异常时透传原生链路。

#include <windows.h>
#include <windows.graphics.capture.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <wrl/client.h>
#include <wrl/implements.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include "CaptureProbe/hdr_state.h"
#include "ToneMap/ToneMapCore.h"

namespace hdrfix {

// 单帧代理：包装经色调映射后的 BGRA8 纹理表面与原始时间戳、尺寸
class ProxyFrame : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::WinRtClassicComMix>,
    ABI::Windows::Graphics::Capture::IDirect3D11CaptureFrame,
    ABI::Windows::Foundation::IClosable>
{
public:
    ProxyFrame(ABI::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface* surface,
               ABI::Windows::Foundation::TimeSpan ts,
               ABI::Windows::Graphics::SizeInt32 size);

    // IDirect3D11CaptureFrame
    IFACEMETHODIMP get_Surface(ABI::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface** value) override;
    IFACEMETHODIMP get_SystemRelativeTime(ABI::Windows::Foundation::TimeSpan* value) override;
    IFACEMETHODIMP get_ContentSize(ABI::Windows::Graphics::SizeInt32* value) override;

    // IClosable
    IFACEMETHODIMP Close() override;

private:
    Microsoft::WRL::ComPtr<ABI::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface> m_surface;
    ABI::Windows::Foundation::TimeSpan m_timeSpan{};
    ABI::Windows::Graphics::SizeInt32 m_size{};
    bool m_closed = false;
};

// 帧池代理：包装真实底层的 FP16 帧池并管理色调映射输出池
class ProxyFramePool : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::WinRtClassicComMix>,
    ABI::Windows::Graphics::Capture::IDirect3D11CaptureFramePool,
    ABI::Windows::Foundation::IClosable>
{
public:
    ProxyFramePool(
        Microsoft::WRL::ComPtr<ABI::Windows::Graphics::Capture::IDirect3D11CaptureFramePool> realPool,
        Microsoft::WRL::ComPtr<ABI::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice> winrtDevice,
        Microsoft::WRL::ComPtr<ID3D11Device> d3dDevice,
        INT32 numberOfBuffers,
        ABI::Windows::Graphics::SizeInt32 size,
        bool isHdrElevated);

    ~ProxyFramePool() override;

    // IDirect3D11CaptureFramePool
    IFACEMETHODIMP Recreate(
        ABI::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice* device,
        ABI::Windows::Graphics::DirectX::DirectXPixelFormat pixelFormat,
        INT32 numberOfBuffers,
        ABI::Windows::Graphics::SizeInt32 size) override;

    IFACEMETHODIMP TryGetNextFrame(
        ABI::Windows::Graphics::Capture::IDirect3D11CaptureFrame** result) override;

    IFACEMETHODIMP add_FrameArrived(
        __FITypedEventHandler_2_Windows__CGraphics__CCapture__CDirect3D11CaptureFramePool_IInspectable* handler,
        EventRegistrationToken* token) override;

    IFACEMETHODIMP remove_FrameArrived(EventRegistrationToken token) override;

    IFACEMETHODIMP CreateCaptureSession(
        ABI::Windows::Graphics::Capture::IGraphicsCaptureItem* item,
        ABI::Windows::Graphics::Capture::IGraphicsCaptureSession** result) override;

    IFACEMETHODIMP get_DispatcherQueue(ABI::Windows::System::IDispatcherQueue** value) override;

    // IClosable
    IFACEMETHODIMP Close() override;

    // 统计数据
    UINT64 GetProcessedFrames() const { return m_processedFrames.load(); }
    bool IsHdrElevated() const { return m_isHdrElevated; }

private:
    bool EnsureOutputPool(UINT width, UINT height);
    void ReleaseOutputPool();

    struct BufferSlot {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
        Microsoft::WRL::ComPtr<ABI::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface> surface;
    };

    std::mutex m_mutex;
    Microsoft::WRL::ComPtr<ABI::Windows::Graphics::Capture::IDirect3D11CaptureFramePool> m_realPool;
    Microsoft::WRL::ComPtr<ABI::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice> m_winrtDevice;
    Microsoft::WRL::ComPtr<ID3D11Device> m_d3dDevice;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_d3dContext;

    ToneMapCore m_toneMapper;
    std::vector<BufferSlot> m_outputPool;
    size_t m_currentSlot = 0;
    UINT m_poolWidth = 0;
    UINT m_poolHeight = 0;
    INT32 m_bufferCount = 3;

    bool m_isHdrElevated = false;
    std::atomic<UINT64> m_processedFrames{0};
};

// 激活工厂代理：拦截创建函数并决定是否升级为 FP16 池
class ProxyFramePoolStatics : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::WinRtClassicComMix>,
    ABI::Windows::Graphics::Capture::IDirect3D11CaptureFramePoolStatics,
    ABI::Windows::Graphics::Capture::IDirect3D11CaptureFramePoolStatics2>
{
public:
    ProxyFramePoolStatics(
        Microsoft::WRL::ComPtr<ABI::Windows::Graphics::Capture::IDirect3D11CaptureFramePoolStatics> realStatics1,
        Microsoft::WRL::ComPtr<ABI::Windows::Graphics::Capture::IDirect3D11CaptureFramePoolStatics2> realStatics2);

    // IDirect3D11CaptureFramePoolStatics
    IFACEMETHODIMP Create(
        ABI::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice* device,
        ABI::Windows::Graphics::DirectX::DirectXPixelFormat pixelFormat,
        INT32 numberOfBuffers,
        ABI::Windows::Graphics::SizeInt32 size,
        ABI::Windows::Graphics::Capture::IDirect3D11CaptureFramePool** result) override;

    // IDirect3D11CaptureFramePoolStatics2
    IFACEMETHODIMP CreateFreeThreaded(
        ABI::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice* device,
        ABI::Windows::Graphics::DirectX::DirectXPixelFormat pixelFormat,
        INT32 numberOfBuffers,
        ABI::Windows::Graphics::SizeInt32 size,
        ABI::Windows::Graphics::Capture::IDirect3D11CaptureFramePool** result) override;

private:
    HRESULT InterceptCreate(
        bool freeThreaded,
        ABI::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice* device,
        ABI::Windows::Graphics::DirectX::DirectXPixelFormat pixelFormat,
        INT32 numberOfBuffers,
        ABI::Windows::Graphics::SizeInt32 size,
        ABI::Windows::Graphics::Capture::IDirect3D11CaptureFramePool** result);

    Microsoft::WRL::ComPtr<ABI::Windows::Graphics::Capture::IDirect3D11CaptureFramePoolStatics> m_realStatics1;
    Microsoft::WRL::ComPtr<ABI::Windows::Graphics::Capture::IDirect3D11CaptureFramePoolStatics2> m_realStatics2;
};

} // namespace hdrfix
