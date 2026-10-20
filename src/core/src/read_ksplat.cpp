#include "model_decode_internal.hpp"

namespace orbelio::detail {
namespace {
constexpr std::size_t fileHeaderBytes = 4096;
constexpr std::size_t sectionHeaderBytes = 1024;
constexpr float byteMaximum = 255.f;
constexpr std::uint32_t defaultQuantizationRange = 32767;
constexpr float defaultShRange = 1.5f;

namespace mainField {
constexpr std::size_t major = 0;
constexpr std::size_t minor = 1;
constexpr std::size_t sections = 4;
constexpr std::size_t count = 16;
constexpr std::size_t compression = 20;
constexpr std::size_t shMin = 36;
constexpr std::size_t shMax = 40;
}

namespace sectionField {
constexpr std::size_t count = 0;
constexpr std::size_t capacity = 4;
constexpr std::size_t bucketCapacity = 8;
constexpr std::size_t buckets = 12;
constexpr std::size_t blockSize = 16;
constexpr std::size_t bucketStride = 20;
constexpr std::size_t quantRange = 24;
constexpr std::size_t fullBuckets = 32;
constexpr std::size_t partialBuckets = 36;
constexpr std::size_t degree = 40;
}

struct RecordLayout {
    std::size_t scales;
    std::size_t rotations;
    std::size_t color;
    std::size_t sh;
    std::size_t geometricBytes;
    std::size_t shBytes;
};

constexpr std::array<RecordLayout, 3> recordLayouts = {
    {{12, 24, 40, 44, 4, 4}, {6, 12, 20, 24, 2, 2}, {6, 12, 20, 24, 2, 1}}};
constexpr std::size_t bucketCenterBytes = 3 * sizeof(float);
constexpr std::size_t firstBandValues = 9;
constexpr std::size_t secondBandValues = 24;

struct Section {
    std::size_t count = 0;
    std::size_t capacity = 0;
    std::size_t bucketCapacity = 0;
    std::size_t bucketCount = 0;
    float blockSize = 0;
    std::size_t bucketStride = 0;
    std::uint32_t quantRange = 0;
    std::size_t fullBuckets = 0;
    std::size_t partialBuckets = 0;
    unsigned degree = 0;
    std::size_t stride = 0;
};

Section readSection(const Bytes& bytes, std::size_t at, unsigned mode) {
    Section section{};
    section.count = bytes.u32(at + sectionField::count);
    section.capacity = bytes.u32(at + sectionField::capacity);
    section.bucketCapacity = bytes.u32(at + sectionField::bucketCapacity);
    section.bucketCount = bytes.u32(at + sectionField::buckets);
    section.blockSize = bytes.f32(at + sectionField::blockSize);
    section.bucketStride = bytes.u16(at + sectionField::bucketStride);
    section.quantRange = bytes.u32(at + sectionField::quantRange);
    if (!section.quantRange) {
        section.quantRange = defaultQuantizationRange;
    }
    section.fullBuckets = bytes.u32(at + sectionField::fullBuckets);
    section.partialBuckets = bytes.u32(at + sectionField::partialBuckets);
    section.degree = bytes.u16(at + sectionField::degree);
    require(section.degree <= 3 && section.count <= section.capacity,
            "Invalid KSPLAT section count/SH degree");
    const auto components = section.degree * (section.degree + 2) * 3;
    const auto& wire = recordLayouts[mode];
    section.stride = wire.sh + components * wire.shBytes;
    if (mode && section.count) {
        require(section.bucketCapacity && section.bucketStride == bucketCenterBytes &&
                    section.fullBuckets + section.partialBuckets == section.bucketCount &&
                    std::isfinite(section.blockSize) && section.blockSize > 0,
                "Invalid KSPLAT quantization buckets");
    }
    return section;
}

std::size_t sectionPrefix(const Section& section) {
    return checkedProduct(section.partialBuckets, sizeof(std::uint32_t)) +
           checkedProduct(section.bucketCount, section.bucketStride);
}

struct Layout {
    unsigned mode = 0;
    unsigned degree = 0;
    std::size_t count = 0;
    std::size_t sections = 0;
    std::size_t dataOffset = 0;
    float shMin = -defaultShRange;
    float shMax = defaultShRange;
};

Layout readLayout(const Bytes& bytes) {
    bytes.slice(0, fileHeaderBytes);
    require(bytes.u8(mainField::major) == 0 && bytes.u8(mainField::minor) >= 1,
            "Unsupported KSPLAT version (expected 0.1+)");
    Layout layout{};
    layout.sections = bytes.u32(mainField::sections);
    layout.count = bytes.u32(mainField::count);
    layout.mode = bytes.u16(mainField::compression);
    require(layout.sections && layout.mode < recordLayouts.size(),
            "Invalid KSPLAT sections/compression mode");
    layout.dataOffset = fileHeaderBytes + checkedProduct(layout.sections, sectionHeaderBytes);
    bytes.slice(0, layout.dataOffset);
    if (bytes.f32(mainField::shMin) != 0) {
        layout.shMin = bytes.f32(mainField::shMin);
    }
    if (bytes.f32(mainField::shMax) != 0) {
        layout.shMax = bytes.f32(mainField::shMax);
    }
    std::size_t total = 0;
    auto end = layout.dataOffset;
    for (std::size_t i = 0; i < layout.sections; ++i) {
        const auto section = readSection(bytes, fileHeaderBytes + i * sectionHeaderBytes, layout.mode);
        total += section.count;
        require(total <= layout.count, "KSPLAT section count mismatch");
        layout.degree = std::max(layout.degree, section.degree);
        const auto size = sectionPrefix(section) + checkedProduct(section.capacity, section.stride);
        bytes.slice(end, size);
        end += size;
    }
    require(total == layout.count && end == bytes.size(), "Truncated/trailing KSPLAT payload");
    return layout;
}

void decodeRecord(const Bytes& bytes,
                  std::size_t at,
                  const Layout& layout,
                  StaticScene& scene,
                  std::size_t index) {
    const auto mode = layout.mode;
    const auto& wire = recordLayouts[mode];
    const auto color = at + wire.color;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const auto scale = mode ? bytes.f16(at + wire.scales + axis * wire.geometricBytes)
                                : bytes.f32(at + wire.scales + axis * wire.geometricBytes);
        scene.logScales[index * 3 + axis] = std::log(scale);
        scene.sh0[index * 3 + axis] = (bytes.u8(color + axis) / byteMaximum - .5f) / shDc;
        if (!mode) {
            scene.positions[index * 3 + axis] = bytes.f32(at + axis * 4);
        }
    }
    scene.opacityLogits[index] = logit(bytes.u8(color + 3) / byteMaximum);
    for (std::size_t axis = 0; axis < 4; ++axis) {
        scene.rotations[index * 4 + axis] = mode
                                                ? bytes.f16(at + wire.rotations + axis * wire.geometricBytes)
                                                : bytes.f32(at + wire.rotations + axis * wire.geometricBytes);
    }
}

void decodeSh(const Bytes& bytes,
              std::size_t at,
              const Section& section,
              const Layout& layout,
              StaticScene& scene,
              std::size_t index) {
    const auto base = at + recordLayouts[layout.mode].sh;
    const auto count = section.degree * (section.degree + 2) * 3;
    for (std::size_t i = 0; i < count; ++i) {
        const auto band = i < firstBandValues ? 0uz : i < secondBandValues ? 9uz : 24uz;
        const auto width = i < firstBandValues ? 3uz : i < secondBandValues ? 5uz : 7uz;
        const auto coefficient = (i - band) % width + band / 3;
        const auto channel = (i - band) / width;
        const auto value = layout.mode == 2
                               ? std::lerp(layout.shMin, layout.shMax, bytes.u8(base + i) / byteMaximum)
                           : layout.mode ? bytes.f16(base + i * 2)
                                         : bytes.f32(base + i * 4);
        scene.shRest[index * scene.shRestCoefficients() * 3 + coefficient * 3 + channel] = value;
    }
}

void decodeSection(const Bytes& bytes,
                   std::size_t start,
                   const Section& section,
                   const Layout& layout,
                   StaticScene& scene,
                   std::size_t first) {
    const auto centers = start + section.partialBuckets * sizeof(std::uint32_t);
    const auto records = start + sectionPrefix(section);
    auto bucket = 0uz;
    auto bucketEnd = section.fullBuckets      ? section.bucketCapacity
                     : section.partialBuckets ? bytes.u32(start)
                                              : 0uz;
    for (std::size_t i = 0; i < section.count; ++i) {
        const auto at = records + i * section.stride;
        decodeRecord(bytes, at, layout, scene, first + i);
        if (layout.mode) {
            while (i >= bucketEnd && bucket < section.bucketCount) {
                ++bucket;
                bucketEnd += bucket < section.fullBuckets ? section.bucketCapacity
                             : bucket < section.bucketCount
                                 ? bytes.u32(start + (bucket - section.fullBuckets) * 4)
                                 : 0;
            }
            require(bucket < section.bucketCount, "KSPLAT bucket capacity mismatch");
            for (std::size_t axis = 0; axis < 3; ++axis) {
                scene.positions[(first + i) * 3 + axis] =
                    (float(bytes.u16(at + axis * 2)) - section.quantRange) *
                        (section.blockSize * .5f / section.quantRange) +
                    bytes.f32(centers + bucket * section.bucketStride + axis * 4);
            }
        }
        decodeSh(bytes, at, section, layout, scene, first + i);
    }
}
}

StaticScene readKsplat(std::span<const std::byte> data, DecodeContext& context) {
    const Bytes bytes(data);
    const auto layout = readLayout(bytes);
    auto scene = context.createScene(layout.count, layout.degree, "KSPLAT");
    auto at = layout.dataOffset;
    auto first = 0uz;
    for (std::size_t i = 0; i < layout.sections; ++i) {
        const auto section = readSection(bytes, fileHeaderBytes + i * sectionHeaderBytes, layout.mode);
        decodeSection(bytes, at, section, layout, scene, first);
        first += section.count;
        at += sectionPrefix(section) + section.capacity * section.stride;
    }
    return scene;
}
}
