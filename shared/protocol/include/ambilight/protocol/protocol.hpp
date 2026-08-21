#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ambilight::protocol {

using Byte = std::uint8_t;

inline constexpr std::array<Byte, 4> kMagic{0x41, 0x4C, 0x42, 0x31};  // ALB1
inline constexpr Byte kProtocolVersion = 2;
inline constexpr std::size_t kDecodedHeaderSize = 16;
inline constexpr std::size_t kDecodedCrcSize = 4;
inline constexpr std::size_t kMaxPayloadSize = 1024;
inline constexpr std::size_t kMaxDecodedPacketSize =
    kDecodedHeaderSize + kMaxPayloadSize + kDecodedCrcSize;
inline constexpr std::size_t kMaxCobsEncodedSize =
    kMaxDecodedPacketSize + (kMaxDecodedPacketSize / 254U) + 1U;
inline constexpr std::size_t kMaxWirePacketSize =
    kMaxCobsEncodedSize + 1U;  // COBS frame plus zero delimiter.

enum class MessageType : Byte {
    Hello = 0x01,
    DeviceInfo = 0x02,
    Ping = 0x03,
    Pong = 0x04,
    GetState = 0x05,
    State = 0x06,

    SetStripConfig = 0x10,
    SetMode = 0x11,
    SetBrightness = 0x12,
    SetEffect = 0x13,
    SaveConfig = 0x14,

    LedFrame = 0x20,

    Ack = 0x70,
    Error = 0x71,
};

enum PacketFlag : std::uint16_t {
    PacketFlagNone = 0,
    PacketFlagAckRequested = 1U << 0U,
    PacketFlagPersist = 1U << 1U,
};

enum class OperatingMode : Byte {
    Off = 0,
    Ambilight = 1,
    Solid = 2,
    Effect = 3,
};

enum class EffectType : Byte {
    Solid = 0,
    Breathing = 1,
    Wave = 2,
    Rainbow = 3,
};

enum class ColorOrder : Byte {
    Rgb = 0,
    Rbg = 1,
    Grb = 2,
    Gbr = 3,
    Brg = 4,
    Bgr = 5,
};

enum class PixelEncoding : Byte {
    Rgb8 = 0,
};

enum class ErrorCode : std::uint16_t {
    None = 0,
    UnsupportedVersion = 1,
    UnsupportedMessage = 2,
    InvalidPayload = 3,
    InvalidState = 4,
    Busy = 5,
    Internal = 6,
};

enum Capability : std::uint32_t {
    CapabilityNone = 0,
    CapabilityRgb8Frames = 1U << 0U,
    CapabilityStandaloneEffects = 1U << 1U,
    CapabilityPersistentConfig = 1U << 2U,
    CapabilityCurrentLimit = 1U << 3U,
    CapabilityAmbientLease = 1U << 4U,
};

struct PacketView {
    Byte version{kProtocolVersion};
    MessageType type{};
    std::uint16_t flags{};
    std::uint32_t sequence{};
    std::span<const Byte> payload{};
};

struct DecoderStats {
    std::uint32_t decodedPackets{};
    std::uint32_t crcErrors{};
    std::uint32_t cobsErrors{};
    std::uint32_t formatErrors{};
    std::uint32_t discardedBytes{};
};

struct EncodeResult {
    bool ok{};
    std::size_t bytesWritten{};
};

struct StripConfig {
    std::uint16_t ledCount{300};
    std::uint16_t topCount{96};
    std::uint16_t rightCount{54};
    std::uint16_t bottomCount{96};
    std::uint16_t leftCount{54};
    std::uint16_t startOffset{};
    bool reversed{};
    ColorOrder colorOrder{ColorOrder::Rgb};
    Byte brightnessLimit{128};
    std::uint16_t currentLimitMilliamps{5000};
    std::uint16_t ambientTimeoutMilliseconds{1000};
    Byte redCalibration{255};
    Byte greenCalibration{255};
    Byte blueCalibration{255};
};

struct LedFrameView {
    bool valid{};
    std::uint16_t ledCount{};
    PixelEncoding encoding{PixelEncoding::Rgb8};
    std::span<const Byte> pixels{};
};

