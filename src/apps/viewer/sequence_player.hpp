#pragma once

#include <orbelio/core/scene_loader.hpp>
#include <orbelio/core/viewport_state.hpp>
#include <QObject>
#include <QTimer>
#include <QElapsedTimer>
#include <QString>
#include <map>
#include <optional>
#include <orbelio/core/performance.hpp>
#include <QVariantMap>

namespace orbelio {
class SequencePlayer final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool active READ active NOTIFY changed)
    Q_PROPERTY(bool playing READ playing WRITE setPlaying NOTIFY changed)
    Q_PROPERTY(bool looping READ looping WRITE setLooping NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(double requestedTime READ requestedTime NOTIFY changed)
    Q_PROPERTY(double frameTime READ frameTime NOTIFY changed)
    Q_PROPERTY(double startTime READ startTime NOTIFY changed)
    Q_PROPERTY(double endTime READ endTime NOTIFY changed)
    Q_PROPERTY(int frameIndex READ frameIndex NOTIFY changed)
    Q_PROPERTY(int frameCount READ frameCount NOTIFY changed)
    Q_PROPERTY(qulonglong cacheBytes READ cacheBytes NOTIFY changed)
    Q_PROPERTY(qulonglong cacheHits READ cacheHits NOTIFY changed)
    Q_PROPERTY(qulonglong frameReads READ frameReads NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(QVariantMap statistics READ statistics NOTIFY changed)
public:
    enum class FrameAcceptance { NotHandled, Committed, FailedConsumed };

    SequencePlayer(SceneLoader& loader,
                   ViewportState& viewport,
                   QObject* parent = nullptr,
                   SceneLoader::Now now = SceneLoader::Clock::now);

    bool active() const { return sequence_ || fdu4d_; }

    bool playing() const { return timer_.isActive(); }

    bool looping() const { return looping_; }

    bool busy() const {
        if (fdu4d_) {
            return suspended_ || pending_ || requestedTime_ != displayedTime_;
        }
        return active() && (suspended_ || readPending_ || pending_ || desiredIndex_ != displayedIndex_);
    }

    double requestedTime() const { return requestedTime_; }

    double frameTime() const {
        return fdu4d_ ? displayedTime_ : sequence_ ? sequence_->frames[displayedIndex_].timestampSeconds : 0;
    }

    double startTime() const {
        return fdu4d_ ? fdu4d_->startTime : sequence_ ? sequence_->frames.front().timestampSeconds : 0;
    }

    double endTime() const { return fdu4d_ ? fdu4d_->endTime : sequence_ ? sequence_->endTimeSeconds : 0; }

    int frameIndex() const { return active() ? int(displayedIndex_) : -1; }

    int frameCount() const {
        return fdu4d_ ? fdu4d_->sampleCount() : sequence_ ? int(sequence_->frames.size()) : 0;
    }

    qulonglong cacheBytes() const { return cacheBytes_; }

    qulonglong cacheHits() const { return cacheHits_; }

    qulonglong frameReads() const { return frameReads_; }

    QString error() const { return error_; }

    QVariantMap statistics() const;

    ViewportState::ContentSnapshot confirmedContent() const {
        return pending_ ? pending_->previous : viewport_.content();
    }

    QString frameAsset() const {
        return sequence_ ? QString::fromStdString(sequence_->frames[displayedIndex_].asset) : QString{};
    }

    void activate(std::shared_ptr<const PlySequence> sequence, std::shared_ptr<const StaticScene> first);
    void activateFdu4d(std::shared_ptr<const Fdu4dScene> scene);
    void suspend();
    void resume();
    void clear();
    void setPlaying(bool enabled);
    void setLooping(bool enabled);
    Q_INVOKABLE void seek(double seconds);
    Q_INVOKABLE void step(int direction);
    void advance(double seconds);
    FrameAcceptance acceptFrame(const FrameStats& frame, const QString& error);
Q_SIGNALS:
    void changed();
    void frameRequested();

private:
    struct CacheEntry {
        std::shared_ptr<const StaticScene> scene;
        std::uint64_t used = 0;
    };

    struct PendingFrame {
        std::size_t index = 0;
        std::uint64_t version = 0;
        double time = 0;
        ViewportState::ContentSnapshot previous;
    };

    struct SeekRequest {
        std::uint64_t id = 0;
        std::size_t index = 0;
        double time = 0;
        std::uint64_t version = 0;
        SceneLoader::Clock::time_point started;
        std::optional<double> firstVisibleMs;
    };

    void request(double seconds);
    void dispatch();
    void finishRead(std::uint64_t generation, std::size_t index, SceneLoadResult result);
    void present(std::size_t index, std::shared_ptr<const StaticScene> scene);
    void cache(std::size_t index, std::shared_ptr<const StaticScene> scene);
    void makeReadHeadroom();
    void restorePendingFrame();
    void fail(QString error);
    void beginSeek(double time, std::size_t index, bool retry);
    void associateSeek();
    void observeSeek(const FrameStats& frame);
    void addLoadTimings(const SceneLoadTimings& timings);
    void cancelSeek();
    void evict(std::map<std::size_t, CacheEntry>::iterator entry);
    SceneLoader& loader_;
    ViewportState& viewport_;
    std::shared_ptr<const PlySequence> sequence_;
    std::shared_ptr<const Fdu4dScene> fdu4d_;
    double displayedTime_ = 0;
    std::map<std::size_t, CacheEntry> cache_;
    std::optional<PendingFrame> pending_;
    std::size_t displayedIndex_ = 0;
    std::size_t desiredIndex_ = 0;
    std::size_t cacheBytes_ = 0;
    std::uint64_t cacheHits_ = 0;
    std::uint64_t frameReads_ = 0;
    std::uint64_t useSerial_ = 0;
    std::uint64_t generation_ = 0;
    double requestedTime_ = 0;
    bool readPending_ = false;
    bool suspended_ = false;
    bool looping_ = false;
    QString error_;
    QTimer timer_;
    QElapsedTimer clock_;
    SceneLoader::Now now_;
    std::optional<SeekRequest> seek_;
    std::uint64_t requestSerial_ = 0;
    std::uint64_t seekRequests_ = 0;
    std::uint64_t supersededSeeks_ = 0;
    std::uint64_t failedSeeks_ = 0;
    std::uint64_t noOpSeeks_ = 0;
    std::uint64_t cancelledSeeks_ = 0;
    std::uint64_t cacheLookups_ = 0;
    std::uint64_t cacheEvictions_ = 0;
    RollingMetric firstVisibleMs_;
    RollingMetric targetReadyMs_;
    std::map<std::string, RollingMetric, std::less<>> loadMetrics_;
    QVariantMap lastCompletedSeek_;
};
}
