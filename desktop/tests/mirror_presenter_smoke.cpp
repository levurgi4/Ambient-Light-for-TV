// Interactive manual test: no screen capture, controller or user settings.
// A small synthetic texture verifies real D3D presentation, rotation, black
// bars, resizing and swap-chain recreation on the same HWND.
#include "capture/gpu_mirror_presenter.hpp"
#include "mirror_window.hpp"
#include <QApplication>
#include <QImage>
#include <QScreen>
#include <QTimer>
#include <iostream>
#include <array>

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    using Microsoft::WRL::ComPtr;
    ambilight::MirrorWindow window(nullptr);
    window.setWindowFlag(Qt::WindowStaysOnTopHint, true);
    window.resize(320, 240);
    window.move(QApplication::primaryScreen()->availableGeometry().center() - QPoint(160, 120));
    window.show();
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL level;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
        device.GetAddressOf(), &level, context.GetAddressOf());
    if (FAILED(hr)) return 1;
    std::array<unsigned, 32> pixels;
    for (int y = 0; y < 4; ++y) for (int x = 0; x < 8; ++x)
        pixels[y * 8 + x] = x < 4 ? 0xff602020 : 0xff206020;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = 8; desc.Height = 4; desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{pixels.data(), 8 * 4, 0};
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> view;
    hr = device->CreateTexture2D(&desc, &data, texture.GetAddressOf());
    if (SUCCEEDED(hr)) hr = device->CreateShaderResourceView(texture.Get(), nullptr, view.GetAddressOf());
    ambilight::capture::GpuMirrorPresenter presenter;
    if (SUCCEEDED(hr)) hr = presenter.initialize(device.Get(), reinterpret_cast<HWND>(window.winId()));
    if (FAILED(hr)) { std::cerr << "Initialization failed: " << std::hex << hr << '\n'; return 1; }
    int phase = 0;
    bool verifying = false;
    QTimer render;
    QObject::connect(&render, &QTimer::timeout, &app, [&] {
        bool ready = false, presented = false;
        HRESULT result = presenter.prepare(&ready);
        if (SUCCEEDED(result) && ready)
            result = presenter.present(view.Get(), (phase % 2) == 1 ? QSize(4, 8) : QSize(8, 4),
                static_cast<UINT>(phase), &presented);
        if (FAILED(result)) { std::cerr << "Render failed: " << std::hex << result << '\n'; app.exit(1); return; }
        if (!presented || verifying) return;
        verifying = true;
        QTimer::singleShot(250, &app, [&] {
            // Read the composed desktop, not the HWND's stale GDI backing
            // surface: flip-model D3D does not paint into that surface.
            const QPoint position = window.mapToGlobal(QPoint{});
            const QImage shot = window.screen()->grabWindow(0, position.x(), position.y(),
                window.width(), window.height()).toImage();
            if (shot.isNull()) { std::cerr << "No window image\n"; app.exit(1); return; }
            const auto at = [&](double x, double y) {
                return shot.pixelColor(static_cast<int>(x * shot.width()), static_cast<int>(y * shot.height()));
            };
            const QColor first = (phase % 2) == 1 ? at(.5, .25) : at(.25, .5);
            const QColor second = (phase % 2) == 1 ? at(.5, .75) : at(.75, .5);
            const QColor border = (phase % 2) == 1 ? at(.05, .5) : at(.5, .05);
            const auto red = [](QColor c) { return c.red() > c.green() + 30; };
            const auto green = [](QColor c) { return c.green() > c.red() + 30; };
            const bool correct = phase >= 2 ? green(first) && red(second) : red(first) && green(second);
            if (!correct || border.red() > 10 || border.green() > 10 || border.blue() > 10) {
                std::cerr << "Incorrect rendered colors/letterbox in phase " << phase
                    << ": " << first.name().toStdString() << ' ' << second.name().toStdString()
                    << ' ' << border.name().toStdString() << '\n';
                app.exit(1); return;
            }
            std::cout << "Mirror phase " << phase << " pixels verified\n";
            if (++phase == 4) { render.stop(); presenter.reset(); app.quit(); return; }
            if (phase == 1) window.resize(320, 320);
            if (phase == 2) {
                window.hide();
                bool readyWhileHidden = true;
                if (FAILED(presenter.prepare(&readyWhileHidden)) || readyWhileHidden) {
                    std::cerr << "Hidden window must skip rendering\n"; app.exit(1); return;
                }
                window.show();
                presenter.reset();
                const HRESULT initialized = presenter.initialize(device.Get(), reinterpret_cast<HWND>(window.winId()));
                if (FAILED(initialized)) { std::cerr << "Recreate failed\n"; app.exit(1); return; }
            }
            verifying = false;
        });
    });
    render.start(16);
    QTimer::singleShot(10000, &app, [&] { std::cerr << "Mirror test timeout\n"; app.exit(1); });
    return app.exec();
}
