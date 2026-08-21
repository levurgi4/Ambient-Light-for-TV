#include "led_preview_widget.hpp"

#include <QFontMetrics>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QPalette>
#include <QRadialGradient>
#include <QSizePolicy>

#include <algorithm>
#include <cmath>
#include <utility>

namespace ambilight {

namespace {

constexpr qreal kHeaderHeight = 44.0;
constexpr qreal kLedDistance = 11.0;

QRectF fittedRect(const QRectF& bounds, const qreal aspectRatio)
{
    if (bounds.isEmpty() || aspectRatio <= 0.0) {
        return {};
    }

    QSizeF size = bounds.size();
    if (size.width() / size.height() > aspectRatio) {
        size.setWidth(size.height() * aspectRatio);
    } else {
        size.setHeight(size.width() / aspectRatio);
    }

    return QRectF(
        bounds.center().x() - (size.width() / 2.0),
        bounds.center().y() - (size.height() / 2.0),
        size.width(),
        size.height());
}

void drawLed(
    QPainter& painter,
    const QRectF& ledRect,
    const QColor& color,
    const bool illuminated)
{
    if (!illuminated) {
        painter.setPen(QPen(QColor(255, 255, 255, 20), 0.75));
        painter.setBrush(QColor(53, 62, 75));
        painter.drawRoundedRect(ledRect, 1.5, 1.5);
        return;
    }

    QColor glow = color.toRgb();
    glow.setAlpha(std::clamp(color.alpha() / 3, 20, 72));
    painter.setPen(Qt::NoPen);
    painter.setBrush(glow);
    painter.drawRoundedRect(ledRect.adjusted(-2.5, -2.5, 2.5, 2.5), 3.5, 3.5);

    QColor core = color.toRgb();
    core.setAlpha(std::max(color.alpha(), 90));
    painter.setPen(QPen(QColor(255, 255, 255, 46), 0.65));
    painter.setBrush(core);
    painter.drawRoundedRect(ledRect, 1.5, 1.5);
}

} // namespace

LedPreviewWidget::LedPreviewWidget(QWidget* parent)
    : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAutoFillBackground(false);
}

void LedPreviewWidget::setLayout(
    const int top,
    const int right,
    const int bottom,
    const int left)
{
    const int newTop = std::max(0, top);
    const int newRight = std::max(0, right);
    const int newBottom = std::max(0, bottom);
    const int newLeft = std::max(0, left);
    if (topCount_ == newTop && rightCount_ == newRight
        && bottomCount_ == newBottom && leftCount_ == newLeft) {
        return;
    }

    topCount_ = newTop;
    rightCount_ = newRight;
    bottomCount_ = newBottom;
    leftCount_ = newLeft;
    update();
}

void LedPreviewWidget::setPhysicalMapping(
    const int startOffset,
    const bool clockwise)
{
    if (startOffset_ == startOffset && clockwise_ == clockwise) {
        return;
    }

    startOffset_ = startOffset;
    clockwise_ = clockwise;
    update();
}

void LedPreviewWidget::setSamplingOverlay(
    const int topZones,
    const int rightZones,
    const int bottomZones,
    const int leftZones,
    const int depthPixels,
    const QSize sourceSize,
    const QMargins contentInsets)
{
    const int newTop = std::max(0, topZones);
    const int newRight = std::max(0, rightZones);
    const int newBottom = std::max(0, bottomZones);
    const int newLeft = std::max(0, leftZones);
    const int newDepth = std::max(0, depthPixels);
    const QSize newSourceSize = sourceSize.isValid() && !sourceSize.isEmpty()
        ? sourceSize
        : QSize {};
    const QMargins newInsets(
        std::max(0, contentInsets.left()),
        std::max(0, contentInsets.top()),
        std::max(0, contentInsets.right()),
        std::max(0, contentInsets.bottom()));
    if (topZoneCount_ == newTop && rightZoneCount_ == newRight
        && bottomZoneCount_ == newBottom && leftZoneCount_ == newLeft
        && samplingDepthPixels_ == newDepth
        && samplingSourceSize_ == newSourceSize
        && samplingContentInsets_ == newInsets) {
        return;
    }

    topZoneCount_ = newTop;
    rightZoneCount_ = newRight;
    bottomZoneCount_ = newBottom;
    leftZoneCount_ = newLeft;
    samplingDepthPixels_ = newDepth;
    samplingSourceSize_ = newSourceSize;
    samplingContentInsets_ = newInsets;
    update();
}

