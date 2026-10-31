#include <app_model.hpp>
#include "fdu_fixture.hpp"
#include <QCoreApplication>
#include <deque>
#include <map>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <chrono>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class Executor final : public orbelio::IExecutor {
public:
    std::deque<Task> tasks;
    bool acceptsTasks = true;

    bool tryPost(Task task, std::size_t) override {
        if (!acceptsTasks) {
            return false;
        }
        tasks.push_back(std::move(task));
        return true;
    }

    void run() {
        require(!tasks.empty(), "No queued task");
        auto task = std::move(tasks.front());
        tasks.pop_front();
        task();
    }
};

std::string ply(int count) {
    std::string result = "ply\nformat ascii 1.0\nelement vertex " + std::to_string(count) + "\n";
    for (const auto* field : {"x",
                              "y",
                              "z",
                              "rot_0",
                              "rot_1",
                              "rot_2",
                              "rot_3",
                              "scale_0",
                              "scale_1",
                              "scale_2",
                              "opacity",
                              "f_dc_0",
                              "f_dc_1",
                              "f_dc_2"}) {
        result += std::string("property float ") + field + "\n";
    }
    result += "end_header\n";
    for (int index = 0; index < count; ++index) {
        result += "0 0 0 1 0 0 0 -2 -2 -2 2 0.5 0.2 0.1\n";
    }
    return result;
}

class Reader final : public orbelio::IAssetReader {
public:
    std::map<std::string, std::string> assets;

    orbelio::BytePool::Lease read(std::string_view asset,
                                  orbelio::BytePool& pool,
                                  std::size_t limit) override {
        const auto entry = assets.find(std::string(asset));
        if (entry == assets.end()) {
            throw std::runtime_error("Missing frame");
        }
        const auto& data = entry->second;
        if (data.size() > limit) {
            throw std::runtime_error("Input budget exceeded");
        }
        auto lease = pool.acquire(data.size());
        std::copy(data.begin(), data.end(), reinterpret_cast<char*>(lease.bytes().data()));
        return lease;
    }
};

struct Fixture {
    orbelio::BytePool pool{1048576};
    Executor worker;
    Executor completions;
    Reader reader;
    orbelio::SceneLoader loader;
    orbelio::ViewportState viewport;
    orbelio::AppModel model;
    std::uint64_t uploads = 0;

    explicit Fixture(orbelio::ModelLimits limits = {})
        : loader(worker, completions, reader, pool, limits), model(false, loader, viewport, pool) {
        std::string manifest =
            R"({"schema":"orbelio.ply-sequence","version":1,"endTimeSeconds":3.2,"frames":[)";
        for (int frame = 0; frame < 30; ++frame) {
            if (frame) {
                manifest += ',';
            }
            const auto path = std::to_string(frame) + ".ply";
            manifest +=
                "{\"path\":\"" + path + "\",\"timestampSeconds\":" + std::to_string(frame / 10.) + "}";
            reader.assets["C:/test/" + path] = ply(3 + frame);
        }
        reader.assets["C:/test/scene.gsseq"] = manifest + "]}";
        model.loadModel("C:/test/scene.gsseq");
        read();
        require(!model.sequencePlayer().active(), "Sequence published before its first GPU frame");
        frame();
        require(model.sequencePlayer().active() && model.sequencePlayer().frameCount() == 30,
                "Sequence did not become active after GPU completion");
    }

    void read() {
        worker.run();
        completions.run();
    }

    orbelio::FrameReport report() {
        orbelio::FrameReport report{};
        report.frame.sceneVersion = viewport.sceneVersion();
        report.frame.gaussianCount = viewport.gaussianCount();
        report.frame.sceneUploads = ++uploads;
        report.frame.completeFrameMs = 10;
        report.frame.frameIntervalMs = 16;
        return report;
    }

    void frame(QString error = {}) { model.acceptFrame(report(), error); }
};

