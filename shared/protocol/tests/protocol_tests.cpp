#include <ambilight/protocol/protocol.hpp>

#include <array>
#include <cstdlib>
#include <iostream>
#include <span>
#include <vector>

namespace protocol = ambilight::protocol;

namespace {

void require(const bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void packetRoundTrip()
{
    constexpr std::array<protocol::Byte, 5> payload{
        0x00, 0x41, 0x4C, 0x7F, 0xFF};
    std::array<protocol::Byte, protocol::kMaxWirePacketSize> wire{};
    const auto encoded = protocol::encodePacket(
        protocol::MessageType::Ping,
        protocol::PacketFlagAckRequested,
        0x1234,
        payload,
        wire);
    require(encoded.ok, "packet encoding");

    protocol::StreamDecoder decoder;
    unsigned callbacks = 0;
    for (std::size_t index = 0; index < encoded.bytesWritten; ++index) {
        decoder.push(
            std::span<const protocol::Byte>(wire.data() + index, 1),
            [&](const protocol::PacketView packet) {
                ++callbacks;
                require(
                    packet.version == protocol::kProtocolVersion,
                    "protocol version");
                require(
                    packet.type == protocol::MessageType::Ping,
                    "message type");
                require(packet.sequence == 0x1234, "sequence");
                require(
                    packet.flags == protocol::PacketFlagAckRequested,
                    "packet flags");
                require(
                    packet.payload.size() == payload.size(),
                    "payload size");
                for (std::size_t byte = 0; byte < payload.size(); ++byte) {
                    require(
                        packet.payload[byte] == payload[byte],
                        "payload contents");
                }
            });
    }

    require(callbacks == 1, "one decoded callback");
    require(decoder.stats().decodedPackets == 1, "decoded packet statistics");
}

void unsupportedHeaderFieldsReachDispatcher()
{
    std::array<protocol::Byte, protocol::kMaxWirePacketSize> wire{};
    const auto originalSize = protocol::encodePacket(
        protocol::MessageType::Hello,
        protocol::PacketFlagNone,
        77,
        {},
        wire);
    require(originalSize.ok, "unsupported-header source packet");

    std::array<protocol::Byte, protocol::kMaxDecodedPacketSize> decoded{};
    const auto decodedSize = protocol::cobsDecode(
        std::span<const protocol::Byte>(
            wire.data(),
            originalSize.bytesWritten - 1U),
        decoded);
    require(decodedSize.ok, "unsupported-header COBS decode");
    decoded[4] = protocol::kProtocolVersion + 1U;
    decoded[5] = 0xFE;
    protocol::writeU32Le(
        decoded.data() + decodedSize.bytesWritten - protocol::kDecodedCrcSize,
        protocol::crc32c(std::span<const protocol::Byte>(
            decoded.data(),
            decodedSize.bytesWritten - protocol::kDecodedCrcSize)));

    const auto changedSize = protocol::cobsEncode(
        std::span<const protocol::Byte>(
            decoded.data(),
            decodedSize.bytesWritten),
        wire);
    require(changedSize.ok, "unsupported-header COBS encode");
    wire[changedSize.bytesWritten] = 0;

    protocol::StreamDecoder decoder;
    unsigned callbacks = 0;
    decoder.push(
        std::span<const protocol::Byte>(
            wire.data(),
            changedSize.bytesWritten + 1U),
        [&](const protocol::PacketView packet) {
            ++callbacks;
            require(
                packet.version == protocol::kProtocolVersion + 1U,
                "unsupported version forwarded");
            require(
                static_cast<protocol::Byte>(packet.type) == 0xFE,
                "unknown message type forwarded");
        });
    require(callbacks == 1, "unsupported header callback");
}

void maximumPacketWithFragmentation()
{
    std::array<protocol::Byte, protocol::kMaxPayloadSize> payload{};
    for (std::size_t index = 0; index < payload.size(); ++index) {
        payload[index] = (index % 17U == 0)
            ? 0
            : static_cast<protocol::Byte>(index & 0xFFU);
    }

    std::array<protocol::Byte, protocol::kMaxWirePacketSize> wire{};
    const auto encoded = protocol::encodePacket(
        protocol::MessageType::State,
        protocol::PacketFlagNone,
        0xFEDCBA98U,
        payload,
        wire);
    require(encoded.ok, "maximum packet encoding");
    require(
        encoded.bytesWritten <= protocol::kMaxWirePacketSize,
        "maximum packet wire bound");

    protocol::StreamDecoder decoder;
    unsigned callbacks = 0;
    std::size_t offset = 0;
    std::size_t chunkSize = 1;
    while (offset < encoded.bytesWritten) {
        const std::size_t remaining = encoded.bytesWritten - offset;
        const std::size_t current = chunkSize < remaining
            ? chunkSize
            : remaining;
        decoder.push(
            std::span<const protocol::Byte>(
                wire.data() + offset,
                current),
            [&](const protocol::PacketView packet) {
                ++callbacks;
                require(
                    packet.sequence == 0xFEDCBA98U,
                    "maximum packet sequence");
                require(
                    packet.payload.size() == payload.size(),
                    "maximum packet payload size");
                require(
                    packet.payload[1020] == payload[1020],
                    "maximum packet payload contents");
            });
        offset += current;
        chunkSize = chunkSize == 17 ? 1 : chunkSize + 1;
    }
    require(callbacks == 1, "fragmented maximum packet callback");
}

void crcGoldenVector()
{
    constexpr std::array<protocol::Byte, 9> input{
        '1', '2', '3', '4', '5', '6', '7', '8', '9'};
    require(
        protocol::crc32c(input) == 0xE3069283U,
        "CRC-32C golden vector");
}

void malformedCobsAndInvalidModels()
{
    constexpr std::array<protocol::Byte, 2> malformedCobs{0x02, 0x00};
    std::array<protocol::Byte, 8> decoded{};
    require(
        !protocol::cobsDecode(malformedCobs, decoded).ok,
        "zero inside COBS block rejected");

    protocol::StripConfig strip;
    strip.ledCount = 0;
    std::array<protocol::Byte, protocol::kStripConfigPayloadSize> stripBytes{};
    require(
        !protocol::encodeStripConfig(strip, stripBytes),
        "invalid strip model rejected by encoder");

    strip = {};
    require(
        protocol::encodeStripConfig(strip, stripBytes),
        "valid strip model accepted");
    stripBytes[12] = 2;
    require(
        !protocol::decodeStripConfig(stripBytes, strip),
        "noncanonical boolean rejected");

    protocol::StateSnapshot state;
    state.currentLimitMilliamps = 0;
    std::array<protocol::Byte, protocol::kStatePayloadSize> stateBytes{};
    require(
        !protocol::encodeState(state, stateBytes),
        "invalid state model rejected by encoder");
}

void cobsBoundariesAndOversizeRecovery()
{
    for (const std::size_t size : {253U, 254U, 255U, 508U}) {
        std::vector<protocol::Byte> input(size, 0x5A);
        std::vector<protocol::Byte> encoded(size + size / 254U + 2U);
        std::vector<protocol::Byte> decoded(size + 1U);
        const auto encodedSize = protocol::cobsEncode(input, encoded);
        require(encodedSize.ok, "COBS boundary encoding");
        const auto decodedSize = protocol::cobsDecode(
            std::span<const protocol::Byte>(
                encoded.data(),
                encodedSize.bytesWritten),
            decoded);
        require(decodedSize.ok, "COBS boundary decoding");
        require(decodedSize.bytesWritten == input.size(), "COBS boundary size");
        require(decoded[decodedSize.bytesWritten - 1U] == 0x5A, "COBS boundary data");
    }

    std::array<protocol::Byte, protocol::kMaxWirePacketSize> valid{};
    const auto validSize = protocol::encodePacket(
        protocol::MessageType::Pong,
        protocol::PacketFlagNone,
        99,
        {},
        valid);
    require(validSize.ok, "recovery packet encoding");

    std::vector<protocol::Byte> stream(
        protocol::kMaxCobsEncodedSize + 10U,
        0x55);
    stream.push_back(0);
    stream.insert(
        stream.end(),
        valid.begin(),
        valid.begin() + static_cast<std::ptrdiff_t>(validSize.bytesWritten));

    protocol::StreamDecoder decoder;
    unsigned callbacks = 0;
    decoder.push(stream, [&](const protocol::PacketView packet) {
        ++callbacks;
        require(packet.sequence == 99, "oversize recovery sequence");
    });
    require(callbacks == 1, "oversize frame recovery");
    require(decoder.stats().formatErrors == 1, "oversize error statistics");
}

void noiseAndCrcRecovery()
{
    constexpr std::array<protocol::Byte, 3> payload{1, 2, 3};
    std::array<protocol::Byte, protocol::kMaxWirePacketSize> original{};
    std::array<protocol::Byte, protocol::kMaxWirePacketSize> damaged{};
    std::array<protocol::Byte, protocol::kMaxWirePacketSize> valid{};
    const auto damagedSize = protocol::encodePacket(
        protocol::MessageType::Hello,
        protocol::PacketFlagNone,
        1,
        payload,
        original);
    const auto validSize = protocol::encodePacket(
        protocol::MessageType::Pong,
        protocol::PacketFlagNone,
        2,
        {},
        valid);

    std::array<protocol::Byte, protocol::kMaxDecodedPacketSize> decoded{};
    const auto decodedSize = protocol::cobsDecode(
        std::span<const protocol::Byte>(
            original.data(),
            damagedSize.bytesWritten - 1U),
        decoded);
    require(decodedSize.ok, "decode packet before intentional corruption");
    decoded[protocol::kDecodedHeaderSize] ^= 0x80U;
    const auto reencoded = protocol::cobsEncode(
        std::span<const protocol::Byte>(
            decoded.data(),
            decodedSize.bytesWritten),
        damaged);
    require(reencoded.ok, "re-encode intentionally corrupted packet");
    damaged[reencoded.bytesWritten] = 0;

    std::vector<protocol::Byte> stream{0x02, 0x55, 0x00};
    stream.insert(
        stream.end(),
        damaged.begin(),
        damaged.begin()
            + static_cast<std::ptrdiff_t>(reencoded.bytesWritten + 1U));
    stream.insert(
        stream.end(),
        valid.begin(),
        valid.begin() + static_cast<std::ptrdiff_t>(validSize.bytesWritten));

    protocol::StreamDecoder decoder;
    unsigned callbacks = 0;
    decoder.push(stream, [&](const protocol::PacketView packet) {
        ++callbacks;
        require(packet.type == protocol::MessageType::Pong, "CRC recovery");
        require(packet.sequence == 2, "recovered packet sequence");
    });

    require(callbacks == 1, "damaged packet rejected");
    require(decoder.stats().crcErrors == 1, "CRC error statistics");
    require(
        decoder.stats().cobsErrors + decoder.stats().formatErrors >= 1,
        "noise statistics");
}

void stripConfiguration()
{
    protocol::StripConfig config;
    config.ledCount = 300;
    config.topCount = 84;
    config.rightCount = 66;
    config.bottomCount = 84;
    config.leftCount = 66;
    config.startOffset = 42;
    config.reversed = true;
    config.colorOrder = protocol::ColorOrder::Grb;
    config.brightnessLimit = 160;
    config.currentLimitMilliamps = 6000;
    config.ambientTimeoutMilliseconds = 1500;
    config.redCalibration = 255;
    config.greenCalibration = 235;
    config.blueCalibration = 200;

    std::array<protocol::Byte, protocol::kStripConfigPayloadSize> payload{};
    require(
        protocol::encodeStripConfig(config, payload),
        "strip configuration encoding");

    protocol::StripConfig decoded;
    require(
        protocol::decodeStripConfig(payload, decoded),
        "strip configuration decoding");
    require(decoded.ledCount == 300, "LED count");
    require(decoded.startOffset == 42, "start offset");
    require(decoded.reversed, "reverse direction");
    require(
        decoded.colorOrder == protocol::ColorOrder::Grb,
        "color order");
    require(decoded.currentLimitMilliamps == 6000, "current limit");
    require(decoded.ambientTimeoutMilliseconds == 1500, "ambient timeout");
    require(decoded.redCalibration == 255, "red calibration");
    require(decoded.greenCalibration == 235, "green calibration");
    require(decoded.blueCalibration == 200, "blue calibration");

    payload[0] = 0;
    payload[1] = 0;
    require(
        !protocol::decodeStripConfig(payload, decoded),
        "invalid LED count rejected");
}

void controlPayloads()
{
    std::array<protocol::Byte, protocol::kStatePayloadSize> storage{};

    protocol::OperatingMode mode{};
    require(
        protocol::encodeOperatingMode(
            protocol::OperatingMode::Effect,
            storage),
        "mode encoding");
    require(
        protocol::decodeOperatingMode(
            std::span<const protocol::Byte>(
                storage.data(),
                protocol::kModePayloadSize),
            mode),
        "mode decoding");
    require(mode == protocol::OperatingMode::Effect, "mode value");

    protocol::EffectConfig effect;
    effect.effect = protocol::EffectType::Wave;
    effect.intensity = 200;
    effect.red = 10;
    effect.green = 20;
    effect.blue = 30;
    effect.speedPermille = 1750;
    require(
        protocol::encodeEffectConfig(effect, storage),
        "effect encoding");
    protocol::EffectConfig decodedEffect;
    require(
        protocol::decodeEffectConfig(
            std::span<const protocol::Byte>(
                storage.data(),
                protocol::kEffectConfigPayloadSize),
            decodedEffect),
        "effect decoding");
    require(
        decodedEffect.effect == protocol::EffectType::Wave,
        "effect type");
    require(decodedEffect.speedPermille == 1750, "effect speed");

    protocol::StateSnapshot state;
    state.mode = protocol::OperatingMode::Ambilight;
    state.flags = protocol::StateFlagAmbientLeaseActive;
    state.lastReceivedSequence = 123456;
    state.lastAppliedSequence = 123455;
    state.droppedFrames = 7;
    require(protocol::encodeState(state, storage), "state encoding");
    protocol::StateSnapshot decodedState;
    require(
        protocol::decodeState(
            std::span<const protocol::Byte>(
                storage.data(),
                protocol::kStatePayloadSize),
            decodedState),
        "state decoding");
    require(
        decodedState.lastReceivedSequence == 123456,
        "state received sequence");
    require(decodedState.droppedFrames == 7, "state dropped frames");
}

void ledFrame()
{
    std::array<protocol::Byte, 9> pixels{255, 0, 1, 2, 3, 4, 5, 6, 7};
    std::array<protocol::Byte, 32> payload{};
    std::size_t payloadSize = 0;
    require(
        protocol::encodeLedFramePayload(
            3,
            pixels,
            payload,
            payloadSize),
        "LED frame encoding");

    const auto frame = protocol::decodeLedFramePayload(
        std::span<const protocol::Byte>(payload.data(), payloadSize));
    require(frame.valid, "LED frame decoding");
    require(frame.ledCount == 3, "LED frame count");
    require(frame.pixels.size() == pixels.size(), "LED frame byte count");
    require(frame.pixels[8] == 7, "LED frame pixels");
}

}  // namespace

int main()
{
    packetRoundTrip();
    unsupportedHeaderFieldsReachDispatcher();
    maximumPacketWithFragmentation();
    crcGoldenVector();
    malformedCobsAndInvalidModels();
    cobsBoundariesAndOversizeRecovery();
    noiseAndCrcRecovery();
    stripConfiguration();
    controlPayloads();
    ledFrame();
    std::cout << "All protocol tests passed.\n";
    return EXIT_SUCCESS;
}
