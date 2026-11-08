#pragma once
#include "capture_types.hpp"
#include <QRectF>

namespace ambilight::capture {
// Stable, explicit center crop. Does not chase dark scenes or subtitles.
inline QRectF mirrorSourceRect(QSize source, int format)
{
    if (source.isEmpty()) return {};
    const double aspect = format == 1 ? 16.0 / 9.0
        : format == 2 ? 21.0 / 9.0 : format == 3 ? 2.39 : 0.0;
    double width = source.width(), height = source.height();
    if (aspect > 0) {
        if (width / height > aspect) width = height * aspect;
        else height = width / aspect;
    }
    return {(source.width() - width) / 2, (source.height() - height) / 2, width, height};
}

inline QRectF mirrorContentRect(QSizeF source, QSize target)
{
    if (source.isEmpty() || target.isEmpty()) return {};
    const double scale = std::min(static_cast<double>(target.width()) / source.width(),
        static_cast<double>(target.height()) / source.height());
    const QSizeF content(source.width() * scale, source.height() * scale);
    return {(target.width() - content.width()) / 2,
        (target.height() - content.height()) / 2, content.width(), content.height()};
}

inline bool separateMirrorOutputs(const CaptureOutput& source, const CaptureOutput& target)
{
    return !source.id.isEmpty() && !target.id.isEmpty()
        && source.id.compare(target.id, Qt::CaseInsensitive) != 0
        && source.legacyId.compare(target.legacyId, Qt::CaseInsensitive) != 0
        && !source.geometry.isEmpty() && !target.geometry.isEmpty()
        && !source.geometry.intersects(target.geometry);
}

// Independent consumer deadlines: a 60 Hz mirror must not double the LED
// processing cost of the 30 Hz gaming preset. Never catch up stale frames.
struct CaptureRateGate {
    qint64 deadline = 0;
    bool due(qint64 now) const { return now >= deadline; }
    void advance(qint64 now, int fps) {
        const qint64 period = (1'000'000'000LL + fps - 1) / fps;
        deadline = (deadline == 0 || now - deadline > period)
            ? now + period : deadline + period;
    }
    // A busy swap chain / empty producer is not a delivered frame. Retry
    // without burning a whole 16/33 ms period. No catch-up burst on success.
    void retry(qint64 now, qint64 delay = 2'000'000) { deadline = now + delay; }
};
}