void serialSeekAndCache() {
    Fixture fixture;
    auto& player = fixture.model.sequencePlayer();
    fixture.viewport.orbitCamera(13, 7);
    const auto camera = fixture.viewport.camera();
    player.seek(0.5);
    player.seek(1.099);
    require(fixture.worker.tasks.size() == 1, "Seek flooded IO with obsolete requests");
    fixture.read();
    require(player.frameIndex() == 0 && player.busy() && fixture.worker.tasks.empty(),
            "Frame advanced or dispatched another IO before GPU completion");
    require(fixture.viewport.frame(100, 1, 1).timestampSeconds == 0.5,
            "Renderer snapshot did not carry the selected frame timestamp");
    fixture.frame();
    require(player.frameIndex() == 5 && fixture.worker.tasks.size() == 1,
            "Accepted in-flight frame was cancelled instead of completed");
    fixture.read();
    fixture.frame();
    require(player.frameIndex() == 10 && player.requestedTime() == 1.099 &&
                fixture.viewport.camera().yawDegrees == camera.yawDegrees,
            "Seek selection or independent camera state is incorrect");
    const auto reads = player.frameReads();
    player.seek(0.5);
    fixture.frame();
    require(fixture.worker.tasks.empty() && player.frameReads() == reads && player.cacheHits() > 0,
            "Cached seek decoded the same frame again");
    const auto statistics = player.statistics();
    require(statistics["cacheLookups"].toInt() == 3 && statistics["cacheHits"].toInt() == 1 &&
                statistics["cacheBytes"].toULongLong() <= statistics["cacheCapacityBytes"].toULongLong(),
            "Cache telemetry counted coalesced requests as lookups or exceeded capacity");
}

void failureAndLifetime() {
    Fixture fixture;
    auto& player = fixture.model.sequencePlayer();
    fixture.reader.assets.erase("C:/test/7.ply");
    player.seek(0.7);
    fixture.read();
    require(!player.error().isEmpty() && player.frameIndex() == 0 && !player.busy(),
            "Missing frame discarded the visible frame or left Seek busy");
    player.seek(0.1);
    fixture.read();
    fixture.viewport.orbitCamera(11, 3);
    const auto camera = fixture.viewport.camera();
    fixture.frame("GPU budget rejected");
    require(player.frameIndex() == 0 && fixture.viewport.gaussianCount() == 3 &&
                fixture.viewport.camera().yawDegrees == camera.yawDegrees,
            "GPU failure did not restore the previous sequence frame");
    player.seek(0);
    require(player.error().isEmpty(), "Seek could not recover after an error");
    fixture.frame();
    player.seek(0.2);
    fixture.model.unloadModel();
    fixture.read();
    require(!fixture.viewport.hasScene() && !player.active(), "Late frame revived an unloaded sequence");
}

void allFramesAndLoop() {
    Fixture fixture;
    auto& player = fixture.model.sequencePlayer();
    for (int index = 1; index < 30; ++index) {
        player.step(1);
        fixture.read();
        fixture.frame();
        require(player.frameIndex() == index && fixture.viewport.gaussianCount() == std::size_t(3 + index),
                "Variable-count sequence step selected the wrong frame");
    }
    player.seek(3.2);
    require(player.frameIndex() == 29 && !player.busy(), "Sequence tail interval lost its final frame");
    player.setLooping(true);
    player.seek(2.9);
    player.setPlaying(true);
    player.advance(player.endTime() - player.requestedTime());
    fixture.frame();
    require(player.frameIndex() == 0 && player.requestedTime() == 0,
            "Loop endpoint did not wrap to first frame");
    player.setPlaying(false);
}

void lowCacheBudget() {
    orbelio::ModelLimits limits{};
    limits.sequenceCacheBytes = 512;
    Fixture fixture(limits);
    auto& player = fixture.model.sequencePlayer();
    player.seek(2.9);
    fixture.read();
    require(!player.error().isEmpty() && player.frameIndex() == 0 && player.cacheBytes() <= 512,
            "Oversized frame escaped the cache budget");
    player.seek(0.1);
    fixture.read();
    fixture.frame();
    require(player.frameIndex() == 1 && player.error().isEmpty() && player.cacheBytes() <= 512,
            "Low-budget failure could not recover to a smaller frame");
}

