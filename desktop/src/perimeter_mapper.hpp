#pragma once

#include <QColor>
#include <QVector>

namespace ambilight {

struct PerimeterLayout final {
    int top{};
    int right{};
    int bottom{};
    int left{};

    [[nodiscard]] int total() const noexcept;
    [[nodiscard]] bool isValid() const noexcept;
};

[[nodiscard]] PerimeterLayout distributeSamplingZones(
    int zoneCount,
    const PerimeterLayout& ledLayout);

[[nodiscard]] QVector<QColor> resamplePerimeter(
    const QVector<QColor>& zoneColors,
    const PerimeterLayout& zoneLayout,
    const PerimeterLayout& ledLayout);

// Produces a logical clockwise perimeter pattern for checking a physical
// installation: top red, right green, bottom blue, and left amber.
[[nodiscard]] QVector<QColor> makeLayoutTestPattern(
    const PerimeterLayout& ledLayout);

[[nodiscard]] QVector<QColor> mapToPhysicalStrip(
    const QVector<QColor>& logicalColors,
    int startOffset,
    bool clockwise);

} // namespace ambilight
