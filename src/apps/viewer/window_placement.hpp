#pragma once
#include <QScreen>
#include <QWindow>

namespace orbelio {
inline void centerWindow(QWindow& window) {
    const auto available = window.screen()->availableGeometry();
    const auto frameSize = window.frameGeometry().size();
    window.setFramePosition(available.topLeft() + QPoint((available.width() - frameSize.width()) / 2,
                                                         (available.height() - frameSize.height()) / 2));
}
}
