#pragma once

#include <orbelio/core/viewport_state.hpp>
#include <renderer.hpp>
#include <renderer_stats.hpp>
#include <QObject>
#include <QString>

namespace orbelio {
class ViewportBridge final : public QObject {
    Q_OBJECT
public:
    ViewportBridge(ViewportState& state, std::shared_ptr<const IVulkanRendererFactory> rendererFactory);
    FrameState frame(double time, std::uint32_t width, std::uint32_t height) const;

    std::shared_ptr<const IVulkanRendererFactory> rendererFactory() const { return rendererFactory_; }

    void publish(FrameReport report, std::string error = {});
    void requestFrame();

Q_SIGNALS:
    void frameCompleted(const FrameReport& report, const QString& error);
    void frameRequested();

private:
    ViewportState& state_;
    std::shared_ptr<const IVulkanRendererFactory> rendererFactory_;
};
}
