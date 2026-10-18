#include "viewport_bridge.hpp"
#include <stdexcept>
#include <utility>

namespace orbelio {
ViewportBridge::ViewportBridge(ViewportState& state,
                               std::shared_ptr<const IVulkanRendererFactory> rendererFactory)
    : state_(state), rendererFactory_(std::move(rendererFactory)) {
    if (!rendererFactory_) {
        throw std::invalid_argument("Viewport requires a renderer factory");
    }
}

FrameState ViewportBridge::frame(double time, std::uint32_t width, std::uint32_t height) const {
    // Read only at Qt's blocked-GUI sync boundary.
    return state_.frame(time, width, height);
}

void ViewportBridge::publish(FrameReport report, std::string error) {
    // Invoking through a context QObject drops queued work when it is destroyed.
    QMetaObject::invokeMethod(
        this,
        [this, report = std::move(report), error = std::move(error)] {
            Q_EMIT frameCompleted(report, QString::fromStdString(error));
        },
        Qt::QueuedConnection);
}

void ViewportBridge::requestFrame() { Q_EMIT frameRequested(); }
}
