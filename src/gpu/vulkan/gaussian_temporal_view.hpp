#pragma once
#include <span>

namespace orbelio {
// Borrowed spans stay alive through the immutable owner in FrameState::scene.
struct GaussianTemporalView {
    std::span<const float> times;
    std::span<const float> variances;
    std::span<const float> velocities;
    std::span<const float> harmonics;
    unsigned degree = 0;
    double time = 0;
    double period = 1;

    bool active() const { return !times.empty(); }
};
}