struct DeviceInfo {
    Byte firmwareMajor{};
    Byte firmwareMinor{1};
    Byte firmwarePatch{};
    Byte protocolVersion{kProtocolVersion};
    std::uint16_t maxLedCount{300};
    std::uint16_t configuredLedCount{300};
    std::uint32_t capabilities{
        CapabilityRgb8Frames
        | CapabilityStandaloneEffects
        | CapabilityPersistentConfig
        | CapabilityCurrentLimit
        | CapabilityAmbientLease};
    std::uint32_t uptimeMilliseconds{};
};

struct EffectConfig {
    EffectType effect{EffectType::Breathing};
    Byte intensity{255};
    Byte red{255};
    Byte green{160};
    Byte blue{80};
    Byte reserved{};
    std::uint16_t speedPermille{1000};
};

struct AckPayload {
    MessageType acknowledgedType{MessageType::Hello};
    Byte status{};
};

struct ErrorPayload {
    ErrorCode code{ErrorCode::Internal};
    MessageType failedType{MessageType::Hello};
    Byte detail{};
};

enum StateFlag : Byte {
    StateFlagNone = 0,
    StateFlagOutputActive = 1U << 0U,
    StateFlagAmbientLeaseActive = 1U << 1U,
    StateFlagConfigDirty = 1U << 2U,
};

struct StateSnapshot {
    OperatingMode mode{OperatingMode::Off};
    EffectType effect{EffectType::Breathing};
    Byte brightness{128};
    Byte flags{StateFlagNone};
    std::uint16_t ledCount{300};
    std::uint16_t currentLimitMilliamps{5000};
    std::uint32_t lastReceivedSequence{};
    std::uint32_t lastAppliedSequence{};
    std::uint32_t crcErrors{};
    std::uint32_t droppedFrames{};
    std::uint32_t uptimeMilliseconds{};
};

inline constexpr std::size_t kStripConfigPayloadSize = 24;
inline constexpr std::size_t kLedFramePreambleSize = 4;
inline constexpr std::size_t kDeviceInfoPayloadSize = 16;
inline constexpr std::size_t kModePayloadSize = 1;
inline constexpr std::size_t kBrightnessPayloadSize = 1;
inline constexpr std::size_t kEffectConfigPayloadSize = 8;
inline constexpr std::size_t kAckPayloadSize = 4;
inline constexpr std::size_t kErrorPayloadSize = 4;
inline constexpr std::size_t kStatePayloadSize = 28;

[[nodiscard]] constexpr bool isValidStripConfig(
    const StripConfig& config) noexcept
{
    const std::uint32_t perimeter =
        static_cast<std::uint32_t>(config.topCount)
        + config.rightCount
        + config.bottomCount
        + config.leftCount;
    return config.ledCount > 0
        && config.ledCount <= 300
        && perimeter == config.ledCount
        && config.startOffset < config.ledCount
        && static_cast<Byte>(config.colorOrder)
            <= static_cast<Byte>(ColorOrder::Bgr)
        && config.currentLimitMilliamps > 0
        && config.currentLimitMilliamps <= 10000
        && config.ambientTimeoutMilliseconds >= 100
        && config.ambientTimeoutMilliseconds <= 60000;
}

[[nodiscard]] constexpr bool isValidStateSnapshot(
    const StateSnapshot& state) noexcept
{
    constexpr Byte knownFlags =
        StateFlagOutputActive
        | StateFlagAmbientLeaseActive
        | StateFlagConfigDirty;
    return static_cast<Byte>(state.mode)
            <= static_cast<Byte>(OperatingMode::Effect)
        && static_cast<Byte>(state.effect)
            <= static_cast<Byte>(EffectType::Rainbow)
        && (state.flags & static_cast<Byte>(~knownFlags)) == 0
        && state.ledCount > 0
        && state.ledCount <= 300
        && state.currentLimitMilliamps > 0
        && state.currentLimitMilliamps <= 10000;
}

[[nodiscard]] constexpr std::uint16_t readU16Le(const Byte* data) noexcept
{
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(data[0])
        | (static_cast<std::uint16_t>(data[1]) << 8U));
}

[[nodiscard]] constexpr std::uint32_t readU32Le(const Byte* data) noexcept
{
    return static_cast<std::uint32_t>(data[0])
        | (static_cast<std::uint32_t>(data[1]) << 8U)
        | (static_cast<std::uint32_t>(data[2]) << 16U)
        | (static_cast<std::uint32_t>(data[3]) << 24U);
}

