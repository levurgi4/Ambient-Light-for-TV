#pragma once

namespace ambilight {
// Capture reset must not erase the frame/keepalive of another active owner.
constexpr bool mayClearCaptureLedOutput(bool requested, bool audio,
    bool whiteBalance, bool layoutTest, bool manualAnimation) noexcept
{
    return requested && !audio && !whiteBalance && !layoutTest && !manualAnimation;
}
}
