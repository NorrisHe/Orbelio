#pragma once

#include "types.hpp"
#include <algorithm>
#include <utility>

namespace orbelio {
// One owner thread mutates state; adapters must freeze it while copying a frame snapshot.
class ViewportState {
public:
    void setScene(std::shared_ptr<const StaticScene> scene) { scene_ = std::move(scene); }

    void requestCapture(std::uint64_t requestId) { captureRequestId_ = requestId; }

    FrameState frame(double time, std::uint32_t width, std::uint32_t height) const {
        return {.scene = scene_,
                .captureRequestId = captureRequestId_,
                .timestampSeconds = time,
                .width = std::max(1u, width),
                .height = std::max(1u, height)};
    }

private:
    std::shared_ptr<const StaticScene> scene_;
    std::uint64_t captureRequestId_ = 0;
};
}
