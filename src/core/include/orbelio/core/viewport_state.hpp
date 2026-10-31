#pragma once

#include "types.hpp"
#include <algorithm>
#include <utility>

namespace orbelio {
// One owner thread mutates state; adapters must freeze it while copying a frame snapshot.
class ViewportState {
public:
    struct ContentSnapshot {
        std::shared_ptr<const StaticScene> scene;
        std::optional<double> sequenceTime;
        std::shared_ptr<const Fdu4dScene> fdu4d;
    };

    ContentSnapshot content() const { return {scene_, sequenceTime_, fdu4d_}; }

    void setScene(std::shared_ptr<const StaticScene> scene) {
        const bool sceneChanged = scene_ != scene || bool(fdu4d_);
        if (sceneChanged || sequenceTime_) {
            scene_ = std::move(scene);
            fdu4d_.reset();
            sequenceTime_.reset();
            ++sceneVersion_;
            if (sceneChanged) {
                resetCamera();
            }
        }
    }

    void setSequenceFrame(std::shared_ptr<const StaticScene> scene, double timestamp) {
        if (scene_ != scene || fdu4d_ || sequenceTime_ != timestamp) {
            scene_ = std::move(scene);
            fdu4d_.reset();
            sequenceTime_ = timestamp;
            ++sceneVersion_;
        }
    }

    void setFdu4d(std::shared_ptr<const Fdu4dScene> scene) {
        const bool changed = scene_ || fdu4d_ != scene;
        scene_.reset();
        fdu4d_ = std::move(scene);
        sequenceTime_ = fdu4d_ ? std::optional(fdu4d_->startTime) : std::nullopt;
        ++sceneVersion_;
        if (changed) {
            resetCamera();
        }
    }

    void setFduTime(double time) {
        if (!fdu4d_) {
            return;
        }
        time = fdu4d_->clampTime(time);
        if (sequenceTime_ != time) {
            sequenceTime_ = time;
            ++sceneVersion_;
        }
    }

    void restoreContent(const ContentSnapshot& previous) {
        scene_ = previous.scene;
        sequenceTime_ = previous.sequenceTime;
        fdu4d_ = previous.fdu4d;
        ++sceneVersion_;
    }

    unsigned shDegree() const { return spatial() ? spatial()->shDegree : 0; }

    bool hasScene() const { return scene_ || fdu4d_; }

    std::uint64_t sceneVersion() const { return sceneVersion_; }

    std::size_t gaussianCount() const { return spatial() ? spatial()->count() : 0; }

    void orbitCamera(float yawDegrees, float pitchDegrees) { camera_.orbit(yawDegrees, pitchDegrees); }

    void panCamera(float horizontalFraction, float verticalFraction) {
        camera_.pan(horizontalFraction, verticalFraction);
    }

    void zoomCamera(float steps) { camera_.zoom(steps); }

    const CameraState& camera() const { return camera_; }

    void setCamera(const CameraState& camera) { camera_ = camera; }

    std::uint64_t nextCaptureRequest() {
        captureSerial_ = std::max(captureSerial_, captureRequestId_) + 1;
        captureRequestId_ = captureSerial_;
        return captureRequestId_;
    }

    void fitCamera() {
        if (spatial()) {
            camera_.fit(spatial()->center, spatial()->radius);
        }
    }

    void resetCamera() {
        camera_.reset(spatial() ? spatial()->center : std::array<float, 3>{},
                      spatial() ? spatial()->radius : 1.f);
    }

    void requestCapture(std::uint64_t requestId) { captureRequestId_ = requestId; }

    const RenderSettings& renderSettings() const { return renderSettings_; }

    void setRenderSettings(RenderSettings settings) { renderSettings_ = settings; }

    FrameState frame(double time, std::uint32_t width, std::uint32_t height) const {
        return {.scene = scene_,
                .fdu4d = fdu4d_,
                .sceneVersion = sceneVersion_,
                .camera = camera_,
                .captureRequestId = captureRequestId_,
                .timestampSeconds = sequenceTime_.value_or(time),
                .width = std::max(1u, width),
                .height = std::max(1u, height),
                .renderSettings = renderSettings_};
    }

private:
    const StaticScene* spatial() const { return fdu4d_ ? &fdu4d_->spatial : scene_.get(); }

    std::shared_ptr<const StaticScene> scene_;
    std::shared_ptr<const Fdu4dScene> fdu4d_;
    std::uint64_t sceneVersion_ = 0;
    CameraState camera_{};
    std::uint64_t captureRequestId_ = 0;
    std::uint64_t captureSerial_ = 0;
    std::optional<double> sequenceTime_;
    RenderSettings renderSettings_;
};
}
