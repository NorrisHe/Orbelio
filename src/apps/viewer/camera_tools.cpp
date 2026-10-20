#include "camera_tools.hpp"
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QSaveFile>

namespace orbelio {
namespace {
constexpr std::size_t maximumBookmarks = 16;
constexpr int captureTimeoutMs = 15000;
constexpr std::size_t maximumScreenshotBytes = 64 * 1024 * 1024;
constexpr std::size_t rgbaChannels = 4;

QJsonArray vectorJson(const std::array<float, 3>& vector) { return {vector[0], vector[1], vector[2]}; }

QByteArray cameraDocument(const CameraState& camera, std::uint32_t width, std::uint32_t height) {
    const auto view = cameraView(camera, width, height);
    QJsonArray matrix;
    for (const auto value : view.worldToCamera) {
        matrix.append(value);
    }
    return QJsonDocument(QJsonObject{{"schema", "orbelio.camera"},
                                     {"version", 1},
                                     {"mode", camera.freeFlight ? "free" : "orbit"},
                                     {"target", vectorJson(camera.target)},
                                     {"distance", camera.distance},
                                     {"yawDegrees", camera.yawDegrees},
                                     {"pitchDegrees", camera.pitchDegrees},
                                     {"verticalFieldOfViewDegrees", camera.verticalFieldOfViewDegrees},
                                     {"fovAxis", "shorter viewport axis"},
                                     {"sceneRadius", camera.sceneRadius},
                                     {"eye", vectorJson(view.eye)},
                                     {"worldToCameraColumnMajor", matrix},
                                     {"coordinates", "right/down/forward camera axes; world up is -Y"},
                                     {"viewportPixels", QJsonArray{int(width), int(height)}}})
        .toJson();
}

QString writeDocument(const QString& path, const QByteArray& document) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(document) != document.size() || !file.commit()) {
        return file.errorString();
    }
    return {};
}

QString writeScreenshot(const QString& path, const FrameCapture& capture) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return file.errorString();
    }
    const QImage image(capture.rgba.data(),
                       int(capture.width),
                       int(capture.height),
                       int(capture.width * rgbaChannels),
                       QImage::Format_RGBA8888);
    if (!image.save(&file, "PNG") || !file.commit()) {
        return "Could not encode or write PNG: " + file.errorString();
    }
    return {};
}
}

CameraTools::CameraTools(ViewportState& viewport,
                         ViewportBridge& bridge,
                         IExecutor& worker,
                         IExecutor& completions)
    : viewport_(viewport), bridge_(bridge), worker_(worker), completions_(completions) {
    captureTimeout_.setSingleShot(true);
    connect(&captureTimeout_, &QTimer::timeout, this, [this] {
        cancelCapture("Screenshot timed out; try again after the scene is ready");
    });
    connect(&bridge_, &ViewportBridge::frameCompleted, this, &CameraTools::acceptFrame);
}

QString CameraTools::screenshotSize() const {
    return QString("Viewport only · %1 × %2 px · PNG").arg(width_).arg(height_);
}

CameraTools::~CameraTools() {
    if (captureRequest_) {
        viewport_.requestCapture(0);
    }
}

void CameraTools::saveBookmark() {
    if (!viewport_.hasScene() || bookmarks_.size() == maximumBookmarks) {
        status_ = "Up to 16 bookmarks per scene";
    } else {
        bookmarks_.push_back({QString("View %1").arg(nextBookmarkId_++), viewport_.camera()});
        status_ = "Bookmark saved for this scene (session only)";
    }
    Q_EMIT changed();
}

QStringList CameraTools::bookmarks() const {
    QStringList names;
    for (const auto& bookmark : bookmarks_) {
        names.append(bookmark.name);
    }
    return names;
}

void CameraTools::restoreBookmark(int index) {
    if (index >= 0 && std::size_t(index) < bookmarks_.size()) {
        bridge_.restoreCamera(bookmarks_[index].camera);
    }
}