constexpr void writeU16Le(Byte* data, const std::uint16_t value) noexcept
{
    data[0] = static_cast<Byte>(value & 0xFFU);
    data[1] = static_cast<Byte>((value >> 8U) & 0xFFU);
}

constexpr void writeU32Le(Byte* data, const std::uint32_t value) noexcept
{
    data[0] = static_cast<Byte>(value & 0xFFU);
    data[1] = static_cast<Byte>((value >> 8U) & 0xFFU);
    data[2] = static_cast<Byte>((value >> 16U) & 0xFFU);
    data[3] = static_cast<Byte>((value >> 24U) & 0xFFU);
}

[[nodiscard]] constexpr bool isKnownMessageType(
    const MessageType type) noexcept
{
    switch (type) {
    case MessageType::Hello:
    case MessageType::DeviceInfo:
    case MessageType::Ping:
    case MessageType::Pong:
    case MessageType::GetState:
    case MessageType::State:
    case MessageType::SetStripConfig:
    case MessageType::SetMode:
    case MessageType::SetBrightness:
    case MessageType::SetEffect:
    case MessageType::SaveConfig:
    case MessageType::LedFrame:
    case MessageType::Ack:
    case MessageType::Error:
        return true;
    }
    return false;
}

[[nodiscard]] constexpr std::uint32_t crc32c(
    const std::span<const Byte> bytes) noexcept
{
    std::uint32_t crc = 0xFFFFFFFFU;
    for (const Byte value : bytes) {
        crc ^= value;
        for (unsigned bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask =
                static_cast<std::uint32_t>(-static_cast<std::int32_t>(crc & 1U));
            crc = (crc >> 1U) ^ (0x82F63B78U & mask);
        }
    }
    return ~crc;
}

[[nodiscard]] inline EncodeResult cobsEncode(
    const std::span<const Byte> input,
    const std::span<Byte> output) noexcept
{
    if (output.empty()) {
        return {};
    }

    std::size_t readIndex = 0;
    std::size_t writeIndex = 1;
    std::size_t codeIndex = 0;
    Byte code = 1;

    while (readIndex < input.size()) {
        if (input[readIndex] == 0) {
            if (codeIndex >= output.size()) {
                return {};
            }
            output[codeIndex] = code;
            codeIndex = writeIndex;
            if (++writeIndex > output.size()) {
                return {};
            }
            code = 1;
            ++readIndex;
            continue;
        }

        if (writeIndex >= output.size()) {
            return {};
        }
        output[writeIndex++] = input[readIndex++];
        ++code;

        if (code == 0xFFU) {
            output[codeIndex] = code;
            codeIndex = writeIndex;
            if (++writeIndex > output.size()) {
                return {};
            }
            code = 1;
        }
    }

    if (codeIndex >= output.size()) {
        return {};
    }
    output[codeIndex] = code;
    return {true, writeIndex};
}

[[nodiscard]] inline EncodeResult cobsDecode(
    const std::span<const Byte> input,
    const std::span<Byte> output) noexcept
{
    std::size_t readIndex = 0;
    std::size_t writeIndex = 0;

    while (readIndex < input.size()) {
        const Byte code = input[readIndex++];
        if (code == 0) {
            return {};
        }

        const std::size_t copyCount = static_cast<std::size_t>(code) - 1U;
        if (readIndex + copyCount > input.size()
            || writeIndex + copyCount > output.size()) {
            return {};
        }

        for (std::size_t index = 0; index < copyCount; ++index) {
            if (input[readIndex] == 0) {
                return {};
            }
            output[writeIndex++] = input[readIndex++];
        }

        if (code != 0xFFU && readIndex < input.size()) {
            if (writeIndex >= output.size()) {
                return {};
            }
            output[writeIndex++] = 0;
        }
    }

    return {true, writeIndex};
}

