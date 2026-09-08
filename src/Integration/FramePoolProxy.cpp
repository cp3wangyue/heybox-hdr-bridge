// Integration/FramePoolProxy.cpp — WGC 帧池与帧代理实现
#include "Integration/FramePoolProxy.h"

#include <dxgi1_6.h>
#include <algorithm>
#include <cstdio>
#include <string>

#include "ColorDetect/ColorDetector.h"
#include "Diagnostics/ConfigManager.h"
#include "Diagnostics/SafetyGuard.h"
#include "Diagnostics/CompatibilityManager.h"
#include "Diagnostics/BridgeLog.h"

using namespace ABI::Windows::Graphics::Capture;
using namespace ABI::Windows::Graphics::DirectX;
using namespace ABI::Windows::Graphics::DirectX::Direct3D11;
using namespace ABI::Windows::Foundation;
using namespace ABI::Windows::Graphics;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Make;

namespace hdrfix {

// -------------------------------------------------------------
// ProxyFrame
// -------------------------------------------------------------
ProxyFrame::ProxyFrame(IDirect3DSurface* surface, TimeSpan ts, SizeInt32 size)
    : m_surface(surface), m_timeSpan(ts), m_size(size)
{
}

HRESULT STDMETHODCALLTYPE ProxyFrame::get_Surface(IDirect3DSurface** value)
{
    if (!value) return E_POINTER;
    if (m_closed || !m_surface) {
        *value = nullptr;
        return RO_E_CLOSED;
    }
    return m_surface.CopyTo(value);
}

HRESULT STDMETHODCALLTYPE ProxyFrame::get_SystemRelativeTime(TimeSpan* value)
{
    if (!value) return E_POINTER;
    *value = m_timeSpan;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ProxyFrame::get_ContentSize(SizeInt32* value)
{
    if (!value) return E_POINTER;
    *value = m_size;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE ProxyFrame::Close()
{
    m_closed = true;
    m_surface.Reset();
    return S_OK;
}

// -------------------------------------------------------------
// ProxyFramePool
// -------------------------------------------------------------
ProxyFramePool::ProxyFramePool(
    ComPtr<IDirect3D11CaptureFramePool> realPool,
    ComPtr<IDirect3DDevice> winrtDevice,
    ComPtr<ID3D11Device> d3dDevice,
    INT32 numberOfBuffers,
    SizeInt32 size,
    bool isHdrElevated)
    : m_realPool(realPool),
      m_winrtDevice(winrtDevice),
      m_d3dDevice(d3dDevice),
      m_bufferCount(numberOfBuffers > 0 ? numberOfBuffers : 3),
      m_isHdrElevated(isHdrElevated)
{
    if (m_d3dDevice) {
        m_d3dDevice->GetImmediateContext(m_d3dContext.GetAddressOf());

        if (m_isHdrElevated) {
            m_toneMapper.Initialize(m_d3dDevice.Get(), L"shaders/tonemap_scrgb.hlsl");

            // 查询系统 SDR 白点与参数
            float sdrWhite = 280.0f;
            for (const auto& s : QueryOutputHdrStates()) {
                if (s.hdrEnabled && s.sdrWhiteNits > 10.0f) {
                    sdrWhite = s.sdrWhiteNits;
                    break;
                }
            }

            const auto& cfg = ConfigManager::Instance().GetConfig();
            ToneMapParams params{};
            params.sdrWhiteNits = (cfg.hdr.sdrReferenceWhite > 10.0f) ? cfg.hdr.sdrReferenceWhite : sdrWhite;
            params.sourcePeakNits = (cfg.hdr.sourcePeakNits > 10.0f) ? cfg.hdr.sourcePeakNits : 1000.0f;
            params.exposure = cfg.hdr.exposure;
            params.highlightRollOff = cfg.hdr.highlightRollOff;

            // 解析 OETF 传递函数（默认 sRGB，暗部扎实对比度通透，与 OBS 对齐）
            std::string outStr = cfg.general.output;
            std::transform(outStr.begin(), outStr.end(), outStr.begin(), [](char c) { return static_cast<char>(::tolower(static_cast<unsigned char>(c))); });
            if (outStr == "rec709" || outStr == "bt709") {
                params.oetfType = static_cast<uint32_t>(OetfType::Rec709);
            } else if (outStr == "linear") {
                params.oetfType = static_cast<uint32_t>(OetfType::Linear);
            } else if (outStr == "gamma24" || outStr == "bt1886") {
                params.oetfType = static_cast<uint32_t>(OetfType::Gamma24);
            } else {
                params.oetfType = static_cast<uint32_t>(OetfType::sRGB);
            }

            // 解析色调映射算子（默认 BT2390 工业级 EETF，与 OBS 对齐）
            std::string tm = cfg.hdr.toneMapper;
            std::transform(tm.begin(), tm.end(), tm.begin(), [](char c) { return static_cast<char>(::tolower(static_cast<unsigned char>(c))); });
            if (tm == "clamp") params.toneMapper = static_cast<uint32_t>(ToneMapperType::Clamp);
            else if (tm == "reinhard") params.toneMapper = static_cast<uint32_t>(ToneMapperType::ExtendedReinhard);
            else if (tm == "obsreinhard" || tm == "reinhard_obs") params.toneMapper = static_cast<uint32_t>(ToneMapperType::OBSReinhard);
            else if (tm == "hable") params.toneMapper = static_cast<uint32_t>(ToneMapperType::Hable);
            else if (tm == "aces") params.toneMapper = static_cast<uint32_t>(ToneMapperType::ACES);
            else if (tm == "lumahuepreserve" || tm == "luma") params.toneMapper = static_cast<uint32_t>(ToneMapperType::LumaHuePreserving);
            else params.toneMapper = static_cast<uint32_t>(ToneMapperType::BT2390); // 默认 BT2390 (OBS Studio 28+ 官方对齐)

            m_toneMapper.SetParams(params);
            BRIDGE_LOG("FramePool", "ToneMapConfig: toneMapper=%u oetf=%u sdrWhite=%.1f peak=%.1f exposure=%.2f rollOff=%.2f",
                       params.toneMapper, params.oetfType, params.sdrWhiteNits, params.sourcePeakNits, params.exposure, params.highlightRollOff);

            EnsureOutputPool(static_cast<UINT>(size.Width), static_cast<UINT>(size.Height));
        }
    }
    BRIDGE_LOG("FramePool", "ProxyFramePool initialized: d3dDevice=%p d3dContext=%p isHdrElevated=%d bufferCount=%d size=%dx%d",
               m_d3dDevice.Get(), m_d3dContext.Get(), m_isHdrElevated ? 1 : 0, m_bufferCount, size.Width, size.Height);
}

ProxyFramePool::~ProxyFramePool()
{
    BRIDGE_LOG("FramePool", "ProxyFramePool destroyed (totalProcessedFrames=%llu)", m_processedFrames.load());
    ReleaseOutputPool();
}

void ProxyFramePool::ReleaseOutputPool()
{
    m_outputPool.clear();
    m_poolWidth = 0;
    m_poolHeight = 0;
    m_currentSlot = 0;
}

bool ProxyFramePool::EnsureOutputPool(UINT width, UINT height)
{
    if (width == 0 || height == 0 || !m_d3dDevice) return false;
    if (m_poolWidth == width && m_poolHeight == height && !m_outputPool.empty()) {
        return true;
    }
    BRIDGE_LOG("FramePool", "EnsureOutputPool reallocating buffers: %ux%u (previous: %ux%u)", width, height, m_poolWidth, m_poolHeight);

    ReleaseOutputPool();

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED; // 编码器共享兼容属性

    size_t count = static_cast<size_t>(std::max(2, m_bufferCount));
    m_outputPool.resize(count);

    for (size_t i = 0; i < count; ++i) {
        auto& slot = m_outputPool[i];
        HRESULT hr = m_d3dDevice->CreateTexture2D(&desc, nullptr, slot.texture.GetAddressOf());
        if (FAILED(hr)) {
            ReleaseOutputPool();
            return false;
        }

        hr = m_d3dDevice->CreateRenderTargetView(slot.texture.Get(), nullptr, slot.rtv.GetAddressOf());
        if (FAILED(hr)) {
            ReleaseOutputPool();
            return false;
        }

        ComPtr<IDXGISurface> dxgiSurface;
        hr = slot.texture.As(&dxgiSurface);
        if (FAILED(hr)) {
            ReleaseOutputPool();
            return false;
        }

        ComPtr<IInspectable> inspectableSurface;
        hr = ::CreateDirect3D11SurfaceFromDXGISurface(dxgiSurface.Get(), inspectableSurface.GetAddressOf());
        if (FAILED(hr)) {
            ReleaseOutputPool();
            return false;
        }

        hr = inspectableSurface.As(&slot.surface);
        if (FAILED(hr)) {
            ReleaseOutputPool();
            return false;
        }
    }

    m_poolWidth = width;
    m_poolHeight = height;
    m_currentSlot = 0;
    return true;
}

HRESULT STDMETHODCALLTYPE ProxyFramePool::Recreate(
    IDirect3DDevice* device,
    DirectXPixelFormat pixelFormat,
    INT32 numberOfBuffers,
    SizeInt32 size)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    DirectXPixelFormat realFormat = pixelFormat;
    if (m_isHdrElevated && pixelFormat == DirectXPixelFormat_B8G8R8A8UIntNormalized) {
        realFormat = DirectXPixelFormat_R16G16B16A16Float;
    }

    HRESULT hr = m_realPool->Recreate(device, realFormat, numberOfBuffers, size);
    if (SUCCEEDED(hr)) {
        m_bufferCount = numberOfBuffers;
        if (m_isHdrElevated) {
            EnsureOutputPool(static_cast<UINT>(size.Width), static_cast<UINT>(size.Height));
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE ProxyFramePool::TryGetNextFrame(IDirect3D11CaptureFrame** result)
{
    if (!result) return E_POINTER;
    *result = nullptr;

    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_realPool) return RO_E_CLOSED;

    ComPtr<IDirect3D11CaptureFrame> realFrame;
    HRESULT hr = m_realPool->TryGetNextFrame(realFrame.GetAddressOf());
    if (FAILED(hr) || !realFrame) {
        return hr; // 无新帧
    }

    // 若未升级（SDR 原生模式）或被 SafetyGuard 旁路/发生异常，Fail-open 原样交还真实帧
    if (!m_isHdrElevated || !SafetyGuard::Instance().CanIntercept() || !m_toneMapper.IsInitialized() || !m_d3dContext) {
        static bool s_loggedBypass = false;
        if (!s_loggedBypass) {
            BRIDGE_LOG("FramePool", "TryGetNextFrame Bypassed: isElevated=%d canIntercept=%d tmInit=%d d3dCtx=%p",
                       m_isHdrElevated ? 1 : 0, SafetyGuard::Instance().CanIntercept() ? 1 : 0,
                       m_toneMapper.IsInitialized() ? 1 : 0, m_d3dContext.Get());
            s_loggedBypass = true;
        }
        *result = realFrame.Detach();
        return S_OK;
    }

    TimeSpan ts{};
    SizeInt32 contentSize{};
    realFrame->get_SystemRelativeTime(&ts);
    realFrame->get_ContentSize(&contentSize);

    ComPtr<IDirect3DSurface> inSurface;
    hr = realFrame->get_Surface(inSurface.GetAddressOf());
    if (FAILED(hr) || !inSurface) {
        BRIDGE_LOG("FramePool", "get_Surface failed (hr=0x%08lX)", hr);
        *result = realFrame.Detach();
        return S_OK;
    }

    ComPtr<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess> access;
    hr = inSurface.As(&access);
    if (FAILED(hr)) {
        BRIDGE_LOG("FramePool", "inSurface.As(IDirect3DDxgiInterfaceAccess) failed (hr=0x%08lX)", hr);
        *result = realFrame.Detach();
        return S_OK;
    }

    ComPtr<ID3D11Texture2D> inTex;
    hr = access->GetInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(inTex.GetAddressOf()));
    if (FAILED(hr) || !inTex) {
        BRIDGE_LOG("FramePool", "access->GetInterface(ID3D11Texture2D) failed (hr=0x%08lX)", hr);
        *result = realFrame.Detach();
        return S_OK;
    }

    D3D11_TEXTURE2D_DESC inDesc{};
    inTex->GetDesc(&inDesc);

    // 确保池尺寸匹配
    if (!EnsureOutputPool(inDesc.Width, inDesc.Height)) {
        BRIDGE_LOG("FramePool", "EnsureOutputPool(%u, %u) failed", inDesc.Width, inDesc.Height);
        *result = realFrame.Detach();
        return S_OK;
    }

    auto& slot = m_outputPool[m_currentSlot];
    m_currentSlot = (m_currentSlot + 1) % m_outputPool.size();

    // 在 GPU 上执行全屏色调映射 Pass (FP16 scRGB -> Rec.709 BGRA8)
    bool ok = m_toneMapper.Execute(m_d3dContext.Get(), inTex.Get(), slot.texture.Get());
    if (!ok) {
        static int s_failCount = 0;
        if (++s_failCount <= 10) {
            BRIDGE_LOG("FramePool", "m_toneMapper.Execute FAILED! Falling back to raw frame (failCount=%d)", s_failCount);
        }
        *result = realFrame.Detach();
        return S_OK;
    }

    uint64_t frameNum = ++m_processedFrames;
    if (frameNum <= 5 || (frameNum % 300 == 0)) {
        BRIDGE_LOG("FramePool", "ToneMapped frame #%llu (in: %ux%u format=%d -> out slot %zu)",
                   frameNum, inDesc.Width, inDesc.Height, inDesc.Format, m_currentSlot);
    }

    // 封装并返回包含映射后 BGRA8 表面、原时间戳与原尺寸的代理帧
    auto proxy = Make<ProxyFrame>(slot.surface.Get(), ts, contentSize);
    return proxy.CopyTo(result);
}

HRESULT STDMETHODCALLTYPE ProxyFramePool::add_FrameArrived(
    __FITypedEventHandler_2_Windows__CGraphics__CCapture__CDirect3D11CaptureFramePool_IInspectable* handler,
    EventRegistrationToken* token)
{
    return m_realPool->add_FrameArrived(handler, token);
}

HRESULT STDMETHODCALLTYPE ProxyFramePool::remove_FrameArrived(EventRegistrationToken token)
{
    return m_realPool->remove_FrameArrived(token);
}

HRESULT STDMETHODCALLTYPE ProxyFramePool::CreateCaptureSession(
    IGraphicsCaptureItem* item,
    IGraphicsCaptureSession** result)
{
    return m_realPool->CreateCaptureSession(item, result);
}

HRESULT STDMETHODCALLTYPE ProxyFramePool::get_DispatcherQueue(ABI::Windows::System::IDispatcherQueue** value)
{
    return m_realPool->get_DispatcherQueue(value);
}

HRESULT STDMETHODCALLTYPE ProxyFramePool::Close()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    ReleaseOutputPool();
    ComPtr<IClosable> closable;
    if (SUCCEEDED(m_realPool.As(&closable))) {
        closable->Close();
    }
    m_realPool.Reset();
    return S_OK;
}

// -------------------------------------------------------------
// ProxyFramePoolStatics
// -------------------------------------------------------------
ProxyFramePoolStatics::ProxyFramePoolStatics(
    ComPtr<IDirect3D11CaptureFramePoolStatics> realStatics1,
    ComPtr<IDirect3D11CaptureFramePoolStatics2> realStatics2)
    : m_realStatics1(realStatics1), m_realStatics2(realStatics2)
{
}

HRESULT ProxyFramePoolStatics::InterceptCreate(
    bool freeThreaded,
    IDirect3DDevice* device,
    DirectXPixelFormat pixelFormat,
    INT32 numberOfBuffers,
    SizeInt32 size,
    IDirect3D11CaptureFramePool** result)
{
    if (!result) return E_POINTER;
    *result = nullptr;

    // 从 WinRT IDirect3DDevice 获取底层的 ID3D11Device
    ComPtr<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess> access;
    ComPtr<ID3D11Device> d3dDevice;
    if (device && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(access.GetAddressOf())))) {
        access->GetInterface(IID_PPV_ARGS(d3dDevice.GetAddressOf()));
    }

