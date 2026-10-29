#include "model_decode_internal.hpp"
#include <orbelio/core/fdu4d_scene.hpp>

namespace orbelio::detail {
namespace {
constexpr std::size_t fduHeaderBytes = 64;
constexpr std::size_t fduBaseFloats = 16;
constexpr double maximumSamples = 1'000'000;
constexpr float maximumParameter = 1e10f;

void readValues(const Bytes& input, std::size_t& offset, std::span<float> destination) {
    for (auto& value : destination) {
        value = input.f32(offset);
        require(std::isfinite(value) && std::abs(value) <= maximumParameter, "Invalid FDU parameter");
        offset += sizeof(float);
    }
}

void readGaussian(const Bytes& input, std::size_t& offset, Fdu4dScene& scene, std::size_t index) {
    readValues(input, offset, std::span(scene.spatial.positions).subspan(index * 3, 3));
    readValues(input, offset, std::span(scene.times).subspan(index, 1));
    readValues(input, offset, std::span(scene.spatial.logScales).subspan(index * 3, 3));
    readValues(input, offset, std::span(scene.spatial.rotations).subspan(index * 4, 4));
    readValues(input, offset, std::span(scene.velocities).subspan(index * 3, 3));
    readValues(input, offset, std::span(scene.variances).subspan(index, 1));
    require(scene.variances[index] > 0, "FDU temporal variance must be positive");
    readValues(input, offset, std::span(scene.spatial.opacityLogits).subspan(index, 1));
    readValues(input, offset, std::span(scene.spatial.sh0).subspan(index * 3, 3));
    const auto rest = scene.spatial.shRestCoefficients() * 3;
    readValues(input, offset, std::span(scene.spatial.shRest).subspan(index * rest, rest));
    const auto temporal = (rest + 3) * scene.temporalDegree;
    readValues(input, offset, std::span(scene.temporalSh).subspan(index * temporal, temporal));
}

void allocateTemporal(Fdu4dScene& scene, DecodeContext& context) {
    const auto count = scene.count();
    const auto coefficients = (scene.spatial.shDegree + 1) * (scene.spatial.shDegree + 1);
    const auto temporalFloats = 5 + coefficients * scene.temporalDegree * 3;
    const auto bytes = checkedProduct(count, temporalFloats * sizeof(float));
    context.checkWorking(bytes);
    scene.temporalMemory = context.resident.reserve(bytes);
    context.outputBytes += bytes;
    scene.resident = context.resident;
    scene.limits = context.limits;
    scene.times.resize(count);
    scene.variances.resize(count);
    scene.velocities.resize(count * 3);
    scene.temporalSh.resize(count * coefficients * scene.temporalDegree * 3);
}
}

Fdu4dScene readFdu4d(std::span<const std::byte> data, DecodeContext& context) {
    const Bytes input(data);
    require(data.size() >= fduHeaderBytes && std::memcmp(data.data(), "FDU4DGS\0", 8) == 0,
            "Invalid FDU 4DGS intermediate header");
    require(input.u32(8) == 1 && input.u32(12) == fduHeaderBytes && input.u64(32) == 0,
            "Unsupported FDU 4DGS intermediate version or flags");
    const auto count = input.u64(16);
    const auto degree = input.u32(24);
    Fdu4dScene scene{};
    scene.temporalDegree = input.u32(28);
    scene.startTime = input.read<double>(40);
    scene.endTime = input.read<double>(48);
    scene.sampleRate = input.read<double>(56);
    require(std::isfinite(scene.startTime) && std::isfinite(scene.endTime) &&
                scene.endTime > scene.startTime && std::isfinite(scene.sampleRate) && scene.sampleRate > 0 &&
                (scene.endTime - scene.startTime) * scene.sampleRate <= maximumSamples,
            "Invalid FDU time range or sampling rate");
    require(degree <= 3 && scene.temporalDegree <= 2 && (!scene.temporalDegree || degree == 3),
            "Unsupported FDU SH layout (temporal SH requires spatial degree 3)");
    const auto coefficients = (degree + 1) * (degree + 1);
    const auto stride = (fduBaseFloats + coefficients * (scene.temporalDegree + 1) * 3) * sizeof(float);
    require(checkedProduct(count, stride) == data.size() - fduHeaderBytes, "Invalid FDU payload length");
    scene.spatial = context.createScene(count, degree, "FDU 4DGS");
    allocateTemporal(scene, context);
    auto offset = fduHeaderBytes;
    for (std::size_t index = 0; index < count; ++index) {
        readGaussian(input, offset, scene, index);
    }
    finishDecodedScene(scene.spatial);
    return scene;
}
}
