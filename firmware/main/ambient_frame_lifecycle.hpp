#pragma once

#include <cstdint>

namespace ambilight::firmware {

enum class AmbientFailureResolution : std::uint8_t {
    none,
    restored,
    dropped,
};

// Tracks ownership of ambient frames independently from their pixel storage.
// A queued frame can be replaced before the render task claims it. Once
// claimed, it is in flight and remains a distinct frame even if UART queues a
// newer one while RMT is transmitting.
class AmbientFrameLifecycle final {
public:
    // Returns true only when an older, still-unclaimed frame was replaced.
    [[nodiscard]] constexpr bool enqueue() noexcept {
        const bool replaced = queued_;
        queued_ = true;
        return replaced;
    }

    // Move the queued frame to the single render/RMT in-flight slot.
    [[nodiscard]] constexpr bool claim(
        const std::uint32_t sequence,
        const std::uint32_t generation) noexcept {
        if (!queued_ || in_flight_) {
            return false;
        }
        queued_ = false;
        in_flight_ = true;
        in_flight_sequence_ = sequence;
        in_flight_generation_ = generation;
        return true;
    }

    // Returns true if this result resolved the current in-flight frame.
    [[nodiscard]] constexpr bool applied(
        const std::uint32_t sequence,
        const std::uint32_t generation) noexcept {
        if (!matches_in_flight(sequence, generation)) {
            return false;
        }
        clear_in_flight();
        return true;
    }

    // Restore a failed frame to the queue only while it is still relevant and
    // no newer queued frame owns the shared pixel storage. Otherwise the failed
    // in-flight frame is irrecoverably superseded and must count as dropped.
    [[nodiscard]] constexpr AmbientFailureResolution failed(
        const std::uint32_t sequence,
        const std::uint32_t generation,
        const bool still_relevant) noexcept {
        if (!matches_in_flight(sequence, generation)) {
            return AmbientFailureResolution::none;
        }
        clear_in_flight();
        if (still_relevant && !queued_) {
            queued_ = true;
            return AmbientFailureResolution::restored;
        }
        return AmbientFailureResolution::dropped;
    }

    // Returns true when a queued frame was discarded.
    [[nodiscard]] constexpr bool discard_queued() noexcept {
        const bool discarded = queued_;
        queued_ = false;
        return discarded;
    }

    [[nodiscard]] constexpr bool queued() const noexcept {
        return queued_;
    }

    [[nodiscard]] constexpr bool in_flight() const noexcept {
        return in_flight_;
    }

private:
    [[nodiscard]] constexpr bool matches_in_flight(
        const std::uint32_t sequence,
        const std::uint32_t generation) const noexcept {
        return in_flight_ &&
               in_flight_sequence_ == sequence &&
               in_flight_generation_ == generation;
    }

    constexpr void clear_in_flight() noexcept {
        in_flight_ = false;
        in_flight_sequence_ = 0;
        in_flight_generation_ = 0;
    }

    std::uint32_t in_flight_sequence_{};
    std::uint32_t in_flight_generation_{};
    bool queued_{};
    bool in_flight_{};
};

}  // namespace ambilight::firmware
