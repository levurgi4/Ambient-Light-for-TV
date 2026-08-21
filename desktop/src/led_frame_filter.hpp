#pragma once

#include <QColor>
#include <QVector>
#include <QtGlobal>

namespace ambilight {

// A small CPU-side temporal filter applied after GPU zone sampling and before
// physical strip mapping. It processes at most 300 colors, so the cost is
// negligible compared with screen capture and GPU readback.
class LedFrameFilter final {
public:
    [[nodiscard]] QVector<QColor> apply(
        const QVector<QColor>& target,
        qint64 timestampMilliseconds,
        int smoothingMilliseconds,
        bool flashProtectionEnabled);

    void reset() noexcept;

private:
    QVector<QColor> previous_;
    qint64 previousTimestampMilliseconds_ = 0;
    bool hasPreviousTimestamp_ = false;
};

} // namespace ambilight
