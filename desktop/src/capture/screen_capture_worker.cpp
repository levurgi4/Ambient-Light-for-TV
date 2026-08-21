#include "screen_capture_worker.hpp"

#include "capture_output_selection.hpp"

#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QMetaObject>
#include <QMutexLocker>
#include <QPointer>
#include <QTimer>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <utility>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

namespace ambilight::capture {

namespace {

using Microsoft::WRL::ComPtr;

constexpr UINT kShaderThreads = 64U;
constexpr UINT kProbeLineCount = 96U;
constexpr UINT kProbeSamplesPerLine = 64U;
constexpr UINT kProbeResultCount = kProbeLineCount * 4U;
constexpr UINT kGpuResultCapacity =
    (kProbeResultCount > static_cast<UINT>(kMaximumLedCount))
    ? kProbeResultCount
    : static_cast<UINT>(kMaximumLedCount);
constexpr quint64 kProbeIntervalFrames = 6U;
constexpr qint64 kWaitingStatusDelayMs = 1'500;
constexpr int kRetryDelayMs = 750;

const char kEdgeSamplingShader[] = R"HLSL(
Texture2D<float4> DesktopTexture : register(t0);
RWStructuredBuffer<uint> Results : register(u0);

cbuffer Parameters : register(b0)
{
    uint SourceWidth;
    uint SourceHeight;
    uint LogicalWidth;
    uint LogicalHeight;

    uint Rotation;
    uint Mode;
    uint ResultCount;
    uint ProbeLineCount;

    uint InsetLeft;
    uint InsetTop;
    uint InsetRight;
    uint InsetBottom;

    uint TopCount;
    uint RightCount;
    uint BottomCount;
    uint LeftCount;

    uint SamplingDepth;
    uint ProbeSamples;
    float BlackThreshold;
    float MaximumHorizontalInsetFraction;

    float MaximumVerticalInsetFraction;
    uint RegionColorMode;
    uint DepthSampleCount;
    uint LedTransferMode;
};

float4 ReadLogical(uint2 logicalPosition)
{
    logicalPosition.x = min(logicalPosition.x, LogicalWidth - 1);
    logicalPosition.y = min(logicalPosition.y, LogicalHeight - 1);

    uint2 sourcePosition = logicalPosition;
    if (Rotation == 1)
    {
        sourcePosition = uint2(
            logicalPosition.y,
            SourceHeight - 1 - logicalPosition.x);
    }
    else if (Rotation == 2)
    {
        sourcePosition = uint2(
            SourceWidth - 1 - logicalPosition.x,
            SourceHeight - 1 - logicalPosition.y);
    }
    else if (Rotation == 3)
    {
        sourcePosition = uint2(
            SourceWidth - 1 - logicalPosition.y,
            logicalPosition.x);
    }

    sourcePosition.x = min(sourcePosition.x, SourceWidth - 1);
    sourcePosition.y = min(sourcePosition.y, SourceHeight - 1);
    return DesktopTexture.Load(int3(sourcePosition, 0));
}

float3 ApplyLedTransfer(float3 color)
{
    color = saturate(color);
    if (LedTransferMode == 0)
    {
        return color;
    }
    if (LedTransferMode == 1)
    {
        return pow(color, 1.6);
    }
    if (LedTransferMode == 3)
    {
        return pow(color, 2.6);
    }

    // Desktop Duplication supplies display-ready sRGB values, while LED PWM
    // controls light output approximately linearly. Decode sRGB before
    // averaging so dark source values do not become disproportionately bright.
    const float3 linearSegment = color / 12.92;
    const float3 powerSegment = pow(
        (color + 0.055) / 1.055,
        2.4);
    return lerp(
        linearSegment,
        powerSegment,
        step(float3(0.04045, 0.04045, 0.04045), color));
}

void ProbeBlackBars(uint resultIndex)
{
    const uint side = resultIndex / ProbeLineCount;
    const uint lineIndex = resultIndex % ProbeLineCount;
    const uint axisLength =
        (side == 0 || side == 2) ? LogicalHeight : LogicalWidth;
    const float maximumInsetFraction =
        (side == 0 || side == 2)
        ? MaximumHorizontalInsetFraction
        : MaximumVerticalInsetFraction;
    const uint maximumDepth =
        (uint)round((float)axisLength * maximumInsetFraction);
    const float linePosition =
        (ProbeLineCount > 1)
        ? ((float)lineIndex / (float)(ProbeLineCount - 1))
        : 0.0;
    const uint depth = (uint)round(linePosition * (float)maximumDepth);

    float luminanceSum = 0.0;
    uint brightPixels = 0;
    const float brightThreshold = max(BlackThreshold * 2.5, 0.06);

    [loop]
    for (uint sampleIndex = 0; sampleIndex < ProbeSamples; ++sampleIndex)
    {
        const float samplePosition =
            (ProbeSamples > 1)
            ? ((float)sampleIndex / (float)(ProbeSamples - 1))
            : 0.5;
        // Ignore the outermost 8% so perpendicular bars and rounded
        // screen corners cannot dominate a probe line.
        const float centralPosition = 0.08 + samplePosition * 0.84;

        uint2 pixelPosition;
        if (side == 0)
        {
            pixelPosition = uint2(
                min((uint)round(
                    centralPosition * (float)(LogicalWidth - 1)),
                    LogicalWidth - 1),
                min(depth, LogicalHeight - 1));
        }
        else if (side == 1)
        {
            pixelPosition = uint2(
                LogicalWidth - 1 - min(depth, LogicalWidth - 1),
                min((uint)round(
                    centralPosition * (float)(LogicalHeight - 1)),
                    LogicalHeight - 1));
        }
        else if (side == 2)
        {
            pixelPosition = uint2(
                min((uint)round(
                    centralPosition * (float)(LogicalWidth - 1)),
                    LogicalWidth - 1),
                LogicalHeight - 1 - min(depth, LogicalHeight - 1));
        }
        else
        {
            pixelPosition = uint2(
                min(depth, LogicalWidth - 1),
                min((uint)round(
                    centralPosition * (float)(LogicalHeight - 1)),
                    LogicalHeight - 1));
        }

        const float3 color = ReadLogical(pixelPosition).rgb;
        const float luminance =
            dot(color, float3(0.2126, 0.7152, 0.0722));
        luminanceSum += luminance;
        brightPixels += (luminance > brightThreshold) ? 1 : 0;
    }

    const float meanLuminance =
        luminanceSum / max((float)ProbeSamples, 1.0);
    const float brightFraction =
        (float)brightPixels / max((float)ProbeSamples, 1.0);
    const uint meanEncoded =
        (uint)round(saturate(meanLuminance) * 65535.0);
    const uint brightEncoded =
        (uint)round(saturate(brightFraction) * 65535.0);
    Results[resultIndex] = meanEncoded | (brightEncoded << 16);
}

float3 ReadEdgeSample(
    uint side,
    uint localIndex,
    uint sideCount,
    uint contentLeft,
    uint contentTop,
    uint contentRight,
    uint contentBottom,
    uint contentWidth,
    uint contentHeight,
    uint usableDepth,
    uint alongIndex,
    uint depthIndex,
    uint alongSamples,
    uint depthSamples)
{
    const float along =
        ((float)localIndex
         + ((float)alongIndex + 0.5) / (float)alongSamples)
        / max((float)sideCount, 1.0);
    const float depth =
        ((float)depthIndex + 0.5) / (float)depthSamples;

    uint2 pixelPosition;
    if (side == 0)
    {
        pixelPosition = uint2(
            contentLeft + min(
                (uint)floor(along * (float)contentWidth),
                contentWidth - 1),
            contentTop + min(
                (uint)floor(depth * (float)usableDepth),
                usableDepth - 1));
    }
    else if (side == 1)
    {
        pixelPosition = uint2(
            contentRight - 1 - min(
                (uint)floor(depth * (float)usableDepth),
                usableDepth - 1),
            contentTop + min(
                (uint)floor(along * (float)contentHeight),
                contentHeight - 1));
    }
    else if (side == 2)
    {
        // Canonical bottom order is right to left.
        pixelPosition = uint2(
            contentRight - 1 - min(
                (uint)floor(along * (float)contentWidth),
                contentWidth - 1),
            contentBottom - 1 - min(
                (uint)floor(depth * (float)usableDepth),
                usableDepth - 1));
    }
    else
    {
        // Canonical left order is bottom to top.
        pixelPosition = uint2(
            contentLeft + min(
                (uint)floor(depth * (float)usableDepth),
                usableDepth - 1),
            contentBottom - 1 - min(
                (uint)floor(along * (float)contentHeight),
                contentHeight - 1));
    }

    return ReadLogical(pixelPosition).rgb;
}

uint QuantizedColorBin(float3 color)
{
    const uint3 quantized = min(
        (uint3)floor(saturate(color) * 4.0),
        uint3(3, 3, 3));
    return quantized.r | (quantized.g << 2) | (quantized.b << 4);
}

uint ColorBinTieBreakPriority(uint colorBin)
{
    const uint red = colorBin & 3;
    const uint green = (colorBin >> 2) & 3;
    const uint blue = (colorBin >> 4) & 3;
    const uint highest = max(red, max(green, blue));
    const uint lowest = min(red, min(green, blue));
    const uint chroma = highest - lowest;
    const uint brightness = red + green * 2 + blue;
    return chroma * 16 + brightness;
}

void SampleEdge(uint resultIndex)
{
    const uint contentLeft = min(InsetLeft, LogicalWidth - 1);
    const uint contentTop = min(InsetTop, LogicalHeight - 1);
    const uint contentRight =
        max(contentLeft + 1, LogicalWidth - min(InsetRight, LogicalWidth - 1));
    const uint contentBottom =
        max(contentTop + 1, LogicalHeight - min(InsetBottom, LogicalHeight - 1));
    const uint contentWidth = contentRight - contentLeft;
    const uint contentHeight = contentBottom - contentTop;
    const uint usableDepth = max(
        1,
        min(SamplingDepth, min(contentWidth, contentHeight) / 2));

    uint side = 0;
    uint localIndex = resultIndex;
    uint sideCount = TopCount;
    if (localIndex >= TopCount)
    {
        localIndex -= TopCount;
        side = 1;
        sideCount = RightCount;
    }
    if (side == 1 && localIndex >= RightCount)
    {
        localIndex -= RightCount;
        side = 2;
        sideCount = BottomCount;
    }
    if (side == 2 && localIndex >= BottomCount)
    {
        localIndex -= BottomCount;
        side = 3;
        sideCount = LeftCount;
    }

    const uint alongSamples = 4;
    const uint depthSamples = min(32u, max(8u, DepthSampleCount));
    const uint sampleCount = alongSamples * depthSamples;
    const uint rootMeanSquareMode = 1;
    const uint dominantMode = 2;

    float3 colorSum = float3(0.0, 0.0, 0.0);
    uint colorHistogram[64];
    if (RegionColorMode == dominantMode)
    {
        [unroll]
        for (uint colorBin = 0; colorBin < 64; ++colorBin)
        {
            colorHistogram[colorBin] = 0;
        }
    }

    [unroll]
    for (uint alongIndex = 0; alongIndex < alongSamples; ++alongIndex)
    {
        [loop]
        for (uint depthIndex = 0; depthIndex < depthSamples; ++depthIndex)
        {
            const float3 sourceColor = ReadEdgeSample(
                side,
                localIndex,
                sideCount,
                contentLeft,
                contentTop,
                contentRight,
                contentBottom,
                contentWidth,
                contentHeight,
                usableDepth,
                alongIndex,
                depthIndex,
                alongSamples,
                depthSamples);
            const float3 color = ApplyLedTransfer(sourceColor);
            if (RegionColorMode == dominantMode)
            {
                colorSum += color;
                colorHistogram[QuantizedColorBin(sourceColor)] += 1;
            }
            else if (RegionColorMode == rootMeanSquareMode)
            {
                colorSum += color * color;
            }
            else
            {
                colorSum += color;
            }
        }
    }

    float3 average = float3(0.0, 0.0, 0.0);
    if (RegionColorMode == dominantMode)
    {
        uint dominantBin = 0;
        uint dominantCount = 0;
        uint dominantPriority = 0;
        [unroll]
        for (uint colorBin = 0; colorBin < 64; ++colorBin)
        {
            const uint count = colorHistogram[colorBin];
            const uint priority = ColorBinTieBreakPriority(colorBin);
            if (count > dominantCount
                || (count == dominantCount && priority > dominantPriority))
            {
                dominantBin = colorBin;
                dominantCount = count;
                dominantPriority = priority;
            }
        }

        // A scattered gradient can leave every quantized bin with only one
        // or two pixels. In that case a "dominant" winner would be arbitrary,
        // so fall back to the stable mean unless at least 20% agree.
        if (dominantCount * 5u < sampleCount)
        {
            average = colorSum / (float)sampleCount;
        }
        else
        {
            float3 dominantSum = float3(0.0, 0.0, 0.0);
            uint matchingSamples = 0;
            [unroll]
            for (uint alongIndex = 0; alongIndex < alongSamples; ++alongIndex)
            {
                [loop]
                for (uint depthIndex = 0; depthIndex < depthSamples; ++depthIndex)
                {
                    const float3 sourceColor = ReadEdgeSample(
                        side,
                        localIndex,
                        sideCount,
                        contentLeft,
                        contentTop,
                        contentRight,
                        contentBottom,
                        contentWidth,
                        contentHeight,
                        usableDepth,
                        alongIndex,
                        depthIndex,
                        alongSamples,
                        depthSamples);
                    if (QuantizedColorBin(sourceColor) == dominantBin)
                    {
                        dominantSum += ApplyLedTransfer(sourceColor);
                        matchingSamples += 1;
                    }
                }
            }
            average = dominantSum / max((float)matchingSamples, 1.0);
        }
    }
    else
    {
        average = colorSum / (float)sampleCount;
        if (RegionColorMode == rootMeanSquareMode)
        {
            average = sqrt(max(average, float3(0.0, 0.0, 0.0)));
        }
    }
    average = saturate(average);
    const uint red = (uint)round(average.r * 255.0);
    const uint green = (uint)round(average.g * 255.0);
    const uint blue = (uint)round(average.b * 255.0);
    Results[resultIndex] =
        0xff000000 | (red << 16) | (green << 8) | blue;
}

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint resultIndex = dispatchThreadId.x;
    if (resultIndex >= ResultCount)
    {
        return;
    }

