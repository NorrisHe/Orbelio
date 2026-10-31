#include <orbelio/core/static_scene.hpp>
#include "model_decode_internal.hpp"
#include <algorithm>
#include <cmath>

namespace orbelio {
namespace {
constexpr std::size_t outlierTrim = 200;
constexpr std::size_t centralFramingTrim = 20;
constexpr float backgroundExtentRatio = 4.f;
constexpr float framingDensityCapPercentile = .95f;
constexpr double framingWeightTailFraction = .05;
constexpr float minimumRadius = .01f;
constexpr std::size_t legacyDecodedBudget = 128 * 1024 * 1024;

struct FramingBounds {
    std::array<float, 3> center{};
    std::array<float, 3> extent{};

    void setAxis(std::size_t component, float low, float high) {
        center[component] = (low + high) * .5f;
        extent[component] = (high - low) * .5f;
    }

    float radius() const {
        return std::max(minimumRadius,
                        std::sqrt(extent[0] * extent[0] + extent[1] * extent[1] + extent[2] * extent[2]));
    }
};

struct WeightedCoordinate {
    float position = 0;
    float weight = 0;
};

static_assert(sizeof(WeightedCoordinate) == 2 * sizeof(float));

float logDensity(const StaticScene& scene, std::size_t index) {
    return -scene.logScales[index * 3] - scene.logScales[index * 3 + 1] - scene.logScales[index * 3 + 2];
}

double buildFramingWeights(const StaticScene& scene, std::vector<float>& weights) {
    for (std::size_t index = 0; index < weights.size(); ++index) {
        weights[index] = logDensity(scene, index);
    }
    const auto capIndex = static_cast<std::size_t>((weights.size() - 1) * framingDensityCapPercentile);
    std::nth_element(weights.begin(), weights.begin() + capIndex, weights.end());
    const auto cap = weights[capIndex];
    double total = 0;
    for (std::size_t index = 0; index < weights.size(); ++index) {
        // Cap density so a nearly degenerate Gaussian cannot own the camera's entire weight.
        weights[index] = std::exp(std::min(0.f, logDensity(scene, index) - cap)) /
                         (1.f + std::exp(-scene.opacityLogits[index]));
        total += weights[index];
    }
    return total;
}

void fitWeightedAxis(FramingBounds& bounds,
                     std::size_t component,
                     std::vector<WeightedCoordinate>& coordinates,
                     double totalWeight) {
    std::sort(coordinates.begin(), coordinates.end(), [](const auto& first, const auto& second) {
        return first.position < second.position;
    });
    double cumulative = 0;
    float low = coordinates.front().position;
    float high = coordinates.back().position;
    bool lowReached = false;
    const auto lowWeight = totalWeight * framingWeightTailFraction;
    const auto highWeight = totalWeight * (1. - framingWeightTailFraction);
    for (const auto& coordinate : coordinates) {
        cumulative += coordinate.weight;
        if (!lowReached && cumulative >= lowWeight) {
            low = coordinate.position;
            lowReached = true;
        }
        if (cumulative >= highWeight) {
            high = coordinate.position;
            break;
        }
    }
    bounds.setAxis(component, low, high);
}

FramingBounds fitDetailBounds(const StaticScene& scene, std::vector<float>& weights) {
    const auto totalWeight = buildFramingWeights(scene, weights);
    std::vector<WeightedCoordinate> coordinates(weights.size());
    FramingBounds bounds{};
    for (std::size_t component = 0; component < bounds.center.size(); ++component) {
        for (std::size_t index = 0; index < weights.size(); ++index) {
            coordinates[index] = {scene.positions[index * 3 + component], weights[index]};
        }
        fitWeightedAxis(bounds, component, coordinates, totalWeight);
    }
    return bounds;
}

void fitRobustBounds(StaticScene& scene, std::size_t count) {
    // Robust framing excludes position outliers only from camera fitting; all Gaussians are rendered.
    FramingBounds outer{};
    FramingBounds central{};
    CenterBounds exact{};
    std::vector<float> axis(count);
    for (int component = 0; component < 3; ++component) {
        for (std::size_t splat = 0; splat < count; ++splat) {
            axis[splat] = scene.positions[3 * splat + component];
        }
        std::sort(axis.begin(), axis.end());
        exact.minimum[component] = axis.front();
        exact.maximum[component] = axis.back();
        outer.setAxis(component, axis[count / outlierTrim], axis[count - 1 - count / outlierTrim]);
        central.setAxis(
            component, axis[count / centralFramingTrim], axis[count - 1 - count / centralFramingTrim]);
    }
    // A separated background shell requires framing the dense detail, rather than the whole environment.
    const auto framing =
        outer.radius() > backgroundExtentRatio * central.radius() ? fitDetailBounds(scene, axis) : outer;
    scene.center = framing.center;
    scene.radius = framing.radius();
    scene.centerBounds = exact;
}

void measureScaleRange(StaticScene& scene) {
    const auto maximumAxis = [&](std::size_t index) {
        return std::max(
            {scene.logScales[index * 3], scene.logScales[index * 3 + 1], scene.logScales[index * 3 + 2]});
    };
    auto minimum = maximumAxis(0);
    auto maximum = minimum;
    for (std::size_t index = 1; index < scene.count(); ++index) {
        const auto value = maximumAxis(index);
        minimum = std::min(minimum, value);
        maximum = std::max(maximum, value);
    }
    scene.logScaleRange = std::array{minimum, maximum};
}
} // namespace

void detail::finishDecodedScene(StaticScene& scene) {
    validateDecodedScene(scene);
    fitRobustBounds(scene, scene.count());
    measureScaleRange(scene);
    const auto bytes = scene.count() * (baseFloatCount + scene.shRestCoefficients() * 3) * sizeof(float);
    scene.memory.shrinkTo(bytes);
}

StaticScene decodeCompressedPly(std::span<const std::byte> bytes) {
    return decodeCompressedPly(bytes, legacyDecodedBudget);
}

StaticScene decodeCompressedPly(std::span<const std::byte> bytes, std::size_t maxDecodedBytes) {
    ModelLimits limits{};
    limits.decodedBytes = maxDecodedBytes;
    return decodeModel(bytes, "model.ply", limits);
}
} // namespace orbelio