void CameraTools::removeBookmark(int index) {
    if (index >= 0 && std::size_t(index) < bookmarks_.size()) {
        bookmarks_.erase(bookmarks_.begin() + index);
        Q_EMIT changed();
    }
}

bool CameraTools::beginWrite(const QUrl& destination) {
    if (busy_) {
        return false;
    }
    if (!viewport_.hasScene() || !destination.isLocalFile() || destination.toLocalFile().isEmpty()) {
        status_ = "Choose a local output file with a scene loaded";
        Q_EMIT changed();
        return false;
    }
    busy_ = true;
    status_ = "Saving...";
    Q_EMIT changed();
    return true;
}

void CameraTools::finishWrite(QString error) {
    busy_ = false;
    status_ = error.isEmpty() ? "Saved" : "Save failed: " + error;
    Q_EMIT changed();
}

void CameraTools::submitWrite(std::function<QString()> write, std::size_t retainedBytes) {
    const QPointer<CameraTools> guard(this);
    auto* completions = &completions_;
    const bool accepted = worker_.tryPost(
        [guard, completions, write = std::move(write)] {
            QString error;
            try {
                error = write();
            } catch (const std::exception& exception) {
                error = QString::fromUtf8(exception.what());
            }
            completions->tryPost([guard, error] {
                if (guard) {
                    guard->finishWrite(error);
                }
            });
        },
        retainedBytes);
    if (!accepted) {
        finishWrite("IO queue is full; retry when loading has finished");
    }
}

void CameraTools::exportCamera(const QUrl& destination) {
    if (!beginWrite(destination)) {
        return;
    }
    const auto document = cameraDocument(viewport_.camera(), width_, height_);
    const auto path = destination.toLocalFile();
    submitWrite([path, document] { return writeDocument(path, document); }, std::size_t(document.size()));
}

void CameraTools::saveScreenshot(const QUrl& destination) {
    if (!beginWrite(destination)) {
        return;
    }
    if (!width_ || !height_ || std::size_t(width_) * height_ > maximumScreenshotBytes / rgbaChannels) {
        finishWrite("Screenshot requires a ready viewport with at most 16 million pixels");
        return;
    }
    screenshotPath_ = destination.toLocalFile();
    captureSceneVersion_ = viewport_.sceneVersion();
    captureRequest_ = viewport_.nextCaptureRequest();
    captureTimeout_.start(captureTimeoutMs);
    bridge_.requestFrame();
}

void CameraTools::acceptFrame(const FrameReport& report, const QString& error) {
    if (report.frame.sceneVersion != viewport_.sceneVersion()) {
        return;
    }
    if (width_ != report.frame.viewportWidth || height_ != report.frame.viewportHeight) {
        width_ = report.frame.viewportWidth;
        height_ = report.frame.viewportHeight;
        Q_EMIT changed();
    }
    if (!captureRequest_) {
        return;
    }
    if (!error.isEmpty()) {
        cancelCapture(error);
        return;
    }
    const auto capture = report.frame.capture;
    if (!capture || capture->requestId != captureRequest_) {
        return;
    }
    completeCapture(capture);
}

void CameraTools::cancelCapture(QString error) {
    captureTimeout_.stop();
    viewport_.requestCapture(0);
    captureRequest_ = 0;
    finishWrite(std::move(error));
}

void CameraTools::completeCapture(std::shared_ptr<const FrameCapture> capture) {
    captureTimeout_.stop();
    viewport_.requestCapture(0);
    captureRequest_ = 0;
    const auto path = screenshotPath_;
    if (capture->rgba.size() > maximumScreenshotBytes) {
        finishWrite("Viewport grew beyond the screenshot memory limit; resize and retry");
        return;
    }
    submitWrite([path, capture] { return writeScreenshot(path, *capture); }, capture->rgba.size());
}

void CameraTools::sceneChanged() {
    bookmarks_.clear();
    nextBookmarkId_ = 1;
    if (captureRequest_ && captureSceneVersion_ != viewport_.sceneVersion()) {
        cancelCapture("Scene changed before screenshot completed");
    }
    Q_EMIT changed();
}
}
