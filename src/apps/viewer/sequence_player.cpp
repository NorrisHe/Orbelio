#include "sequence_player.hpp"
#include <QPointer>
#include <QSignalBlocker>
#include <algorithm>
#include <cmath>

namespace orbelio {
namespace {
constexpr int playbackTickMilliseconds = 16;
constexpr double millisecondsPerSecond = 1000.;
}

SequencePlayer::SequencePlayer(SceneLoader& loader,
                               ViewportState& viewport,
                               QObject* parent,
                               SceneLoader::Now now)
    : QObject(parent), loader_(loader), viewport_(viewport), now_(std::move(now)) {
    timer_.setInterval(playbackTickMilliseconds);
    timer_.setTimerType(Qt::PreciseTimer);
    connect(&timer_, &QTimer::timeout, this, [this] { advance(clock_.restart() / millisecondsPerSecond); });
}

void SequencePlayer::suspend() {
    timer_.stop();
    suspended_ = true;
    ++generation_;
    cancelSeek();
    restorePendingFrame();
    Q_EMIT changed();
}

void SequencePlayer::resume() {
    suspended_ = false;
    desiredIndex_ = displayedIndex_;
    requestedTime_ = frameTime();
    Q_EMIT changed();
}

void SequencePlayer::clear() {
    QSignalBlocker notifications(this);
    suspend();
    sequence_.reset();
    fdu4d_.reset();
    displayedTime_ = 0;
    cache_.clear();
    cacheBytes_ = 0;
    cacheHits_ = 0;
    frameReads_ = 0;
    seekRequests_ = 0;
    supersededSeeks_ = 0;
    failedSeeks_ = 0;
    noOpSeeks_ = 0;
    cancelledSeeks_ = 0;
    cacheLookups_ = 0;
    cacheEvictions_ = 0;
    firstVisibleMs_ = RollingMetric{};
    targetReadyMs_ = RollingMetric{};
    loadMetrics_.clear();
    lastCompletedSeek_.clear();
    requestedTime_ = 0;
    error_.clear();
    suspended_ = false;
    notifications.unblock();
    Q_EMIT changed();
}

void SequencePlayer::activate(std::shared_ptr<const PlySequence> sequence,
                              std::shared_ptr<const StaticScene> first) {
    QSignalBlocker notifications(this);
    clear();
    sequence_ = std::move(sequence);
    displayedIndex_ = 0;
    desiredIndex_ = 0;
    if (active()) {
        requestedTime_ = startTime();
        cache(0, std::move(first));
        frameReads_ = 1;
    }
    notifications.unblock();
    Q_EMIT changed();
}

void SequencePlayer::setLooping(bool enabled) {
    looping_ = enabled;
    Q_EMIT changed();
}

void SequencePlayer::activateFdu4d(std::shared_ptr<const Fdu4dScene> scene) {
    QSignalBlocker notifications(this);
    clear();
    fdu4d_ = std::move(scene);
    displayedIndex_ = 0;
    desiredIndex_ = 0;
    displayedTime_ = startTime();
    requestedTime_ = displayedTime_;
    notifications.unblock();
    Q_EMIT changed();
}

void SequencePlayer::setPlaying(bool enabled) {
    if (enabled && active() && !suspended_ && error_.isEmpty()) {
        cancelSeek();
        if (requestedTime_ >= endTime()) {
            request(startTime());
        }
        if (active() && !suspended_ && error_.isEmpty()) {
            clock_.start();
            timer_.start();
        }
    } else {
        timer_.stop();
    }
    Q_EMIT changed();
}

void SequencePlayer::seek(double seconds) {
    setPlaying(false);
    if (active() && !suspended_ && std::isfinite(seconds)) {
        const bool retry = !error_.isEmpty();
        error_.clear();
        const auto target = fdu4d_ ? fdu4d_->clampTime(seconds) : sequence_->clampTime(seconds);
        const auto index = fdu4d_ ? std::size_t(fdu4d_->sampleAt(target)) : sequence_->frameAt(target);
        beginSeek(fdu4d_ ? target : sequence_->frames[index].timestampSeconds, index, retry);
        if (fdu4d_) {
            requestedTime_ = target;
            desiredIndex_ = std::size_t(fdu4d_->sampleAt(target));
            if (retry && !pending_) {
                present(desiredIndex_, {});
                return;
            }
            request(target);
            return;
        }
        if (retry && !pending_ && !readPending_ && sequence_->frameAt(target) == displayedIndex_) {
            requestedTime_ = target;
            desiredIndex_ = displayedIndex_;
            present(displayedIndex_, viewport_.content().scene);
            return;
        }
        request(target);
    }
}

void SequencePlayer::step(int direction) {
    setPlaying(false);
    if (!active() || suspended_) {
        return;
    }
    const auto index = std::clamp(std::int64_t(desiredIndex_) + (direction < 0 ? -1 : 1),
                                  std::int64_t(0),
                                  std::int64_t(frameCount() - 1));
    seek(fdu4d_ ? fdu4d_->sampleTime(int(index)) : sequence_->frames[std::size_t(index)].timestampSeconds);
}

void SequencePlayer::advance(double seconds) {
    if (!playing() || !active() || !std::isfinite(seconds) || seconds < 0) {
        return;
    }
    request(fdu4d_ ? fdu4d_->advance(requestedTime_, seconds, looping_)
                   : sequence_->advance(requestedTime_, seconds, looping_));
    if (!looping_ && requestedTime_ >= endTime()) {
        setPlaying(false);
    }
}

void SequencePlayer::request(double seconds) {
    requestedTime_ = seconds;
    desiredIndex_ = fdu4d_ ? std::size_t(fdu4d_->sampleAt(seconds)) : sequence_->frameAt(seconds);
    dispatch();
    Q_EMIT changed();
}

void SequencePlayer::dispatch() {
    if (fdu4d_) {
        if (!suspended_ && !pending_ && error_.isEmpty() && requestedTime_ != displayedTime_) {
            present(desiredIndex_, {});
        }
        return;
    }
    if (!active() || suspended_ || readPending_ || pending_ || desiredIndex_ == displayedIndex_ ||
        !error_.isEmpty()) {
        return;
    }
    const auto hit = cache_.find(desiredIndex_);
    ++cacheLookups_;
    if (hit != cache_.end()) {
        ++cacheHits_;
        hit->second.used = ++useSerial_;
        present(desiredIndex_, hit->second.scene);
        return;
    }
    const auto index = desiredIndex_;
    const auto generation = generation_;
    const QPointer<SequencePlayer> receiver(this);
    makeReadHeadroom();
    readPending_ = true;
    const auto accepted =
        loader_.load(sequence_->frames[index].asset, [receiver, generation, index](SceneLoadResult result) {
            if (receiver) {
                receiver->finishRead(generation, index, std::move(result));
            }
        });
    if (!accepted) {
        readPending_ = false;
        fail("Sequence IO queue is busy or stopped; seek again to retry");
    }
}

void SequencePlayer::makeReadHeadroom() {
    const auto& limits = loader_.limits();
    const auto headroom = std::min(limits.decodedBytes, limits.residentBytes / 2);
    while (cache_.size() > 1 && loader_.residentBytes() > limits.residentBytes - headroom) {
        auto oldest = cache_.end();
        for (auto candidate = cache_.begin(); candidate != cache_.end(); ++candidate) {
            if (candidate->first != displayedIndex_ &&
                (oldest == cache_.end() || candidate->second.used < oldest->second.used)) {
                oldest = candidate;
            }
        }
        if (oldest == cache_.end()) {
            break;
        }
        evict(oldest);
    }
}

void SequencePlayer::finishRead(std::uint64_t generation, std::size_t index, SceneLoadResult result) {
    readPending_ = false;
    if (generation != generation_ || !active() || suspended_) {
        result.scene.reset();
        result.fdu4d.reset();
        dispatch();
        return;
    }
    ++frameReads_;
    addLoadTimings(result.timings);
    if (!result.scene) {
        fail(result.fdu4d ? "PLY Sequence requires static frames" : QString::fromStdString(result.error));
    } else if (result.scene->memory.bytes() > loader_.limits().sequenceCacheBytes) {
        fail("Sequence frame exceeds the configured cache budget");
    } else {
        // Complete this accepted frame before servicing a newer seek target.
        present(index, std::move(result.scene));
    }
}

void SequencePlayer::present(std::size_t index, std::shared_ptr<const StaticScene> scene) {
    PendingFrame pending{};
    pending.index = index;
    pending.time = fdu4d_ ? requestedTime_ : sequence_->frames[index].timestampSeconds;
    pending.previous = viewport_.content();
    const auto previousVersion = viewport_.sceneVersion();
    if (fdu4d_) {
        viewport_.setFduTime(pending.time);
    } else {
        viewport_.setSequenceFrame(std::move(scene), pending.time);
    }
    // Retrying unchanged content must bypass the renderer's failed-version guard.
    if (viewport_.sceneVersion() == previousVersion) {
        viewport_.restoreContent(viewport_.content());
    }
    pending.version = viewport_.sceneVersion();
    pending_ = std::move(pending);
    associateSeek();
    Q_EMIT changed();
    Q_EMIT frameRequested();
}

void SequencePlayer::cache(std::size_t index, std::shared_ptr<const StaticScene> scene) {
    if (cache_.contains(index)) {
        cache_[index].used = ++useSerial_;
        return;
    }
    const auto bytes = scene->memory.bytes();
    const auto limit = loader_.limits().sequenceCacheBytes;
    while (!cache_.empty() && cacheBytes_ > limit - bytes) {
        const auto oldest =
            std::min_element(cache_.begin(), cache_.end(), [](const auto& first, const auto& second) {
                return first.second.used < second.second.used;
            });
        evict(oldest);
    }
    cache_[index] = {std::move(scene), ++useSerial_};
    cacheBytes_ += bytes;
}

SequencePlayer::FrameAcceptance SequencePlayer::acceptFrame(const FrameStats& frame, const QString& error) {
    if (!active() || suspended_) {
        return FrameAcceptance::NotHandled;
    }
    if (!pending_) {
        if (!error.isEmpty() && frame.sceneVersion == viewport_.sceneVersion()) {
            fail(error);
            return FrameAcceptance::FailedConsumed;
        }
        return FrameAcceptance::NotHandled;
    }
    if (frame.sceneVersion != pending_->version) {
        return FrameAcceptance::NotHandled;
    }
    if (!error.isEmpty()) {
        restorePendingFrame();
        fail(error);
        Q_EMIT frameRequested();
        return FrameAcceptance::FailedConsumed;
    }
    observeSeek(frame);
    if (!frame.gaussianCount || frame.refining) {
        return FrameAcceptance::NotHandled;
    }
    displayedIndex_ = pending_->index;
    if (fdu4d_) {
        displayedTime_ = pending_->time;
    } else {
        cache(displayedIndex_, viewport_.content().scene);
    }
    pending_.reset();
    Q_EMIT changed();
    dispatch();
    return FrameAcceptance::Committed;
}

void SequencePlayer::restorePendingFrame() {
    if (!pending_) {
        return;
    }
    viewport_.restoreContent(pending_->previous);
    pending_.reset();
}

void SequencePlayer::fail(QString error) {
    if (seek_) {
        ++failedSeeks_;
        seek_.reset();
    }
    timer_.stop();
    error_ = std::move(error);
    desiredIndex_ = displayedIndex_;
    if (fdu4d_) {
        requestedTime_ = displayedTime_;
    }
    Q_EMIT changed();
}

void SequencePlayer::evict(std::map<std::size_t, CacheEntry>::iterator entry) {
    cacheBytes_ -= entry->second.scene->memory.bytes();
    cache_.erase(entry);
    ++cacheEvictions_;
}

void SequencePlayer::beginSeek(double time, std::size_t index, bool retry) {
    ++seekRequests_;
    ++requestSerial_;
    if (seek_) {
        ++supersededSeeks_;
    }
    seek_.reset();
    if (!retry && !pending_ && !readPending_ && index == displayedIndex_ &&
        (!fdu4d_ || time == displayedTime_)) {
        ++noOpSeeks_;
        return;
    }
    seek_ = SeekRequest{requestSerial_, index, time, 0, now_(), {}};
    associateSeek();
}

void SequencePlayer::associateSeek() {
    if (seek_ && pending_ && seek_->index == pending_->index && seek_->time == pending_->time) {
        seek_->version = pending_->version;
    }
}

void SequencePlayer::observeSeek(const FrameStats& frame) {
    if (!seek_ || !seek_->version || frame.sceneVersion != seek_->version || !frame.gaussianCount) {
        return;
    }
    const auto elapsed = std::chrono::duration<double, std::milli>(now_() - seek_->started).count();
    if (!seek_->firstVisibleMs) {
        seek_->firstVisibleMs = elapsed;
    }
    if (!frame.refining) {
        firstVisibleMs_.add(seek_->firstVisibleMs);
        targetReadyMs_.add(elapsed);
        lastCompletedSeek_ = {{"requestId", qulonglong(seek_->id)},
                              {"frameIndex", qulonglong(seek_->index)},
                              {"timestampSeconds", seek_->time},
                              {"sceneVersion", qulonglong(frame.sceneVersion)},
                              {"firstVisibleMs", *seek_->firstVisibleMs},
                              {"targetReadyMs", elapsed}};
        seek_.reset();
    }
}

void SequencePlayer::cancelSeek() {
    if (seek_) {
        ++cancelledSeeks_;
        seek_.reset();
    }
}

void SequencePlayer::addLoadTimings(const SceneLoadTimings& timings) {
    loadMetrics_.try_emplace("workerQueueMs").first->second.add(timings.workerQueueMs);
    loadMetrics_.try_emplace("readMs").first->second.add(timings.readMs);
    loadMetrics_.try_emplace("decodeMs").first->second.add(timings.decodeMs);
    loadMetrics_.try_emplace("completionQueueMs").first->second.add(timings.completionQueueMs);
    loadMetrics_.try_emplace("loadTotalMs").first->second.add(timings.totalMs);
}

QVariantMap SequencePlayer::statistics() const {
    const auto metric = [](const RollingMetric& samples) {
        const auto p50 = samples.percentile(.5);
        const auto p95 = samples.percentile(.95);
        return QVariantMap{{"samples", qulonglong(samples.count())},
                           {"p50", p50 ? QVariant(*p50) : QVariant{}},
                           {"p95", p95 ? QVariant(*p95) : QVariant{}}};
    };
    QVariantMap result{{"requestId", qulonglong(requestSerial_)},
                       {"requestVersion", qulonglong(seek_ ? seek_->version : 0)},
                       {"seekRequests", qulonglong(seekRequests_)},
                       {"supersededSeeks", qulonglong(supersededSeeks_)},
                       {"failedSeeks", qulonglong(failedSeeks_)},
                       {"noOpSeeks", qulonglong(noOpSeeks_)},
                       {"cancelledSeeks", qulonglong(cancelledSeeks_)},
                       {"lastCompletedSeek", lastCompletedSeek_},
                       {"seekFirstVisibleMs", metric(firstVisibleMs_)},
                       {"seekTargetReadyMs", metric(targetReadyMs_)}};
    if (sequence_) {
        result["cacheBytes"] = qulonglong(cacheBytes_);
        result["cacheCapacityBytes"] = qulonglong(loader_.limits().sequenceCacheBytes);
        result["residentBytes"] = qulonglong(loader_.residentBytes());
        result["cacheHits"] = qulonglong(cacheHits_);
        result["cacheLookups"] = qulonglong(cacheLookups_);
        result["cacheEvictions"] = qulonglong(cacheEvictions_);
        result["cacheHitRatio"] =
            cacheLookups_ ? QVariant(double(cacheHits_) / double(cacheLookups_)) : QVariant{};
        for (const auto& [name, samples] : loadMetrics_) {
            result[QString::fromStdString(name)] = metric(samples);
        }
    }
    return result;
}
}