    if (Mode == 0)
    {
        ProbeBlackBars(resultIndex);
    }
    else
    {
        SampleEdge(resultIndex);
    }
}
)HLSL";

[[nodiscard]] qint64 nativeError(const HRESULT result) noexcept
{
    return static_cast<qint64>(static_cast<quint32>(result));
}

[[nodiscard]] QString hresultText(const HRESULT result)
{
    return QStringLiteral("HRESULT 0x%1")
        .arg(
            static_cast<quint32>(result),
            8,
            16,
            QLatin1Char('0'));
}

[[nodiscard]] QString normalizedLegacyOutputId(const QStringView value)
{
    return value.trimmed().toString().toCaseFolded();
}

using StableOutputIdentities = QHash<QString, QString>;

[[nodiscard]] StableOutputIdentities enumerateStableOutputIdentities()
{
    constexpr UINT queryFlags = QDC_ONLY_ACTIVE_PATHS;
    constexpr int maximumAttempts = 3;

    for (int attempt = 0; attempt < maximumAttempts; ++attempt) {
        UINT pathCount = 0;
        UINT modeCount = 0;
        const LONG sizeResult = GetDisplayConfigBufferSizes(
            queryFlags,
            &pathCount,
            &modeCount);
        if (sizeResult != ERROR_SUCCESS) {
            return {};
        }

        std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
        const LONG queryResult = QueryDisplayConfig(
            queryFlags,
            &pathCount,
            paths.data(),
            &modeCount,
            modes.data(),
            nullptr);
        if (queryResult == ERROR_INSUFFICIENT_BUFFER) {
            continue;
        }
        if (queryResult != ERROR_SUCCESS) {
            return {};
        }
        paths.resize(pathCount);

        StableOutputIdentities identities;
        for (const auto& path : paths) {
            DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName{};
            sourceName.header.type =
                DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            sourceName.header.size = sizeof(sourceName);
            sourceName.header.adapterId = path.sourceInfo.adapterId;
            sourceName.header.id = path.sourceInfo.id;
            if (DisplayConfigGetDeviceInfo(&sourceName.header)
                != ERROR_SUCCESS) {
                continue;
            }

            DISPLAYCONFIG_TARGET_DEVICE_NAME targetName{};
            targetName.header.type =
                DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
            targetName.header.size = sizeof(targetName);
            targetName.header.adapterId = path.targetInfo.adapterId;
            targetName.header.id = path.targetInfo.id;
            if (DisplayConfigGetDeviceInfo(&targetName.header)
                != ERROR_SUCCESS) {
                continue;
            }

            const QString sourceId = normalizedLegacyOutputId(
                QString::fromWCharArray(sourceName.viewGdiDeviceName));
            const QString stableId = stableCaptureOutputId(
                QString::fromWCharArray(targetName.monitorDevicePath));
            if (sourceId.isEmpty() || stableId.isEmpty()) {
                continue;
            }

            const auto existing = identities.constFind(sourceId);
            if (existing == identities.cend()
                || stableId.compare(
                       existing.value(),
                       Qt::CaseInsensitive)
                    < 0) {
                // Clone paths can expose more than one physical target for
                // the same DXGI source. Their pixels are identical, so choose
                // a deterministic target identity for persistence.
                identities.insert(sourceId, stableId);
            }
        }
        return identities;
    }

    return {};
}

