#include <orbelio/core/scene_loader.hpp>
#include <orbelio/core/viewport_state.hpp>
#include <deque>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <utility>

namespace {
constexpr std::size_t expectedGaussians = 90854;
constexpr std::uint64_t captureId = 7;
constexpr std::uint32_t viewportWidth = 640;
constexpr std::uint32_t viewportHeight = 480;
constexpr double sceneTime = 2.5;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class ManualExecutor final : public orbelio::IExecutor {
public:
    bool acceptsTasks = true;
    std::size_t reservation = 0;
    std::deque<Task> tasks;

    bool tryPost(Task task, std::size_t bytes = 0) override {
        if (!acceptsTasks || !task) {
            return false;
        }
        reservation = bytes;
        tasks.push_back(std::move(task));
        return true;
    }

    void runOne() {
        require(!tasks.empty(), "Expected a queued task");
        auto task = std::move(tasks.front());
        tasks.pop_front();
        task();
    }
};

class MemoryReader final : public orbelio::IAssetReader {
public:
    std::vector<std::byte> bytes;
    std::string requestedAsset;
    bool readShouldFail = false;

    MemoryReader() {
        std::ifstream file(ORBELIO_TEST_MODEL, std::ios::binary | std::ios::ate);
        require(bool(file), "Could not open guitar fixture");
        bytes.resize(static_cast<std::size_t>(file.tellg()));
        file.seekg(0);
        file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        require(bool(file), "Could not read guitar fixture");
    }

