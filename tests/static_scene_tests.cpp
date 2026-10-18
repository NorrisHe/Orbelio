#include <orbelio/core/static_scene.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void append(std::vector<std::byte>& bytes, std::uint32_t value) {
    const auto at = bytes.size();
    bytes.resize(at + 4);
    std::memcpy(bytes.data() + at, &value, 4);
}

template <class F> void rejects(F&& f) {
    bool threw = false;
    try {
        f();
    } catch (const std::runtime_error&) {
        threw = true;
    }
    require(threw, "Invalid PLY was accepted");
}

void replaceToken(std::string& text, std::string_view from, std::string_view to) {
    text.replace(text.find(from), from.size(), to);
}

// The scene stores wxyz; this mirrors the loader's squared-length check.
float squaredQuaternionLength(std::span<const float> rotations, std::size_t splat) {
    float sum = 0;
    for (int component = 0; component < 4; ++component) {
        const float value = rotations[4 * splat + component];
        sum += value * value;
    }
    return sum;
}

// One-splat chunk frame: min/max position, min/max log scale, min/max RGB.
constexpr std::array<float, 18> chunkFrame = {1, 2, 3, 5, 6, 7, -3, -2, -1, 1, 2, 3, 0, 0, 0, 1, 1, 1};
constexpr std::uint32_t packedPositionAtMaximum = 0xffffffffu;
constexpr std::uint32_t packedNearIdentityRotation = (512u << 20) | (512u << 10) | 512u;
constexpr std::uint32_t packedScaleAtMinimum = 0u;
constexpr std::uint32_t packedRedWithHalfOpacity = 0xff008080u; // RGBA = ff 00 80 80
constexpr std::string_view endHeaderLine = "end_header\n";
constexpr float sh0RedReference = 1.77245385f;

std::vector<std::byte> readSampleAsset() {
    std::ifstream file(ORBELIO_TEST_MODEL, std::ios::binary | std::ios::ate);
    require(bool(file), "Sample asset missing");
    std::vector<std::byte> bytes(static_cast<std::size_t>(file.tellg()));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
    return bytes;
}

void checkRealAsset(const std::vector<std::byte>& bytes) {
    const auto scene = orbelio::decodeCompressedPly(bytes);
    require(scene.count() == 90854, "Wrong real sample count");
    require(std::isfinite(scene.radius) && scene.radius > 0, "Invalid fitted bounds");
    for (std::size_t splat = 0; splat < scene.count(); ++splat) {
        require(std::abs(squaredQuaternionLength(scene.rotations, splat) - 1) < 1e-4f,
                "Decoded quaternion not normalized");
        require(std::isfinite(scene.opacityLogits[splat]), "Nonfinite opacity");
    }
}

// Rewritten header plus one chunk frame and one splat, so every packed field is known analytically.
struct OneSplatFixture {
    std::vector<std::byte> bytes;
    std::size_t firstSplatOffset = 0;
};

OneSplatFixture buildOneSplatFixture(const std::vector<std::byte>& bytes) {
    const std::string input(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    std::string header = input.substr(0, input.find(endHeaderLine) + endHeaderLine.size());
    replaceToken(header, "element chunk 355", "element chunk 1");
    replaceToken(header, "element vertex 90854", "element vertex 1");
    std::vector<std::byte> fixture(header.size());
    std::memcpy(fixture.data(), header.data(), header.size());
    for (float value : chunkFrame) {
        append(fixture, std::bit_cast<std::uint32_t>(value));
    }
    append(fixture, packedPositionAtMaximum);
    append(fixture, packedNearIdentityRotation);
    append(fixture, packedScaleAtMinimum);
    append(fixture, packedRedWithHalfOpacity);
    return {std::move(fixture), header.size()};
}

// Analytic one-splat fixture: packed positions interpolate independent axes;
// SH0/logit conventions and omitted-component quaternion ordering are checked against known values.
void checkAnalyticConventions(const OneSplatFixture& fixture) {
    const auto one = orbelio::decodeCompressedPly(fixture.bytes);
    require(one.positions[0] == 5 && one.positions[1] == 6 && one.positions[2] == 7,
            "Position unpack/lerp wrong");
    require(one.logScales[0] == -3 && one.logScales[1] == -2 && one.logScales[2] == -1,
            "Scale must remain logarithmic");
    require(one.rotations[0] > .999f && std::abs(one.rotations[1]) < .002f, "Quaternion must use WXYZ");
    require(std::abs(one.sh0[0] - sh0RedReference) < 1e-5f && one.sh0[1] < -1.77f,
            "Color->SH0 conversion wrong");
    require(std::abs(one.opacityLogits[0] - std::log(128.f / 127.f)) < 1e-5f, "Opacity must be logit");
}

// Ordered: empty input, then a truncated asset, then a nonfinite chunk frame.
void checkRejections(std::vector<std::byte>& bytes, OneSplatFixture& fixture) {
    rejects([&] { orbelio::decodeCompressedPly({}); });
    bytes.pop_back();
    rejects([&] { orbelio::decodeCompressedPly(bytes); });
    const auto infinity = std::bit_cast<std::array<std::byte, 4>>(std::numeric_limits<float>::infinity());
    std::copy(infinity.begin(),
              infinity.end(),
              fixture.bytes.begin() + static_cast<std::ptrdiff_t>(fixture.firstSplatOffset));
    rejects([&] { orbelio::decodeCompressedPly(fixture.bytes); });
}
} // namespace

int main() {
    try {
        auto bytes = readSampleAsset();
        checkRealAsset(bytes);
        auto fixture = buildOneSplatFixture(bytes);
        checkAnalyticConventions(fixture);
        checkRejections(bytes, fixture);
        std::cout << "Compressed PLY: analytic conventions, real asset, bounds, truncation and nonfinite "
                     "rejection PASS\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
