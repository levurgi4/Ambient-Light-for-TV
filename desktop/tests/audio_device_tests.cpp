#include "audio/audio_capture_worker.hpp"

#include <QCoreApplication>
#include <QEventLoop>
#include <QTextStream>
#include <QTimer>

namespace {

bool enumerate(
    const ambilight::audio::AudioDeviceKind kind,
    const QString& label,
    int* deviceCount)
{
    QString error;
    const auto devices =
        ambilight::audio::AudioCaptureWorker::enumerateDevices(kind, &error);
    *deviceCount = devices.size();
    QTextStream output(stdout);
    output << label << ": " << devices.size() << " active device(s)\n";
    for (const auto& device : devices) {
        output << "  " << (device.isDefault ? "* " : "  ")
               << device.name << '\n';
    }
    if (!error.isEmpty()) {
        QTextStream(stderr) << label << " enumeration failed: " << error
                            << '\n';
        return false;
    }
    return true;
}

bool captureStarts(
    const ambilight::audio::AudioSourceMode sourceMode,
    const QString& label)
{
    QEventLoop loop;
    ambilight::audio::AudioCaptureWorker worker;
    ambilight::audio::AudioConfiguration configuration;
    configuration.sourceMode = sourceMode;
    configuration.perimeter = {.top = 2, .right = 2, .bottom = 2, .left = 2};
    bool running = false;
    QObject::connect(
        &worker,
        &ambilight::audio::AudioCaptureWorker::statusChanged,
        &loop,
        [&](const ambilight::audio::AudioStatus& status) {
            if (status.state == ambilight::audio::AudioState::Running) {
                running = true;
                loop.quit();
            } else if (status.state == ambilight::audio::AudioState::Error) {
                QTextStream(stderr) << label << " capture failed: "
                                    << status.message << '\n';
                loop.quit();
            }
        });
    QTimer::singleShot(3'000, &loop, &QEventLoop::quit);
    worker.updateConfiguration(configuration);
    worker.start();
    loop.exec();
    worker.stop();
    if (!running) {
        QTextStream(stderr) << label
                            << " capture did not reach Running state.\n";
        return false;
    }
    QTextStream(stdout) << label << " capture started.\n";
    return true;
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    int outputCount = 0;
    int inputCount = 0;
    const bool outputOk = enumerate(
        ambilight::audio::AudioDeviceKind::Output,
        QStringLiteral("Output"),
        &outputCount);
    const bool inputOk = enumerate(
        ambilight::audio::AudioDeviceKind::Input,
        QStringLiteral("Input"),
        &inputCount);
    if (!outputOk || !inputOk) {
        return 1;
    }
    if (outputCount == 0) {
        QTextStream(stdout) << "No output endpoint; loopback smoke test skipped.\n";
    } else if (!captureStarts(
                   ambilight::audio::AudioSourceMode::WindowsOutput,
                   QStringLiteral("Default Windows loopback"))) {
        return 1;
    }
    if (inputCount == 0) {
        QTextStream(stdout) << "No input endpoint; microphone smoke test skipped.\n";
    } else if (!captureStarts(
                   ambilight::audio::AudioSourceMode::Microphone,
                   QStringLiteral("Default microphone"))) {
        return 1;
    }
    if (outputCount > 0 && inputCount > 0
        && !captureStarts(
            ambilight::audio::AudioSourceMode::Mixed,
            QStringLiteral("Mixed output + microphone"))) {
        return 1;
    }
    return 0;
}
