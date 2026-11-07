#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace orbelio {
enum class AttributeView : std::uint32_t { Normal, Depth, Opacity, Scale };

enum class RenderOutput : std::uint32_t { Display = 1, FloatColorAlpha = 2, Depth = 4, Normals = 8 };
enum class RasterMode : std::uint32_t { OrderedQuads, TiledCompute };

constexpr RenderOutput operator|(RenderOutput a, RenderOutput b) {
    return RenderOutput(std::uint32_t(a) | std::uint32_t(b));
}

constexpr bool hasOutput(RenderOutput outputs, RenderOutput plane) {
    return (std::uint32_t(outputs) & std::uint32_t(plane)) != 0;
}

constexpr bool validOutputs(RenderOutput outputs) {
    constexpr auto available =
        RenderOutput::Display | RenderOutput::FloatColorAlpha | RenderOutput::Depth | RenderOutput::Normals;
    return std::uint32_t(outputs) != 0 && (std::uint32_t(outputs) & ~std::uint32_t(available)) == 0;
}

struct RenderSettings {
    std::uint32_t maxGaussians = 0;
    std::uint32_t uploadGaussiansPerFrame = 262144;
    AttributeView attributeView = AttributeView::Normal;
    float attributeMinimum = 0;
    float attributeMaximum = 10;
    RenderOutput outputs = RenderOutput::Display;
    std::array<float, 3> background{0.035f, 0.045f, 0.065f};
    RasterMode rasterMode = RasterMode::OrderedQuads;
    std::uint32_t tilePairCapacity = 0;
    std::uint32_t tileBinningWidth = 16;
    bool separateShading = false;
    bool operator==(const RenderSettings&) const = default;

    std::size_t budgetedCount(std::size_t total) const {
        return maxGaussians ? std::min(total, std::size_t(maxGaussians)) : total;
    }
};

// Stable sampling spans the source rather than retaining a spatially biased file prefix.
inline std::size_t sampledGaussian(std::size_t index, std::size_t total, std::size_t selected) {
    return std::uint64_t(index) * total / selected;
}
}