[[nodiscard]] inline EncodeResult encodePacket(
    const MessageType type,
    const std::uint16_t flags,
    const std::uint32_t sequence,
    const std::span<const Byte> payload,
    const std::span<Byte> output) noexcept
{
    if (!isKnownMessageType(type) || payload.size() > kMaxPayloadSize) {
        return {};
    }

    std::array<Byte, kMaxDecodedPacketSize> decoded{};
    for (std::size_t index = 0; index < kMagic.size(); ++index) {
        decoded[index] = kMagic[index];
    }
    decoded[4] = kProtocolVersion;
    decoded[5] = static_cast<Byte>(type);
    writeU16Le(decoded.data() + 6, flags);
    writeU32Le(decoded.data() + 8, sequence);
    writeU16Le(
        decoded.data() + 12,
        static_cast<std::uint16_t>(payload.size()));
    writeU16Le(decoded.data() + 14, 0);

    for (std::size_t index = 0; index < payload.size(); ++index) {
        decoded[kDecodedHeaderSize + index] = payload[index];
    }

    const std::size_t crcOffset = kDecodedHeaderSize + payload.size();
    writeU32Le(
        decoded.data() + crcOffset,
        crc32c(std::span<const Byte>(decoded.data(), crcOffset)));

    if (output.size() < 2) {
        return {};
    }
    const auto encoded = cobsEncode(
        std::span<const Byte>(
            decoded.data(),
            crcOffset + kDecodedCrcSize),
        output.first(output.size() - 1U));
    if (!encoded.ok || encoded.bytesWritten >= output.size()) {
        return {};
    }

    output[encoded.bytesWritten] = 0;
    return {true, encoded.bytesWritten + 1U};
}

[[nodiscard]] inline bool encodeStripConfig(
    const StripConfig& config,
    const std::span<Byte> output) noexcept
{
    if (output.size() < kStripConfigPayloadSize
        || !isValidStripConfig(config)) {
        return false;
    }

    writeU16Le(output.data() + 0, config.ledCount);
    writeU16Le(output.data() + 2, config.topCount);
    writeU16Le(output.data() + 4, config.rightCount);
    writeU16Le(output.data() + 6, config.bottomCount);
    writeU16Le(output.data() + 8, config.leftCount);
    writeU16Le(output.data() + 10, config.startOffset);
    output[12] = config.reversed ? 1U : 0U;
    output[13] = static_cast<Byte>(config.colorOrder);
    output[14] = config.brightnessLimit;
    output[15] = 0;
    writeU16Le(output.data() + 16, config.currentLimitMilliamps);
    writeU16Le(output.data() + 18, config.ambientTimeoutMilliseconds);
    output[20] = config.redCalibration;
    output[21] = config.greenCalibration;
    output[22] = config.blueCalibration;
    output[23] = 0;
    return true;
}

[[nodiscard]] inline bool decodeStripConfig(
    const std::span<const Byte> payload,
    StripConfig& config) noexcept
{
    if (payload.size() != kStripConfigPayloadSize) {
        return false;
    }

    StripConfig decoded;
    decoded.ledCount = readU16Le(payload.data() + 0);
    decoded.topCount = readU16Le(payload.data() + 2);
    decoded.rightCount = readU16Le(payload.data() + 4);
    decoded.bottomCount = readU16Le(payload.data() + 6);
    decoded.leftCount = readU16Le(payload.data() + 8);
    decoded.startOffset = readU16Le(payload.data() + 10);
    decoded.reversed = payload[12] == 1;
    decoded.colorOrder = static_cast<ColorOrder>(payload[13]);
    decoded.brightnessLimit = payload[14];
    decoded.currentLimitMilliamps = readU16Le(payload.data() + 16);
    decoded.ambientTimeoutMilliseconds = readU16Le(payload.data() + 18);
    decoded.redCalibration = payload[20];
    decoded.greenCalibration = payload[21];
    decoded.blueCalibration = payload[22];

    if (payload[12] > 1
        || payload[15] != 0
        || payload[23] != 0
        || !isValidStripConfig(decoded)) {
        return false;
    }

    config = decoded;
    return true;
}

[[nodiscard]] inline bool encodeDeviceInfo(
    const DeviceInfo& info,
    const std::span<Byte> output) noexcept
{
    if (output.size() < kDeviceInfoPayloadSize
        || info.maxLedCount == 0
        || info.configuredLedCount > info.maxLedCount) {
        return false;
    }

    output[0] = info.firmwareMajor;
    output[1] = info.firmwareMinor;
    output[2] = info.firmwarePatch;
    output[3] = info.protocolVersion;
    writeU16Le(output.data() + 4, info.maxLedCount);
    writeU16Le(output.data() + 6, info.configuredLedCount);
    writeU32Le(output.data() + 8, info.capabilities);
    writeU32Le(output.data() + 12, info.uptimeMilliseconds);
    return true;
}

