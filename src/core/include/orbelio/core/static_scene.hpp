#pragma once
#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace orbelio {
struct StaticScene {
    std::vector<float> positions;
    std::vector<float> rotations; // wxyz
    std::vector<float> logScales;
    std::vector<float> opacityLogits;
    std::vector<float> sh0;
    std::array<float, 3> center{};
    float radius = 1;

    std::size_t count() const { return opacityLogits.size(); }
};

// PlayCanvas compressed PLY: binary little endian, 18 floats/chunk, SH0.
// Decoding layout is derived from MIT-licensed PlayCanvas engine; see samples/guitar/README.md.
StaticScene decodeCompressedPly(std::span<const std::byte> bytes);
}
