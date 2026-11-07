#include "native_capture.hpp"
#include <cstring>
#include <limits>
#include <stdexcept>

namespace orbelio {
namespace {
constexpr std::size_t rgbaBytes = 4;
constexpr std::size_t floatRgbaBytes = 4 * sizeof(float);

void copyFloats(std::vector<float>& target, const std::byte* source, std::size_t bytes) {
    target.resize(bytes / sizeof(float));
    std::memcpy(target.data(), source, bytes);
}
}

NativeCapturePlan nativeCapturePlan(const FrameState& frame) {
    if (!validOutputs(frame.captureOutputs)) {
        throw std::invalid_argument("Invalid capture outputs");
    }
    const auto pixels = std::uint64_t(frame.width) * frame.height;
    constexpr auto maximumPixels =
        (std::numeric_limits<std::size_t>::max() - floatRgbaBytes) / (rgbaBytes + 3 * floatRgbaBytes);
    if (!pixels || pixels > maximumPixels) {
        throw std::length_error("Invalid capture dimensions");
    }
    NativeCapturePlan result{};
    for (const auto output :
         {RenderOutput::Display, RenderOutput::FloatColorAlpha, RenderOutput::Depth, RenderOutput::Normals}) {
        if (!hasOutput(frame.captureOutputs, output)) {
            continue;
        }
        const auto pixelBytes = output == RenderOutput::Display ? rgbaBytes : floatRgbaBytes;
        // Buffer-image offsets must align to a complete texel, including after an odd RGBA8 plane.
        const auto offset = (result.bytes + pixelBytes - 1) / pixelBytes * pixelBytes;
        const auto bytes = std::size_t(pixels) * pixelBytes;
        result.planes[result.count++] = {output, offset, bytes};
        result.bytes = offset + bytes;
    }
    if (result.bytes > frame.captureBudgetBytes) {
        throw std::length_error("Capture exceeds captureBudgetBytes");
    }
    return result;
}

std::shared_ptr<const FrameCapture> nativeReadCapture(const FrameState& frame,
                                                      const NativeCapturePlan& plan,
                                                      const void* mapped) {
    auto result = std::make_shared<FrameCapture>();
    result->requestId = frame.captureRequestId;
    result->width = frame.width;
    result->height = frame.height;
    result->outputs = frame.captureOutputs;
    for (std::size_t i = 0; i < plan.count; ++i) {
        const auto& plane = plan.planes[i];
        const auto* source = static_cast<const std::byte*>(mapped) + plane.offset;
        if (plane.output == RenderOutput::Display) {
            result->rgba.resize(plane.bytes);
            std::memcpy(result->rgba.data(), source, plane.bytes);
        } else if (plane.output == RenderOutput::Normals) {
            copyFloats(result->normals, source, plane.bytes);
        } else {
            copyFloats(plane.output == RenderOutput::Depth ? result->depth : result->floatColorAlpha,
                       source,
                       plane.bytes);
        }
    }
    return result;
}
}
