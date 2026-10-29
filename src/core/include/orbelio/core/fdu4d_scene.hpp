#pragma once
#include "static_scene.hpp"
#include <memory>

namespace orbelio {
// A conditional representation: covariance is constant, mean and appearance vary with time.
struct Fdu4dScene {
    StaticScene spatial;
    MemoryBudget::Reservation temporalMemory;
    MemoryBudget resident{0};
    ModelLimits limits;
    std::vector<float> times;
    std::vector<float> variances;
    std::vector<float> velocities;
    std::vector<float> temporalSh;
    unsigned temporalDegree = 0;
    double startTime = 0;
    double endTime = 1;
    double sampleRate = 30;

    std::size_t count() const { return spatial.count(); }

    int sampleCount() const;
    int sampleAt(double time) const;
    double sampleTime(int index) const;
    double clampTime(double time) const;
    double advance(double time, double delta, bool loop) const;
};

StaticScene evaluateFdu4d(const Fdu4dScene& scene, double time, std::size_t maxGaussians = 0);
}
