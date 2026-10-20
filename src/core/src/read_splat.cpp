#include "model_decode_internal.hpp"

namespace orbelio::detail {
namespace {
constexpr std::size_t recordBytes = 32;
constexpr std::size_t scaleOffset = 12;
constexpr std::size_t colorOffset = 24;
constexpr std::size_t rotationOffset = 28;
constexpr float byteMaximum = 255.f;
}

StaticScene readSplat(std::span<const std::byte> data, DecodeContext& context) {
    require(data.size() % recordBytes == 0, "SPLAT size must be a multiple of 32 bytes");
    const Bytes bytes(data);
    auto scene = context.createScene(data.size() / recordBytes, 0, "SPLAT");
    for (std::size_t i = 0; i < scene.count(); ++i) {
        const auto at = i * recordBytes;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            scene.positions[i * 3 + axis] = bytes.f32(at + axis * sizeof(float));
            scene.logScales[i * 3 + axis] = std::log(bytes.f32(at + scaleOffset + axis * sizeof(float)));
            scene.sh0[i * 3 + axis] = (bytes.u8(at + colorOffset + axis) / byteMaximum - .5f) / shDc;
        }
        scene.opacityLogits[i] = logit(bytes.u8(at + colorOffset + 3) / byteMaximum);
        for (std::size_t axis = 0; axis < 4; ++axis) {
            scene.rotations[i * 4 + axis] = bytes.u8(at + rotationOffset + axis) / byteMaximum * 2.f - 1.f;
        }
    }
    return scene;
}
}
