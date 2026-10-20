#pragma once

#include <viewport_bridge.hpp>
#include <orbelio/core/executor.hpp>
#include <QTimer>
#include <QUrl>
#include <QStringList>
#include <functional>

namespace orbelio {
class CameraTools final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QStringList bookmarks READ bookmarks NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString screenshotSize READ screenshotSize NOTIFY changed)
public:
    CameraTools(ViewportState& viewport, ViewportBridge& bridge, IExecutor& worker, IExecutor& completions);
    ~CameraTools() override;

    QStringList bookmarks() const;

    bool busy() const { return busy_; }

    QString status() const { return status_; }

    QString screenshotSize() const;
    Q_INVOKABLE void saveBookmark();
    Q_INVOKABLE void restoreBookmark(int index);
    Q_INVOKABLE void removeBookmark(int index);
    Q_INVOKABLE void exportCamera(const QUrl& destination);
    Q_INVOKABLE void saveScreenshot(const QUrl& destination);
    void acceptFrame(const FrameReport& report, const QString& error);
    void sceneChanged();
Q_SIGNALS:
    void changed();

private:
    void finishWrite(QString error);
    void cancelCapture(QString error);
    void completeCapture(std::shared_ptr<const FrameCapture> capture);
    void submitWrite(std::function<QString()> write, std::size_t retainedBytes);
    bool beginWrite(const QUrl& destination);
    ViewportState& viewport_;
    ViewportBridge& bridge_;
    IExecutor& worker_;
    IExecutor& completions_;

    struct Bookmark {
        QString name;
        CameraState camera{};
    };

    std::vector<Bookmark> bookmarks_;
    unsigned nextBookmarkId_ = 1;
    QString status_;
    bool busy_ = false;
    std::uint64_t captureRequest_ = 0;
    std::uint64_t captureSceneVersion_ = 0;
    QString screenshotPath_;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    QTimer captureTimeout_;
};
}
