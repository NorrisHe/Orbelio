#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace orbelio {
struct RenderSettings {
    std::uint32_t maxGaussians = 0;
    std::uint32_t uploadGaussiansPerFrame = 262144;
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