[[nodiscard]] CaptureOutput describeOutput(
    const DXGI_OUTPUT_DESC& description,
    const StableOutputIdentities& stableIdentities)
{
    CaptureOutput output;
    output.legacyId =
        QString::fromWCharArray(description.DeviceName).trimmed();
    output.id = stableIdentities.value(
        normalizedLegacyOutputId(output.legacyId),
        output.legacyId);

    DISPLAY_DEVICEW displayDevice{};
    displayDevice.cb = static_cast<DWORD>(sizeof(displayDevice));
    if (EnumDisplayDevicesW(
            description.DeviceName,
            0,
            &displayDevice,
            0)
        != FALSE) {
        output.label = QString::fromWCharArray(displayDevice.DeviceString).trimmed();
    }
    if (output.label.isEmpty()) {
        output.label = output.id;
    }

    const RECT& rectangle = description.DesktopCoordinates;
    const int width = std::max<LONG>(0, rectangle.right - rectangle.left);
    const int height = std::max<LONG>(0, rectangle.bottom - rectangle.top);
    output.geometry = {
        rectangle.left,
        rectangle.top,
        width,
        height};

    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = static_cast<DWORD>(sizeof(monitorInfo));
    if (description.Monitor != nullptr
        && GetMonitorInfoW(description.Monitor, &monitorInfo) != FALSE) {
        output.primary = (monitorInfo.dwFlags & MONITORINFOF_PRIMARY) != 0;
    }

    output.label = QStringLiteral("%1 — %2×%3 (%4, %5)")
                       .arg(output.label)
                       .arg(width)
                       .arg(height)
                       .arg(rectangle.left)
                       .arg(rectangle.top);
    return output;
}

struct SelectedOutput final {
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput1> output;
    CaptureOutput description;
};

