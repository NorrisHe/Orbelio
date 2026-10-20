#pragma once

#include <orbelio/core/viewport_state.hpp>
#include <renderer.hpp>
#include <renderer_stats.hpp>
#include <QObject>
#include <QString>

namespace orbelio {
class ViewportBridge final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool freeCamera READ freeCamera WRITE setFreeCamera NOTIFY cameraModeChanged)
public:
    ViewportBridge(ViewportState& state, std::shared_ptr<const IVulkanRendererFactory> rendererFactory);
    FrameState frame(double time, std::uint32_t width, std::uint32_t height) const;

    std::shared_ptr<const IVulkanRendererFactory> rendererFactory() const { return rendererFactory_; }

    void publish(FrameReport report, std::string error = {});
    void requestFrame();
    void refreshCamera();

    bool freeCamera() const { return state_.camera().freeFlight; }

    void setFreeCamera(bool enabled);
    void restoreCamera(const CameraState& camera);

    Q_INVOKABLE void moveCamera(float right, float up, float forward, float seconds, bool fast) {
        auto camera = state_.camera();
        camera.moveLocal(right, up, forward, seconds, fast);
        state_.setCamera(camera);
        requestFrame();
    }

    Q_INVOKABLE void orbitCamera(float yawDegrees, float pitchDegrees) {
        state_.orbitCamera(yawDegrees, pitchDegrees);
        requestFrame();
    }

    Q_INVOKABLE void panCamera(float horizontalFraction, float verticalFraction) {
        state_.panCamera(horizontalFraction, verticalFraction);
        requestFrame();
    }

    Q_INVOKABLE void zoomCamera(float steps) {
        state_.zoomCamera(steps);
        requestFrame();
    }

    Q_INVOKABLE void fitCamera() {
        state_.fitCamera();
        requestFrame();
    }

    Q_INVOKABLE void resetCamera() {
        state_.resetCamera();
        refreshCamera();
    }

Q_SIGNALS:
    void frameCompleted(const FrameReport& report, const QString& error);
    void frameRequested();
    void cameraModeChanged();

private:
    ViewportState& state_;
    std::shared_ptr<const IVulkanRendererFactory> rendererFactory_;
};
}
