#pragma once
#include "memory_budget.hpp"
#include <array>
#include <cstddef>
#include <span>
#include <vector>
#include <string>

namespace orbelio {
struct StaticScene {
    MemoryBudget::Reservation memory;
    std::vector<float> positions;
    std::vector<float> rotations; // wxyz
    std::vector<float> logScales;
    std::vector<float> opacityLogits;
    std::vector<float> sh0;
    std::vector<float> shRest; // Per Gaussian, coefficient-major RGB; DC is stored separately in sh0.
    unsigned shDegree = 0;
    bool antialiased = false;
    std::string format = "Compressed PLY";
    std::array<float, 3> center{};
    float radius = 1;

    std::size_t count() const { return opacityLogits.size(); }

    std::size_t shRestCoefficients() const { return shDegree * (shDegree + 2); }
};

// Compatibility entry point for callers that already know the input is PLY.
StaticScene decodeCompressedPly(std::span<const std::byte> bytes);
StaticScene decodeCompressedPly(std::span<const std::byte> bytes, std::size_t maxDecodedBytes);
}