    // 1. 四级版本兼容性与能力检测 (CompatibilityManager)
    auto compatDecision = CompatibilityManager::Instance().Evaluate(d3dDevice.Get(), pixelFormat);

    BRIDGE_LOG("FramePool", "InterceptCreate: freeThreaded=%d reqFmt=%d buffers=%d size=%dx%d",
               freeThreaded ? 1 : 0, static_cast<int>(pixelFormat), numberOfBuffers, size.Width, size.Height);
    BRIDGE_LOG("FramePool", "Decision: tier=%d, bridgeEnabled=%d, CanIntercept=%d",
               static_cast<int>(compatDecision.tier), compatDecision.bridgeEnabled ? 1 : 0,
               SafetyGuard::Instance().CanIntercept() ? 1 : 0);

    // 2. 安全前置检查 (SafetyGuard 与 兼容性守门：Incompatible/DiagnoseOnly/KillSwitch 均强制 Fail-open)
    if (!SafetyGuard::Instance().CanIntercept() || !compatDecision.bridgeEnabled) {
        BRIDGE_LOG("FramePool", "SafetyGuard CanIntercept=false or bridgeEnabled=false -> Passthrough native");
        if (freeThreaded && m_realStatics2) {
            return m_realStatics2->CreateFreeThreaded(device, pixelFormat, numberOfBuffers, size, result);
        } else if (m_realStatics1) {
            return m_realStatics1->Create(device, pixelFormat, numberOfBuffers, size, result);
        }
        return E_NOINTERFACE;
    }

