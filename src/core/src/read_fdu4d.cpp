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

struct FduLayout {
    std::size_t stride = 0;
    Fdu4dScene scene;
};

FduLayout readLayout(std::span<const std::byte> header, std::size_t fileBytes, DecodeContext& context) {
    const Bytes input(header);
    require(fileBytes >= fduHeaderBytes && header.size() >= fduHeaderBytes &&
                std::memcmp(header.data(), "FDU4DGS\0", 8) == 0,
            "Invalid FDU 4DGS intermediate header");
    require(input.u32(8) == 1 && input.u32(12) == fduHeaderBytes && input.u64(32) == 0,
            "Unsupported FDU 4DGS intermediate version or flags");
    const auto count = input.u64(16);
    require(std::in_range<std::size_t>(count), "FDU Gaussian count overflow");
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
    require(checkedProduct(count, stride) == fileBytes - fduHeaderBytes, "Invalid FDU payload length");
    context.checkWorking(checkedProduct(count, stride));
    scene.spatial = context.createScene(count, degree, "FDU 4DGS");
    allocateTemporal(scene, context);
    return {stride, std::move(scene)};
}

void readRows(std::span<const std::byte> data, Fdu4dScene& scene, std::size_t start, std::size_t count) {
    const Bytes input(data);
    std::size_t offset = 0;
    for (std::size_t index = start; index < start + count; ++index) {
        readGaussian(input, offset, scene, index);
    }
}
}

Fdu4dScene readFdu4d(std::span<const std::byte> data, DecodeContext& context) {
    auto layout = readLayout(data, data.size(), context);
    auto& scene = layout.scene;
    readRows(data.subspan(fduHeaderBytes), scene, 0, scene.count());
    finishDecodedScene(scene.spatial);
    return std::move(scene);
}

Fdu4dScene readFdu4d(IAssetStream& stream, BytePool& pool, DecodeContext& context) {
    std::array<std::byte, fduHeaderBytes> header{};
    require(stream.size() >= header.size(), "Invalid FDU 4DGS intermediate header");
    stream.read(header);
    auto layout = readLayout(header, stream.size(), context);
    auto& scene = layout.scene;
    const auto stride = layout.stride;
    const auto batch =
        std::min(scene.count(), std::min(context.limits.inputBytes, IAssetStream::blockBytes) / stride);
    require(batch != 0, "FDU input pool budget cannot hold a Gaussian record");
    auto buffer = pool.acquire(batch * stride);
    for (std::size_t start = 0; start < scene.count();) {
        const auto count = std::min(batch, scene.count() - start);
        const auto bytes = buffer.bytes().first(count * stride);
        stream.read(bytes);
        readRows(bytes, scene, start, count);
        start += count;
    }
    buffer = {};
    finishDecodedScene(scene.spatial);
    return std::move(scene);
}
}
