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
constexpr std::size_t framingSubjectCount = 2048;
constexpr std::size_t framingBackgroundCount = 96;
constexpr std::size_t framingChunkCount = 8;
constexpr std::size_t fixtureChunkSize = 256;
constexpr std::size_t fixtureGridSide = 16;
constexpr std::size_t fixtureGridDepth = 8;
constexpr std::size_t backgroundDirectionCount = 6;
constexpr float normalizedShellOffset = .25f;
constexpr float subjectRadiusSquared = 5.25f;
constexpr std::uint32_t elevenBitMaximum = 2047;
constexpr std::uint32_t tenBitMaximum = 1023;
constexpr std::uint32_t packedHighShift = 21;
constexpr std::uint32_t packedMiddleShift = 11;
constexpr std::uint32_t packedOpaqueWhite = 0xffffffffu;
constexpr std::uint32_t packedScaleAtMaximum = 0xffffffffu;
constexpr std::array<float, 18> subjectChunkFrame =
    {-1, -2, -.5f, 1, 2, .5f, -5, -5, -5, -5, -5, -5, 0, 0, 0, 1, 1, 1};
constexpr std::array<float, 18> backgroundChunkFrame =
    {-200, -200, -200, 200, 200, 200, -40, -40, -40, 0, 0, 0, 0, 0, 0, 1, 1, 1};

std::uint32_t packNormalizedPosition(const std::array<float, 3>& values) {
    return (std::uint32_t(std::round(values[0] * elevenBitMaximum)) << packedHighShift) |
           (std::uint32_t(std::round(values[1] * tenBitMaximum)) << packedMiddleShift) |
           std::uint32_t(std::round(values[2] * elevenBitMaximum));
}

void appendFramingVertex(std::vector<std::byte>& bytes,
                         const std::array<float, 3>& normalizedPosition,
                         std::uint32_t scale) {
    append(bytes, packNormalizedPosition(normalizedPosition));
    append(bytes, packedNearIdentityRotation);
    append(bytes, scale);
    append(bytes, packedOpaqueWhite);
}

void appendFramingPayload(std::vector<std::byte>& bytes, bool includeBackground) {
    for (std::size_t index = 0; index < framingSubjectCount; ++index) {
        appendFramingVertex(bytes,
                            {float(index % fixtureGridSide) / (fixtureGridSide - 1),
                             float(index / fixtureGridSide % fixtureGridSide) / (fixtureGridSide - 1),
                             float(index / fixtureChunkSize) / (fixtureGridDepth - 1)},
                            packedScaleAtMinimum);
    }
    if (!includeBackground) {
        return;
    }
    for (std::size_t index = 0; index < framingBackgroundCount; ++index) {
        std::array<float, 3> position{.5f, .5f, .5f};
        position[index % backgroundDirectionCount / 2] +=
            index % 2 ? normalizedShellOffset : -normalizedShellOffset;
        appendFramingVertex(bytes, position, packedScaleAtMaximum);
    }
    appendFramingVertex(bytes, {1, .5f, .5f}, packedScaleAtMinimum);
}

std::vector<std::byte> buildFramingFixture(const std::vector<std::byte>& sample, bool includeBackground) {
    const auto chunks = framingChunkCount + (includeBackground ? 1 : 0);
    const auto count = framingSubjectCount + (includeBackground ? framingBackgroundCount + 1 : 0);
    const std::string input(reinterpret_cast<const char*>(sample.data()), sample.size());
    std::string header = input.substr(0, input.find(endHeaderLine) + endHeaderLine.size());
    replaceToken(header, "element chunk 355", "element chunk " + std::to_string(chunks));
    replaceToken(header, "element vertex 90854", "element vertex " + std::to_string(count));
    std::vector<std::byte> bytes(header.size());
    std::memcpy(bytes.data(), header.data(), header.size());
    for (std::size_t index = 0; index < chunks; ++index) {
        for (const auto value : index < framingChunkCount ? subjectChunkFrame : backgroundChunkFrame) {
            append(bytes, std::bit_cast<std::uint32_t>(value));
        }
    }
    appendFramingPayload(bytes, includeBackground);
    return bytes;
}

void checkBackgroundDoesNotOwnFraming(const std::vector<std::byte>& bytes) {
    const auto subject = orbelio::decodeCompressedPly(buildFramingFixture(bytes, false));
    const auto environment = orbelio::decodeCompressedPly(buildFramingFixture(bytes, true));
    require(std::abs(subject.radius - std::sqrt(subjectRadiusSquared)) < 1e-4f,
            "Ordinary object framing changed");
    require(environment.count() == framingSubjectCount + framingBackgroundCount + 1,
            "Framing removed background Gaussians");
    require(environment.center == subject.center && std::abs(environment.radius - subject.radius) < 1e-4f,
            "Distant background or one degenerate Gaussian displaced the subject framing");
    require(std::equal(subject.positions.begin(), subject.positions.end(), environment.positions.begin()),
            "Framing changed decoded subject positions");
}

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
        checkBackgroundDoesNotOwnFraming(bytes);
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