[[nodiscard]] inline bool decodeDeviceInfo(
    const std::span<const Byte> payload,
    DeviceInfo& info) noexcept
{
    if (payload.size() != kDeviceInfoPayloadSize) {
        return false;
    }

    DeviceInfo decoded;
    decoded.firmwareMajor = payload[0];
    decoded.firmwareMinor = payload[1];
    decoded.firmwarePatch = payload[2];
    decoded.protocolVersion = payload[3];
    decoded.maxLedCount = readU16Le(payload.data() + 4);
    decoded.configuredLedCount = readU16Le(payload.data() + 6);
    decoded.capabilities = readU32Le(payload.data() + 8);
    decoded.uptimeMilliseconds = readU32Le(payload.data() + 12);
    if (decoded.protocolVersion != kProtocolVersion
        || decoded.maxLedCount == 0
        || decoded.configuredLedCount > decoded.maxLedCount) {
        return false;
    }
    info = decoded;
    return true;
}

[[nodiscard]] inline bool encodeOperatingMode(
    const OperatingMode mode,
    const std::span<Byte> output) noexcept
{
    if (output.size() < kModePayloadSize
        || static_cast<Byte>(mode)
            > static_cast<Byte>(OperatingMode::Effect)) {
        return false;
    }
    output[0] = static_cast<Byte>(mode);
    return true;
}

[[nodiscard]] inline bool decodeOperatingMode(
    const std::span<const Byte> payload,
    OperatingMode& mode) noexcept
{
    if (payload.size() != kModePayloadSize
        || payload[0] > static_cast<Byte>(OperatingMode::Effect)) {
        return false;
    }
    mode = static_cast<OperatingMode>(payload[0]);
    return true;
}

[[nodiscard]] inline bool encodeBrightness(
    const Byte brightness,
    const std::span<Byte> output) noexcept
{
    if (output.size() < kBrightnessPayloadSize) {
        return false;
    }
    output[0] = brightness;
    return true;
}

[[nodiscard]] inline bool decodeBrightness(
    const std::span<const Byte> payload,
    Byte& brightness) noexcept
{
    if (payload.size() != kBrightnessPayloadSize) {
        return false;
    }
    brightness = payload[0];
    return true;
}

[[nodiscard]] inline bool encodeEffectConfig(
    const EffectConfig& config,
    const std::span<Byte> output) noexcept
{
    if (output.size() < kEffectConfigPayloadSize
        || static_cast<Byte>(config.effect)
            > static_cast<Byte>(EffectType::Rainbow)
        || config.reserved != 0
        || config.speedPermille < 10
        || config.speedPermille > 10000) {
        return false;
    }

    output[0] = static_cast<Byte>(config.effect);
    output[1] = config.intensity;
    output[2] = config.red;
    output[3] = config.green;
    output[4] = config.blue;
    output[5] = 0;
    writeU16Le(output.data() + 6, config.speedPermille);
    return true;
}

[[nodiscard]] inline bool decodeEffectConfig(
    const std::span<const Byte> payload,
    EffectConfig& config) noexcept
{
    if (payload.size() != kEffectConfigPayloadSize
        || payload[0] > static_cast<Byte>(EffectType::Rainbow)
        || payload[5] != 0) {
        return false;
    }

    EffectConfig decoded;
    decoded.effect = static_cast<EffectType>(payload[0]);
    decoded.intensity = payload[1];
    decoded.red = payload[2];
    decoded.green = payload[3];
    decoded.blue = payload[4];
    decoded.speedPermille = readU16Le(payload.data() + 6);
    if (decoded.speedPermille < 10
        || decoded.speedPermille > 10000) {
        return false;
    }
    config = decoded;
    return true;
}

[[nodiscard]] inline bool encodeAck(
    const AckPayload& ack,
    const std::span<Byte> output) noexcept
{
    if (output.size() < kAckPayloadSize
        || !isKnownMessageType(ack.acknowledgedType)) {
        return false;
    }
    output[0] = static_cast<Byte>(ack.acknowledgedType);
    output[1] = ack.status;
    output[2] = 0;
    output[3] = 0;
    return true;
}

[[nodiscard]] inline bool decodeAck(
    const std::span<const Byte> payload,
    AckPayload& ack) noexcept
{
    if (payload.size() != kAckPayloadSize
        || payload[2] != 0
        || payload[3] != 0) {
        return false;
    }
    const auto type = static_cast<MessageType>(payload[0]);
    if (!isKnownMessageType(type)) {
        return false;
    }
    ack = {type, payload[1]};
    return true;
}

