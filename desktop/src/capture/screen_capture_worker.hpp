#pragma once

#include "capture_types.hpp"

#include <QMutex>
#include <QObject>
#include <QThread>
#include <QVector>

#include <atomic>
#include <optional>

namespace ambilight::capture {

class ScreenCaptureRuntime;

// Public facade that stays in the caller's thread. All DXGI/D3D11 work is
// serialized on a dedicated QThread owned by this object.
class ScreenCaptureWorker final : public QObject {
    Q_OBJECT

public:
    explicit ScreenCaptureWorker(QObject* parent = nullptr);
    ~ScreenCaptureWorker() override;

    ScreenCaptureWorker(const ScreenCaptureWorker&) = delete;
    ScreenCaptureWorker& operator=(const ScreenCaptureWorker&) = delete;
    ScreenCaptureWorker(ScreenCaptureWorker&&) = delete;
    ScreenCaptureWorker& operator=(ScreenCaptureWorker&&) = delete;

    [[nodiscard]] static QVector<CaptureOutput> enumerateOutputs(
        QString* errorMessage = nullptr);

public slots:
    void start();
    void stop();
    void updateConfiguration(
        const ambilight::capture::CaptureConfiguration& configuration);

signals:
    void sampleReady(const ambilight::capture::CaptureSample& sample);
    void statusChanged(const ambilight::capture::CaptureStatus& status);
    void outputsChanged(
        const QVector<ambilight::capture::CaptureOutput>& outputs);
    void mirrorStatistics(quint64 presented, quint64 skipped, double processingMilliseconds);

private:
    friend class ScreenCaptureRuntime;

    QThread captureThread_;
    ScreenCaptureRuntime* runtime_ = nullptr;
    std::atomic<quint64> commandGeneration_{1};
    QMutex pendingSampleMutex_;
    std::optional<CaptureSample> pendingSample_;
    bool sampleDeliveryScheduled_ = false;
};

} // namespace ambilight::capture