[[nodiscard]] QVector<CaptureOutput> enumerateDxgiOutputs(
    QString* errorMessage)
{
    QVector<CaptureOutput> result;
    const StableOutputIdentities stableIdentities =
        enumerateStableOutputIdentities();

    ComPtr<IDXGIFactory1> factory;
    const HRESULT factoryResult =
        CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(factoryResult)) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("CreateDXGIFactory1 failed: %1")
                                .arg(hresultText(factoryResult));
        }
        return result;
    }

    for (UINT adapterIndex = 0;; ++adapterIndex) {
        ComPtr<IDXGIAdapter1> adapter;
        const HRESULT adapterResult =
            factory->EnumAdapters1(adapterIndex, adapter.GetAddressOf());
        if (adapterResult == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (FAILED(adapterResult)) {
            if (errorMessage != nullptr) {
                *errorMessage = QStringLiteral("DXGI adapter enumeration failed: %1")
                                    .arg(hresultText(adapterResult));
            }
            return {};
        }

        for (UINT outputIndex = 0;; ++outputIndex) {
            ComPtr<IDXGIOutput> output;
            const HRESULT outputResult =
                adapter->EnumOutputs(outputIndex, output.GetAddressOf());
            if (outputResult == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            if (FAILED(outputResult)) {
                if (errorMessage != nullptr) {
                    *errorMessage =
                        QStringLiteral("DXGI output enumeration failed: %1")
                            .arg(hresultText(outputResult));
                }
                return {};
            }

            DXGI_OUTPUT_DESC description{};
            const HRESULT descriptionResult = output->GetDesc(&description);
            if (FAILED(descriptionResult)) {
                continue;
            }
            if (description.AttachedToDesktop == FALSE) {
                continue;
            }
            result.push_back(
                describeOutput(description, stableIdentities));
        }
    }

    if (result.isEmpty() && errorMessage != nullptr) {
        *errorMessage = QStringLiteral("No desktop-attached DXGI outputs found.");
    }
    return result;
}

[[nodiscard]] std::optional<SelectedOutput> selectDxgiOutput(
    const QString& requestedId,
    QString* errorMessage,
    HRESULT* nativeResult)
{
    const StableOutputIdentities stableIdentities =
        enumerateStableOutputIdentities();
    ComPtr<IDXGIFactory1> factory;
    HRESULT result = CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(result)) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("CreateDXGIFactory1 failed: %1")
                                .arg(hresultText(result));
        }
        if (nativeResult != nullptr) {
            *nativeResult = result;
        }
        return std::nullopt;
    }

    std::optional<SelectedOutput> firstOutput;
    std::optional<SelectedOutput> primaryOutput;

    for (UINT adapterIndex = 0;; ++adapterIndex) {
        ComPtr<IDXGIAdapter1> adapter;
        result = factory->EnumAdapters1(adapterIndex, adapter.GetAddressOf());
        if (result == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (FAILED(result)) {
            break;
        }

        for (UINT outputIndex = 0;; ++outputIndex) {
            ComPtr<IDXGIOutput> baseOutput;
            result = adapter->EnumOutputs(outputIndex, baseOutput.GetAddressOf());
            if (result == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            if (FAILED(result)) {
                break;
            }

            DXGI_OUTPUT_DESC dxgiDescription{};
            if (FAILED(baseOutput->GetDesc(&dxgiDescription))
                || dxgiDescription.AttachedToDesktop == FALSE) {
                continue;
            }

            ComPtr<IDXGIOutput1> output;
            if (FAILED(baseOutput.As(&output))) {
                continue;
            }

            SelectedOutput candidate{
                adapter,
                output,
                describeOutput(dxgiDescription, stableIdentities)};
            if (captureOutputMatchesId(
                    candidate.description,
                    requestedId)) {
                return candidate;
            }
            if (!firstOutput.has_value()) {
                firstOutput = candidate;
            }
            if (candidate.description.primary && !primaryOutput.has_value()) {
                primaryOutput = candidate;
            }
        }
    }

    if (requestedId.trimmed().isEmpty()) {
        if (primaryOutput.has_value()) {
            return primaryOutput;
        }
        if (firstOutput.has_value()) {
            return firstOutput;
        }
    }

    if (errorMessage != nullptr) {
        if (requestedId.trimmed().isEmpty()) {
            *errorMessage = QStringLiteral("No desktop-attached DXGI outputs found.");
        } else {
            *errorMessage = QStringLiteral("DXGI output \"%1\" is not available.")
                                .arg(requestedId);
        }
    }
    if (nativeResult != nullptr) {
        *nativeResult = DXGI_ERROR_NOT_FOUND;
    }
    return std::nullopt;
}

class PairStabilizer final {
public:
    void reset() noexcept
    {
        stable_ = 0;
        pending_ = 0;
        pendingHits_ = 0;
        hasPending_ = false;
    }

    void update(const std::optional<int> proposal, const int axisLength) noexcept
    {
        if (!proposal.has_value()) {
            hasPending_ = false;
            pendingHits_ = 0;
            return;
        }

        const int tolerance = std::max(3, axisLength / 1'000);
        if (std::abs(*proposal - stable_) <= tolerance) {
            hasPending_ = false;
            pendingHits_ = 0;
            return;
        }

        if (!hasPending_ || std::abs(*proposal - pending_) > tolerance) {
            pending_ = *proposal;
            pendingHits_ = 1;
            hasPending_ = true;
        } else {
            ++pendingHits_;
        }

        // Enter bars quickly, but require twice as much evidence before
        // restoring the full frame. This prevents flashes and scene fades
        // from making the sampled perimeter jump.
        const int requiredHits = (pending_ == 0) ? 6 : 3;
        if (pendingHits_ >= requiredHits) {
            stable_ = pending_;
            hasPending_ = false;
            pendingHits_ = 0;
        }
    }

    [[nodiscard]] int value() const noexcept
    {
        return stable_;
    }

private:
    int stable_ = 0;
    int pending_ = 0;
    int pendingHits_ = 0;
    bool hasPending_ = false;
};

class BlackBarDetector final {
public:
    void reset() noexcept
    {
        horizontal_.reset();
        vertical_.reset();
        insets_ = {};
        frameSize_ = {};
    }

    void update(
        const std::array<quint32, kProbeResultCount>& probes,
        const QSize frameSize,
        const float blackThreshold,
        const BlackBarSearchLimits limits) noexcept
    {
        if (frameSize != frameSize_) {
            reset();
            frameSize_ = frameSize;
        }

        const auto top = limits.horizontalProbeFraction > 0.0F
            ? estimate(
                  probes.data(),
                  frameSize.height(),
                  blackThreshold,
                  limits.horizontalProbeFraction,
                  limits.horizontalFraction)
            : std::optional<int>{0};
        const auto right = limits.verticalProbeFraction > 0.0F
            ? estimate(
                  probes.data() + kProbeLineCount,
                  frameSize.width(),
                  blackThreshold,
                  limits.verticalProbeFraction,
                  limits.verticalFraction)
            : std::optional<int>{0};
        const auto bottom = limits.horizontalProbeFraction > 0.0F
            ? estimate(
                  probes.data() + (kProbeLineCount * 2U),
                  frameSize.height(),
                  blackThreshold,
                  limits.horizontalProbeFraction,
                  limits.horizontalFraction)
            : std::optional<int>{0};
        const auto left = limits.verticalProbeFraction > 0.0F
            ? estimate(
                  probes.data() + (kProbeLineCount * 3U),
                  frameSize.width(),
                  blackThreshold,
                  limits.verticalProbeFraction,
                  limits.verticalFraction)
            : std::optional<int>{0};

        const auto horizontalProposal =
            reconcilePair(top, bottom, frameSize.height());
        const auto verticalProposal =
            reconcilePair(left, right, frameSize.width());

        horizontal_.update(horizontalProposal, frameSize.height());
        vertical_.update(verticalProposal, frameSize.width());

        const int maximumHorizontal =
            std::max(0, (frameSize.height() - 32) / 2);
        const int maximumVertical =
            std::max(0, (frameSize.width() - 32) / 2);
        const int horizontalInset =
            std::clamp(horizontal_.value(), 0, maximumHorizontal);
        const int verticalInset =
            std::clamp(vertical_.value(), 0, maximumVertical);
        insets_ = {
            verticalInset,
            horizontalInset,
            verticalInset,
            horizontalInset};
    }

    [[nodiscard]] CaptureInsets insets() const noexcept
    {
        return insets_;
    }

private:
    [[nodiscard]] static std::optional<int> estimate(
        const quint32* probes,
        const int axisLength,
        const float blackThreshold,
        const float probeFraction,
        const float maximumInsetFraction) noexcept
    {
        const auto isDark = [blackThreshold](const quint32 encoded) {
            const float mean =
                static_cast<float>(encoded & 0xffffU) / 65'535.0F;
            const float brightFraction =
                static_cast<float>(encoded >> 16U) / 65'535.0F;
            return mean <= (blackThreshold * 1.35F)
                && brightFraction <= 0.06F;
        };

        if (!isDark(probes[0])) {
            return 0;
        }

        int consecutiveContentLines = 0;
        UINT firstContentLine = 0;
        for (UINT line = 1; line < kProbeLineCount; ++line) {
            if (!isDark(probes[line])) {
                if (consecutiveContentLines == 0) {
                    firstContentLine = line;
                }
                ++consecutiveContentLines;
                if (consecutiveContentLines >= 2) {
                    const int probeDepth = static_cast<int>(
                        std::lround(
                            static_cast<double>(axisLength)
                            * static_cast<double>(probeFraction)));
                    const int maximumInset = static_cast<int>(
                        std::lround(
                            static_cast<double>(axisLength)
                            * static_cast<double>(maximumInsetFraction)));
                    const double position =
                        static_cast<double>(firstContentLine)
                        / static_cast<double>(kProbeLineCount - 1U);
                    return std::clamp(
                        static_cast<int>(
                            std::lround(position * probeDepth)),
                        0,
                        maximumInset);
                }
            } else {
                consecutiveContentLines = 0;
            }
        }

        // An all-black scan has no trustworthy content boundary. Keep the
        // previous stable inset instead of treating a fade as a giant bar.
        return std::nullopt;
    }

    [[nodiscard]] static std::optional<int> reconcilePair(
        const std::optional<int> first,
        const std::optional<int> second,
        const int axisLength) noexcept
    {
        if (!first.has_value() || !second.has_value()) {
            return std::nullopt;
        }
        if (*first == 0 && *second == 0) {
            return 0;
        }
        if (*first == 0 || *second == 0) {
            return std::nullopt;
        }

        const int larger = std::max(*first, *second);
        const int tolerance =
            std::max({12, axisLength / 300, larger * 15 / 100});
        if (std::abs(*first - *second) > tolerance) {
            return std::nullopt;
        }
        return (*first + *second) / 2;
    }

    PairStabilizer horizontal_;
    PairStabilizer vertical_;
    CaptureInsets insets_;
    QSize frameSize_;
};

struct alignas(16) ShaderParameters final {
    UINT sourceWidth = 0;
    UINT sourceHeight = 0;
    UINT logicalWidth = 0;
    UINT logicalHeight = 0;

    UINT rotation = 0;
    UINT mode = 0;
    UINT resultCount = 0;
    UINT probeLineCount = 0;

    UINT insetLeft = 0;
    UINT insetTop = 0;
    UINT insetRight = 0;
    UINT insetBottom = 0;

    UINT topCount = 0;
    UINT rightCount = 0;
    UINT bottomCount = 0;
    UINT leftCount = 0;

    UINT samplingDepth = 0;
    UINT probeSamples = 0;
    float blackThreshold = 0.0F;
    float maximumHorizontalInsetFraction = 0.0F;

    float maximumVerticalInsetFraction = 0.0F;
    UINT regionColorMode = 0;
    UINT depthSampleCount = 0;
    UINT ledTransferMode = 0;
};

static_assert(sizeof(ShaderParameters) == 96U);
static_assert((sizeof(ShaderParameters) % 16U) == 0U);

enum class FrameResult {
    Sampled,
    WaitTimeout,
    AccessLost,
    Failed
};

struct CapturedFrame final {
    FrameResult result = FrameResult::Failed;
    QSize frameSize;
    CaptureInsets contentInsets;
    QVector<QRgb> colors;
    HRESULT error = S_OK;
    QString errorMessage;
};

class DxgiCaptureBackend final {
public:
    [[nodiscard]] bool initialize(
        const QString& requestedOutputId,
        CaptureOutput* selectedDescription,
        QString* errorMessage,
        HRESULT* nativeResult)
    {
        reset();

        auto selection =
            selectDxgiOutput(requestedOutputId, errorMessage, nativeResult);
        if (!selection.has_value()) {
            return false;
        }
        selectedOutput_ = selection->description;

        constexpr D3D_FEATURE_LEVEL requestedFeatureLevels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL actualFeatureLevel = D3D_FEATURE_LEVEL_11_0;
        UINT deviceFlags =
            D3D11_CREATE_DEVICE_BGRA_SUPPORT
            | D3D11_CREATE_DEVICE_SINGLETHREADED;

        HRESULT result = D3D11CreateDevice(
            selection->adapter.Get(),
            D3D_DRIVER_TYPE_UNKNOWN,
            nullptr,
            deviceFlags,
            requestedFeatureLevels,
            static_cast<UINT>(std::size(requestedFeatureLevels)),
            D3D11_SDK_VERSION,
            device_.GetAddressOf(),
            &actualFeatureLevel,
            context_.GetAddressOf());
        if (result == E_INVALIDARG) {
            result = D3D11CreateDevice(
                selection->adapter.Get(),
                D3D_DRIVER_TYPE_UNKNOWN,
                nullptr,
                deviceFlags,
                requestedFeatureLevels + 1,
                1,
                D3D11_SDK_VERSION,
                device_.GetAddressOf(),
                &actualFeatureLevel,
                context_.GetAddressOf());
        }
        if (FAILED(result)) {
            return failInitialization(
                QStringLiteral("D3D11CreateDevice failed: %1")
                    .arg(hresultText(result)),
                result,
                errorMessage,
                nativeResult);
        }
        if (actualFeatureLevel < D3D_FEATURE_LEVEL_11_0) {
            return failInitialization(
                QStringLiteral("D3D feature level 11.0 is required."),
                E_NOINTERFACE,
                errorMessage,
                nativeResult);
        }

        result = selection->output->DuplicateOutput(
            device_.Get(),
            duplication_.GetAddressOf());
        if (FAILED(result)) {
            return failInitialization(
                QStringLiteral("IDXGIOutput1::DuplicateOutput failed: %1")
                    .arg(hresultText(result)),
                result,
                errorMessage,
                nativeResult);
        }
        duplication_->GetDesc(&duplicationDescription_);

        if (!createShader(errorMessage, &result)
            || !createBuffers(errorMessage, &result)) {
            if (nativeResult != nullptr) {
                *nativeResult = result;
            }
            reset();
            return false;
        }

        if (selectedDescription != nullptr) {
            *selectedDescription = selectedOutput_;
        }
        if (nativeResult != nullptr) {
            *nativeResult = S_OK;
        }
        return true;
    }

    void reset() noexcept
    {
        sampleView_.Reset();
        sampleTexture_.Reset();
        sampleWidth_ = 0;
        sampleHeight_ = 0;
        sampleFormat_ = DXGI_FORMAT_UNKNOWN;
        stagingBuffer_.Reset();
        resultBuffer_.Reset();
        resultView_.Reset();
        parameterBuffer_.Reset();
        shader_.Reset();
        duplication_.Reset();
        context_.Reset();
        device_.Reset();
        duplicationDescription_ = {};
        selectedOutput_ = {};
    }

    [[nodiscard]] CapturedFrame capture(
        const CaptureConfiguration& configuration,
        BlackBarDetector& detector,
        const bool runBlackBarProbe)
    {
        CapturedFrame captured;
        if (duplication_ == nullptr) {
            captured.error = E_UNEXPECTED;
            captured.errorMessage = QStringLiteral(
                "Desktop duplication is not initialized.");
            return captured;
        }

        DXGI_OUTDUPL_FRAME_INFO frameInformation{};
        ComPtr<IDXGIResource> desktopResource;
        const HRESULT acquireResult = duplication_->AcquireNextFrame(
            0,
            &frameInformation,
            desktopResource.GetAddressOf());
        if (acquireResult == DXGI_ERROR_WAIT_TIMEOUT) {
            captured.result = FrameResult::WaitTimeout;
            captured.error = acquireResult;
            return captured;
        }
        if (acquireResult == DXGI_ERROR_ACCESS_LOST
            || acquireResult == DXGI_ERROR_DEVICE_REMOVED
            || acquireResult == DXGI_ERROR_DEVICE_RESET) {
            captured.result = FrameResult::AccessLost;
            captured.error = acquireResult;
            captured.errorMessage =
                QStringLiteral("Desktop duplication access was lost: %1")
                    .arg(hresultText(acquireResult));
            return captured;
        }
        if (FAILED(acquireResult)) {
            captured.error = acquireResult;
            captured.errorMessage =
                QStringLiteral("AcquireNextFrame failed: %1")
                    .arg(hresultText(acquireResult));
            return captured;
        }

        struct FrameReleaseGuard final {
            IDXGIOutputDuplication* duplication = nullptr;
            ~FrameReleaseGuard()
            {
                if (duplication != nullptr) {
                    duplication->ReleaseFrame();
                }
            }
        } releaseGuard{duplication_.Get()};

        ComPtr<ID3D11Texture2D> desktopTexture;
        HRESULT result = desktopResource.As(&desktopTexture);
        if (FAILED(result)) {
            captured.error = result;
            captured.errorMessage =
                QStringLiteral("The duplicated resource is not a D3D11 texture: %1")
                    .arg(hresultText(result));
            return captured;
        }

        D3D11_TEXTURE2D_DESC sourceDescription{};
        desktopTexture->GetDesc(&sourceDescription);
        if (!prepareSampleTexture(sourceDescription, &captured.errorMessage, &result)) {
            captured.error = result;
            return captured;
        }
        context_->CopyResource(sampleTexture_.Get(), desktopTexture.Get());

        const bool quarterTurn =
            duplicationDescription_.Rotation == DXGI_MODE_ROTATION_ROTATE90
            || duplicationDescription_.Rotation == DXGI_MODE_ROTATION_ROTATE270;
        const int logicalWidth = static_cast<int>(
            quarterTurn ? sourceDescription.Height : sourceDescription.Width);
        const int logicalHeight = static_cast<int>(
            quarterTurn ? sourceDescription.Width : sourceDescription.Height);
        if (logicalWidth < 2 || logicalHeight < 2) {
            captured.error = E_UNEXPECTED;
            captured.errorMessage =
                QStringLiteral("The duplicated surface has invalid dimensions.");
            return captured;
        }
        captured.frameSize = {logicalWidth, logicalHeight};

        ShaderParameters parameters =
            makeParameters(configuration, sourceDescription, captured.frameSize);
        const BlackBarSearchLimits searchLimits = blackBarSearchLimits(
            configuration.blackBarDetectionMode,
            captured.frameSize,
            configuration.maximumBlackBarFraction);

        if (configuration.blackBarDetectionMode
                != BlackBarDetectionMode::Off
            && runBlackBarProbe) {
            parameters.mode = 0;
            parameters.resultCount = kProbeResultCount;
            std::array<quint32, kProbeResultCount> probes{};
            if (!dispatch(
                    parameters,
                    probes.data(),
                    static_cast<UINT>(probes.size()),
                    &captured.errorMessage,
                    &result)) {
                captured.error = result;
                return captured;
            }
            detector.update(
                probes,
                captured.frameSize,
                configuration.blackLuminanceThreshold,
                searchLimits);
        }

        captured.contentInsets =
            configuration.blackBarDetectionMode
                != BlackBarDetectionMode::Off
            ? detector.insets()
            : CaptureInsets{};
        parameters.insetLeft =
            static_cast<UINT>(captured.contentInsets.left);
        parameters.insetTop =
            static_cast<UINT>(captured.contentInsets.top);
        parameters.insetRight =
            static_cast<UINT>(captured.contentInsets.right);
        parameters.insetBottom =
            static_cast<UINT>(captured.contentInsets.bottom);
        parameters.samplingDepth = static_cast<UINT>(
            resolvedSamplingDepthPixels(
                captured.frameSize,
                captured.contentInsets,
                configuration.samplingDepthPixels));
        parameters.depthSampleCount = static_cast<UINT>(
            adaptiveDepthSampleCount(
                static_cast<int>(parameters.samplingDepth)));
        parameters.mode = 1;
        parameters.resultCount =
            static_cast<UINT>(configuration.ledCount());

        std::array<quint32, kMaximumLedCount> encodedColors{};
        if (!dispatch(
                parameters,
                encodedColors.data(),
                parameters.resultCount,
                &captured.errorMessage,
                &result)) {
            captured.error = result;
            return captured;
        }

        captured.colors.resize(configuration.ledCount());
        for (int index = 0; index < configuration.ledCount(); ++index) {
            captured.colors[index] = static_cast<QRgb>(
                encodedColors[static_cast<std::size_t>(index)]);
        }
        captured.result = FrameResult::Sampled;
        captured.error = S_OK;
        return captured;
    }

private:
    [[nodiscard]] bool failInitialization(
        const QString& message,
        const HRESULT result,
        QString* errorMessage,
        HRESULT* nativeResult)
    {
        if (errorMessage != nullptr) {
            *errorMessage = message;
        }
        if (nativeResult != nullptr) {
            *nativeResult = result;
        }
        reset();
        return false;
    }

    [[nodiscard]] bool createShader(
        QString* errorMessage,
        HRESULT* nativeResult)
    {
        ComPtr<ID3DBlob> bytecode;
        ComPtr<ID3DBlob> diagnostics;
        constexpr UINT compileFlags =
            D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
        HRESULT result = D3DCompile(
            kEdgeSamplingShader,
            std::strlen(kEdgeSamplingShader),
            "ambilight_edge_sampling.hlsl",
            nullptr,
            nullptr,
            "main",
            "cs_5_0",
            compileFlags,
            0,
            bytecode.GetAddressOf(),
            diagnostics.GetAddressOf());
        if (FAILED(result)) {
            QString diagnosticText;
            if (diagnostics != nullptr) {
                diagnosticText = QString::fromUtf8(
                    static_cast<const char*>(diagnostics->GetBufferPointer()),
                    static_cast<qsizetype>(diagnostics->GetBufferSize()))
                                     .trimmed();
            }
            if (errorMessage != nullptr) {
                *errorMessage =
                    QStringLiteral("D3DCompile failed: %1%2")
                        .arg(hresultText(result))
                        .arg(
                            diagnosticText.isEmpty()
                                ? QString()
                                : QStringLiteral(" — %1").arg(diagnosticText));
            }
            if (nativeResult != nullptr) {
                *nativeResult = result;
            }
            return false;
        }

        result = device_->CreateComputeShader(
            bytecode->GetBufferPointer(),
            bytecode->GetBufferSize(),
            nullptr,
            shader_.GetAddressOf());
        if (FAILED(result)) {
            if (errorMessage != nullptr) {
                *errorMessage =
                    QStringLiteral("CreateComputeShader failed: %1")
                        .arg(hresultText(result));
            }
            if (nativeResult != nullptr) {
                *nativeResult = result;
            }
            return false;
        }
        return true;
    }

    [[nodiscard]] bool createBuffers(
        QString* errorMessage,
        HRESULT* nativeResult)
    {
        D3D11_BUFFER_DESC parameterDescription{};
        parameterDescription.ByteWidth =
            static_cast<UINT>(sizeof(ShaderParameters));
        parameterDescription.Usage = D3D11_USAGE_DEFAULT;
        parameterDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

        HRESULT result = device_->CreateBuffer(
            &parameterDescription,
            nullptr,
            parameterBuffer_.GetAddressOf());
        if (FAILED(result)) {
            return bufferFailure(
                QStringLiteral("CreateBuffer(parameters)"),
                result,
                errorMessage,
                nativeResult);
        }

        D3D11_BUFFER_DESC resultDescription{};
        resultDescription.ByteWidth =
            kGpuResultCapacity * static_cast<UINT>(sizeof(quint32));
        resultDescription.Usage = D3D11_USAGE_DEFAULT;
        resultDescription.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        resultDescription.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        resultDescription.StructureByteStride =
            static_cast<UINT>(sizeof(quint32));
        result = device_->CreateBuffer(
            &resultDescription,
            nullptr,
            resultBuffer_.GetAddressOf());
        if (FAILED(result)) {
            return bufferFailure(
                QStringLiteral("CreateBuffer(results)"),
                result,
                errorMessage,
                nativeResult);
        }

        D3D11_UNORDERED_ACCESS_VIEW_DESC viewDescription{};
        viewDescription.Format = DXGI_FORMAT_UNKNOWN;
        viewDescription.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        viewDescription.Buffer.FirstElement = 0;
        viewDescription.Buffer.NumElements = kGpuResultCapacity;
        result = device_->CreateUnorderedAccessView(
            resultBuffer_.Get(),
            &viewDescription,
            resultView_.GetAddressOf());
        if (FAILED(result)) {
            return bufferFailure(
                QStringLiteral("CreateUnorderedAccessView"),
                result,
                errorMessage,
                nativeResult);
        }

        D3D11_BUFFER_DESC stagingDescription = resultDescription;
        stagingDescription.Usage = D3D11_USAGE_STAGING;
        stagingDescription.BindFlags = 0;
        stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        stagingDescription.MiscFlags = 0;
        stagingDescription.StructureByteStride = 0;
        result = device_->CreateBuffer(
            &stagingDescription,
            nullptr,
            stagingBuffer_.GetAddressOf());
        if (FAILED(result)) {
            return bufferFailure(
                QStringLiteral("CreateBuffer(readback)"),
                result,
                errorMessage,
                nativeResult);
        }

        return true;
    }

    [[nodiscard]] static bool bufferFailure(
        const QString& operation,
        const HRESULT result,
        QString* errorMessage,
        HRESULT* nativeResult)
    {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("%1 failed: %2")
                                .arg(operation, hresultText(result));
        }
        if (nativeResult != nullptr) {
            *nativeResult = result;
        }
        return false;
    }

    [[nodiscard]] bool prepareSampleTexture(
        const D3D11_TEXTURE2D_DESC& sourceDescription,
        QString* errorMessage,
        HRESULT* nativeResult)
    {
        if (sampleTexture_ != nullptr
            && sampleWidth_ == sourceDescription.Width
            && sampleHeight_ == sourceDescription.Height
            && sampleFormat_ == sourceDescription.Format) {
            return true;
        }

        sampleView_.Reset();
        sampleTexture_.Reset();
        sampleWidth_ = 0;
        sampleHeight_ = 0;
        sampleFormat_ = DXGI_FORMAT_UNKNOWN;

        D3D11_TEXTURE2D_DESC sampleDescription = sourceDescription;
        sampleDescription.MipLevels = 1;
        sampleDescription.ArraySize = 1;
        sampleDescription.SampleDesc.Count = 1;
        sampleDescription.SampleDesc.Quality = 0;
        sampleDescription.Usage = D3D11_USAGE_DEFAULT;
        sampleDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        sampleDescription.CPUAccessFlags = 0;
        sampleDescription.MiscFlags = 0;

        HRESULT result = device_->CreateTexture2D(
            &sampleDescription,
            nullptr,
            sampleTexture_.GetAddressOf());
        if (FAILED(result)) {
            if (errorMessage != nullptr) {
                *errorMessage =
                    QStringLiteral("CreateTexture2D(sample) failed: %1")
                        .arg(hresultText(result));
            }
            if (nativeResult != nullptr) {
                *nativeResult = result;
            }
            return false;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC viewDescription{};
        viewDescription.Format = sourceDescription.Format;
        viewDescription.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        viewDescription.Texture2D.MostDetailedMip = 0;
        viewDescription.Texture2D.MipLevels = 1;
        result = device_->CreateShaderResourceView(
            sampleTexture_.Get(),
            &viewDescription,
            sampleView_.GetAddressOf());
        if (FAILED(result)) {
            if (errorMessage != nullptr) {
                *errorMessage =
                    QStringLiteral("CreateShaderResourceView failed: %1")
                        .arg(hresultText(result));
            }
            if (nativeResult != nullptr) {
                *nativeResult = result;
            }
            sampleTexture_.Reset();
            return false;
        }

        sampleWidth_ = sourceDescription.Width;
        sampleHeight_ = sourceDescription.Height;
        sampleFormat_ = sourceDescription.Format;
        return true;
    }

    [[nodiscard]] ShaderParameters makeParameters(
        const CaptureConfiguration& configuration,
        const D3D11_TEXTURE2D_DESC& sourceDescription,
        const QSize logicalSize) const noexcept
    {
        ShaderParameters parameters;
        parameters.sourceWidth = sourceDescription.Width;
        parameters.sourceHeight = sourceDescription.Height;
        parameters.logicalWidth = static_cast<UINT>(logicalSize.width());
        parameters.logicalHeight = static_cast<UINT>(logicalSize.height());
        parameters.rotation = shaderRotation(duplicationDescription_.Rotation);
        parameters.probeLineCount = kProbeLineCount;
        parameters.topCount =
            static_cast<UINT>(configuration.topLedCount);
        parameters.rightCount =
            static_cast<UINT>(configuration.rightLedCount);
        parameters.bottomCount =
            static_cast<UINT>(configuration.bottomLedCount);
        parameters.leftCount =
            static_cast<UINT>(configuration.leftLedCount);

        parameters.samplingDepth = static_cast<UINT>(
            resolvedSamplingDepthPixels(
                logicalSize,
                {},
                configuration.samplingDepthPixels));
        parameters.depthSampleCount = static_cast<UINT>(
            adaptiveDepthSampleCount(
                static_cast<int>(parameters.samplingDepth)));
        parameters.probeSamples = kProbeSamplesPerLine;
        parameters.blackThreshold =
            configuration.blackLuminanceThreshold;
        const BlackBarSearchLimits searchLimits = blackBarSearchLimits(
            configuration.blackBarDetectionMode,
            logicalSize,
            configuration.maximumBlackBarFraction);
        parameters.maximumHorizontalInsetFraction =
            searchLimits.horizontalProbeFraction;
        parameters.maximumVerticalInsetFraction =
            searchLimits.verticalProbeFraction;
        parameters.regionColorMode =
            static_cast<UINT>(configuration.regionColorMode);
        parameters.ledTransferMode =
            static_cast<UINT>(configuration.ledTransferMode);
        return parameters;
    }

    [[nodiscard]] static UINT shaderRotation(
        const DXGI_MODE_ROTATION rotation) noexcept
    {
        switch (rotation) {
        case DXGI_MODE_ROTATION_ROTATE90:
            return 1;
        case DXGI_MODE_ROTATION_ROTATE180:
            return 2;
        case DXGI_MODE_ROTATION_ROTATE270:
            return 3;
        case DXGI_MODE_ROTATION_UNSPECIFIED:
        case DXGI_MODE_ROTATION_IDENTITY:
        default:
            return 0;
        }
    }

    [[nodiscard]] bool dispatch(
        const ShaderParameters& parameters,
        quint32* destination,
        const UINT resultCount,
        QString* errorMessage,
        HRESULT* nativeResult)
    {
        context_->UpdateSubresource(
            parameterBuffer_.Get(),
            0,
            nullptr,
            &parameters,
            0,
            0);

        ID3D11Buffer* parameterBuffer = parameterBuffer_.Get();
        ID3D11ShaderResourceView* sampleView = sampleView_.Get();
        ID3D11UnorderedAccessView* resultView = resultView_.Get();
        context_->CSSetShader(shader_.Get(), nullptr, 0);
        context_->CSSetConstantBuffers(0, 1, &parameterBuffer);
        context_->CSSetShaderResources(0, 1, &sampleView);
        context_->CSSetUnorderedAccessViews(0, 1, &resultView, nullptr);
        context_->Dispatch(
            (resultCount + kShaderThreads - 1U) / kShaderThreads,
            1,
            1);

        ID3D11ShaderResourceView* nullShaderResource = nullptr;
        ID3D11UnorderedAccessView* nullUnorderedAccess = nullptr;
        ID3D11Buffer* nullConstantBuffer = nullptr;
        context_->CSSetShaderResources(0, 1, &nullShaderResource);
        context_->CSSetUnorderedAccessViews(
            0,
            1,
            &nullUnorderedAccess,
            nullptr);
        context_->CSSetConstantBuffers(0, 1, &nullConstantBuffer);
        context_->CSSetShader(nullptr, nullptr, 0);

        context_->CopyResource(stagingBuffer_.Get(), resultBuffer_.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT result = context_->Map(
            stagingBuffer_.Get(),
            0,
            D3D11_MAP_READ,
            0,
            &mapped);
        if (FAILED(result)) {
            if (errorMessage != nullptr) {
                *errorMessage = QStringLiteral("GPU result readback failed: %1")
                                    .arg(hresultText(result));
            }
            if (nativeResult != nullptr) {
                *nativeResult = result;
            }
            return false;
        }

        std::memcpy(
            destination,
            mapped.pData,
            static_cast<std::size_t>(resultCount) * sizeof(quint32));
        context_->Unmap(stagingBuffer_.Get(), 0);
        return true;
    }

    CaptureOutput selectedOutput_;
    DXGI_OUTDUPL_DESC duplicationDescription_{};

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGIOutputDuplication> duplication_;
    ComPtr<ID3D11ComputeShader> shader_;
    ComPtr<ID3D11Buffer> parameterBuffer_;
    ComPtr<ID3D11Buffer> resultBuffer_;
    ComPtr<ID3D11UnorderedAccessView> resultView_;
    ComPtr<ID3D11Buffer> stagingBuffer_;
    ComPtr<ID3D11Texture2D> sampleTexture_;
    ComPtr<ID3D11ShaderResourceView> sampleView_;
    UINT sampleWidth_ = 0;
    UINT sampleHeight_ = 0;
    DXGI_FORMAT sampleFormat_ = DXGI_FORMAT_UNKNOWN;
};

} // namespace

class ScreenCaptureRuntime final : public QObject {
public:
    explicit ScreenCaptureRuntime(ScreenCaptureWorker* owner)
        : owner_(owner)
    {
    }

    void startCapture(const quint64 commandGeneration)
    {
        if (!adoptGeneration(commandGeneration)) {
            return;
        }
        if (runningRequested_) {
            return;
        }
        runningRequested_ = true;
        ensureTimer();
        resetStatistics();
        publishStatus(
            CaptureState::Starting,
            QStringLiteral("Starting desktop capture."));
        initializeBackend();
    }

    void stopCapture(const quint64 commandGeneration)
    {
        if (!adoptGeneration(commandGeneration)) {
            return;
        }
        runningRequested_ = false;
        if (timer_ != nullptr) {
            timer_->stop();
        }
        backend_.reset();
        detector_.reset();
        selectedOutputId_.clear();
        selectedOutputLegacyId_.clear();
        resetStatistics();
        publishStatus(
            CaptureState::Stopped,
            QStringLiteral("Desktop capture stopped."));
    }

    void updateConfiguration(
        const CaptureConfiguration& configuration,
        const quint64 commandGeneration)
    {
        if (!adoptGeneration(commandGeneration)) {
            return;
        }
        QString validationError;
        if (!configuration.isValid(&validationError)) {
            publishStatus(
                CaptureState::Error,
                QStringLiteral("Capture configuration rejected: %1")
                    .arg(validationError));
            return;
        }

        bool outputChanged =
            configuration.outputId.compare(
                configuration_.outputId,
                Qt::CaseInsensitive)
            != 0;
        if (runningRequested_
            && (outputChanged
                || configuration.outputId.trimmed().isEmpty())) {
            const QVector<CaptureOutput> outputs =
                enumerateDxgiOutputs(nullptr);
            postOutputs(outputs);

            if (configuration.outputId.trimmed().isEmpty()) {
                outputChanged = automaticCaptureOutputChanged(
                    outputs,
                    selectedOutputId_,
                    selectedOutputLegacyId_);
            } else if (captureOutputIdEquals(
                           configuration.outputId,
                           selectedOutputId_)
                || captureOutputIdEquals(
                    configuration.outputId,
                    selectedOutputLegacyId_)) {
                // The UI may migrate a legacy DISPLAYn selection to its
                // stable target identity while this backend is already using
                // that same output.
                outputChanged = false;
            } else if (const CaptureOutput* desired =
                           resolveCaptureOutput(
                               outputs,
                               configuration.outputId);
                       desired != nullptr
                       && captureOutputIdentityEquals(
                           *desired,
                           selectedOutputId_,
                           selectedOutputLegacyId_)) {
                outputChanged = false;
            }
        }
        const bool detectorChanged =
            configuration.blackBarDetectionMode
                != configuration_.blackBarDetectionMode
            || configuration.blackLuminanceThreshold
                != configuration_.blackLuminanceThreshold
            || configuration.maximumBlackBarFraction
                != configuration_.maximumBlackBarFraction;

        configuration_ = configuration;
        if (detectorChanged) {
            detector_.reset();
        }

        if (runningRequested_ && outputChanged) {
            if (timer_ != nullptr) {
                timer_->stop();
            }
            backend_.reset();
            selectedOutputId_.clear();
            selectedOutputLegacyId_.clear();
            publishStatus(
                CaptureState::Reinitializing,
                QStringLiteral("Capture output changed; reinitializing."));
            initializeBackend();
        }
    }

private:
    [[nodiscard]] bool adoptGeneration(
        const quint64 commandGeneration) noexcept
    {
        if (commandGeneration < configurationGeneration_) {
            return false;
        }
        configurationGeneration_ = commandGeneration;
        return true;
    }

    void ensureTimer()
    {
        if (timer_ != nullptr) {
            return;
        }
        timer_ = new QTimer(this);
        timer_->setSingleShot(true);
        timer_->setTimerType(Qt::PreciseTimer);
        connect(timer_, &QTimer::timeout, this, [this] {
            captureTick();
        });
    }

    void initializeBackend()
    {
        if (!runningRequested_) {
            return;
        }

        postOutputs(enumerateDxgiOutputs(nullptr));

        CaptureOutput selectedOutput;
        QString errorMessage;
        HRESULT result = S_OK;
        if (!backend_.initialize(
                configuration_.outputId,
                &selectedOutput,
                &errorMessage,
                &result)) {
            selectedOutputId_.clear();
            selectedOutputLegacyId_.clear();
            publishStatus(
                CaptureState::Error,
                errorMessage,
                result);
            schedule(kRetryDelayMs);
            return;
        }

        selectedOutputId_ = selectedOutput.id;
        selectedOutputLegacyId_ = selectedOutput.legacyId;
        detector_.reset();
        resetStatistics();
        lastFrameClock_.start();
        publishStatus(
            CaptureState::Running,
            QStringLiteral("Capturing %1.").arg(selectedOutput.label));
        schedule(0);
    }

    void captureTick()
    {
        if (!runningRequested_) {
            return;
        }

        if (!pacingClock_.isValid()) {
            pacingClock_.start();
        }
        const qint64 frameStartedNanoseconds = pacingClock_.nsecsElapsed();

        if (selectedOutputId_.isEmpty()) {
            publishStatus(
                CaptureState::Reinitializing,
                QStringLiteral("Retrying desktop capture initialization."));
            initializeBackend();
            return;
        }

        QElapsedTimer processingClock;
        processingClock.start();
        const bool runProbe =
            (frameNumber_ % kProbeIntervalFrames) == 0U;
        CapturedFrame captured =
            backend_.capture(configuration_, detector_, runProbe);

        if (captured.result == FrameResult::WaitTimeout) {
            if (lastFrameClock_.isValid()
                && lastFrameClock_.elapsed() >= kWaitingStatusDelayMs) {
                publishStatus(
                    CaptureState::WaitingForFrame,
                    QStringLiteral("Waiting for a new desktop frame."));
            }
            schedule(nextFrameDelay(frameStartedNanoseconds));
            return;
        }

        if (captured.result == FrameResult::AccessLost) {
            backend_.reset();
            selectedOutputId_.clear();
            selectedOutputLegacyId_.clear();
            detector_.reset();
            publishStatus(
                CaptureState::Reinitializing,
                captured.errorMessage,
                captured.error);
            schedule(kRetryDelayMs);
            return;
        }

        if (captured.result == FrameResult::Failed) {
            backend_.reset();
            selectedOutputId_.clear();
            selectedOutputLegacyId_.clear();
            detector_.reset();
            publishStatus(
                CaptureState::Error,
                captured.errorMessage,
                captured.error);
            schedule(kRetryDelayMs);
            return;
        }

        lastFrameClock_.restart();
        ++frameNumber_;
        updateFps();

        CaptureSample sample;
        sample.colors = std::move(captured.colors);
        sample.samplingLayout = configuration_.samplingLayout();
        sample.configurationGeneration = configurationGeneration_;
        sample.frameSize = captured.frameSize;
        sample.contentInsets = captured.contentInsets;
        sample.framesPerSecond = measuredFps_;
        sample.processingMilliseconds =
            static_cast<double>(processingClock.nsecsElapsed())
            / 1'000'000.0;
        sample.frameNumber = frameNumber_;
        sample.capturedAtMilliseconds =
            QDateTime::currentMSecsSinceEpoch();
        postSample(std::move(sample));
        publishStatus(
            CaptureState::Running,
            QStringLiteral("Desktop capture is running."));

        schedule(nextFrameDelay(frameStartedNanoseconds));
    }

    void updateFps()
    {
        if (!fpsClock_.isValid()) {
            fpsClock_.start();
            framesSinceFpsUpdate_ = 0;
        }
        ++framesSinceFpsUpdate_;
        const qint64 elapsed = fpsClock_.elapsed();
        if (elapsed >= 500) {
            measuredFps_ =
                static_cast<double>(framesSinceFpsUpdate_) * 1'000.0
                / static_cast<double>(elapsed);
            fpsClock_.restart();
            framesSinceFpsUpdate_ = 0;
        }
    }

    [[nodiscard]] int nextFrameDelay(const qint64 frameStartedNanoseconds) noexcept
    {
        constexpr qint64 nanosecondsPerSecond = 1'000'000'000;
        constexpr qint64 nanosecondsPerMillisecond = 1'000'000;
        const int targetFps = std::max(1, configuration_.targetFps);

        if (pacingTargetFps_ != targetFps
            || nextFrameDeadlineNanoseconds_ <= 0) {
            pacingTargetFps_ = targetFps;
            nextFrameDeadlineNanoseconds_ = frameStartedNanoseconds;
            framePeriodRemainder_ = 0;
        }

        qint64 framePeriodNanoseconds =
            nanosecondsPerSecond / static_cast<qint64>(targetFps);
        framePeriodRemainder_ +=
            nanosecondsPerSecond % static_cast<qint64>(targetFps);
        if (framePeriodRemainder_ >= targetFps) {
            ++framePeriodNanoseconds;
            framePeriodRemainder_ -= targetFps;
        }

        if (frameStartedNanoseconds - nextFrameDeadlineNanoseconds_
            > framePeriodNanoseconds) {
            // Do not issue a burst of catch-up captures after a debugger,
            // modal dialog, or an overloaded system delayed this worker.
            nextFrameDeadlineNanoseconds_ = frameStartedNanoseconds;
        }
        nextFrameDeadlineNanoseconds_ += framePeriodNanoseconds;

        const qint64 remainingNanoseconds =
            nextFrameDeadlineNanoseconds_ - pacingClock_.nsecsElapsed();
        if (remainingNanoseconds <= 0) {
            return 0;
        }
        return static_cast<int>(std::clamp<qint64>(
            (remainingNanoseconds + nanosecondsPerMillisecond - 1)
                / nanosecondsPerMillisecond,
            0,
            1'000));
    }

    void schedule(const int delayMs)
    {
        if (runningRequested_ && timer_ != nullptr) {
            timer_->start(std::max(0, delayMs));
        }
    }

    void resetStatistics() noexcept
    {
        frameNumber_ = 0;
        framesSinceFpsUpdate_ = 0;
        measuredFps_ = 0.0;
        fpsClock_.invalidate();
        lastFrameClock_.invalidate();
        pacingClock_.start();
        nextFrameDeadlineNanoseconds_ = 0;
        framePeriodRemainder_ = 0;
        pacingTargetFps_ = 0;
    }

    void publishStatus(
        const CaptureState state,
        const QString& message,
        const HRESULT result = S_OK)
    {
        CaptureStatus status;
        status.state = state;
        status.outputId = selectedOutputId_;
        status.message = message;
        status.nativeError =
            (result == S_OK) ? 0 : nativeError(result);
        if (status == lastStatus_
            && lastStatusGeneration_ == configurationGeneration_) {
            return;
        }
        lastStatus_ = status;
        lastStatusGeneration_ = configurationGeneration_;

        const QPointer<ScreenCaptureWorker> owner = owner_;
        const quint64 generation = configurationGeneration_;
        if (owner == nullptr) {
            return;
        }
        QMetaObject::invokeMethod(
            owner_.data(),
            [owner, generation, status = std::move(status)] {
                if (owner != nullptr
                    && owner->commandGeneration_.load(
                           std::memory_order_relaxed)
                        == generation) {
                    emit owner->statusChanged(status);
                }
            },
            Qt::QueuedConnection);
    }

    void postSample(CaptureSample sample)
    {
        const QPointer<ScreenCaptureWorker> owner = owner_;
        if (owner == nullptr) {
            return;
        }

        bool scheduleDelivery = false;
        {
            const QMutexLocker locker(&owner->pendingSampleMutex_);
            if (owner->commandGeneration_.load(std::memory_order_relaxed)
                != sample.configurationGeneration) {
                return;
            }
            owner->pendingSample_ = std::move(sample);
            if (!owner->sampleDeliveryScheduled_) {
                owner->sampleDeliveryScheduled_ = true;
                scheduleDelivery = true;
            }
        }
        if (!scheduleDelivery) {
            return;
        }

        QMetaObject::invokeMethod(
            owner_.data(),
            [owner] {
                if (owner == nullptr) {
                    return;
                }

                std::optional<CaptureSample> sample;
                {
                    const QMutexLocker locker(&owner->pendingSampleMutex_);
                    sample = std::move(owner->pendingSample_);
                    owner->pendingSample_.reset();
                    owner->sampleDeliveryScheduled_ = false;
                }
                if (sample.has_value()
                    && owner->commandGeneration_.load(
                           std::memory_order_relaxed)
                        == sample->configurationGeneration) {
                    emit owner->sampleReady(*sample);
                }
            },
            Qt::QueuedConnection);
    }

    void postOutputs(QVector<CaptureOutput> outputs)
    {
        const QPointer<ScreenCaptureWorker> owner = owner_;
        if (owner == nullptr) {
            return;
        }
        QMetaObject::invokeMethod(
            owner_.data(),
            [owner, outputs = std::move(outputs)] {
                if (owner != nullptr) {
                    emit owner->outputsChanged(outputs);
                }
            },
            Qt::QueuedConnection);
    }

    QPointer<ScreenCaptureWorker> owner_;
    QTimer* timer_ = nullptr;
    DxgiCaptureBackend backend_;
    BlackBarDetector detector_;
    CaptureConfiguration configuration_;
    quint64 configurationGeneration_ = 1;
    QString selectedOutputId_;
    QString selectedOutputLegacyId_;
    bool runningRequested_ = false;

    CaptureStatus lastStatus_;
    quint64 lastStatusGeneration_ = 0;
    QElapsedTimer fpsClock_;
    QElapsedTimer lastFrameClock_;
    QElapsedTimer pacingClock_;
    quint64 frameNumber_ = 0;
    int framesSinceFpsUpdate_ = 0;
    qint64 nextFrameDeadlineNanoseconds_ = 0;
    qint64 framePeriodRemainder_ = 0;
    int pacingTargetFps_ = 0;
    double measuredFps_ = 0.0;
};

ScreenCaptureWorker::ScreenCaptureWorker(QObject* parent)
    : QObject(parent),
      runtime_(new ScreenCaptureRuntime(this))
{
    qRegisterMetaType<CaptureOutput>();
    qRegisterMetaType<QVector<CaptureOutput>>();
    qRegisterMetaType<CaptureInsets>();
    qRegisterMetaType<SamplingLayout>();
    qRegisterMetaType<CaptureConfiguration>();
    qRegisterMetaType<CaptureSample>();
    qRegisterMetaType<CaptureState>();
    qRegisterMetaType<CaptureStatus>();

    captureThread_.setObjectName(QStringLiteral("AmbilightScreenCapture"));
    runtime_->moveToThread(&captureThread_);
    connect(
        &captureThread_,
        &QThread::finished,
        runtime_,
        &QObject::deleteLater);
    captureThread_.start();
}

ScreenCaptureWorker::~ScreenCaptureWorker()
{
    if (runtime_ != nullptr && captureThread_.isRunning()) {
        const quint64 generation =
            commandGeneration_.fetch_add(1, std::memory_order_relaxed) + 1;
        QMetaObject::invokeMethod(
            runtime_,
            [runtime = runtime_, generation] {
                runtime->stopCapture(generation);
            },
            Qt::BlockingQueuedConnection);
    }
    captureThread_.quit();
    captureThread_.wait();
    runtime_ = nullptr;
}

QVector<CaptureOutput> ScreenCaptureWorker::enumerateOutputs(
    QString* errorMessage)
{
    return enumerateDxgiOutputs(errorMessage);
}

void ScreenCaptureWorker::start()
{
    if (runtime_ == nullptr || !captureThread_.isRunning()) {
        return;
    }
    const quint64 generation =
        commandGeneration_.fetch_add(1, std::memory_order_relaxed) + 1;
    QMetaObject::invokeMethod(
        runtime_,
        [runtime = runtime_, generation] {
            runtime->startCapture(generation);
        },
        Qt::QueuedConnection);
}

void ScreenCaptureWorker::stop()
{
    if (runtime_ == nullptr || !captureThread_.isRunning()) {
        return;
    }
    const quint64 generation =
        commandGeneration_.fetch_add(1, std::memory_order_relaxed) + 1;
    if (QThread::currentThread() == &captureThread_) {
        runtime_->stopCapture(generation);
        return;
    }
    QMetaObject::invokeMethod(
        runtime_,
        [runtime = runtime_, generation] {
            runtime->stopCapture(generation);
        },
        Qt::BlockingQueuedConnection);
}

void ScreenCaptureWorker::updateConfiguration(
    const CaptureConfiguration& configuration)
{
    if (runtime_ == nullptr || !captureThread_.isRunning()) {
        return;
    }
    const quint64 generation =
        commandGeneration_.fetch_add(1, std::memory_order_relaxed) + 1;
    QMetaObject::invokeMethod(
        runtime_,
        [runtime = runtime_, configuration, generation] {
            runtime->updateConfiguration(configuration, generation);
        },
        Qt::QueuedConnection);
}

} // namespace ambilight::capture
