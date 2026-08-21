#pragma once

#include "capture_types.hpp"

#include <QCryptographicHash>
#include <QStringView>
#include <QVector>

#include <algorithm>

namespace ambilight::capture {

inline constexpr auto kStableDisplayIdentityPrefix =
    "displayconfig-v1|";

[[nodiscard]] inline QString stableCaptureOutputId(
    const QStringView monitorDevicePath)
{
    const QString normalizedPath =
        monitorDevicePath.trimmed().toString().toCaseFolded();
    if (normalizedPath.isEmpty()) {
        return {};
    }

    // Persist a compact, privacy-friendly identity rather than the raw
    // monitor interface path, which may contain a hardware serial number.
    const QByteArray digest = QCryptographicHash::hash(
        normalizedPath.toUtf8(),
        QCryptographicHash::Sha256);
    return QString::fromLatin1(kStableDisplayIdentityPrefix)
        + QString::fromLatin1(digest.toHex().left(32));
}

[[nodiscard]] inline bool captureOutputIdEquals(
    const QStringView first,
    const QStringView second) noexcept
{
    return !first.isEmpty()
        && !second.isEmpty()
        && first.compare(second, Qt::CaseInsensitive) == 0;
}

[[nodiscard]] inline bool captureOutputMatchesId(
    const CaptureOutput& output,
    const QStringView requestedId) noexcept
{
    return captureOutputIdEquals(output.id, requestedId)
        || captureOutputIdEquals(output.legacyId, requestedId);
}

[[nodiscard]] inline const CaptureOutput* resolveCaptureOutput(
    const QVector<CaptureOutput>& outputs,
    const QStringView requestedId) noexcept
{
    if (!requestedId.trimmed().isEmpty()) {
        const auto match = std::find_if(
            outputs.cbegin(),
            outputs.cend(),
            [requestedId](const CaptureOutput& output) {
                return captureOutputMatchesId(output, requestedId);
            });
        return match == outputs.cend() ? nullptr : &*match;
    }

    const auto primary = std::find_if(
        outputs.cbegin(),
        outputs.cend(),
        [](const CaptureOutput& output) {
            return output.primary;
        });
    if (primary != outputs.cend()) {
        return &*primary;
    }
    return outputs.isEmpty() ? nullptr : &outputs.constFirst();
}

[[nodiscard]] inline bool captureOutputIdentityEquals(
    const CaptureOutput& output,
    const QStringView selectedId,
    const QStringView selectedLegacyId) noexcept
{
    return captureOutputIdEquals(output.id, selectedId)
        || captureOutputIdEquals(output.id, selectedLegacyId)
        || captureOutputIdEquals(output.legacyId, selectedId)
        || captureOutputIdEquals(output.legacyId, selectedLegacyId);
}

[[nodiscard]] inline bool automaticCaptureOutputChanged(
    const QVector<CaptureOutput>& outputs,
    const QStringView selectedId,
    const QStringView selectedLegacyId) noexcept
{
    const CaptureOutput* desired = resolveCaptureOutput(outputs, {});
    // Treat a transient enumeration failure as inconclusive. A real detach or
    // mode change will invalidate Desktop Duplication and use its recovery
    // path; eagerly tearing down a still-working backend would create flicker.
    return desired != nullptr
        && !captureOutputIdentityEquals(
            *desired,
            selectedId,
            selectedLegacyId);
}

} // namespace ambilight::capture
