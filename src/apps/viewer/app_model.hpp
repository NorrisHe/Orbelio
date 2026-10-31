#pragma once

#include "sequence_player.hpp"

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
#include <array>

namespace orbelio {
class AppModel final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString gpuName READ gpuName CONSTANT)
    Q_PROPERTY(QString ioStatus READ ioStatus NOTIFY changed)
    Q_PROPERTY(QVariantMap performance READ performance NOTIFY changed)
    Q_PROPERTY(QVariantMap inspector READ inspector NOTIFY inspectorChanged)
    Q_PROPERTY(int attributeView READ attributeView WRITE setAttributeView NOTIFY renderSettingsChanged)
    Q_PROPERTY(double attributeMinimum READ attributeMinimum NOTIFY renderSettingsChanged)
    Q_PROPERTY(double attributeMaximum READ attributeMaximum NOTIFY renderSettingsChanged)
    Q_PROPERTY(QString loadState READ loadState NOTIFY changed)
    Q_PROPERTY(bool loading READ isLoading NOTIFY changed)
    Q_PROPERTY(bool hasScene READ hasScene NOTIFY changed)
    Q_PROPERTY(QString modelName READ modelName NOTIFY changed)
    Q_PROPERTY(QObject* timeline READ timeline CONSTANT)
    Q_PROPERTY(int maxGaussians READ maxGaussians WRITE setMaxGaussians NOTIFY renderSettingsChanged)
    Q_PROPERTY(int uploadGaussiansPerFrame READ uploadGaussiansPerFrame WRITE setUploadGaussiansPerFrame
                   NOTIFY renderSettingsChanged)
public:
    AppModel(SceneLoader& sceneLoader, ViewportState& viewport, BytePool& cpuPool);

    QString gpuName() const { return gpuName_; }

    void setGpuName(QString name) { gpuName_ = std::move(name); } // before QML creation

    QString ioStatus() const { return ioStatus_; }

    QVariantMap performance() const { return performance_; }

    QVariantMap inspector() const;

    int attributeView() const { return int(viewport_.renderSettings().attributeView); }

    double attributeMinimum() const { return viewport_.renderSettings().attributeMinimum; }

    double attributeMaximum() const { return viewport_.renderSettings().attributeMaximum; }

    void setAttributeView(int value);
    Q_INVOKABLE bool setAttributeRange(double minimum, double maximum);

    QString loadState() const;

    bool isLoading() const { return loadState_ == LoadState::Loading; }

    bool hasScene() const { return viewport_.hasScene(); }

    QString modelName() const;

    QString modelPath() const { return modelPath_; }

    qulonglong gaussianCount() const { return viewport_.gaussianCount(); }

    unsigned shDegree() const { return viewport_.shDegree(); }

    QObject* timeline() { return &sequencePlayer_; }

    int maxGaussians() const { return int(viewport_.renderSettings().maxGaussians); }

    int uploadGaussiansPerFrame() const { return int(viewport_.renderSettings().uploadGaussiansPerFrame); }

    void setMaxGaussians(int value);
    void setUploadGaussiansPerFrame(int value);

    SequencePlayer& sequencePlayer() { return sequencePlayer_; }

    void loadModel(const QString& path);
    Q_INVOKABLE void openFiles(const QList<QUrl>& urls);
    Q_INVOKABLE void unloadModel();
    void acceptFrame(const FrameReport& report, const QString& error);
Q_SIGNALS:
    void sceneLoaded(double ioMilliseconds);
    void sceneChanged();
    void inspectorChanged();
    void changed();
    void frameRequested();
    void renderSettingsChanged();

private:
    enum class LoadState { Empty, Loading, Ready, Error };

    struct LoadRequest {
        std::uint64_t id = 0;
        QString path;
    };

    static constexpr unsigned warmupSceneFrames = 30;

    void resetMeasurements();
    void setRenderSettings(RenderSettings settings);
    void resetAttributeRanges();

    void dispatchPendingRequest();
    void finishModelRead(const LoadRequest& request, SceneLoadResult result);
    void completeModelLoad(const QString& path, SceneLoadResult result);
    void failModelLoad(QString error);
    void restoreLastScene();
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
    QString modelPath_;
    LoadState loadState_ = LoadState::Empty;
    std::uint64_t requestGeneration_ = 0;
    bool readPending_ = false;

    struct DisplayedScene {
        ViewportState::ContentSnapshot content;
        CameraState camera;
        QString path;
    };

    std::optional<DisplayedScene> previousScene_;
    std::optional<LoadRequest> pendingRequest_;
    std::optional<std::uint64_t> uploadPendingVersion_;
    bool deviceLost_ = false;
    SceneLoader& sceneLoader_;
    ViewportState& viewport_;
    BytePool& cpuPool_;
    SequencePlayer sequencePlayer_;
    std::shared_ptr<const PlySequence> pendingSequence_;
    QVariantMap performance_;
    std::uint32_t viewportWidth_ = 0;
    std::uint32_t viewportHeight_ = 0;
    std::array<std::pair<float, float>, 4> attributeRanges_{{{0, 1}, {0, 10}, {0, 1}, {.001f, 1}}};
};
} // namespace orbelio
