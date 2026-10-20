#pragma once

#include "types.hpp"
#include <algorithm>
#include <utility>

namespace orbelio {
// One owner thread mutates state; adapters must freeze it while copying a frame snapshot.
class ViewportState {
public:
    void setScene(std::shared_ptr<const StaticScene> scene) {
        if (scene_ != scene) {
            scene_ = std::move(scene);
            ++sceneVersion_;
            resetCamera();
        }
    }

    void restore(const ViewportState& previous) {
        scene_ = previous.scene_;
        camera_ = previous.camera_;
        ++sceneVersion_;
    }

    unsigned shDegree() const { return scene_ ? scene_->shDegree : 0; }

    std::string sceneFormat() const { return scene_ ? scene_->format : std::string{}; }

    bool hasScene() const { return static_cast<bool>(scene_); }

    std::uint64_t sceneVersion() const { return sceneVersion_; }

    std::size_t gaussianCount() const { return scene_ ? scene_->count() : 0; }

    void orbitCamera(float yawDegrees, float pitchDegrees) { camera_.orbit(yawDegrees, pitchDegrees); }

    void panCamera(float horizontalFraction, float verticalFraction) {
        camera_.pan(horizontalFraction, verticalFraction);
    }

    void zoomCamera(float steps) { camera_.zoom(steps); }

    void fitCamera() {
        if (scene_) {
            camera_.fit(scene_->center, scene_->radius);
        }
    }

    void resetCamera() {
        camera_.reset(scene_ ? scene_->center : std::array<float, 3>{}, scene_ ? scene_->radius : 1.f);
    }

    void requestCapture(std::uint64_t requestId) { captureRequestId_ = requestId; }

    FrameState frame(double time, std::uint32_t width, std::uint32_t height) const {
        return {.scene = scene_,
                .sceneVersion = sceneVersion_,
                .camera = camera_,
                .captureRequestId = captureRequestId_,
                .timestampSeconds = time,
                .width = std::max(1u, width),
                .height = std::max(1u, height)};
    }

private:
    std::shared_ptr<const StaticScene> scene_;
    std::uint64_t sceneVersion_ = 0;
    CameraState camera_{};
    std::uint64_t captureRequestId_ = 0;
};
}
