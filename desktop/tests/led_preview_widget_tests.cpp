#include "led_preview_widget.hpp"

#include <QApplication>
#include <QImage>

#include <iostream>

namespace {

bool expect(const bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
    }
    return condition;
}

QImage render(ambilight::LedPreviewWidget& widget)
{
    QImage image(widget.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    widget.render(&image);
    return image;
}

qsizetype differentPixels(const QImage& first, const QImage& second)
{
    if (first.size() != second.size() || first.format() != second.format()) {
        return -1;
    }
    qsizetype result = 0;
    for (int y = 0; y < first.height(); ++y) {
        const auto* firstLine = reinterpret_cast<const QRgb*>(
            first.constScanLine(y));
        const auto* secondLine = reinterpret_cast<const QRgb*>(
            second.constScanLine(y));
        for (int x = 0; x < first.width(); ++x) {
            if (firstLine[x] != secondLine[x]) {
                ++result;
            }
        }
    }
    return result;
}

} // namespace

int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    ambilight::LedPreviewWidget widget;
    widget.resize(720, 440);
    widget.setLayout(96, 54, 96, 54);

    widget.setSamplingOverlay(0, 0, 0, 0, 0, {3840, 2160}, {});
    const QImage withoutOverlay = render(widget);

    widget.setSamplingOverlay(96, 54, 96, 54, 120, {3840, 2160}, {});
    const QImage withOverlay = render(widget);
    if (!expect(
            differentPixels(withoutOverlay, withOverlay) > 2'000,
            "sampling overlay changes a visible area")) {
        return 1;
    }

    widget.setSamplingOverlay(
        96, 54, 96, 54, 120, {3840, 2160}, {0, 240, 0, 240});
    const QImage withBlackBars = render(widget);
    if (!expect(
            differentPixels(withOverlay, withBlackBars) > 1'000,
            "sampling overlay follows visible-content insets")) {
        return 1;
    }

    std::cout << "All LED preview widget tests passed.\n";
    return 0;
}
