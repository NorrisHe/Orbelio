#include "app_model.hpp"
#include <QPointer>
#include <QFileInfo>
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

QString AppModel::budgetDescription() const {
    const auto& limits = sceneLoader_.limits();
    return "Input pool: " + mebibytes(limits.inputBytes) +
           ". Decode/workspace: " + mebibytes(limits.decodedBytes) +
           "/model. CPU scenes + queued results: " + mebibytes(limits.residentBytes) +
           ". GPU allocations/blocks: " + mebibytes(limits.gpuBytes) + ". Qt and driver excluded.";
}

void AppModel::restoreLastScene() {
    if (!previousScene_) {
        return;
    }
    viewport_.restore(previousScene_->viewport);
    modelPath_ = previousScene_->path;
    modelName_ = previousScene_->name;
    previousScene_.reset();
    resetMeasurements();
    performance_.clear();
    lastUiRefresh_ = {};
    Q_EMIT sceneChanged();
}

void AppModel::resetMeasurements() {
    metrics_.clear();
    sceneFrames_ = 0;
}

QString AppModel::loadState() const {
    switch (loadState_) {
    case LoadState::Empty:
        return "Empty";
    case LoadState::Loading:
        return "Loading";
    case LoadState::Ready:
        return "Ready";
    case LoadState::Error:
        return "Error";
    }
    return "Error";
}

void AppModel::failModelLoad(QString error) {
    pendingRequest_.reset();
    uploadPendingVersion_.reset();
    restoreLastScene();
    loadState_ = LoadState::Error;
    ioStatus_ = "Model error: " + error;
    Q_EMIT changed();
}

void AppModel::completeModelLoad(const QString& path, SceneLoadResult result) {
    if (result.scene) {
        loadState_ = LoadState::Loading;
        resetMeasurements();
        performance_.clear();
        lastUiRefresh_ = {};
        if (!previousScene_) {
            previousScene_ = DisplayedScene{viewport_, modelPath_, modelName_};
        }
        viewport_.setScene(std::move(result.scene));
        modelPath_ = path;
        modelName_ = QFileInfo(path).fileName();
        uploadPendingVersion_ = viewport_.sceneVersion();
        ioStatus_ = "Preparing " + modelName_ + " for display...";
        Q_EMIT sceneLoaded(result.ioMilliseconds);
        Q_EMIT sceneChanged();
    } else {
        failModelLoad(QString::fromStdString(result.error));
        return;
    }
    Q_EMIT changed();
}

void AppModel::loadModel(const QString& path) {
    pendingRequest_ = LoadRequest{++requestGeneration_, path};
    uploadPendingVersion_.reset();
    loadState_ = LoadState::Loading;
    ioStatus_ = readPending_ ? "Waiting to load " + QFileInfo(path).fileName() + "..."
                             : "Loading " + QFileInfo(path).fileName() + "...";
    dispatchPendingRequest();
    Q_EMIT changed();
}

void AppModel::dispatchPendingRequest() {
    if (readPending_ || !pendingRequest_) {
        return;
    }
    const auto request = std::move(*pendingRequest_);
    pendingRequest_.reset();
    const QPointer<AppModel> receiver(this);
    readPending_ = true;
    const bool accepted =
        sceneLoader_.load(request.path.toUtf8().toStdString(), [receiver, request](SceneLoadResult result) {
            if (receiver) {
                receiver->finishModelRead(request, std::move(result));
            }
        });
    if (!accepted) {
        readPending_ = false;
        failModelLoad("IO queue is busy or stopped; try opening the file again");
    }
}

void AppModel::finishModelRead(const LoadRequest& request, SceneLoadResult result) {
    readPending_ = false;
    if (request.id == requestGeneration_) {
        completeModelLoad(request.path, std::move(result));
    }
    result.scene.reset();
    dispatchPendingRequest();
    if (loadState_ == LoadState::Empty) {
        ioStatus_ = "No model loaded";
        Q_EMIT changed();
    }
}

void AppModel::openFiles(const QList<QUrl>& urls) {
    if (urls.size() != 1 || !urls.front().isLocalFile()) {
        ++requestGeneration_;
        failModelLoad("Open one local Gaussian model at a time");
        return;
    }
    loadModel(urls.front().toLocalFile());
}

void AppModel::unloadModel() {
    ++requestGeneration_;
    pendingRequest_.reset();
    uploadPendingVersion_.reset();
    previousScene_.reset();
    viewport_.setScene({});
    modelName_ = "No model";
    modelPath_.clear();
    loadState_ = LoadState::Empty;
    ioStatus_ = readPending_ ? "Unloaded; finishing an obsolete IO request" : "No model loaded";
    resetMeasurements();
    performance_.clear();
    lastUiRefresh_ = {};
    Q_EMIT sceneChanged();
    Q_EMIT changed();
}

bool AppModel::finishSceneUpload(const FrameStats& frame) {
    if (!uploadPendingVersion_ || *uploadPendingVersion_ != frame.sceneVersion || !frame.gaussianCount) {
        return false;
    }
    uploadPendingVersion_.reset();
    previousScene_.reset();
    loadState_ = LoadState::Ready;
    ioStatus_ = QString("Loaded %1 Gaussians").arg(frame.gaussianCount);
    return true;
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
    row("CPU scenes + queued", mebibytes(sceneLoader_.residentBytes()));
    row("GPU allocation limit", mebibytes(backend.allocatorBudgetBytes));
    row("Input active / reserved", mebibytes(cpu.activeBytes) + " / " + mebibytes(cpu.reservedBytes));
    row("Input peak / allocations", mebibytes(cpu.peakBytes) + " / " + QString::number(cpu.allocations));
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
        if (report.frame.sceneVersion && report.frame.sceneVersion != viewport_.sceneVersion()) {
            return;
        }
        uploadPendingVersion_.reset();
        restoreLastScene();
        loadState_ = LoadState::Error;
        ioStatus_ = "Render error: " + error;
        Q_EMIT changed();
        return;
    }
    const auto& frame = report.frame;
    if (frame.sceneVersion != viewport_.sceneVersion()) {
        return;
    }
    const bool sceneBecameReady = finishSceneUpload(frame);
    if (viewportWidth_ != frame.viewportWidth || viewportHeight_ != frame.viewportHeight ||
        sceneUploads_ != frame.sceneUploads) {
        resetMeasurements();
    }
    viewportWidth_ = frame.viewportWidth;
    viewportHeight_ = frame.viewportHeight;
    sceneUploads_ = frame.sceneUploads;
    addFrameSamples(frame);

    const auto now = Clock::now();
    if (!sceneBecameReady && lastUiRefresh_.time_since_epoch().count() &&
        now - lastUiRefresh_ < uiRefreshInterval) {
        return;
    }
    lastUiRefresh_ = now;
    refreshPerformancePanel(report);
    Q_EMIT changed();
}

} // namespace orbelio