void failedReplacementRestoresDisplayedFrame() {
    Fixture fixture;
    auto& player = fixture.model.sequencePlayer();
    player.seek(.5);
    fixture.read();
    fixture.reader.assets["C:/test/replacement.ply"] = ply(4);
    fixture.model.loadModel("C:/test/replacement.ply");
    fixture.read();
    fixture.frame("GPU rejected replacement");
    require(player.active() && player.frameIndex() == 0 && fixture.viewport.gaussianCount() == 3,
            "Failed model replacement restored an unacknowledged sequence frame");
    player.seek(.1);
    fixture.read();
    fixture.frame();
    require(player.frameIndex() == 1 && !player.busy(), "Restored sequence could not seek again");
}

void rejectedReplayRemainsStopped() {
    orbelio::ModelLimits limits{};
    limits.sequenceCacheBytes = 1800;
    Fixture fixture(limits);
    auto& player = fixture.model.sequencePlayer();
    player.seek(player.endTime());
    fixture.read();
    fixture.frame();
    require(player.frameIndex() == 29 && player.error().isEmpty(), "Replay fixture did not reach its end");
    fixture.worker.acceptsTasks = false;
    player.setPlaying(true);
    require(!player.error().isEmpty() && !player.playing() && !player.busy() && player.frameIndex() == 29,
            "Rejected replay restarted the playback timer after reporting failure");
    fixture.worker.acceptsTasks = true;
    player.seek(player.startTime());
    fixture.read();
    fixture.frame();
    player.setPlaying(true);
    require(player.playing() && player.error().isEmpty(), "Replay could not recover after queue rejection");
    player.setPlaying(false);
}

void sequenceUploadsDoNotResetPerformanceWarmup() {
    Fixture fixture;
    auto& player = fixture.model.sequencePlayer();
    for (int index = 0; index < 60; ++index) {
        player.seek((index % 30) / 10.);
        if (!fixture.worker.tasks.empty()) {
            fixture.read();
        }
        fixture.frame();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(270));
    fixture.frame();
    require(fixture.model.performance()["samples"].toInt() >= 31 &&
                fixture.model.performance()["fps"].toDouble() > 0,
            "Per-frame GPU uploads repeatedly reset sequence performance warmup");
}

void acknowledgedFrameStatisticsSurviveCachedDispatch() {
    Fixture fixture;
    auto& player = fixture.model.sequencePlayer();
    for (int index = 0; index < 35; ++index) {
        fixture.frame();
    }
    player.seek(.1);
    fixture.read();
    fixture.frame();
    std::this_thread::sleep_for(std::chrono::milliseconds(270));
    fixture.frame();
    const auto before = fixture.model.performance()["samples"].toInt();
    player.seek(0);
    player.seek(.1);
    const auto acknowledged = fixture.report();
    std::this_thread::sleep_for(std::chrono::milliseconds(270));
    fixture.model.acceptFrame(acknowledged, {});
    require(player.frameIndex() == 0 && player.busy() &&
                fixture.viewport.sceneVersion() != acknowledged.frame.sceneVersion &&
                fixture.model.performance()["samples"].toInt() == before + 1,
            "Dispatching a cached target discarded the just-acknowledged frame statistics");
    fixture.frame();
}

void currentRenderFailureStopsAndSameFrameSeekRetries() {
    Fixture fixture;
    auto& player = fixture.model.sequencePlayer();
    player.setPlaying(true);
    const auto failedVersion = fixture.viewport.sceneVersion();
    fixture.frame("Resize exceeded the GPU budget");
    require(!player.playing() && !player.error().isEmpty() && !player.busy(),
            "Render failure without a pending upload left sequence playback running");
    player.seek(player.frameTime());
    require(player.busy() && player.error().isEmpty() && fixture.worker.tasks.empty() &&
                fixture.viewport.sceneVersion() != failedVersion,
            "Same-frame retry did not publish a new version of cached content");
    fixture.frame();
    require(!player.busy() && !player.playing() && player.frameIndex() == 0,
            "Same-frame retry failed or automatically resumed playback");
    orbelio::FrameReport obsolete{};
    obsolete.frame.sceneVersion = failedVersion;
    fixture.model.acceptFrame(obsolete, "Obsolete resize error");
    require(player.error().isEmpty(), "Obsolete render failure poisoned the recovered sequence");
}

