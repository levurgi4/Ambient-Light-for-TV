#pragma once

#include <cstdint>
#include <span>

namespace ambilight::firmware {

class ByteStreamSink {
public:
    virtual ~ByteStreamSink() = default;
    virtual void consume(std::span<const std::uint8_t> bytes) = 0;
};

class ByteStreamWriter {
public:
    virtual ~ByteStreamWriter() = default;
    [[nodiscard]] virtual bool write(
        std::span<const std::uint8_t> bytes) = 0;
};

}  // namespace ambilight::firmware
