#include "wgc_frame_source.hpp"

#include <roapi.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <algorithm>
#include <atomic>

namespace ambilight::capture {
namespace wgc = winrt::Windows::Graphics::Capture;
namespace graphics = winrt::Windows::Graphics;
namespace directx = winrt::Windows::Graphics::DirectX;

struct WgcFrameSource::Impl {
    bool apartmentInitialized = false;
    directx::Direct3D11::IDirect3DDevice device{nullptr};
    wgc::GraphicsCaptureItem item{nullptr};
    wgc::Direct3D11CaptureFramePool pool{nullptr};
    wgc::GraphicsCaptureSession session{nullptr};
    wgc::Direct3D11CaptureFrame frame{nullptr};
    graphics::SizeInt32 size{};
    int targetFps = -1;
    HWND window = nullptr;
    HMONITOR sourceMonitor = nullptr;
    DWORD processId = 0;
    std::shared_ptr<std::atomic_bool> itemClosed;

    void setTargetFps(const int fps)
    {
        if (targetFps == fps) return;
        // Older Windows versions lack the producer-side rate limiter;
        // the application still polls and processes at its selected FPS.
#if defined(____x_ABI_CWindows_CGraphics_CCapture_CIGraphicsCaptureSession5_INTERFACE_DEFINED__)
        if (const auto pacing = session.try_as<wgc::IGraphicsCaptureSession5>()) {
            // Mirror cadence is controlled by its consumer. A second strict
            // producer limiter quantized to vblank can halve 60/59.94 Hz and
            // distort 24/25 FPS. Zero disables that extra limiter for mirrors.
            const int rate = std::max(1, fps);
            pacing.MinUpdateInterval(winrt::Windows::Foundation::TimeSpan{
                fps == 0 ? 0 : std::max(0, 10'000'000 / rate - 10'000)});
        }
#endif
        targetFps = fps;
    }
};

WgcFrameSource::WgcFrameSource() : impl_(std::make_unique<Impl>()) {}
WgcFrameSource::~WgcFrameSource()
{
    reset();
    if (impl_->apartmentInitialized) {
        winrt::clear_factory_cache();
        RoUninitialize();
    }
}

HRESULT WgcFrameSource::initialize(
    ID3D11Device* device, HMONITOR monitor, const int targetFps, QString* error,
    HWND sourceWindow, DWORD sourceProcessId)
{
    reset();
    try {
        // Keep WinRT alive for the worker's lifetime. Uninitializing between
        // DXGI/WGC switches can invalidate cached activation factories.
        if (!impl_->apartmentInitialized) {
            winrt::check_hresult(RoInitialize(RO_INIT_MULTITHREADED));
            impl_->apartmentInitialized = true;
        }
        if (!wgc::GraphicsCaptureSession::IsSupported()) {
            if (error) *error = QStringLiteral("Windows Graphics Capture is unavailable.");
            reset();
            return E_NOTIMPL;
        }
        winrt::com_ptr<IDXGIDevice> dxgiDevice;
        winrt::check_hresult(device->QueryInterface(
            __uuidof(IDXGIDevice), dxgiDevice.put_void()));
        winrt::com_ptr<IInspectable> inspectable;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(
            dxgiDevice.get(), inspectable.put()));
        impl_->device = inspectable.as<directx::Direct3D11::IDirect3DDevice>();
        const auto interop = winrt::get_activation_factory<
            wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        if (sourceWindow) {
            DWORD processId = 0;
            if (!IsWindow(sourceWindow)
                || !GetWindowThreadProcessId(sourceWindow, &processId)
                || processId != sourceProcessId) {
                if (error) *error = QStringLiteral("The selected window closed. Select the source window again.");
                return HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE);
            }
            winrt::check_hresult(interop->CreateForWindow(sourceWindow,
                winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(impl_->item)));
            impl_->window = sourceWindow;
            impl_->sourceMonitor = monitor;
            impl_->processId = sourceProcessId;
        } else {
            winrt::check_hresult(interop->CreateForMonitor(monitor,
                winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(impl_->item)));
        }
        impl_->itemClosed = std::make_shared<std::atomic_bool>(false);
        impl_->item.Closed([closed = impl_->itemClosed](auto&&, auto&&) {
            closed->store(true, std::memory_order_relaxed);
        });
        impl_->size = impl_->item.Size();
        impl_->pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
            impl_->device, directx::DirectXPixelFormat::B8G8R8A8UIntNormalized,
            2, impl_->size);
        impl_->session = impl_->pool.CreateCaptureSession(impl_->item);
        if (const auto cursor = impl_->session.try_as<wgc::IGraphicsCaptureSession2>()) {
            cursor.IsCursorCaptureEnabled(false);
        }
        impl_->setTargetFps(targetFps);
        impl_->session.StartCapture();
        return S_OK;
    } catch (const winrt::hresult_error& exception) {
        if (error) *error = QStringLiteral("Windows Graphics Capture: %1")
            .arg(QString::fromWCharArray(exception.message().c_str()));
        const HRESULT result = exception.code();
        reset();
        return result;
    }
}

HRESULT WgcFrameSource::acquire(
    ID3D11Texture2D** texture, const int targetFps, QString* error)
{
    *texture = nullptr;
    releaseFrame();
    if (!impl_->pool) return E_UNEXPECTED;
    DWORD processId = 0;
    if (impl_->window && (!IsWindow(impl_->window)
        || !GetWindowThreadProcessId(impl_->window, &processId)
        || processId != impl_->processId)) {
        if (error) *error = QStringLiteral("The selected window closed. Select the source window again.");
        return HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE);
    }
    if (impl_->itemClosed && impl_->itemClosed->load(std::memory_order_relaxed)) {
        if (impl_->window) {
            if (error) *error = QStringLiteral("The selected capture window closed. Select it again.");
            return HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE);
        }
        return DXGI_ERROR_ACCESS_LOST;
    }
    if (impl_->window && IsIconic(impl_->window)) return DXGI_ERROR_WAIT_TIMEOUT;
    if (impl_->window && MonitorFromWindow(impl_->window, MONITOR_DEFAULTTONEAREST) != impl_->sourceMonitor) {
        if (error) *error = QStringLiteral("The source moved to another display. Select its window again.");
        return HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE);
    }
    try {
        impl_->setTargetFps(targetFps);
        // Drain at most the two buffered frames, keeping the freshest one.
        for (int index = 0; index < 2; ++index) {
            auto next = impl_->pool.TryGetNextFrame();
            if (!next) break;
            releaseFrame();
            impl_->frame = std::move(next);
        }
        if (!impl_->frame) return DXGI_ERROR_WAIT_TIMEOUT;
        const auto size = impl_->frame.ContentSize();
        if (size.Width <= 0 || size.Height <= 0) {
            releaseFrame();
            return DXGI_ERROR_WAIT_TIMEOUT;
        }
        if (size.Width != impl_->size.Width || size.Height != impl_->size.Height) {
            releaseFrame();
            // Recreate the whole source through the existing recovery path.
            // Recreate(pool) alone may leave a resized but now static window
            // without a fresh full-size frame until it paints again.
            return DXGI_ERROR_ACCESS_LOST;
        }
        const auto surface = impl_->frame.Surface().as<
            Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        winrt::check_hresult(surface->GetInterface(__uuidof(ID3D11Texture2D),
            reinterpret_cast<void**>(texture)));
        return S_OK;
    } catch (const winrt::hresult_error& exception) {
        if (error) *error = QStringLiteral("Windows Graphics Capture: %1")
            .arg(QString::fromWCharArray(exception.message().c_str()));
        releaseFrame();
        return exception.code();
    }
}

void WgcFrameSource::releaseFrame() noexcept
{
    if (impl_->frame) {
        try { impl_->frame.Close(); } catch (...) {}
        impl_->frame = nullptr;
    }
}

void WgcFrameSource::reset() noexcept
{
    releaseFrame();
    if (impl_->session) {
        try { impl_->session.Close(); } catch (...) {}
        impl_->session = nullptr;
    }
    if (impl_->pool) {
        try { impl_->pool.Close(); } catch (...) {}
        impl_->pool = nullptr;
    }
    impl_->item = nullptr;
    impl_->itemClosed.reset();
    impl_->device = nullptr;
    impl_->targetFps = -1;
    impl_->window = nullptr;
    impl_->sourceMonitor = nullptr;
    impl_->processId = 0;
    impl_->size = {};
}

} // namespace ambilight::capture
