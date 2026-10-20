#include "viewport_item.hpp"
#include <QPointer>
#include <QQuickWindow>
#include <QSGSimpleTextureNode>
#include <QThread>
#include <QtQuick/qsgtexture_platform.h>
#include <cmath>
#include <memory>

namespace orbelio {
namespace {
class ViewportNode final : public QObject, public QSGSimpleTextureNode {
public:
    ViewportNode(QQuickWindow* window, ViewportBridge& bridge)
        : window_(window), bridge_(&bridge), rendererFactory_(bridge.rendererFactory()) {
        setOwnsTexture(false);
        connect(window, &QQuickWindow::beforeRendering, this, [this] { prepare(); }, Qt::DirectConnection);
        connect(window, &QQuickWindow::afterFrameEnd, this, [this] { finish(); }, Qt::DirectConnection);
    }

    ~ViewportNode() override {
        // Destruction is on the render thread after Qt has stopped using the node.
        if (renderer_) {
            renderer_->waitIdle();
        }
        texture_.reset();
        renderer_.reset();
    }

    void setFrame(const FrameState& state, const QRectF& rect) {
        state_ = state;
        setRect(rect);
        setSourceRect(0, 0, state.width, state.height);
    }

    bool isSubtreeBlocked() const override { return failed_ || !texture_; }

    bool usesBridge(const ViewportBridge* bridge) const { return bridge_ == bridge; }

private:
    void publish(FrameReport report, std::string error = {}) {
        if (!bridge_) {
            return;
        }
        bridge_->publish(std::move(report), std::move(error));
    }

    void prepare() {
        if (failed_ && failedVersion_ == state_.sceneVersion) {
            return;
        }
        failed_ = false;
        window_->beginExternalCommands();
        try {
            QThread::currentThread()->setObjectName("Orbelio.Render");
            if (!renderer_) {
                renderer_ = rendererFactory_->create();
                if (!renderer_) {
                    throw std::runtime_error("Renderer factory returned no renderer");
                }
            }
            resizeTargetIfNeeded();
            renderer_->render(state_);
            if (!texture_) {
                importTexture();
            }
            rendered_ = true;
        } catch (const std::exception& error) {
            failFrame(error.what());
        }
        window_->endExternalCommands();
    }

    void failFrame(const char* message) {
        failed_ = true;
        failedVersion_ = state_.sceneVersion;
        rendered_ = false;
        if (renderer_) {
            renderer_->waitIdle();
        }
        texture_.reset();
        renderer_.reset();
        markDirty(QSGNode::DirtySubtreeBlocked);
        FrameReport report{};
        report.frame.sceneVersion = state_.sceneVersion;
        publish(std::move(report), message);
        qWarning() << "Viewport frame failed:" << message;
    }

    void resizeTargetIfNeeded() {
        const bool needsResize =
            !texture_ || state_.width > renderer_->width() || state_.height > renderer_->height();
        if (!needsResize) {
            return;
        }
        // The previous complete-frame fence covers Qt sampling this image.
        texture_.reset();
        renderer_->resize(state_.width, state_.height);
    }

    void importTexture() {
        texture_.reset(
            QNativeInterface::QSGVulkanTexture::fromNative(renderer_->image(),
                                                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                                           window_,
                                                           QSize(renderer_->width(), renderer_->height())));
        if (!texture_) {
            throw std::runtime_error("Qt could not import the native Vulkan image");
        }
        texture_->setFiltering(QSGTexture::Linear);
        setTexture(texture_.get());
        markDirty(QSGNode::DirtySubtreeBlocked);
    }

    void finish() {
        if (failed_ || !rendered_) {
            return;
        }
        try {
            publish(renderer_->finishFrame());
        } catch (const std::exception& error) {
            failFrame(error.what());
        }
        rendered_ = false;
    }

    QQuickWindow* window_;
    QPointer<ViewportBridge> bridge_;
    std::shared_ptr<const IVulkanRendererFactory> rendererFactory_;
    FrameState state_{};
    std::unique_ptr<IVulkanRenderer> renderer_;
    std::unique_ptr<QSGTexture> texture_;
    bool rendered_ = false;
    bool failed_ = false;
    std::uint64_t failedVersion_ = 0;
};
} // namespace

ViewportItem::ViewportItem(QQuickItem* parent) : QQuickItem(parent) { setFlag(ItemHasContents); }

void ViewportItem::setSceneTime(double time) {
    if (time_ == time) {
        return;
    }
    time_ = time;
    Q_EMIT sceneTimeChanged();
    update();
}

void ViewportItem::setBridge(ViewportBridge* bridge) {
    if (bridge_ == bridge) {
        return;
    }
    if (bridge_) {
        disconnect(bridge_, nullptr, this, nullptr);
    }
    bridge_ = bridge;
    if (bridge_) {
        connect(bridge_, &ViewportBridge::frameRequested, this, [this] { update(); });
        connect(bridge_, &QObject::destroyed, this, [this] { update(); });
    }
    Q_EMIT bridgeChanged();
    update();
}

FrameState ViewportItem::currentFrameState() const {
    const auto ratio = window()->effectiveDevicePixelRatio();
    const auto physicalWidth = static_cast<std::uint32_t>(std::max(1.0, std::ceil(width() * ratio)));
    const auto physicalHeight = static_cast<std::uint32_t>(std::max(1.0, std::ceil(height() * ratio)));
    return bridge_->frame(time_, physicalWidth, physicalHeight);
}

QSGNode* ViewportItem::updatePaintNode(QSGNode* previous, UpdatePaintNodeData*) {
    auto* node = static_cast<ViewportNode*>(previous);
    if (node && (!bridge_ || !node->usesBridge(bridge_))) {
        delete node;
        node = nullptr;
    }
    if (!bridge_) {
        return nullptr;
    }
    if (!node) {
        node = new ViewportNode(window(), *bridge_);
    }
    node->setFrame(currentFrameState(), boundingRect());
    return node;
}
} // namespace orbelio
