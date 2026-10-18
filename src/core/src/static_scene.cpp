#include <orbelio/core/static_scene.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace orbelio {
namespace {

constexpr std::string_view endHeaderMarker = "end_header\n";
constexpr std::size_t maxHeaderBytes = 16384;
constexpr std::size_t maxElementCount = 2'000'000;
constexpr std::size_t gaussiansPerChunk = 256;
constexpr std::size_t chunkFloatCount = 18;
constexpr std::size_t chunkBytes = chunkFloatCount * sizeof(std::uint32_t);
constexpr std::size_t vertexBytes = 16;
constexpr std::size_t wordBytes = sizeof(std::uint32_t);
constexpr std::size_t rotationOffset = 4;
constexpr std::size_t scaleOffset = 8;
constexpr std::size_t colorOffset = 12;
constexpr std::size_t outlierTrim = 200; // one half percent of the cloud trimmed from each axis end
constexpr float positionRangeLimit = 1e10f;
constexpr float logScaleRangeLimit = 40.f;
constexpr float opacityEpsilon = 1e-6f;
constexpr float minimumRadius = .01f;
constexpr float shDcConstant = 0.28209479177387814f;
constexpr std::uint32_t elevenBitMask = 2047;
constexpr std::uint32_t tenBitMask = 1023;
constexpr std::uint32_t eightBitMask = 255;
constexpr std::uint32_t fullByteShift = 8;
constexpr std::uint32_t tripleHighShift = 21;
constexpr std::uint32_t tripleMidShift = 11;
constexpr std::uint32_t quaternionHighShift = 20;
constexpr std::uint32_t quaternionMidShift = 10;
constexpr std::uint32_t quaternionSelectorShift = 30;

// Chunk frame layout matches the property order below: min/max position, min/max log scale, min/max RGB.
constexpr std::array<std::string_view, chunkFloatCount> expectedChunkProperties = {
    "property float min_x",
    "property float min_y",
    "property float min_z",
    "property float max_x",
    "property float max_y",
    "property float max_z",
    "property float min_scale_x",
    "property float min_scale_y",
    "property float min_scale_z",
    "property float max_scale_x",
    "property float max_scale_y",
    "property float max_scale_z",
    "property float min_r",
    "property float min_g",
    "property float min_b",
    "property float max_r",
    "property float max_g",
    "property float max_b"};
constexpr std::array<std::string_view, 4> expectedVertexProperties = {"property uint packed_position",
                                                                      "property uint packed_rotation",
                                                                      "property uint packed_scale",
                                                                      "property uint packed_color"};

struct Header {
    bool binaryLittleEndian = false;
    std::size_t chunks = 0;
    std::size_t count = 0;
    std::size_t payloadOffset = 0;
    std::vector<std::string> chunkProperties;
    std::vector<std::string> vertexProperties;
};

std::uint32_t readWord(std::span<const std::byte> bytes, std::size_t position) {
    static_assert(std::endian::native == std::endian::little);
    std::uint32_t value = 0;
    std::memcpy(&value, bytes.data() + position, wordBytes);
    return value;
}

void readElement(std::string_view line, Header& header, std::string& element) {
    std::istringstream row{std::string(line)};
    std::string tag;
    std::size_t instances = 0;
    row >> tag >> element >> instances;
    if (!row || instances > maxElementCount) {
        throw std::runtime_error("Invalid/oversized PLY element");
    }
    if (element == "chunk") {
        header.chunks = instances;
    } else if (element == "vertex") {
        header.count = instances;
    } else {
        throw std::runtime_error("This bring-up loader supports compressed SH0 PLY only");
    }
}

Header readHeader(std::string_view input) {
    const auto marker = input.find(endHeaderMarker);
    if (marker == std::string_view::npos || marker > maxHeaderBytes) {
        throw std::runtime_error("Missing/bounded PLY header");
    }

    const auto headerText = input.substr(0, marker + endHeaderMarker.size());
    std::istringstream lines{std::string(headerText)};
    Header header;
    header.payloadOffset = headerText.size();

    std::string line;
    std::string element;
    if (!std::getline(lines, line) || line != "ply") {
        throw std::runtime_error("Not a PLY file");
    }
    while (std::getline(lines, line)) {
        if (line == "format binary_little_endian 1.0") {
            header.binaryLittleEndian = true;
            continue;
        }
        if (line.starts_with("element ")) {
            readElement(line, header, element);
            continue;
        }
        if (line.starts_with("property ")) {
            (element == "chunk" ? header.chunkProperties : header.vertexProperties).push_back(line);
        }
    }

    const bool layoutMatches = header.binaryLittleEndian && header.count &&
                               header.chunks == (header.count + gaussiansPerChunk - 1) / gaussiansPerChunk &&
                               std::ranges::equal(header.chunkProperties, expectedChunkProperties) &&
                               std::ranges::equal(header.vertexProperties, expectedVertexProperties);
    if (!layoutMatches) {
        throw std::runtime_error("Unsupported compressed PLY layout (expected 18-float chunks / SH0)");
    }
    if (input.size() - header.payloadOffset != header.chunks * chunkBytes + header.count * vertexBytes) {
        throw std::runtime_error("Truncated or trailing compressed PLY payload");
    }
    return header;
}

std::vector<float> readChunkFrames(std::span<const std::byte> bytes, const Header& header) {
    std::vector<float> frames(header.chunks * chunkFloatCount);
    for (std::size_t index = 0; index < frames.size(); ++index) {
        frames[index] = std::bit_cast<float>(readWord(bytes, header.payloadOffset + wordBytes * index));
        if (!std::isfinite(frames[index])) {
            throw std::runtime_error("Nonfinite PLY chunk");
        }
    }
    return frames;
}

// PlayCanvas packs a normalized triple as 11 + 10 + 11 bits.
std::array<float, 3> unpackTriple(std::uint32_t value) {
    const std::uint32_t high = (value >> tripleHighShift) & elevenBitMask;
    const std::uint32_t middle = (value >> tripleMidShift) & tenBitMask;
    const std::uint32_t low = value & elevenBitMask;
    return {float(high) / elevenBitMask, float(middle) / tenBitMask, float(low) / elevenBitMask};
}

// Smallest-three quaternion: three 10-bit components plus a 2-bit selector for the dropped one.
float unpackQuaternionComponent(std::uint32_t value, std::uint32_t shift) {
    return (float((value >> shift) & tenBitMask) / tenBitMask - .5f) * std::sqrt(2.f);
}

std::array<float, 4> unpackQuaternion(std::uint32_t value) {
    const float a = unpackQuaternionComponent(value, quaternionHighShift);
    const float b = unpackQuaternionComponent(value, quaternionMidShift);
    const float c = unpackQuaternionComponent(value, 0);
    const float missing = std::sqrt(std::max(0.f, 1 - a * a - b * b - c * c));
    switch (value >> quaternionSelectorShift) {
    case 0:
        return {a, b, c, missing};
    case 1:
        return {missing, b, c, a};
    case 2:
        return {b, missing, c, a};
    default:
        return {b, c, missing, a};
    }
}

void requireInRange(float value, float limit, const char* message) {
    if (std::abs(value) > limit) {
        throw std::runtime_error(message);
    }
}

void decodeSplats(std::span<const std::byte> bytes,
                  const Header& header,
                  const std::vector<float>& frames,
                  StaticScene& scene) {
    const auto vertexBase = header.payloadOffset + header.chunks * chunkBytes;
    for (std::size_t splat = 0; splat < header.count; ++splat) {
        const auto base = vertexBase + splat * vertexBytes;
        const auto position = unpackTriple(readWord(bytes, base));
        const auto scale = unpackTriple(readWord(bytes, base + scaleOffset));
        const std::uint32_t rotation = readWord(bytes, base + rotationOffset);
        const std::uint32_t color = readWord(bytes, base + colorOffset);
        const float* bounds = frames.data() + (splat / gaussiansPerChunk) * chunkFloatCount;

        for (int axis = 0; axis < 3; ++axis) {
            const float channel = float((color >> (24 - fullByteShift * axis)) & eightBitMask) / eightBitMask;
            scene.positions[splat * 3 + axis] = std::lerp(bounds[axis], bounds[3 + axis], position[axis]);
            scene.logScales[splat * 3 + axis] = std::lerp(bounds[6 + axis], bounds[9 + axis], scale[axis]);
            scene.sh0[splat * 3 + axis] =
                (std::lerp(bounds[12 + axis], bounds[15 + axis], channel) - .5f) / shDcConstant;
            requireInRange(scene.positions[splat * 3 + axis],
                           positionRangeLimit,
                           "PLY position outside supported range");
            requireInRange(
                scene.logScales[splat * 3 + axis], logScaleRangeLimit, "PLY scale outside supported range");
        }

        const float alpha =
            std::clamp(float(color & eightBitMask) / eightBitMask, opacityEpsilon, 1 - opacityEpsilon);
        scene.opacityLogits[splat] = std::log(alpha / (1 - alpha));

        // The scene stores wxyz while the packed encoding orders xyzw.
        const auto xyzw = unpackQuaternion(rotation);
        scene.rotations[splat * 4] = xyzw[3];
        for (int axis = 0; axis < 3; ++axis) {
            scene.rotations[splat * 4 + axis + 1] = xyzw[axis];
        }
    }
}

void fitRobustBounds(StaticScene& scene, std::size_t count) {
    // Robust framing excludes position outliers only from camera fitting; all Gaussians are rendered.
    std::array<float, 3> extent{};
    std::vector<float> axis(count);
    for (int component = 0; component < 3; ++component) {
        for (std::size_t splat = 0; splat < count; ++splat) {
            axis[splat] = scene.positions[3 * splat + component];
        }
        std::sort(axis.begin(), axis.end());
        const float low = axis[count / outlierTrim];
        const float high = axis[count - 1 - count / outlierTrim];
        scene.center[component] = (low + high) * .5f;
        extent[component] = (high - low) * .5f;
    }
    scene.radius = std::max(minimumRadius,
                            std::sqrt(extent[0] * extent[0] + extent[1] * extent[1] + extent[2] * extent[2]));
}
} // namespace

StaticScene decodeCompressedPly(std::span<const std::byte> bytes) {
    if (bytes.empty()) {
        throw std::runtime_error("Empty PLY file");
    }
    const std::string_view input(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    const auto header = readHeader(input);
    const auto frames = readChunkFrames(bytes, header);

    StaticScene scene;
    scene.positions.resize(header.count * 3);
    scene.rotations.resize(header.count * 4);
    scene.logScales.resize(header.count * 3);
    scene.opacityLogits.resize(header.count);
    scene.sh0.resize(header.count * 3);

    decodeSplats(bytes, header, frames, scene);
    fitRobustBounds(scene, header.count);
    return scene;
}
} // namespace orbelio
