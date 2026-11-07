#pragma once
#include <orbelio/core/types.hpp>
#include <array>

namespace orbelio {
struct NativeCapturePlane {
    RenderOutput output = RenderOutput::Display;
    std::size_t offset = 0;
    std::size_t bytes = 0;
};

struct NativeCapturePlan {
    std::array<NativeCapturePlane, 4> planes{};
    std::size_t count = 0;
    std::size_t bytes = 0;
};

NativeCapturePlan nativeCapturePlan(const FrameState& frame);
std::shared_ptr<const FrameCapture> nativeReadCapture(const FrameState& frame,
                                                      const NativeCapturePlan& plan,
                                                      const void* mapped);
}
