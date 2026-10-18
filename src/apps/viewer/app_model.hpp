#pragma once

#include <orbelio/core/scene_loader.hpp>
#include <orbelio/core/viewport_state.hpp>
#include <renderer_stats.hpp>
#include <QObject>
#include <QVariantMap>
#include <QString>
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
public:
    AppModel(bool validation, SceneLoader& sceneLoader, ViewportState& viewport, BytePool& cpuPool);

    QString gpuName() const { return gpuName_; }

    void setGpuName(QString name) { gpuName_ = std::move(name); } // before QML creation

    bool validation() const { return validation_; }

    QString ioStatus() const { return ioStatus_; }

    QVariantMap performance() const { return performance_; }

    void loadModel(const QString& path);
    void acceptFrame(const FrameReport& report, const QString& error);
Q_SIGNALS:
    void sceneLoaded(double ioMilliseconds);
    void changed();

private:
    static constexpr unsigned warmupSceneFrames = 30;

    void resetMeasurements();

    void completeModelLoad(SceneLoadResult result);

    void addFrameSamples(const FrameStats& stats);
    void refreshPerformancePanel(const FrameReport& report);
    QVariantMap timingRows() const;
    QVariantMap resourceRows(const BackendStats& backend) const;

    std::map<std::string, RollingMetric, std::less<>> metrics_;
    std::chrono::steady_clock::time_point lastUiRefresh_{};
    unsigned sceneFrames_ = 0;
    QString gpuName_;
    QString ioStatus_ = "Idle";
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
