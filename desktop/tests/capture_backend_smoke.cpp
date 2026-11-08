// Manual integration check: requires an interactive Windows desktop and GPU.
// No serial transport or persistent settings are opened. A small animated
// window ensures there are real desktop updates even on an idle workstation.
#include "capture/screen_capture_worker.hpp"
#include <QBackingStore>
#include <QGuiApplication>
#include <QPainter>
#include <QScreen>
#include <QTimer>
#include <QWindow>
#include <algorithm>
#include <array>
#include <iostream>
#include <vector>
#include <windows.h>

class TestWindow final : public QWindow {
public:
    TestWindow() : store_(this)
    {
        setTitle(QStringLiteral("Ambilight capture integration check"));
        resize(360, 90);
        const QRect area = QGuiApplication::primaryScreen()->availableGeometry();
        setPosition(area.center() - QPoint(width() / 2, height() / 2));
    }
    void render()
    {
        if (!isExposed()) return;
        store_.resize(size());
        const QRect area(QPoint{}, size());
        store_.beginPaint(area);
        QPainter painter(store_.paintDevice());
        painter.fillRect(area, QColor(24, 28, 32));
        painter.setPen(QColor(160, 180, 190));
        painter.drawText(area.adjusted(12, 8, -12, -35),
            Qt::AlignCenter, QStringLiteral("Checking capture and backend switching"));
        painter.fillRect(QRect(12, 60, 20 + (tick_++ % 300), 8), QColor(55, 100, 110));
        painter.end();
        store_.endPaint();
        store_.flush(area);
    }
private:
    QBackingStore store_;
    int tick_ = 0;
};

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    using namespace ambilight::capture;
    TestWindow window;
    window.show();
    QTimer animation;
    animation.setTimerType(Qt::PreciseTimer);
    QObject::connect(&animation, &QTimer::timeout, &app, [&] { window.render(); });
    animation.start(16);
    ScreenCaptureWorker worker;
    CaptureConfiguration configuration;
    configuration.topLedCount = configuration.rightLedCount = 20;
    configuration.bottomLedCount = configuration.leftLedCount = 20;
    configuration.blackBarDetectionMode = BlackBarDetectionMode::Off;
    struct Scenario {
        bool wgc;
        bool gaming;
        int fps;
        BlackBarDetectionMode bars;
        RegionColorMode color;
        bool restart;
        bool window = false;
    };
    using B = BlackBarDetectionMode;
    using C = RegionColorMode;
    const std::array scenarios{
        Scenario{false, false, 60, B::Off, C::Mean, false},
        Scenario{true, false, 60, B::Off, C::Mean, false},
        Scenario{true, true, 30, B::Off, C::Mean, false},
        Scenario{false, true, 30, B::Off, C::Mean, false},
        Scenario{false, false, 60, B::FirstContent, C::RootMeanSquare, false},
        Scenario{true, false, 60, B::FirstContent, C::Dominant, false},
        Scenario{true, false, 15, B::Limited21x9, C::Mean, false},
        Scenario{false, false, 15, B::Limited21x9, C::Dominant, false},
        Scenario{true, true, 30, B::Off, C::Mean, true},
        Scenario{false, false, 60, B::Off, C::Mean, true},
        Scenario{true, false, 60, B::Off, C::Mean, false, true},
        Scenario{true, true, 30, B::Off, C::Mean, false, true},
        Scenario{true, false, 30, B::Off, C::Mean, true, true}};
    int phase = 0;
    int frameCount = 0;
    quint64 lastFrame = 0;
    qint64 firstTimestamp = 0;
    std::vector<double> processingTimes;
    bool transitioning = false;
    bool awaitingStart = false;
    bool awaitingClosed = false;
    bool awaitingStaticFrame = false;
    quint64 finalStopRequest = 0;
    bool stopAcknowledged = false;
    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&worker, &ScreenCaptureWorker::stopped, &app, [&](quint64 request) {
        if (request != finalStopRequest || finalStopRequest == 0) return;
        stopAcknowledged = true;
        std::cout << "Asynchronous stop acknowledged after source release\n";
        QTimer::singleShot(150, &app, [&] { watchdog.stop(); app.quit(); });
    });
    QObject::connect(&watchdog, &QTimer::timeout, &app, [&] {
        std::cerr << "No frame in phase " << phase << '\n';
        // A driver teardown failure must fail the test, not leave it hanging
        // in destructors after the UI message loop has already stopped.
        ExitProcess(1);
    });
    const auto configure = [&] {
        std::cerr << "Starting phase " << phase << std::endl;
        const auto& scenario = scenarios[phase];
        if (scenario.restart) worker.stop();
        configuration.useWindowsGraphicsCapture = scenario.wgc;
        configuration.sourceWindow = scenario.window ? static_cast<quintptr>(window.winId()) : 0;
        configuration.sourceWindowProcessId = scenario.window ? GetCurrentProcessId() : 0;
        if (phase == 11) window.resize(620, 240);
        configuration.gamingMode = scenario.gaming;
        configuration.targetFps = scenario.fps;
        configuration.blackBarDetectionMode = scenario.bars;
        configuration.regionColorMode = scenario.color;
        configuration.samplingDepthPixels = (phase % 2) == 0 ? 0 : 120;
        frameCount = 0;
        lastFrame = 0;
        processingTimes.clear();
        awaitingStart = true;
        worker.updateConfiguration(configuration);
        worker.start();
        watchdog.start(15000);
    };
    QObject::connect(&worker, &ScreenCaptureWorker::statusChanged,
        &app, [&](const CaptureStatus& status) {
            std::cerr << "Status: " << status.message.toStdString() << std::endl;
            if (status.state == CaptureState::Running) awaitingStart = false;
            if (status.state == CaptureState::Error) {
                if (awaitingClosed && status.nativeError == static_cast<qint64>(
                    static_cast<quint32>(HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE)))) {
                    std::cout << "Closed WGC source stops without desktop fallback\n";
                    finalStopRequest = worker.stop();
                    return;
                }
                std::cerr << status.message.toStdString() << '\n';
                app.exit(1);
            }
        });
    QObject::connect(&worker, &ScreenCaptureWorker::sampleReady,
        &app, [&](const CaptureSample& sample) {
            if (stopAcknowledged) {
                std::cerr << "Frame delivered after stop acknowledgement\n";
                app.exit(1);
                return;
            }
            if (awaitingStaticFrame) {
                if (sample.frameSize.width() < 640) return;
                awaitingStaticFrame = false;
                awaitingClosed = true;
                std::cout << "Final static window frame read back after animation stopped\n";
                window.close();
                window.destroy();
                watchdog.start(15000);
                return;
            }
            if (transitioning || awaitingStart) return;
            // WGC can still have a pre-resize frame queued. The watchdog
            // requires the new size to arrive; do not count old frames.
            if (phase >= 11 && sample.frameSize.width() < 600) return;
            const QRect frameBounds(QPoint{}, sample.frameSize);
            const QRect content = sample.contentInsets.contentRect(sample.frameSize);
            if (configuration.sourceWindow != 0
                && (sample.frameSize.width() > 700 || sample.frameSize.height() > 340)) {
                std::cerr << "Window capture returned monitor size or failed to resize\n";
                app.exit(1);
                return;
            }
            if (!sample.frameSize.isValid() || sample.colors.size() != 80
                || content.isEmpty() || !frameBounds.contains(content)
                || sample.frameNumber <= lastFrame
                || (configuration.blackBarDetectionMode == B::Off
                    && sample.contentInsets != CaptureInsets{})) {
                std::cerr << "Invalid frame or layout\n";
                app.exit(1);
                return;
            }
            lastFrame = sample.frameNumber;
            if (frameCount == 0) firstTimestamp = sample.capturedAtMilliseconds;
            processingTimes.push_back(sample.processingMilliseconds);
            if (++frameCount < 24) return;
            // Allow scheduling/timestamp jitter, but reject an uncapped loop.
            if (sample.capturedAtMilliseconds - firstTimestamp
                < (frameCount - 1) * 800 / configuration.targetFps) {
                std::cerr << "Capture exceeded the requested processing rate\n";
                app.exit(1);
                return;
            }
            std::sort(processingTimes.begin(), processingTimes.end());
            std::cout << (configuration.useWindowsGraphicsCapture ? "WGC" : "DXGI")
                << " game=" << configuration.gamingMode << " "
                << sample.frameSize.width() << 'x' << sample.frameSize.height()
                << " zones=" << sample.colors.size()
                << " bars=" << static_cast<int>(configuration.blackBarDetectionMode)
                << " target=" << configuration.targetFps
                << " frames=" << frameCount
                << " median-processing-ms=" << processingTimes[processingTimes.size() / 2]
                << std::endl;
            transitioning = true;
            watchdog.stop();
            if (++phase == static_cast<int>(scenarios.size())) {
                awaitingStaticFrame = true;
                animation.stop();
                window.resize(640, 260);
                window.render();
                watchdog.start(15000);
                return;
            }
            // Schedule outside sample delivery; the next generation discards
            // any old queued sample before changing the active backend.
            QTimer::singleShot(0, &app, [&] {
                configure();
                transitioning = false;
            });
        });
    configure();
    return app.exec();
}
