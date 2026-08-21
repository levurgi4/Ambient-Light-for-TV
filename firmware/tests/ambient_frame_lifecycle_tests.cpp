#include <iostream>
#include <string_view>

#include "ambient_frame_lifecycle.hpp"

namespace {

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void test_arrival_during_in_flight_transmit_is_not_replacement() {
    using ambilight::firmware::AmbientFrameLifecycle;

    AmbientFrameLifecycle frames;
    expect(!frames.enqueue(), "the first queued frame replaces nothing");
    expect(frames.claim(10, 3), "frame 10 must enter the in-flight slot");
    expect(!frames.queued() && frames.in_flight(), "frame 10 must be claimed");

    expect(
        !frames.enqueue(),
        "frame 11 arriving during frame 10 transmit is not a queued replacement");
    expect(
        frames.queued() && frames.in_flight(),
        "new queued and older in-flight frames must coexist");
    expect(frames.applied(10, 3), "frame 10 must resolve as applied");
    expect(
        frames.queued() && !frames.in_flight(),
        "applying frame 10 must leave frame 11 queued");
    expect(frames.claim(11, 3), "frame 11 must be claimable next");
    expect(frames.applied(11, 3), "frame 11 must resolve as applied");
}

void test_failed_output_without_replacement_restores_frame() {
    using namespace ambilight::firmware;

    AmbientFrameLifecycle frames;
    static_cast<void>(frames.enqueue());
    expect(frames.claim(20, 4), "frame 20 must enter the in-flight slot");
    expect(
        frames.failed(20, 4, true) ==
            AmbientFailureResolution::restored,
        "a still-relevant failed frame must return to the queue");
    expect(
        frames.queued() && !frames.in_flight(),
        "restored frame 20 must be queued for a retry");
    expect(frames.claim(20, 4), "restored frame 20 must be claimable again");
}

void test_failed_output_with_replacement_drops_only_in_flight_frame() {
    using namespace ambilight::firmware;

    AmbientFrameLifecycle frames;
    static_cast<void>(frames.enqueue());
    expect(frames.claim(30, 5), "frame 30 must enter the in-flight slot");
    expect(
        !frames.enqueue(),
        "frame 31 is the first queued frame while 30 is in flight");
    expect(
        frames.failed(30, 5, true) ==
            AmbientFailureResolution::dropped,
        "failed frame 30 is superseded by queued frame 31");
    expect(
        frames.queued() && !frames.in_flight(),
        "dropping frame 30 must preserve queued frame 31");
    expect(frames.claim(31, 5), "replacement frame 31 must remain claimable");
}

void test_only_queued_unclaimed_frames_count_as_replaced() {
    using ambilight::firmware::AmbientFrameLifecycle;

    AmbientFrameLifecycle frames;
    expect(!frames.enqueue(), "first queue insertion replaces nothing");
    expect(
        frames.enqueue(),
        "a second insertion before claim replaces the queued frame");
}

void test_invalidation_discards_only_queued_frame() {
    using namespace ambilight::firmware;

    AmbientFrameLifecycle frames;
    static_cast<void>(frames.enqueue());
    expect(
        frames.discard_queued(),
        "config/session invalidation must report an accepted queued frame");
    expect(
        !frames.discard_queued(),
        "repeating invalidation with an empty queue must report no drop");

    static_cast<void>(frames.enqueue());
    expect(frames.claim(40, 6), "frame 40 must enter the in-flight slot");
    expect(
        !frames.discard_queued() && frames.in_flight(),
        "invalidation must leave an in-flight frame for its real result");
    expect(
        frames.failed(40, 6, false) ==
            AmbientFailureResolution::dropped,
        "a failed invalidated in-flight frame is resolved separately");
}

}  // namespace

int main() {
    test_arrival_during_in_flight_transmit_is_not_replacement();
    test_failed_output_without_replacement_restores_frame();
    test_failed_output_with_replacement_drops_only_in_flight_frame();
    test_only_queued_unclaimed_frames_count_as_replaced();
    test_invalidation_discards_only_queued_frame();

    if (failures == 0) {
        std::cout << "All ambient frame lifecycle tests passed.\n";
    }
    return failures == 0 ? 0 : 1;
}