    orbelio::BytePool::Lease read(std::string_view asset,
                                  orbelio::BytePool& pool,
                                  std::size_t limitBytes) override {
        requestedAsset = asset;
        if (readShouldFail) {
            throw std::runtime_error("Injected read failure");
        }
        require(bytes.size() <= limitBytes, "Reader received the wrong byte limit");
        auto storage = pool.acquire(bytes.size());
        std::copy(bytes.begin(), bytes.end(), storage.bytes().begin());
        return storage;
    }
};

void deferredLoadSurvivesLoaderDestruction() {
    MemoryReader reader;
    orbelio::BytePool pool(orbelio::SceneLoader::modelByteBudget);
    ManualExecutor worker;
    ManualExecutor completions;
    std::optional<orbelio::SceneLoadResult> result;
    {
        orbelio::SceneLoader loader(worker, completions, reader, pool);
        require(loader.load("memory:guitar", [&](auto loaded) { result = std::move(loaded); }),
                "Load rejected");
    }
    require(!result && reader.requestedAsset.empty(), "Load ran on the caller");
    require(worker.reservation == orbelio::SceneLoader::modelByteBudget, "Missing queue reservation");
    worker.runOne();
    require(reader.requestedAsset == "memory:guitar", "Reader dependency was not used");
    require(!result && completions.tasks.size() == 1, "Completion escaped its injected executor");
    require(pool.stats().activeBytes == 0, "Read lease survived into the completion queue");
    completions.runOne();
    require(result && result->scene && result->scene->count() == expectedGaussians, "Decoded scene changed");
    require(result->error.empty() && result->ioMilliseconds >= 0, "Invalid load result");
}

void readAndDecodeFailuresUseCompletionExecutor() {
    MemoryReader reader;
    reader.readShouldFail = true;
    orbelio::BytePool pool(orbelio::SceneLoader::modelByteBudget);
    ManualExecutor worker;
    ManualExecutor completions;
    orbelio::SceneLoader loader(worker, completions, reader, pool);
    std::optional<orbelio::SceneLoadResult> result;
    require(loader.load("memory:failure", [&](auto value) { result = std::move(value); }), "Load rejected");
    worker.runOne();
    require(!result, "Read failure bypassed the completion executor");
    completions.runOne();
    require(!result->scene && result->error == "Injected read failure", "Read failure was lost");
    reader.readShouldFail = false;
    reader.bytes.assign(1, std::byte{});
    result.reset();
    require(loader.load("memory:invalid", [&](auto value) { result = std::move(value); }), "Load rejected");
    worker.runOne();
    require(!result, "Decode failure bypassed the completion executor");
    completions.runOne();
    require(!result->scene && !result->error.empty(), "Invalid PLY was accepted");
    require(pool.stats().activeBytes == 0, "Failure leaked a pool lease");
}

void rejectedWorkDoesNotRead() {
    MemoryReader reader;
    orbelio::BytePool pool(orbelio::SceneLoader::modelByteBudget);
    ManualExecutor worker;
    ManualExecutor completions;
    orbelio::SceneLoader loader(worker, completions, reader, pool);
    worker.acceptsTasks = false;
    require(!loader.load("memory:busy", [](auto) {}), "Worker backpressure was ignored");
    require(reader.requestedAsset.empty() && completions.tasks.empty(), "Rejected work touched dependencies");
    worker.acceptsTasks = true;
    require(!loader.load("memory:no-callback", {}), "Empty completion was accepted");
    require(worker.tasks.empty(), "Invalid load was queued");
    completions.acceptsTasks = false;
    require(loader.load("memory:closed-completions", [](auto) {}), "Load rejected");
    bool rejectionReported = false;
    try {
        worker.runOne();
    } catch (const std::runtime_error&) {
        rejectionReported = true;
    }
    require(rejectionReported && pool.stats().activeBytes == 0, "Completion rejection was silently lost");
}

void queuedResultsShareTheResidentBudget() {
    MemoryReader reader;
    orbelio::BytePool pool(orbelio::SceneLoader::modelByteBudget);
    ManualExecutor worker;
    ManualExecutor completions;
    orbelio::ModelLimits limits{};
    limits.residentBytes = 12 * orbelio::ModelLimits::mebibyte;
    orbelio::SceneLoader loader(worker, completions, reader, pool, limits);
    std::vector<orbelio::SceneLoadResult> results;
    auto completion = [&](auto result) { results.push_back(std::move(result)); };
    for (int request = 0; request < 3; ++request) {
        require(loader.load("memory:guitar", completion), "Load rejected");
        worker.runOne();
    }
    require(loader.residentBytes() > 0 && loader.residentBytes() <= limits.residentBytes,
            "Queued results escaped the resident budget");
    while (!completions.tasks.empty()) {
        completions.runOne();
    }
    require(results.size() == 3 && results[0].scene && results[1].scene && !results[2].scene &&
                !results[2].error.empty(),
            "Queued result budget did not reject the third scene");
    results.clear();
    require(loader.residentBytes() == 0, "Delivered results leaked resident reservations");
    require(loader.load("memory:guitar", completion), "Retry rejected");
    worker.runOne();
    require(loader.residentBytes() > 0, "Undelivered scene has no reservation");
    completions.tasks.clear();
    require(loader.residentBytes() == 0, "Dropped completion leaked its scene reservation");
}

void viewportsHaveIndependentSnapshots() {
    orbelio::ViewportState first;
    orbelio::ViewportState second;
    auto scene = std::make_shared<orbelio::StaticScene>();
    first.setScene(scene);
    first.requestCapture(captureId);
    const auto snapshot = first.frame(sceneTime, viewportWidth, viewportHeight);
    require(snapshot.scene == scene && snapshot.captureRequestId == captureId, "Injected state was lost");
    require(snapshot.timestampSeconds == sceneTime && snapshot.width == viewportWidth &&
                snapshot.height == viewportHeight,
            "Frame dimensions or clock changed");
    const auto other = second.frame(0, 0, 0);
    require(!other.scene && !other.captureRequestId, "Scene or capture leaked between viewports");
    require(other.width == 1 && other.height == 1, "Empty viewport was not bounded");
    first.setScene({});
    first.requestCapture(0);
    require(snapshot.scene == scene && snapshot.captureRequestId == captureId, "Snapshot mutated later");
    require(!first.frame(0, 1, 1).scene, "Scene could not be released");
}
}

int main() {
    try {
        deferredLoadSurvivesLoaderDestruction();
        readAndDecodeFailuresUseCompletionExecutor();
        rejectedWorkDoesNotRead();
        queuedResultsShareTheResidentBudget();
        viewportsHaveIndependentSnapshots();
        std::cout << "Injected loading, dispatch, lifetime, failure and viewport isolation: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