void frameObserversMayReplaceTheLogicalModel() {
    for (const bool unload : {true, false}) {
        Fixture fixture;
        auto& player = fixture.model.sequencePlayer();
        fixture.reader.assets["C:/test/replacement.ply"] = ply(6);
        player.seek(.1);
        fixture.read();
        const auto before = fixture.model.performance();
        bool armed = true;
        QObject::connect(&player, &orbelio::SequencePlayer::changed, &fixture.model, [&] {
            if (!armed || player.frameIndex() != 1) {
                return;
            }
            armed = false;
            if (unload) {
                fixture.model.unloadModel();
            } else {
                fixture.model.loadModel("C:/test/replacement.ply");
            }
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(270));
        fixture.frame();
        require(!armed && fixture.model.loadState() == (unload ? "Empty" : "Loading"),
                "Frame observer did not replace the model");
        require(unload ? fixture.model.performance().isEmpty() : fixture.model.performance() == before,
                "Committed old frame repopulated statistics after logical model replacement");
        if (!unload) {
            fixture.read();
            fixture.frame();
            require(fixture.model.loadState() == "Ready" && !player.active(),
                    "Reentrant model replacement did not finish");
        }
    }
}

void activationPublishesOnlyCompleteStateAndRespectsUnload() {
    Fixture fixture;
    auto& player = fixture.model.sequencePlayer();
    bool sawInactive = false;
    bool unloadWhenReady = false;
    QObject::connect(&player, &orbelio::SequencePlayer::changed, &fixture.model, [&] {
        sawInactive |= !player.active();
        if (unloadWhenReady && fixture.model.loadState() == "Ready" && player.active()) {
            unloadWhenReady = false;
            fixture.model.unloadModel();
        }
    });
    fixture.model.loadModel("C:/test/scene.gsseq");
    fixture.read();
    fixture.frame();
    require(!sawInactive && player.active(), "Sequence activation exposed intermediate cleared state");
    fixture.model.loadModel("C:/test/scene.gsseq");
    fixture.read();
    unloadWhenReady = true;
    fixture.frame();
    require(!unloadWhenReady && fixture.model.loadState() == "Empty" && !player.active() &&
                fixture.model.performance().isEmpty() && fixture.model.ioStatus() == "No model loaded",
            "Activation notification revived or repopulated an unloaded model");
}

void fduPlaybackAndAcknowledgement() {
    Fixture fixture;
    fixture.reader.assets["C:/test/native.fdu4d"] = fdu_fixture::model();
    fixture.model.loadModel("C:/test/native.fdu4d");
    fixture.read();
    fixture.frame();
    auto& player = fixture.model.sequencePlayer();
    require(player.active() && player.frameCount() == 5 && player.frameReads() == 0 &&
                fixture.viewport.content().fdu4d && !fixture.viewport.content().scene,
            "FDU loader did not publish a distinct dynamic scene");
    const auto camera = fixture.viewport.camera();
    player.seek(.123);
    const auto inFlight = fixture.report();
    player.seek(.456);
    require(player.frameTime() == 0 && player.busy(), "FDU displayed an unacknowledged time");
    fixture.model.acceptFrame(inFlight, {});
    require(player.frameTime() == .123 && player.busy(), "FDU did not coalesce to latest requested time");
    fixture.frame();
    require(player.frameTime() == .456 && !player.busy() && fixture.worker.tasks.empty() &&
                fixture.viewport.camera().yawDegrees == camera.yawDegrees,
            "FDU seek reloaded a file, lost the camera or rounded time to a sample");
    player.step(1);
    fixture.frame();
    require(player.frameTime() == .5, "FDU single step did not use its declared sample rate");
    player.seek(.75);
    fixture.frame("injected FDU render failure");
    require(!player.playing() && player.frameTime() == .5 && !player.busy() && !player.error().isEmpty(),
            "FDU render failure did not roll back and stop");
    player.seek(.5);
    fixture.frame();
    require(player.error().isEmpty() && !player.busy(), "FDU same-time retry failed");
    player.setLooping(true);
    player.seek(1);
    fixture.frame();
    player.setPlaying(true);
    fixture.frame();
    player.setPlaying(false);
    require(player.frameTime() == 0, "FDU replay endpoint did not return to start");
    fixture.model.unloadModel();
    require(!player.active() && !fixture.viewport.hasScene(), "FDU unload retained dynamic state");
}

void retryNotificationMayUnloadTheModel() {
    for (const bool dynamic : {false, true}) {
        Fixture fixture;
        if (dynamic) {
            fixture.reader.assets["C:/test/native.fdu4d"] = fdu_fixture::model();
            fixture.model.loadModel("C:/test/native.fdu4d");
            fixture.read();
            fixture.frame();
        }
        auto& player = fixture.model.sequencePlayer();
        fixture.frame("injected current-frame error");
        bool armed = true;
        QObject::connect(&player, &orbelio::SequencePlayer::changed, &fixture.model, [&] {
            if (armed && player.busy()) {
                armed = false;
                fixture.model.unloadModel();
            }
        });
        player.seek(player.frameTime());
        require(!armed && !player.active() && fixture.model.loadState() == "Empty" &&
                    !fixture.viewport.hasScene(),
                "Retry continued a request after its notification unloaded the model");
    }
}

void renderSettingsKeepContentAndScheduleRefinement() {
    Fixture fixture;
    const auto content = fixture.viewport.content();
    const auto version = fixture.viewport.sceneVersion();
    int requested = 0;
    QObject::connect(
        &fixture.model, &orbelio::AppModel::frameRequested, &fixture.model, [&] { ++requested; });
    fixture.model.setMaxGaussians(1000);
    fixture.model.setUploadGaussiansPerFrame(64);
    require(fixture.viewport.sceneVersion() == version && fixture.viewport.content().scene == content.scene &&
                fixture.model.maxGaussians() == 1000 && fixture.model.uploadGaussiansPerFrame() == 64,
            "Render settings changed scene identity or failed to reach frame snapshots");
    const auto snapshot = fixture.viewport.frame(0, 800, 500);
    require(snapshot.renderSettings.maxGaussians == 1000 &&
                snapshot.renderSettings.uploadGaussiansPerFrame == 64,
            "Render settings did not cross the immutable frame boundary");
    auto report = fixture.report();
    report.frame.refining = true;
    report.frame.submittedGaussians = 32;
    report.frame.budgetedGaussians = 1000;
    const auto before = requested;
    fixture.model.acceptFrame(report, {});
    require(requested == before + 1, "Paused refinement did not request another frame");
    fixture.model.setMaxGaussians(0);
    require(fixture.model.maxGaussians() == 0, "Unlimited quality mode was not restored");
}

void partialUploadsPreserveRollback() {
    Fixture fixture;
    const auto previous = fixture.viewport.content().scene;
    fixture.reader.assets["C:/test/partial.ply"] = ply(100);
    fixture.model.loadModel("C:/test/partial.ply");
    fixture.read();
    auto partial = fixture.report();
    partial.frame.refining = true;
    fixture.model.acceptFrame(partial, {});
    require(fixture.model.isLoading(), "Partial structural upload discarded its rollback target");
    fixture.frame("injected later-batch allocation failure");
    require(fixture.model.loadState() == "Error" && fixture.viewport.content().scene == previous &&
                fixture.model.sequencePlayer().active(),
            "Later progressive allocation failure did not restore the prior sequence");
    auto& player = fixture.model.sequencePlayer();
    player.seek(.5);
    fixture.read();
    partial = fixture.report();
    partial.frame.refining = true;
    fixture.model.acceptFrame(partial, {});
    require(player.frameIndex() == 0 && player.busy(), "Partial sequence upload advanced confirmed time");
    fixture.frame("injected later sequence-batch failure");
    require(player.frameIndex() == 0 && fixture.viewport.content().scene == previous && !player.playing(),
            "Partial sequence failure lost the last completed content");
}

void seekMetricsRequireTheMatchingCompletedTarget() {
    using namespace std::chrono_literals;
    Fixture fixture;
    orbelio::SceneLoader::Clock::time_point time{};
    orbelio::SequencePlayer player(fixture.loader, fixture.viewport, nullptr, [&] { return time; });
    auto sequence = std::make_shared<orbelio::PlySequence>();
    sequence->frames = {{0., "C:/test/0.ply"}, {.1, "C:/test/1.ply"}, {.2, "C:/test/2.ply"}};
    sequence->endTimeSeconds = .3;
    player.activate(sequence, fixture.viewport.content().scene);
    player.seek(.1);
    time += 2ms;
    player.seek(.2);
    fixture.read();
    player.acceptFrame(fixture.report().frame, {});
    require(player.statistics()["seekTargetReadyMs"].toMap()["samples"].toInt() == 0,
            "Obsolete frame completed a newer Seek request");
    time += 3ms;
    fixture.read();
    auto target = fixture.report().frame;
    target.refining = true;
    player.acceptFrame(target, {});
    time += 5ms;
    target.refining = false;
    player.acceptFrame(target, {});
    player.acceptFrame(target, {});
    const auto stats = player.statistics();
    require(stats["supersededSeeks"].toInt() == 1 &&
                stats["seekFirstVisibleMs"].toMap()["p50"].toDouble() == 3. &&
                stats["seekTargetReadyMs"].toMap()["p50"].toDouble() == 8. &&
                stats["seekTargetReadyMs"].toMap()["samples"].toInt() == 1,
            "Seek metrics mixed first visibility, target readiness or duplicate completion");
    const auto completed = stats["lastCompletedSeek"].toMap();
    require(completed["requestId"].toInt() == 2 && completed["frameIndex"].toInt() == 2 &&
                completed["timestampSeconds"].toDouble() == .2 &&
                completed["sceneVersion"].toULongLong() == target.sceneVersion,
            "Completed Seek lost request, target or rendered-version identity");
    player.seek(.2);
    require(player.statistics()["noOpSeeks"].toInt() == 1 &&
                player.statistics()["seekTargetReadyMs"].toMap()["samples"].toInt() == 1,
            "No-op Seek fabricated a zero latency sample");
    player.seek(.1);
    auto failed = fixture.report().frame;
    failed.refining = true;
    player.acceptFrame(failed, {});
    player.acceptFrame(failed, "injected upload failure");
    require(player.statistics()["failedSeeks"].toInt() == 1 &&
                player.statistics()["seekFirstVisibleMs"].toMap()["samples"].toInt() == 1 &&
                player.statistics()["seekTargetReadyMs"].toMap()["samples"].toInt() == 1,
            "Failed progressive Seek entered successful latency samples");
}

void inspectorPublishesTheConfirmedFrameWithoutPerformanceRefresh() {
    Fixture fixture;
    int notifications = 0;
    QObject::connect(
        &fixture.model, &orbelio::AppModel::inspectorChanged, &fixture.model, [&] { ++notifications; });
    const auto initialInfo = fixture.model.inspector();
    auto& player = fixture.model.sequencePlayer();
    player.setLooping(true);
    player.setPlaying(true);
    player.advance(.01);
    player.setPlaying(false);
    player.setLooping(false);
    require(notifications == 0 && fixture.model.inspector() == initialInfo,
            "Playback controls refreshed unchanged Inspector metadata");
    fixture.model.sequencePlayer().seek(.5);
    fixture.read();
    const auto beforeCompletion = notifications;
    fixture.frame();
    require(notifications > beforeCompletion && fixture.model.inspector()["frameIndex"].toInt() == 5 &&
                fixture.model.inspector()["gaussians"].toInt() == 8,
            "Performance throttling hid the newly confirmed Inspector frame");
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        serialSeekAndCache();
        failureAndLifetime();
        allFramesAndLoop();
        lowCacheBudget();
        failedReplacementRestoresDisplayedFrame();
        rejectedReplayRemainsStopped();
        sequenceUploadsDoNotResetPerformanceWarmup();
        acknowledgedFrameStatisticsSurviveCachedDispatch();
        currentRenderFailureStopsAndSameFrameSeekRetries();
        frameObserversMayReplaceTheLogicalModel();
        activationPublishesOnlyCompleteStateAndRespectsUnload();
        fduPlaybackAndAcknowledgement();
        retryNotificationMayUnloadTheModel();
        renderSettingsKeepContentAndScheduleRefinement();
        partialUploadsPreserveRollback();
        seekMetricsRequireTheMatchingCompletedTarget();
        inspectorPublishesTheConfirmedFrameWithoutPerformanceRefresh();
        std::cout << "30 variable-count frames, serialized GPU acknowledgement, cache and recovery passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