[[nodiscard]] inline bool encodeError(
    const ErrorPayload& error,
    const std::span<Byte> output) noexcept
{
    if (output.size() < kErrorPayloadSize
        || error.code == ErrorCode::None
        || static_cast<std::uint16_t>(error.code)
            > static_cast<std::uint16_t>(ErrorCode::Internal)) {
        return false;
    }
    writeU16Le(output.data(), static_cast<std::uint16_t>(error.code));
    output[2] = static_cast<Byte>(error.failedType);
    output[3] = error.detail;
    return true;
}

[[nodiscard]] inline bool decodeError(
    const std::span<const Byte> payload,
    ErrorPayload& error) noexcept
{
    if (payload.size() != kErrorPayloadSize) {
        return false;
    }
    const auto code = static_cast<ErrorCode>(readU16Le(payload.data()));
    const auto type = static_cast<MessageType>(payload[2]);
    if (code == ErrorCode::None
        || static_cast<std::uint16_t>(code)
            > static_cast<std::uint16_t>(ErrorCode::Internal)) {
        return false;
    }
    error = {code, type, payload[3]};
    return true;
}

[[nodiscard]] inline bool encodeState(
    const StateSnapshot& state,
    const std::span<Byte> output) noexcept
{
    if (output.size() < kStatePayloadSize
        || !isValidStateSnapshot(state)) {
        return false;
    }

    output[0] = static_cast<Byte>(state.mode);
    output[1] = static_cast<Byte>(state.effect);
    output[2] = state.brightness;
    output[3] = state.flags;
    writeU16Le(output.data() + 4, state.ledCount);
    writeU16Le(output.data() + 6, state.currentLimitMilliamps);
    writeU32Le(output.data() + 8, state.lastReceivedSequence);
    writeU32Le(output.data() + 12, state.lastAppliedSequence);
    writeU32Le(output.data() + 16, state.crcErrors);
    writeU32Le(output.data() + 20, state.droppedFrames);
    writeU32Le(output.data() + 24, state.uptimeMilliseconds);
    return true;
}

[[nodiscard]] inline bool decodeState(
    const std::span<const Byte> payload,
    StateSnapshot& state) noexcept
{
    if (payload.size() != kStatePayloadSize
        || payload[0] > static_cast<Byte>(OperatingMode::Effect)
        || payload[1] > static_cast<Byte>(EffectType::Rainbow)) {
        return false;
    }

    StateSnapshot decoded;
    decoded.mode = static_cast<OperatingMode>(payload[0]);
    decoded.effect = static_cast<EffectType>(payload[1]);
    decoded.brightness = payload[2];
    decoded.flags = payload[3];
    decoded.ledCount = readU16Le(payload.data() + 4);
    decoded.currentLimitMilliamps = readU16Le(payload.data() + 6);
    decoded.lastReceivedSequence = readU32Le(payload.data() + 8);
    decoded.lastAppliedSequence = readU32Le(payload.data() + 12);
    decoded.crcErrors = readU32Le(payload.data() + 16);
    decoded.droppedFrames = readU32Le(payload.data() + 20);
    decoded.uptimeMilliseconds = readU32Le(payload.data() + 24);
    if (!isValidStateSnapshot(decoded)) {
        return false;
    }
    state = decoded;
    return true;
}

[[nodiscard]] inline bool encodeLedFramePayload(
    const std::uint16_t ledCount,
    const std::span<const Byte> rgbPixels,
    const std::span<Byte> output,
    std::size_t& bytesWritten) noexcept
{
    const std::size_t pixelBytes = static_cast<std::size_t>(ledCount) * 3U;
    const std::size_t payloadSize = kLedFramePreambleSize + pixelBytes;
    if (ledCount == 0
        || ledCount > 300
        || rgbPixels.size() != pixelBytes
        || output.size() < payloadSize) {
        bytesWritten = 0;
        return false;
    }

    writeU16Le(output.data(), ledCount);
    output[2] = static_cast<Byte>(PixelEncoding::Rgb8);
    output[3] = 0;
    for (std::size_t index = 0; index < rgbPixels.size(); ++index) {
        output[kLedFramePreambleSize + index] = rgbPixels[index];
    }
    bytesWritten = payloadSize;
    return true;
}