    // 3. AutoDetect 判定树 (ColorDetector: HDR 状态、格式判定与色彩空间识别)
    DXGI_FORMAT reqFormat = static_cast<DXGI_FORMAT>(pixelFormat);
    DecisionResult decision = ColorDetector::Evaluate(reqFormat);
    bool shouldElevate = (decision.action == DecisionAction::ElevateAndConvert);

    BRIDGE_LOG("FramePool", "ColorDecision: action=%d, colorSpace=%d, shouldElevate=%d, sdrWhite=%.1f",
               static_cast<int>(decision.action), static_cast<int>(decision.colorSpace),
               shouldElevate ? 1 : 0, decision.sdrWhiteNits);

    DirectXPixelFormat targetFormat = pixelFormat;
    if (shouldElevate) {
        targetFormat = DirectXPixelFormat_R16G16B16A16Float;
    }

    ComPtr<IDirect3D11CaptureFramePool> realPool;
    HRESULT hr = S_OK;

    if (freeThreaded && m_realStatics2) {
        hr = m_realStatics2->CreateFreeThreaded(device, targetFormat, numberOfBuffers, size, realPool.GetAddressOf());
    } else if (m_realStatics1) {
        hr = m_realStatics1->Create(device, targetFormat, numberOfBuffers, size, realPool.GetAddressOf());
    } else {
        hr = E_NOINTERFACE;
    }

