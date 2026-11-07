#include <orbelio/core/fdu4d_scene.hpp>
#include <orbelio/core/render_settings.hpp>
#include "model_decode_internal.hpp"
#include <numbers>

namespace orbelio {
namespace {
constexpr double temporalCullWeight = .05;
constexpr float culledOpacityLogit = -100.f;
constexpr double sampleRoundingTolerance = 1e-9;

void evaluateGaussian(const Fdu4dScene& source, StaticScene& target, std::size_t index, double time) {
    const auto input = sampledGaussian(index, source.count(), target.count());
    const auto dt = time - source.times[input];
    const auto weight = std::exp(-.5 * dt * dt / source.variances[input]);
    const auto alpha = weight / (1. + std::exp(-double(source.spatial.opacityLogits[input])));
    target.opacityLogits[index] =
        weight > temporalCullWeight ? detail::logit(float(alpha)) : culledOpacityLogit;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        target.positions[index * 3 + axis] =
            float(source.spatial.positions[input * 3 + axis] + source.velocities[input * 3 + axis] * dt);
    }
    const auto coefficients = (target.shDegree + 1) * (target.shDegree + 1);
    for (unsigned band = 0; band < source.temporalDegree; ++band) {
        const auto factor =
            float(std::cos(2 * std::numbers::pi * (band + 1) * dt / (source.endTime - source.startTime)));
        const auto offset = (input * source.temporalDegree + band) * coefficients * 3;
        for (std::size_t component = 0; component < coefficients * 3; ++component) {
            auto& value = component < 3 ? target.sh0[index * 3 + component]
                                        : target.shRest[index * (coefficients - 1) * 3 + component - 3];
            value += source.temporalSh[offset + component] * factor;
        }
    }
}
}

int Fdu4dScene::sampleCount() const { return int(std::ceil((endTime - startTime) * sampleRate)) + 1; }

int Fdu4dScene::sampleAt(double time) const {
    time = clampTime(time);
    if (time == endTime) {
        return sampleCount() - 1;
    }
    return std::clamp(
        int(std::floor((time - startTime) * sampleRate + sampleRoundingTolerance)), 0, sampleCount() - 1);
}

double Fdu4dScene::sampleTime(int index) const {
    return std::min(endTime, startTime + std::clamp(index, 0, sampleCount() - 1) / sampleRate);
}

double Fdu4dScene::clampTime(double time) const {
    detail::require(std::isfinite(time), "FDU time must be finite");
    return std::clamp(time, startTime, endTime);
}

double Fdu4dScene::advance(double time, double delta, bool loop) const {
    detail::require(std::isfinite(delta) && delta >= 0, "FDU playback delta must be finite and non-negative");
    time = clampTime(time);
    if (!loop) {
        return delta >= endTime - time ? endTime : time + delta;
    }
    const auto remainder = std::fmod(delta, endTime - startTime);
    return remainder >= endTime - time ? startTime + (remainder - (endTime - time)) : time + remainder;
}

StaticScene evaluateFdu4d(const Fdu4dScene& source, double time, std::size_t maxGaussians) {
    time = source.clampTime(time);
    detail::DecodeContext context{source.limits, source.resident, {}};
    const auto count = maxGaussians ? std::min(source.count(), maxGaussians) : source.count();
    auto target = context.createScene(count, source.spatial.shDegree, "FDU 4DGS");
    auto copy = [&](const auto& from, auto& to, std::size_t components) {
        if (count == source.count()) {
            to = from;
            return;
        }
        for (std::size_t index = 0; index < count; ++index) {
            const auto input = sampledGaussian(index, source.count(), count);
            std::copy_n(from.begin() + input * components, components, to.begin() + index * components);
        }
    };
    copy(source.spatial.rotations, target.rotations, 4);
    copy(source.spatial.logScales, target.logScales, 3);
    copy(source.spatial.sh0, target.sh0, 3);
    copy(source.spatial.shRest, target.shRest, source.spatial.shRestCoefficients() * 3);
    for (std::size_t index = 0; index < count; ++index) {
        evaluateGaussian(source, target, index, time);
    }
    detail::validateDecodedScene(target);
    target.center = source.spatial.center;
    target.radius = source.spatial.radius;
    target.antialiased = source.spatial.antialiased;
    target.memory.shrinkTo(count * (detail::baseFloatCount + target.shRestCoefficients() * 3) *
                           sizeof(float));
    return target;
}
}
