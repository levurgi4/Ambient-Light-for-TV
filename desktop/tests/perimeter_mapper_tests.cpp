#include "perimeter_mapper.hpp"

#include <QColor>
#include <QVector>

#include <iostream>

namespace {

bool expect(const bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
    }
    return condition;
}

bool testZoneDistribution()
{
    const ambilight::PerimeterLayout leds{96, 54, 96, 54};
    const auto full = ambilight::distributeSamplingZones(300, leds);
    const auto reduced = ambilight::distributeSamplingZones(60, leds);

    return expect(full.top == 96 && full.right == 54
            && full.bottom == 96 && full.left == 54,
        "full zone distribution")
        && expect(reduced.total() == 60, "reduced zone total")
        && expect(reduced.top == reduced.bottom, "horizontal zone symmetry")
        && expect(reduced.right == reduced.left, "vertical zone symmetry")
        && expect(reduced.top > reduced.right, "aspect-aware zone weighting");
}

bool testSideResampling()
{
    const QVector<QColor> zones{
        QColor(0, 0, 0),
        QColor(255, 0, 0),
    };
    const auto colors = ambilight::resamplePerimeter(
        zones,
        {.top = 2},
        {.top = 4});

    return expect(colors.size() == 4, "resampled size")
        && expect(colors[0].red() == 0, "resampled first endpoint")
        && expect(colors[1].red() == 64, "resampled first interior")
        && expect(colors[2].red() == 191, "resampled second interior")
        && expect(colors[3].red() == 255, "resampled last endpoint");
}

bool testSideBoundaries()
{
    const QVector<QColor> zones{
        Qt::red,
        Qt::green,
        Qt::blue,
        Qt::white,
    };
    const ambilight::PerimeterLayout layout{1, 1, 1, 1};
    const auto colors = ambilight::resamplePerimeter(zones, layout, layout);

    return expect(colors == zones, "side order is preserved");
}

bool testPhysicalMapping()
{
    const QVector<QColor> logical{
        QColor(1, 0, 0),
        QColor(2, 0, 0),
        QColor(3, 0, 0),
        QColor(4, 0, 0),
    };
    const auto clockwise =
        ambilight::mapToPhysicalStrip(logical, 1, true);
    const auto counterclockwise =
        ambilight::mapToPhysicalStrip(logical, 1, false);

    return expect(
        clockwise[0].red() == 2
            && clockwise[1].red() == 3
            && clockwise[2].red() == 4
            && clockwise[3].red() == 1,
        "clockwise physical mapping")
        && expect(
            counterclockwise[0].red() == 2
                && counterclockwise[1].red() == 1
                && counterclockwise[2].red() == 4
                && counterclockwise[3].red() == 3,
            "counterclockwise physical mapping");
}

bool testLayoutPattern()
{
    const ambilight::PerimeterLayout layout{2, 1, 2, 1};
    const auto colors = ambilight::makeLayoutTestPattern(layout);

    return expect(colors.size() == layout.total(), "layout pattern size")
        && expect(colors[0] == QColor(255, 0, 0)
                && colors[1] == QColor(255, 0, 0),
            "top side is red")
        && expect(colors[2] == QColor(0, 255, 0),
            "right side is green")
        && expect(colors[3] == QColor(0, 0, 255)
                && colors[4] == QColor(0, 0, 255),
            "bottom side is blue")
        && expect(colors[5] == QColor(255, 96, 0),
            "left side is amber")
        && expect(
            ambilight::makeLayoutTestPattern({}).isEmpty(),
            "invalid layout has no test pattern");
}

} // namespace

int main()
{
    const bool success = testZoneDistribution()
        && testSideResampling()
        && testSideBoundaries()
        && testPhysicalMapping()
        && testLayoutPattern();
    if (success) {
        std::cout << "All perimeter mapper tests passed.\n";
    }
    return success ? 0 : 1;
}
