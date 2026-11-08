// Interactive, one-monitor WGC -> GPU mirror integration test. No settings,
// serial device, display mode changes or user applications are touched.
#include "capture/gpu_mirror_presenter.hpp"
#include "capture/mirror_geometry.hpp"
#include "capture/wgc_frame_source.hpp"
#include "mirror_window.hpp"
#include <QApplication>
#include <QElapsedTimer>
#include <QPainter>
#include <QScreen>
#include <QTimer>
#include <QThread>
#include <QEventLoop>
#include <QWidget>
#include <d3d11_4.h>
#include <array>
#include <iostream>

class VideoPattern final : public QWidget {
public:
    int tick = 0;
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), QColor(20 + tick % 100, 45, 70));
        painter.fillRect(QRect(tick % width(), 0, 25, height()), QColor(180, 120, 80));
    }
};

static int runStream(HWND sourceHwnd, HWND targetHwnd, double refreshRate)
{
    using namespace ambilight::capture;
    using Microsoft::WRL::ComPtr;
    QEventLoop loop;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL level;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
        device.GetAddressOf(), &level, context.GetAddressOf());
    if (FAILED(hr)) return 1;
    ComPtr<ID3D11Multithread> protection;
    if (SUCCEEDED(context.As(&protection))) protection->SetMultithreadProtected(TRUE);
    WgcFrameSource capture;
    GpuMirrorPresenter presenter;
    QString error;
    hr = capture.initialize(device.Get(), MonitorFromWindow(sourceHwnd, MONITOR_DEFAULTTOPRIMARY),
        0, &error, sourceHwnd, GetCurrentProcessId());
    if (SUCCEEDED(hr)) hr = presenter.initialize(device.Get(), targetHwnd);
    if (FAILED(hr)) { std::cerr << "Initialize: " << std::hex << hr << ' ' << error.toStdString(); return 1; }
    ComPtr<ID3D11Texture2D> cached;
    ComPtr<ID3D11ShaderResourceView> view;
    QSize sourceSize;
    CaptureRateGate gate;
    CaptureConfiguration options;
    constexpr std::array rates{24, 30, 60, 25};
    int phase = 0, submitted = 0, received = 0;
    bool pending = false;
    QElapsedTimer clock, phaseClock;
    clock.start(); phaseClock.start();
    QTimer render;
    render.setTimerType(Qt::PreciseTimer);
    QObject::connect(&render, &QTimer::timeout, &loop, [&] {
        const qint64 now = clock.nsecsElapsed();
        if (!gate.due(now)) return;
        ComPtr<ID3D11Texture2D> frame;
        HRESULT result = capture.acquire(frame.GetAddressOf(), 0, &error);
        if (SUCCEEDED(result)) {
            ++received;
            D3D11_TEXTURE2D_DESC desc{};
            frame->GetDesc(&desc);
            if (!cached) {
                desc.Usage = D3D11_USAGE_DEFAULT;
                desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                desc.CPUAccessFlags = desc.MiscFlags = 0;
                result = device->CreateTexture2D(&desc, nullptr, cached.GetAddressOf());
                if (SUCCEEDED(result)) result = device->CreateShaderResourceView(cached.Get(), nullptr, view.GetAddressOf());
                sourceSize = QSize(desc.Width, desc.Height);
            }
            if (SUCCEEDED(result)) { context->CopyResource(cached.Get(), frame.Get()); pending = true; }
            context->Flush();
            capture.releaseFrame();
        } else if (result == DXGI_ERROR_WAIT_TIMEOUT) result = S_OK;
        bool ready = false, presented = false;
        if (SUCCEEDED(result) && pending) result = presenter.prepare(&ready);
        if (SUCCEEDED(result) && ready) result = presenter.present(view.Get(), sourceSize, 0, &presented, options);
        if (FAILED(result)) {
            std::cerr << "Render/capture failed: " << std::hex << result << ' ' << error.toStdString() << '\n';
            loop.exit(1); return;
        }
        if (presented) { ++submitted; pending = false; gate.advance(now, rates[phase]); }
        else gate.retry(now);
        if (phaseClock.elapsed() < 3500) return;
        const double fps = submitted * 1000.0 / phaseClock.elapsed();
        std::cout << "Requested " << rates[phase] << ": submitted " << fps
            << " FPS, source frames " << received << ", fill " << options.mirrorFillMode << std::endl;
        const double expected = std::min(double(rates[phase]), refreshRate);
        if (fps < expected * 0.80 || fps > rates[phase] + 2) {
            std::cerr << "Unexpected mirror cadence (needs an interactive unoccluded desktop)\n";
            loop.exit(1); return;
        }
        if (++phase == int(rates.size())) { loop.quit(); return; }
        options.mirrorFillMode = phase;
        submitted = received = 0;
        gate = {};
        phaseClock.restart();
    });
    render.start(2);
    QTimer::singleShot(25000, &loop, [&] { std::cerr << "Stream test timeout\n"; loop.exit(1); });
    const int result = loop.exec();
    render.stop();
    presenter.reset();
    context->ClearState(); context->Flush();
    capture.reset();
    return result;
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    using namespace ambilight::capture;
    using Microsoft::WRL::ComPtr;
    VideoPattern source;
    ambilight::MirrorWindow target(nullptr);
    const auto area = QApplication::primaryScreen()->availableGeometry();
    source.setWindowTitle(QStringLiteral("Synthetic video — capture FPS test"));
    source.setGeometry(area.x() + 30, area.y() + 60, 480, 200);
    target.setGeometry(area.x() + 540, area.y() + 60, 320, 240);
    source.show(); target.show();
    const HWND sourceHwnd = reinterpret_cast<HWND>(source.winId());
    const HWND targetHwnd = reinterpret_cast<HWND>(target.winId());
    const double refreshRate = source.screen()->refreshRate();
    QTimer animation;
    animation.setTimerType(Qt::PreciseTimer);
    QObject::connect(&animation, &QTimer::timeout, &app, [&] { ++source.tick; source.update(); });
    animation.start(8);
    // Match production: WinRT and D3D are owned by a worker, UI HWNDs stay
    // responsive through capture initialization and asynchronous teardown.
    auto* worker = QThread::create([&] {
        const int workerResult = runStream(sourceHwnd, targetHwnd, refreshRate);
        QMetaObject::invokeMethod(&app, [&, workerResult] { app.exit(workerResult); });
    });
    worker->start();
    // Fail hard if a driver deadlocks teardown rather than hanging test runs.
    QTimer::singleShot(35000, &app, [] { std::cerr << "Worker stop timeout\n"; ExitProcess(1); });
    const int result = app.exec();
    worker->wait();
    delete worker;
    return result;
}