[[nodiscard]] inline LedFrameView decodeLedFramePayload(
    const std::span<const Byte> payload) noexcept
{
    if (payload.size() < kLedFramePreambleSize) {
        return {};
    }

    const std::uint16_t ledCount = readU16Le(payload.data());
    const auto encoding = static_cast<PixelEncoding>(payload[2]);
    const std::size_t expectedPixelBytes =
        static_cast<std::size_t>(ledCount) * 3U;
    if (ledCount == 0
        || ledCount > 300
        || encoding != PixelEncoding::Rgb8
        || payload[3] != 0
        || payload.size() != kLedFramePreambleSize + expectedPixelBytes) {
        return {};
    }

    return {
        true,
        ledCount,
        encoding,
        payload.subspan(kLedFramePreambleSize),
    };
}

class StreamDecoder {
public:
    template <typename PacketHandler>
    void push(
        const std::span<const Byte> bytes,
        PacketHandler&& packetHandler)
    {
        auto&& handler = packetHandler;
        for (const Byte value : bytes) {
            consume(value, handler);
        }
    }

    void reset() noexcept
    {
        encodedSize_ = 0;
        droppingOversizeFrame_ = false;
    }

    [[nodiscard]] const DecoderStats& stats() const noexcept
    {
        return stats_;
    }

private:
    template <typename PacketHandler>
    void consume(const Byte value, PacketHandler& packetHandler)
    {
        if (value == 0) {
            if (droppingOversizeFrame_) {
                droppingOversizeFrame_ = false;
                encodedSize_ = 0;
                return;
            }
            if (encodedSize_ != 0) {
                finishFrame(packetHandler);
                encodedSize_ = 0;
            }
            return;
        }

        if (droppingOversizeFrame_) {
            ++stats_.discardedBytes;
            return;
        }

        if (encodedSize_ >= encoded_.size()) {
            ++stats_.formatErrors;
            stats_.discardedBytes +=
                static_cast<std::uint32_t>(encodedSize_ + 1U);
            encodedSize_ = 0;
            droppingOversizeFrame_ = true;
            return;
        }
        encoded_[encodedSize_++] = value;
    }

    template <typename PacketHandler>
    void finishFrame(PacketHandler& packetHandler)
    {
        const auto decoded = cobsDecode(
            std::span<const Byte>(encoded_.data(), encodedSize_),
            decoded_);
        if (!decoded.ok) {
            ++stats_.cobsErrors;
            return;
        }

        if (decoded.bytesWritten
            < kDecodedHeaderSize + kDecodedCrcSize) {
            ++stats_.formatErrors;
            return;
        }

        for (std::size_t index = 0; index < kMagic.size(); ++index) {
            if (decoded_[index] != kMagic[index]) {
                ++stats_.formatErrors;
                return;
            }
        }

        const auto type = static_cast<MessageType>(decoded_[5]);
        const std::size_t payloadSize = readU16Le(decoded_.data() + 12);
        const std::size_t expectedSize =
            kDecodedHeaderSize + payloadSize + kDecodedCrcSize;
        if (readU16Le(decoded_.data() + 14) != 0
            || payloadSize > kMaxPayloadSize
            || decoded.bytesWritten != expectedSize) {
            ++stats_.formatErrors;
            return;
        }

        const std::size_t crcOffset = kDecodedHeaderSize + payloadSize;
        const std::uint32_t receivedCrc =
            readU32Le(decoded_.data() + crcOffset);
        const std::uint32_t computedCrc =
            crc32c(std::span<const Byte>(decoded_.data(), crcOffset));
        if (receivedCrc != computedCrc) {
            ++stats_.crcErrors;
            return;
        }

        const PacketView packet{
            decoded_[4],
            type,
            readU16Le(decoded_.data() + 6),
            readU32Le(decoded_.data() + 8),
            std::span<const Byte>(
                decoded_.data() + kDecodedHeaderSize,
                payloadSize),
        };
        ++stats_.decodedPackets;
        packetHandler(packet);
    }

    std::array<Byte, kMaxCobsEncodedSize> encoded_{};
    std::array<Byte, kMaxDecodedPacketSize> decoded_{};
    std::size_t encodedSize_{};
    bool droppingOversizeFrame_{};
    DecoderStats stats_{};
};

}  // namespace ambilight::protocol
