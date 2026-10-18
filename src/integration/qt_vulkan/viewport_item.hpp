#pragma once

#include "viewport_bridge.hpp"
#include <QQuickItem>
#include <QPointer>

namespace orbelio {
// All fields below are assigned/copied on the GUI/scene-sync boundary.
class ViewportItem : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(double sceneTime READ sceneTime WRITE setSceneTime NOTIFY sceneTimeChanged)
    Q_PROPERTY(ViewportBridge* bridge READ bridge WRITE setBridge NOTIFY bridgeChanged)
public:
    explicit ViewportItem(QQuickItem* parent = nullptr);

    double sceneTime() const { return time_; }

    void setSceneTime(double time);

    ViewportBridge* bridge() const { return bridge_; }

    void setBridge(ViewportBridge* bridge);
Q_SIGNALS:
    void sceneTimeChanged();
    void bridgeChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* node, UpdatePaintNodeData*) override;

private:
    FrameState currentFrameState() const;

    double time_ = 0;
    QPointer<ViewportBridge> bridge_;
};
} // namespace orbelio