    BRIDGE_LOG("FramePool", "Native Create/CreateFreeThreaded: hr=0x%08lX (targetFormat=%d)", hr, static_cast<int>(targetFormat));

    if (FAILED(hr) || !realPool) {
        return hr;
    }

    if (!shouldElevate) {
        *result = realPool.Detach();
        return S_OK;
    }

    auto proxyPool = Make<ProxyFramePool>(realPool, device, d3dDevice, numberOfBuffers, size, true);
    return proxyPool.CopyTo(result);
}

HRESULT STDMETHODCALLTYPE ProxyFramePoolStatics::Create(
    IDirect3DDevice* device,
    DirectXPixelFormat pixelFormat,
    INT32 numberOfBuffers,
    SizeInt32 size,
    IDirect3D11CaptureFramePool** result)
{
    return InterceptCreate(false, device, pixelFormat, numberOfBuffers, size, result);
}

HRESULT STDMETHODCALLTYPE ProxyFramePoolStatics::CreateFreeThreaded(
    IDirect3DDevice* device,
    DirectXPixelFormat pixelFormat,
    INT32 numberOfBuffers,
    SizeInt32 size,
    IDirect3D11CaptureFramePool** result)
{
    return InterceptCreate(true, device, pixelFormat, numberOfBuffers, size, result);
}

} // namespace hdrfix
