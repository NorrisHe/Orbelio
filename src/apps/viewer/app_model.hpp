#pragma once

#include <orbelio/core/scene_loader.hpp>
#include <orbelio/core/viewport_state.hpp>
#include <renderer_stats.hpp>
#include <QObject>
#include <QVariantMap>
#include <QString>
#include <QUrl>
#include <QList>
#include <orbelio/core/performance.hpp>
#include <map>
#include <chrono>
#include <optional>

namespace orbelio {
class AppModel final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString gpuName READ gpuName CONSTANT)
    Q_PROPERTY(bool validation READ validation CONSTANT)
    Q_PROPERTY(QString ioStatus READ ioStatus NOTIFY changed)
    Q_PROPERTY(QVariantMap performance READ performance NOTIFY changed)
    Q_PROPERTY(QString loadState READ loadState NOTIFY changed)
    Q_PROPERTY(bool loading READ isLoading NOTIFY changed)
    Q_PROPERTY(bool hasScene READ hasScene NOTIFY changed)
    Q_PROPERTY(QString modelName READ modelName NOTIFY changed)
    Q_PROPERTY(QString modelPath READ modelPath NOTIFY changed)
    Q_PROPERTY(qulonglong gaussianCount READ gaussianCount NOTIFY changed)
public:
    AppModel(bool validation, SceneLoader& sceneLoader, ViewportState& viewport, BytePool& cpuPool);

    QString gpuName() const { return gpuName_; }

    void setGpuName(QString name) { gpuName_ = std::move(name); } // before QML creation

    bool validation() const { return validation_; }

    QString ioStatus() const { return ioStatus_; }

    QVariantMap performance() const { return performance_; }

    QString loadState() const;

    bool isLoading() const { return loadState_ == LoadState::Loading; }

    bool hasScene() const { return viewport_.hasScene(); }

    QString modelName() const { return modelName_; }

    QString modelPath() const { return modelPath_; }

    qulonglong gaussianCount() const { return viewport_.gaussianCount(); }

    void loadModel(const QString& path);
    Q_INVOKABLE void openFiles(const QList<QUrl>& urls);
    Q_INVOKABLE void unloadModel();
    void acceptFrame(const FrameReport& report, const QString& error);
Q_SIGNALS:
    void sceneLoaded(double ioMilliseconds);
    void sceneChanged();
    void changed();

private:
    enum class LoadState { Empty, Loading, Ready, Error };

    struct LoadRequest {
        std::uint64_t id = 0;
        QString path;
    };

    static constexpr unsigned warmupSceneFrames = 30;

    void resetMeasurements();

    void dispatchPendingRequest();
    void finishModelRead(const LoadRequest& request, SceneLoadResult result);
    void completeModelLoad(const QString& path, SceneLoadResult result);
    void failModelLoad(QString error);
    bool finishSceneUpload(const FrameStats& frame);

    void addFrameSamples(const FrameStats& stats);
    void refreshPerformancePanel(const FrameReport& report);
    QVariantMap timingRows() const;
    QVariantMap resourceRows(const BackendStats& backend) const;

    std::map<std::string, RollingMetric, std::less<>> metrics_;
    std::chrono::steady_clock::time_point lastUiRefresh_{};
    unsigned sceneFrames_ = 0;
    QString gpuName_;
    QString ioStatus_ = "Idle";
    QString modelName_ = "No model";
    QString modelPath_;
    LoadState loadState_ = LoadState::Empty;
    std::uint64_t requestGeneration_ = 0;
    bool readPending_ = false;
    std::optional<LoadRequest> pendingRequest_;
    std::optional<std::uint64_t> uploadPendingVersion_;
    bool validation_;
    SceneLoader& sceneLoader_;
    ViewportState& viewport_;
    BytePool& cpuPool_;
    QVariantMap performance_;
    std::uint32_t viewportWidth_ = 0;
    std::uint32_t viewportHeight_ = 0;
    std::uint64_t sceneUploads_ = 0;
};
} // namespace orbelio
