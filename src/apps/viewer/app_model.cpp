#include "app_model.hpp"
#include <QPointer>
#include <QFileInfo>
#include <chrono>
#include <cmath>
#include <limits>

namespace orbelio {
namespace {
using Clock = std::chrono::steady_clock;
constexpr auto uiRefreshInterval = std::chrono::milliseconds(250);
constexpr double mebibyte = 1048576.;
constexpr double millisecondsPerSecond = 1000.;

QString mebibytes(std::size_t bytes) { return QString::number(double(bytes) / mebibyte, 'f', 2) + " MiB"; }
} // namespace

AppModel::AppModel(SceneLoader& sceneLoader, ViewportState& viewport, BytePool& cpuPool)
    : sceneLoader_(sceneLoader), viewport_(viewport), cpuPool_(cpuPool),
      sequencePlayer_(sceneLoader, viewport, this) {
    connect(&sequencePlayer_, &SequencePlayer::frameRequested, this, &AppModel::frameRequested);
    connect(&sequencePlayer_, &SequencePlayer::frameRequested, this, &AppModel::changed);
    connect(this, &AppModel::changed, this, &AppModel::inspectorChanged);
}

QString AppModel::modelName() const {
    return modelPath_.isEmpty() ? QStringLiteral("No model") : QFileInfo(modelPath_).fileName();
}

QVariantMap AppModel::inspector() const {
    const auto content = previousScene_ ? previousScene_->content : sequencePlayer_.confirmedContent();
    const auto* spatial = content.fdu4d ? &content.fdu4d->spatial : content.scene.get();
    if (!spatial) {
        return {};
    }
    const auto path = previousScene_ ? previousScene_->path : modelPath_;
    QVariantMap result{{"name", QFileInfo(path).fileName()},
                       {"path", path},
                       {"format", QString::fromStdString(spatial->format)},
                       {"gaussians", qulonglong(spatial->count())},
                       {"shDegree", spatial->shDegree},
                       {"representation", content.fdu4d ? "FDU 4DGS" : "Static 3DGS"},
                       {"hasStableIds", false},
                       {"boundsUnavailableReason",
                        content.fdu4d ? "Dynamic bounds have not been measured" : "No decoded bounds"}};
    if (!content.fdu4d && spatial->centerBounds) {
        const auto& bounds = *spatial->centerBounds;
        result["boundsMinimum"] = QVariantList{bounds.minimum[0], bounds.minimum[1], bounds.minimum[2]};
        result["boundsMaximum"] = QVariantList{bounds.maximum[0], bounds.maximum[1], bounds.maximum[2]};
    }
    if (!previousScene_ && sequencePlayer_.active()) {
        result["representation"] = content.fdu4d ? "FDU 4DGS" : "PLY Sequence";
        result["framePath"] = sequencePlayer_.frameAsset();
        result["frameIndex"] = sequencePlayer_.frameIndex();
        result["frameTime"] = sequencePlayer_.frameTime();
    }
    return result;
}

void AppModel::setAttributeView(int value) {
    if (value < int(AttributeView::Normal) || value > int(AttributeView::Scale)) {
        return;
    }
    auto settings = viewport_.renderSettings();
    settings.attributeView = AttributeView(value);
    const auto& range = attributeRanges_[std::size_t(value)];
    settings.attributeMinimum = range.first;
    settings.attributeMaximum = range.second;
    setRenderSettings(settings);
}

bool AppModel::setAttributeRange(double minimum, double maximum) {
    auto settings = viewport_.renderSettings();
    if (!std::isfinite(minimum) || !std::isfinite(maximum) || minimum >= maximum ||
        std::abs(minimum) > std::numeric_limits<float>::max() ||
        std::abs(maximum) > std::numeric_limits<float>::max() ||
        settings.attributeView == AttributeView::Normal || settings.attributeView == AttributeView::Opacity ||
        (settings.attributeView == AttributeView::Scale && minimum <= 0)) {
        return false;
    }
    settings.attributeMinimum = float(minimum);
    settings.attributeMaximum = float(maximum);
    if (settings.attributeMinimum >= settings.attributeMaximum ||
        (settings.attributeView == AttributeView::Scale &&
         (settings.attributeMinimum <= 0 ||
          std::log(settings.attributeMinimum) >= std::log(settings.attributeMaximum)))) {
        return false;
    }
    attributeRanges_[std::size_t(settings.attributeView)] = {settings.attributeMinimum,
                                                             settings.attributeMaximum};
    setRenderSettings(settings);
    return true;
}

void AppModel::resetAttributeRanges() {
    const auto content = viewport_.content();
    const auto* spatial = content.fdu4d ? &content.fdu4d->spatial : content.scene.get();
    attributeRanges_ = {{{0, 1}, {0, 10}, {0, 1}, {.001f, 1}}};
    if (spatial) {
        attributeRanges_[1] = {0, std::max(1.f, viewport_.camera().distance + spatial->radius * 2)};
        if (spatial->logScaleRange) {
            const auto& range = *spatial->logScaleRange;
            attributeRanges_[3] = {std::exp(range[0]) * .5f, std::exp(range[1]) * 2};
        }
    }
    const auto range = attributeRanges_[std::size_t(attributeView())];
    auto settings = viewport_.renderSettings();
    settings.attributeMinimum = range.first;
    settings.attributeMaximum = range.second;
    viewport_.setRenderSettings(settings);
}

void AppModel::restoreLastScene() {
    if (!previousScene_) {
        return;
    }
    viewport_.restoreContent(previousScene_->content);
    viewport_.setCamera(previousScene_->camera);
    modelPath_ = previousScene_->path;
    previousScene_.reset();
    sequencePlayer_.resume();
    resetMeasurements();
    performance_.clear();
    lastUiRefresh_ = {};
    Q_EMIT sceneChanged();
}

void AppModel::resetMeasurements() {
    metrics_.clear();
    sceneFrames_ = 0;
}

void AppModel::setRenderSettings(RenderSettings settings) {
    if (settings == viewport_.renderSettings()) {
        return;
    }
    viewport_.setRenderSettings(settings);
    resetMeasurements();
    performance_.clear();
    lastUiRefresh_ = {};
    Q_EMIT renderSettingsChanged();
    Q_EMIT changed();
    Q_EMIT frameRequested();
}

void AppModel::setMaxGaussians(int value) {
    auto settings = viewport_.renderSettings();
    settings.maxGaussians = std::clamp(value, 0, int(ModelLimits{}.maxGaussians));
    setRenderSettings(settings);
}

void AppModel::setUploadGaussiansPerFrame(int value) {
    auto settings = viewport_.renderSettings();
    settings.uploadGaussiansPerFrame = std::clamp(value, 0, int(ModelLimits{}.maxGaussians));
    setRenderSettings(settings);
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
    pendingSequence_.reset();
    restoreLastScene();
    loadState_ = LoadState::Error;
    ioStatus_ = "Model error: " + error;
    Q_EMIT changed();
}

void AppModel::completeModelLoad(const QString& path, SceneLoadResult result) {
    if (result.scene || result.fdu4d) {
        sequencePlayer_.suspend();
        loadState_ = LoadState::Loading;
        resetMeasurements();
        performance_.clear();
        lastUiRefresh_ = {};
        if (!previousScene_) {
            previousScene_ = DisplayedScene{viewport_.content(), viewport_.camera(), modelPath_};
        }
        if (result.fdu4d) {
            viewport_.setFdu4d(std::move(result.fdu4d));
        } else {
            viewport_.setScene(std::move(result.scene));
        }
        pendingSequence_ = std::move(result.sequence);
        if (pendingSequence_) {
            viewport_.setSequenceFrame(viewport_.content().scene,
                                       pendingSequence_->frames.front().timestampSeconds);
        }
        modelPath_ = path;
        uploadPendingVersion_ = viewport_.sceneVersion();
        ioStatus_ = "Preparing " + modelName() + " for display...";
        Q_EMIT sceneLoaded(result.ioMilliseconds);
        Q_EMIT sceneChanged();
    } else {
        failModelLoad(QString::fromStdString(result.error));
        return;
    }
    Q_EMIT changed();
}

void AppModel::loadModel(const QString& path) {
    if (deviceLost_) {
        return;
    }
    sequencePlayer_.setPlaying(false);
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
    result.fdu4d.reset();
    result.sequence.reset();
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
    sequencePlayer_.clear();
    pendingSequence_.reset();
    ++requestGeneration_;
    pendingRequest_.reset();
    uploadPendingVersion_.reset();
    previousScene_.reset();
    viewport_.setScene({});
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
    if (!uploadPendingVersion_ || *uploadPendingVersion_ != frame.sceneVersion || !frame.gaussianCount ||
        frame.refining) {
        return false;
    }
    uploadPendingVersion_.reset();
    previousScene_.reset();
    loadState_ = LoadState::Ready;
    resetAttributeRanges();
    ioStatus_ = QString("Loaded %1 Gaussians").arg(frame.gaussianCount);
    if (viewport_.content().fdu4d) {
        sequencePlayer_.activateFdu4d(viewport_.content().fdu4d);
    } else {
        sequencePlayer_.activate(std::move(pendingSequence_), viewport_.content().scene);
    }
    Q_EMIT renderSettingsChanged();
    Q_EMIT frameRequested();
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
    add("temporalEvaluationCpuMs", stats.temporalEvaluationCpuMs);
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
    performance_["submittedGaussians"] = QVariant::fromValue<qulonglong>(frame.submittedGaussians);
    performance_["budgetedGaussians"] = QVariant::fromValue<qulonglong>(frame.budgetedGaussians);
    performance_["refining"] = frame.refining;
}

void AppModel::acceptFrame(const FrameReport& report, const QString& error) {
    if (report.frame.failure == RenderFailure::DeviceLost) {
        deviceLost_ = true;
        unloadModel();
        loadState_ = LoadState::Error;
        ioStatus_ = "Vulkan device lost: " + error + ". Restart the viewer.";
        Q_EMIT changed();
        return;
    }
    const auto generation = requestGeneration_;
    const auto sequenceFrame = sequencePlayer_.acceptFrame(report.frame, error);
    if (generation != requestGeneration_) {
        return;
    }
    if (sequenceFrame == SequencePlayer::FrameAcceptance::FailedConsumed) {
        Q_EMIT changed();
        return;
    }
    if (!error.isEmpty()) {
        if (report.frame.sceneVersion && report.frame.sceneVersion != viewport_.sceneVersion()) {
            return;
        }
        uploadPendingVersion_.reset();
        pendingSequence_.reset();
        restoreLastScene();
        loadState_ = LoadState::Error;
        ioStatus_ = "Render error: " + error;
        Q_EMIT changed();
        return;
    }
    const auto& frame = report.frame;
    if (sequenceFrame != SequencePlayer::FrameAcceptance::Committed &&
        frame.sceneVersion != viewport_.sceneVersion()) {
        return;
    }
    const bool sceneBecameReady = finishSceneUpload(frame);
    if (sequenceFrame == SequencePlayer::FrameAcceptance::Committed) {
        Q_EMIT inspectorChanged();
    }
    if (generation != requestGeneration_) {
        return;
    }
    if (viewportWidth_ != frame.viewportWidth || viewportHeight_ != frame.viewportHeight) {
        resetMeasurements();
    }
    viewportWidth_ = frame.viewportWidth;
    viewportHeight_ = frame.viewportHeight;
    addFrameSamples(frame);
    const auto now = Clock::now();
    if (!sceneBecameReady && !frame.refining && !performance_.value("refining").toBool() &&
        lastUiRefresh_.time_since_epoch().count() && now - lastUiRefresh_ < uiRefreshInterval) {
        return;
    }
    lastUiRefresh_ = now;
    refreshPerformancePanel(report);
    Q_EMIT changed();
    if (frame.refining && generation == requestGeneration_) {
        Q_EMIT frameRequested();
    }
}

} // namespace orbelio