void LedPreviewWidget::setFrame(
    QVector<QColor> colors,
    const QSize sourceSize,
    const QMargins contentInsets,
    const double fps,
    const double processingMs)
{
    colors_ = std::move(colors);
    sourceSize_ =
        sourceSize.isValid() && !sourceSize.isEmpty() ? sourceSize : QSize {};
    contentInsets_ = QMargins(
        std::max(0, contentInsets.left()),
        std::max(0, contentInsets.top()),
        std::max(0, contentInsets.right()),
        std::max(0, contentInsets.bottom()));
    fps_ = std::isfinite(fps) ? std::max(0.0, fps) : 0.0;
    processingMs_ =
        std::isfinite(processingMs) ? std::max(0.0, processingMs) : 0.0;
    hasFrame_ = true;
    update();
}

void LedPreviewWidget::setStatus(QString status, const bool active)
{
    if (status_ == status && active_ == active) {
        return;
    }

    status_ = std::move(status);
    active_ = active;
    update();
}

void LedPreviewWidget::clearFrame()
{
    if (!hasFrame_ && colors_.isEmpty() && !sourceSize_.isValid()) {
        return;
    }

    colors_.clear();
    sourceSize_ = {};
    contentInsets_ = {};
    fps_ = 0.0;
    processingMs_ = 0.0;
    hasFrame_ = false;
    update();
}

QSize LedPreviewWidget::sizeHint() const
{
    return {720, 440};
}

QSize LedPreviewWidget::minimumSizeHint() const
{
    return {420, 280};
}

