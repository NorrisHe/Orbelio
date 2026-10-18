#include "app_model.hpp"
#include <QPointer>
#include <chrono>

namespace orbelio {
namespace {
using Clock = std::chrono::steady_clock;
constexpr auto uiRefreshInterval = std::chrono::milliseconds(250);
constexpr double mebibyte = 1048576.;
constexpr double millisecondsPerSecond = 1000.;

QString mebibytes(std::size_t bytes) { return QString::number(double(bytes) / mebibyte, 'f', 2) + " MiB"; }
} // namespace

AppModel::AppModel(bool validation, SceneLoader& sceneLoader, ViewportState& viewport, BytePool& cpuPool)
    : validation_(validation), sceneLoader_(sceneLoader), viewport_(viewport), cpuPool_(cpuPool) {}

void AppModel::resetMeasurements() {
    metrics_.clear();
    sceneFrames_ = 0;
}

void AppModel::completeModelLoad(SceneLoadResult result) {
    if (result.scene) {
        resetMeasurements();
        viewport_.setScene(result.scene);
        ioStatus_ = QString("Loaded %1 Gaussians on pooled IO worker").arg(result.scene->count());
        Q_EMIT sceneLoaded(result.ioMilliseconds);
    } else {
        ioStatus_ = "Model error: " + QString::fromStdString(result.error);
    }
    Q_EMIT changed();
}

void AppModel::loadModel(const QString& path) {
    const QPointer<AppModel> receiver(this);
    const bool accepted = sceneLoader_.load(path.toUtf8().toStdString(), [receiver](SceneLoadResult result) {
        if (receiver) {
            receiver->completeModelLoad(std::move(result));
        }
    });
    ioStatus_ = accepted ? "Loading compressed PLY..." : "IO queue is busy or stopped";
    Q_EMIT changed();
}

void AppModel::addFrameSamples(const FrameStats& stats) {
    if (!stats.gaussianCount || ++sceneFrames_ <= warmupSceneFrames) {
        return;
    }
    auto add = [&](std::string_view key, std::optional<double> value) {
        auto entry = metrics_.find(key);
        if (entry == metrics_.end()) {
            entry = metrics_.try_emplace(std::string(key)).first;
        }
        entry->second.add(value);
    };
    add("completedIntervalMs", stats.frameIntervalMs);
    add("completeFrameMs", stats.completeFrameMs);
    add("engineBlockingMs", stats.cpuSubmitMs);
    add("qtCompletionWaitMs", stats.gpuWaitMs);
    add("backendWaitMs", stats.backendWaitMs);
    add("allGpuWaitsMs", stats.backendWaitMs + stats.gpuWaitMs);
    add("queryReadbackCpuMs", stats.queryReadbackCpuMs);
    add("qtCompositionCpuSpanMs", std::max(0., stats.completeFrameMs - stats.cpuSubmitMs));
    add("gpuPreprocessMs", stats.projectionGpuMs);
    add("gpuDepthSortMs", stats.sortingGpuMs);
    add("gpuRasterAndTileSortMs", stats.rasterGpuMs);
    add("gpuImageComposeMs", stats.composeGpuMs);
    add("gpuGsStagesMs", stats.gpuRenderMs);
}

QVariantMap AppModel::timingRows() const {
    QVariantMap rows;
    for (const auto& [key, metric] : metrics_) {
        const auto p50 = metric.percentile(.5);
        const auto p95 = metric.percentile(.95);
        rows[QString::fromStdString(key)] =
            p50 ? QString::number(*p50, 'f', 2) + " / " + QString::number(*p95, 'f', 2) : "Unavailable";
    }
    return rows;
}

QVariantMap AppModel::resourceRows(const BackendStats& backend) const {
    const auto cpu = cpuPool_.stats();
    QVariantMap rows;
    auto row = [&](QString name, QString value) { rows[name] = std::move(value); };
    row("Model buffers", mebibytes(backend.modelBytes));
    row("Preprocess buffers", mebibytes(backend.preprocessingBytes));
    row("Sort / tile scratch", mebibytes(backend.sortingBytes));
    row("Pixel buffers + target", mebibytes(backend.pixelBytes + backend.imageReservedBytes));
    row("Statistics / capture", mebibytes(backend.telemetryBytes));
    row("GS reserved / growths",
        mebibytes(backend.gsReservedBytes) + " / " + QString::number(backend.gsGrowths));
    row("VMA used / reserved",
        mebibytes(backend.allocatorUsedBytes) + " / " + mebibytes(backend.allocatorReservedBytes));
    row("VMA used peak", mebibytes(backend.allocatorPeakBytes));
    row("CPU active / reserved", mebibytes(cpu.activeBytes) + " / " + mebibytes(cpu.reservedBytes));
    row("CPU peak / allocations", mebibytes(cpu.peakBytes) + " / " + QString::number(cpu.allocations));
    row("GS allocations / reuse",
        QString::number(backend.gsBufferAllocations) + " / " + QString::number(backend.gsBufferReuses));
    row("Image creates / growths",
        QString::number(backend.imageAllocations) + " / " +
            QString::number(backend.imageAllocations ? backend.imageAllocations - 1 : 0));
    return rows;
}

void AppModel::refreshPerformancePanel(const FrameReport& report) {
    const auto& frame = report.frame;
    const auto& backend = report.backend;
    const auto interval = metrics_.find("completedIntervalMs");
    const auto mean = interval == metrics_.end() ? std::optional<double>{} : interval->second.mean();
    performance_ = {{"gaussians", QVariant::fromValue<qulonglong>(frame.gaussianCount)},
                    {"tileInstances",
                     backend.telemetryReferences
                         ? QVariant::fromValue<qulonglong>(*backend.telemetryReferences)
                         : QVariant()},
                    {"depthWaves", QVariant::fromValue<qulonglong>(backend.depthWaves)}};
    performance_["fps"] = mean && *mean > 0 ? QVariant(millisecondsPerSecond / *mean) : QVariant();
    performance_["timingRows"] = timingRows();
    performance_["samples"] =
        static_cast<int>(metrics_.contains("completeFrameMs") ? metrics_.at("completeFrameMs").count() : 0);
    performance_["visible"] =
        backend.visibleGaussians ? QVariant::fromValue<qulonglong>(*backend.visibleGaussians) : QVariant();
    performance_["maxTile"] =
        backend.maxTileList ? QVariant::fromValue<qulonglong>(*backend.maxTileList) : QVariant();
    performance_["resourceRows"] = resourceRows(backend);
}

void AppModel::acceptFrame(const FrameReport& report, const QString& error) {
    if (!error.isEmpty()) {
        ioStatus_ = "Render error: " + error;
        Q_EMIT changed();
        return;
    }
    const auto& frame = report.frame;
    if (viewportWidth_ != frame.viewportWidth || viewportHeight_ != frame.viewportHeight ||
        sceneUploads_ != frame.sceneUploads) {
        resetMeasurements();
    }
    viewportWidth_ = frame.viewportWidth;
    viewportHeight_ = frame.viewportHeight;
    sceneUploads_ = frame.sceneUploads;
    addFrameSamples(frame);

    const auto now = Clock::now();
    if (lastUiRefresh_.time_since_epoch().count() && now - lastUiRefresh_ < uiRefreshInterval) {
        return;
    }
    lastUiRefresh_ = now;
    refreshPerformancePanel(report);
    Q_EMIT changed();
}

} // namespace orbelio
