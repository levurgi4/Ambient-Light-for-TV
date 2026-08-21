#pragma once

#include <QColor>
#include <QMargins>
#include <QSize>
#include <QString>
#include <QVector>
#include <QWidget>

class QPaintEvent;
class QPainter;
class QRectF;

namespace ambilight {

class LedPreviewWidget final : public QWidget {
public:
    explicit LedPreviewWidget(QWidget* parent = nullptr);

    void setLayout(int top, int right, int bottom, int left);
    void setPhysicalMapping(int startOffset, bool clockwise);
    void setSamplingOverlay(
        int topZones,
        int rightZones,
        int bottomZones,
        int leftZones,
        int depthPixels,
        QSize sourceSize,
        QMargins contentInsets);
    void setFrame(
        QVector<QColor> colors,
        QSize sourceSize,
        QMargins contentInsets,
        double fps,
        double processingMs);
    void setStatus(QString status, bool active);
    void clearFrame();

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    [[nodiscard]] QColor colorAt(int index) const;
    void drawHorizontalStrip(
        QPainter& painter,
        const QRectF& screen,
        int firstIndex,
        int count,
        bool leftToRight,
        bool topSide) const;
    void drawVerticalStrip(
        QPainter& painter,
        const QRectF& screen,
        int firstIndex,
        int count,
        bool topToBottom,
        bool rightSide) const;
    void drawPhysicalStartMarker(
        QPainter& painter,
        const QRectF& ledRect,
        int logicalIndex) const;
    void drawSamplingOverlay(
        QPainter& painter,
        const QRectF& content) const;
    [[nodiscard]] int normalizedStartOffset() const noexcept;

    QVector<QColor> colors_;
    QSize sourceSize_;
    QMargins contentInsets_;
    QSize samplingSourceSize_;
    QMargins samplingContentInsets_;
    QString status_;
    int topCount_ = 96;
    int rightCount_ = 54;
    int bottomCount_ = 96;
    int leftCount_ = 54;
    int startOffset_ = 0;
    int topZoneCount_ = 0;
    int rightZoneCount_ = 0;
    int bottomZoneCount_ = 0;
    int leftZoneCount_ = 0;
    int samplingDepthPixels_ = 0;
    double fps_ = 0.0;
    double processingMs_ = 0.0;
    bool hasFrame_ = false;
    bool active_ = false;
    bool clockwise_ = true;
};

} // namespace ambilight