void LedPreviewWidget::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);

    const QRectF outer = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    painter.fillRect(rect(), QColor(8, 11, 16));

    QLinearGradient panelGradient(outer.topLeft(), outer.bottomRight());
    panelGradient.setColorAt(0.0, QColor(18, 25, 35));
    panelGradient.setColorAt(1.0, QColor(10, 14, 21));
    painter.setPen(QPen(QColor(255, 255, 255, 24), 1.0));
    painter.setBrush(panelGradient);
    painter.drawRoundedRect(outer, 10.0, 10.0);

    const QRectF header(
        outer.left(),
        outer.top(),
        outer.width(),
        std::min(kHeaderHeight, outer.height()));
    painter.setPen(QPen(QColor(255, 255, 255, 18), 1.0));
    painter.drawLine(header.bottomLeft(), header.bottomRight());

    QFont statusFont = font();
    statusFont.setPixelSize(12);
    statusFont.setWeight(QFont::DemiBold);
    painter.setFont(statusFont);

    const QPointF statusCenter(header.left() + 18.0, header.center().y());
    QColor statusGlow = active_ ? QColor(72, 226, 139, 55) : QColor(245, 176, 75, 45);
    const QColor statusColor = active_ ? QColor(77, 231, 144) : QColor(245, 180, 80);
    painter.setPen(Qt::NoPen);
    painter.setBrush(statusGlow);
    painter.drawEllipse(statusCenter, 7.0, 7.0);
    painter.setBrush(statusColor);
    painter.drawEllipse(statusCenter, 3.5, 3.5);

    const QString statusText =
        status_.isEmpty() ? QStringLiteral("LED PREVIEW") : status_;
    const QRectF statusBounds(
        statusCenter.x() + 12.0,
        header.top(),
        std::max(0.0, header.width() * 0.38),
        header.height());
    painter.setPen(QColor(222, 229, 239));
    painter.drawText(
        statusBounds,
        Qt::AlignLeft | Qt::AlignVCenter,
        QFontMetrics(statusFont).elidedText(
            statusText,
            Qt::ElideRight,
            static_cast<int>(statusBounds.width())));

    QFont telemetryFont = font();
    telemetryFont.setPixelSize(11);
    painter.setFont(telemetryFont);
    QString telemetry;
    if (sourceSize_.isValid()) {
        telemetry = QStringLiteral("%1 × %2  ·  %3 FPS  ·  %4 ms")
                        .arg(sourceSize_.width())
                        .arg(sourceSize_.height())
                        .arg(fps_, 0, 'f', 1)
                        .arg(processingMs_, 0, 'f', 2);
    } else {
        telemetry = QStringLiteral("%1 LED")
                        .arg(topCount_ + rightCount_ + bottomCount_ + leftCount_);
    }
    telemetry += QStringLiteral("  ·  LED 0:%1 %2")
                     .arg(normalizedStartOffset())
                     .arg(clockwise_ ? QStringLiteral("↻") : QStringLiteral("↺"));
    const int zoneCount = topZoneCount_ + rightZoneCount_
        + bottomZoneCount_ + leftZoneCount_;
    if (zoneCount > 0 && samplingDepthPixels_ > 0) {
        telemetry += QStringLiteral("  ·  Z:%1  ·  D:%2 px")
                         .arg(zoneCount)
                         .arg(samplingDepthPixels_);
    }
    painter.setPen(QColor(143, 155, 173));
    painter.drawText(
        header.adjusted(statusBounds.right() - header.left() + 12.0, 0.0, -16.0, 0.0),
        Qt::AlignRight | Qt::AlignVCenter,
        QFontMetrics(telemetryFont).elidedText(
            telemetry,
            Qt::ElideLeft,
            static_cast<int>(
                header.right() - statusBounds.right() - 28.0)));

    QRectF canvas = outer.adjusted(24.0, kHeaderHeight + 14.0, -24.0, -20.0);
    if (canvas.width() < 80.0 || canvas.height() < 60.0) {
        return;
    }

    const QSize previewSourceSize = sourceSize_.isValid()
        ? sourceSize_
        : samplingSourceSize_;
    const QMargins previewInsets = sourceSize_.isValid()
        ? contentInsets_
        : samplingContentInsets_;
    const qreal aspectRatio = previewSourceSize.isValid()
        ? static_cast<qreal>(previewSourceSize.width())
            / static_cast<qreal>(previewSourceSize.height())
        : (16.0 / 9.0);
    const QRectF screen =
        fittedRect(canvas.adjusted(22.0, 22.0, -22.0, -22.0), aspectRatio);
    if (screen.isEmpty()) {
        return;
    }

    QRadialGradient aura(screen.center(), std::max(screen.width(), screen.height()) * 0.68);
    aura.setColorAt(0.0, QColor(60, 118, 184, 28));
    aura.setColorAt(0.62, QColor(35, 68, 108, 12));
    aura.setColorAt(1.0, QColor(8, 11, 16, 0));
    painter.setPen(Qt::NoPen);
    painter.setBrush(aura);
    painter.drawEllipse(screen.adjusted(-30.0, -30.0, 30.0, 30.0));

    const QRectF bezel = screen.adjusted(-5.0, -5.0, 5.0, 5.0);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 105));
    painter.drawRoundedRect(bezel.translated(0.0, 4.0), 6.0, 6.0);
    painter.setPen(QPen(QColor(112, 129, 151, 80), 1.0));
    painter.setBrush(QColor(25, 31, 42));
    painter.drawRoundedRect(bezel, 5.0, 5.0);

    QLinearGradient screenGradient(screen.topLeft(), screen.bottomRight());
    screenGradient.setColorAt(0.0, QColor(22, 31, 44));
    screenGradient.setColorAt(0.55, QColor(14, 20, 30));
    screenGradient.setColorAt(1.0, QColor(9, 13, 20));
    painter.setPen(Qt::NoPen);
    painter.setBrush(screenGradient);
    painter.drawRect(screen);

    QRectF content = screen;
    bool hasInsets = false;
    if (previewSourceSize.isValid()) {
        const int sourceWidth = previewSourceSize.width();
        const int sourceHeight = previewSourceSize.height();
        const int left = std::clamp(previewInsets.left(), 0, sourceWidth);
        const int right =
            std::clamp(previewInsets.right(), 0, sourceWidth - left);
        const int top = std::clamp(previewInsets.top(), 0, sourceHeight);
        const int bottom =
            std::clamp(previewInsets.bottom(), 0, sourceHeight - top);

        content = QRectF(
            screen.left()
                + screen.width() * static_cast<qreal>(left)
                    / static_cast<qreal>(sourceWidth),
            screen.top()
                + screen.height() * static_cast<qreal>(top)
                    / static_cast<qreal>(sourceHeight),
            screen.width() * static_cast<qreal>(sourceWidth - left - right)
                / static_cast<qreal>(sourceWidth),
            screen.height() * static_cast<qreal>(sourceHeight - top - bottom)
                / static_cast<qreal>(sourceHeight));
        hasInsets = left > 0 || top > 0 || right > 0 || bottom > 0;
    }

    drawSamplingOverlay(painter, content);

    if (hasInsets) {
        QPainterPath bars;
        bars.setFillRule(Qt::OddEvenFill);
        bars.addRect(screen);
        if (!content.isEmpty()) {
            bars.addRect(content);
        }
        painter.fillPath(bars, QColor(1, 3, 6, 220));

        if (!content.isEmpty()) {
            painter.setPen(QPen(QColor(105, 188, 255, 95), 1.0, Qt::DashLine));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(content.adjusted(0.5, 0.5, -0.5, -0.5));
        }

        const QString insetText =
            QStringLiteral("L %1  T %2  R %3  B %4 px")
                .arg(previewInsets.left())
                .arg(previewInsets.top())
                .arg(previewInsets.right())
                .arg(previewInsets.bottom());
        const QFontMetrics metrics(telemetryFont);
        const qreal labelWidth =
            static_cast<qreal>(metrics.horizontalAdvance(insetText)) + 16.0;
        const QRectF label(
            screen.left() + 8.0,
            screen.bottom() - 25.0,
            std::min(labelWidth, screen.width() - 16.0),
            18.0);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(4, 8, 14, 205));
        painter.drawRoundedRect(label, 4.0, 4.0);
        painter.setPen(QColor(151, 188, 219));
        painter.setFont(telemetryFont);
        painter.drawText(
            label.adjusted(7.0, 0.0, -7.0, 0.0),
            Qt::AlignLeft | Qt::AlignVCenter,
            metrics.elidedText(
                insetText,
                Qt::ElideRight,
                static_cast<int>(label.width() - 14.0)));
    }

    drawHorizontalStrip(painter, screen, 0, topCount_, true, true);
    drawVerticalStrip(
        painter,
        screen,
        topCount_,
        rightCount_,
        true,
        true);
    drawHorizontalStrip(
        painter,
        screen,
        topCount_ + rightCount_,
        bottomCount_,
        false,
        false);
    drawVerticalStrip(
        painter,
        screen,
        topCount_ + rightCount_ + bottomCount_,
        leftCount_,
        false,
        false);
}

