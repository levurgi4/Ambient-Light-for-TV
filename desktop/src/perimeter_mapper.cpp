#include "perimeter_mapper.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace ambilight {

namespace {

[[nodiscard]] std::array<int, 4> asArray(const PerimeterLayout& layout)
{
    return {layout.top, layout.right, layout.bottom, layout.left};
}

[[nodiscard]] PerimeterLayout fromArray(const std::array<int, 4>& values)
{
    return {
        .top = values[0],
        .right = values[1],
        .bottom = values[2],
        .left = values[3],
    };
}

[[nodiscard]] QColor interpolate(
    const QColor& first,
    const QColor& second,
    const double amount)
{
    const auto channel = [amount](const int from, const int to) {
        return std::clamp(
            static_cast<int>(std::lround(
                static_cast<double>(from)
                + (static_cast<double>(to - from) * amount))),
            0,
            255);
    };

    return {
        channel(first.red(), second.red()),
        channel(first.green(), second.green()),
        channel(first.blue(), second.blue()),
        channel(first.alpha(), second.alpha()),
    };
}

void resampleSide(
    const QVector<QColor>& source,
    const int sourceOffset,
    const int sourceCount,
    const int destinationCount,
    QVector<QColor>& destination)
{
    if (destinationCount <= 0) {
        return;
    }
    if (sourceCount <= 0) {
        for (int index = 0; index < destinationCount; ++index) {
            destination.push_back(Qt::black);
        }
        return;
    }

    for (int index = 0; index < destinationCount; ++index) {
        const double sourcePosition =
            ((static_cast<double>(index) + 0.5)
                * static_cast<double>(sourceCount)
                / static_cast<double>(destinationCount))
            - 0.5;
        const double clampedPosition = std::clamp(
            sourcePosition,
            0.0,
            static_cast<double>(sourceCount - 1));
        const int lower = static_cast<int>(std::floor(clampedPosition));
        const int upper = std::min(lower + 1, sourceCount - 1);
        const double fraction = clampedPosition - static_cast<double>(lower);
        destination.push_back(interpolate(
            source[sourceOffset + lower],
            source[sourceOffset + upper],
            fraction));
    }
}

} // namespace

int PerimeterLayout::total() const noexcept
{
    return top + right + bottom + left;
}

bool PerimeterLayout::isValid() const noexcept
{
    return top >= 0 && right >= 0 && bottom >= 0 && left >= 0
        && total() > 0;
}

PerimeterLayout distributeSamplingZones(
    const int zoneCount,
    const PerimeterLayout& ledLayout)
{
    if (!ledLayout.isValid()) {
        return {};
    }

    const auto weights = asArray(ledLayout);
    const int activeSides = static_cast<int>(std::count_if(
        weights.begin(),
        weights.end(),
        [](const int value) { return value > 0; }));
    const int normalizedZoneCount = std::clamp(zoneCount, activeSides, 300);

    std::array<int, 4> result{};
    std::array<int, 4> remainders{};
    int assigned = 0;
    for (std::size_t side = 0; side < weights.size(); ++side) {
        if (weights[side] > 0) {
            result[side] = 1;
            ++assigned;
        }
    }

    const int remaining = normalizedZoneCount - assigned;
    const int weightTotal = ledLayout.total();
    for (std::size_t side = 0; side < weights.size(); ++side) {
        if (weights[side] <= 0) {
            remainders[side] = std::numeric_limits<int>::min();
            continue;
        }
        const int product = remaining * weights[side];
        const int extra = product / weightTotal;
        result[side] += extra;
        assigned += extra;
        remainders[side] = product % weightTotal;
    }

    while (assigned < normalizedZoneCount) {
        const auto best = static_cast<std::size_t>(
            std::distance(
                remainders.begin(),
                std::max_element(remainders.begin(), remainders.end())));
        ++result[best];
        remainders[best] = std::numeric_limits<int>::min();
        ++assigned;
    }

    return fromArray(result);
}

QVector<QColor> resamplePerimeter(
    const QVector<QColor>& zoneColors,
    const PerimeterLayout& zoneLayout,
    const PerimeterLayout& ledLayout)
{
    if (!zoneLayout.isValid()
        || !ledLayout.isValid()
        || zoneColors.size() != zoneLayout.total()) {
        return {};
    }

    const auto sourceCounts = asArray(zoneLayout);
    const auto destinationCounts = asArray(ledLayout);
    QVector<QColor> result;
    result.reserve(ledLayout.total());

    int sourceOffset = 0;
    for (std::size_t side = 0; side < sourceCounts.size(); ++side) {
        resampleSide(
            zoneColors,
            sourceOffset,
            sourceCounts[side],
            destinationCounts[side],
            result);
        sourceOffset += sourceCounts[side];
    }
    return result;
}

QVector<QColor> makeLayoutTestPattern(const PerimeterLayout& ledLayout)
{
    if (!ledLayout.isValid()) {
        return {};
    }

    QVector<QColor> result;
    result.reserve(ledLayout.total());
    const auto appendSide = [&result](const int count, const QColor color) {
        for (int index = 0; index < count; ++index) {
            result.push_back(color);
        }
    };
    appendSide(ledLayout.top, QColor(255, 0, 0));
    appendSide(ledLayout.right, QColor(0, 255, 0));
    appendSide(ledLayout.bottom, QColor(0, 0, 255));
    appendSide(ledLayout.left, QColor(255, 96, 0));
    return result;
}

QVector<QColor> mapToPhysicalStrip(
    const QVector<QColor>& logicalColors,
    const int startOffset,
    const bool clockwise)
{
    if (logicalColors.isEmpty()) {
        return {};
    }

    const int count = logicalColors.size();
    const int normalizedOffset = ((startOffset % count) + count) % count;
    QVector<QColor> result;
    result.reserve(count);

    for (int physicalIndex = 0; physicalIndex < count; ++physicalIndex) {
        const int logicalIndex = clockwise
            ? (normalizedOffset + physicalIndex) % count
            : (normalizedOffset - physicalIndex + count) % count;
        result.push_back(logicalColors[logicalIndex]);
    }
    return result;
}

} // namespace ambilight