void LedPreviewWidget::drawSamplingOverlay(
    QPainter& painter,
    const QRectF& content) const
{
    const int totalZones = topZoneCount_ + rightZoneCount_
        + bottomZoneCount_ + leftZoneCount_;
    if (content.isEmpty() || totalZones <= 0 || samplingDepthPixels_ <= 0) {
        return;
    }

    const QSize sourceSize = samplingSourceSize_.isValid()
        ? samplingSourceSize_
        : sourceSize_;
    if (!sourceSize.isValid() || sourceSize.isEmpty()) {
        return;
    }

    const int sourceWidth = sourceSize.width();
    const int sourceHeight = sourceSize.height();
    const int leftInset = std::clamp(
        samplingContentInsets_.left(), 0, sourceWidth - 1);
    const int rightInset = std::clamp(
        samplingContentInsets_.right(), 0, sourceWidth - leftInset - 1);
    const int topInset = std::clamp(
        samplingContentInsets_.top(), 0, sourceHeight - 1);
    const int bottomInset = std::clamp(
        samplingContentInsets_.bottom(), 0, sourceHeight - topInset - 1);
    const int contentPixelWidth = std::max(
        1, sourceWidth - leftInset - rightInset);
    const int contentPixelHeight = std::max(
        1, sourceHeight - topInset - bottomInset);
    const int usableDepth = std::clamp(
        samplingDepthPixels_,
        1,
        std::max(1, std::min(contentPixelWidth, contentPixelHeight) / 2));
    const qreal horizontalDepth = content.width()
        * static_cast<qreal>(usableDepth)
        / static_cast<qreal>(contentPixelWidth);
    const qreal verticalDepth = content.height()
        * static_cast<qreal>(usableDepth)
        / static_cast<qreal>(contentPixelHeight);

    const QRectF topRegion(
        content.left(), content.top(), content.width(), verticalDepth);
    const QRectF rightRegion(
        content.right() - horizontalDepth,
        content.top(),
        horizontalDepth,
        content.height());
    const QRectF bottomRegion(
        content.left(),
        content.bottom() - verticalDepth,
        content.width(),
        verticalDepth);
    const QRectF leftRegion(
        content.left(), content.top(), horizontalDepth, content.height());

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, false);
    const auto drawRegion = [&painter](
                                const QRectF& region,
                                const int zones,
                                const QColor& color,
                                const bool horizontal) {
        if (zones <= 0 || region.isEmpty()) {
            return;
        }
        QColor fill = color;
        fill.setAlpha(42);
        QColor outline = color;
        outline.setAlpha(150);
        QColor separator = color;
        separator.setAlpha(95);
        painter.fillRect(region, fill);
        painter.setPen(QPen(outline, 1.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(region.adjusted(0.5, 0.5, -0.5, -0.5));
        painter.setPen(QPen(separator, 1.0));
        for (int index = 1; index < zones; ++index) {
            const qreal fraction = static_cast<qreal>(index)
                / static_cast<qreal>(zones);
            if (horizontal) {
                const qreal x = region.left() + region.width() * fraction;
                painter.drawLine(
                    QPointF(x, region.top()), QPointF(x, region.bottom()));
            } else {
                const qreal y = region.top() + region.height() * fraction;
                painter.drawLine(
                    QPointF(region.left(), y), QPointF(region.right(), y));
            }
        }
    };

    drawRegion(topRegion, topZoneCount_, QColor(58, 190, 255), true);
    drawRegion(rightRegion, rightZoneCount_, QColor(84, 230, 150), false);
    drawRegion(bottomRegion, bottomZoneCount_, QColor(170, 116, 255), true);
    drawRegion(leftRegion, leftZoneCount_, QColor(255, 178, 76), false);
    painter.restore();
}

QColor LedPreviewWidget::colorAt(const int index) const
{
    if (!hasFrame_ || index < 0
        || static_cast<qsizetype>(index) >= colors_.size()
        || !colors_.at(index).isValid()) {
        return {};
    }
    return colors_.at(index);
}

void LedPreviewWidget::drawHorizontalStrip(
    QPainter& painter,
    const QRectF& screen,
    const int firstIndex,
    const int count,
    const bool leftToRight,
    const bool topSide) const
{
    if (count <= 0) {
        return;
    }

    const qreal cellWidth = screen.width() / static_cast<qreal>(count);
    const qreal gap = std::clamp(cellWidth * 0.18, 0.35, 1.4);
    const qreal thickness = std::clamp(
        std::min(cellWidth * 0.72, screen.height() * 0.024),
        2.0,
        6.5);
    const qreal y = topSide
        ? screen.top() - kLedDistance - (thickness / 2.0)
        : screen.bottom() + kLedDistance - (thickness / 2.0);

    for (int index = 0; index < count; ++index) {
        const int position = leftToRight ? index : (count - 1 - index);
        const QRectF led(
            screen.left() + static_cast<qreal>(position) * cellWidth + gap,
            y,
            std::max(0.65, cellWidth - (2.0 * gap)),
            thickness);
        const QColor color = colorAt(firstIndex + index);
        drawLed(painter, led, color, color.isValid());
        drawPhysicalStartMarker(painter, led, firstIndex + index);
    }
}

void LedPreviewWidget::drawVerticalStrip(
    QPainter& painter,
    const QRectF& screen,
    const int firstIndex,
    const int count,
    const bool topToBottom,
    const bool rightSide) const
{
    if (count <= 0) {
        return;
    }

    const qreal cellHeight = screen.height() / static_cast<qreal>(count);
    const qreal gap = std::clamp(cellHeight * 0.18, 0.35, 1.4);
    const qreal thickness = std::clamp(
        std::min(cellHeight * 0.72, screen.width() * 0.014),
        2.0,
        6.5);
    const qreal x = rightSide
        ? screen.right() + kLedDistance - (thickness / 2.0)
        : screen.left() - kLedDistance - (thickness / 2.0);

    for (int index = 0; index < count; ++index) {
        const int position = topToBottom ? index : (count - 1 - index);
        const QRectF led(
            x,
            screen.top() + static_cast<qreal>(position) * cellHeight + gap,
            thickness,
            std::max(0.65, cellHeight - (2.0 * gap)));
        const QColor color = colorAt(firstIndex + index);
        drawLed(painter, led, color, color.isValid());
        drawPhysicalStartMarker(painter, led, firstIndex + index);
    }
}

void LedPreviewWidget::drawPhysicalStartMarker(
    QPainter& painter,
    const QRectF& ledRect,
    const int logicalIndex) const
{
    if (logicalIndex != normalizedStartOffset()) {
        return;
    }

    painter.save();
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor(255, 220, 92, 235), 1.6));
    painter.drawRoundedRect(
        ledRect.adjusted(-2.5, -2.5, 2.5, 2.5),
        3.0,
        3.0);
    painter.restore();
}

int LedPreviewWidget::normalizedStartOffset() const noexcept
{
    const int count = topCount_ + rightCount_ + bottomCount_ + leftCount_;
    return count > 0 ? ((startOffset_ % count) + count) % count : 0;
}

} // namespace ambilight
