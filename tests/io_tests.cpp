#include <io_executor.hpp>
#include <asset_reader.hpp>
#include <orbelio/core/scene_loader.hpp>
#include "fdu_fixture.hpp"
#include <QCoreApplication>
#include <QThread>
#include <atomic>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <mutex>
#include <set>
#include <stdexcept>
#include <QTemporaryFile>
#include <QSemaphore>
#include <QScopeGuard>
#include <deque>
#include <QDir>
#include <QDataStream>

namespace {
constexpr std::size_t maxQueuedTasks = 4;
constexpr std::size_t ioByteBudget = 8192;
constexpr std::size_t taskReservationBytes = 4096;
constexpr std::size_t fullBudgetReservationBytes = ioByteBudget;
constexpr std::size_t overflowReservationBytes = 1;
constexpr int postedTasks = 20;

void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

void post(orbelio::QtIoExecutor& io, std::function<void()> task, std::size_t bytes, const char* message) {
    require(io.tryPost(std::move(task), bytes), message);
}

void recordWorkerThread(std::set<Qt::HANDLE>& threads, std::mutex& mutex, std::atomic_int& completed) {
    std::lock_guard lock(mutex);
    threads.insert(QThread::currentThreadId());
    ++completed;
}

void checkSingleWorkerReuse(orbelio::QtIoExecutor& io) {
    std::atomic_int completed{0};
    std::set<Qt::HANDLE> threads;
    std::mutex mutex;
    for (int i = 0; i < postedTasks; ++i) {
        post(
            io,
            [&] { recordWorkerThread(threads, mutex, completed); },
            taskReservationBytes,
            "Valid task rejected");
        io.drain();
    }
    require(completed == postedTasks, "Tasks did not all run");
    require(threads.size() == 1, "IO must reuse exactly one worker");
    require(*threads.begin() != QThread::currentThreadId(), "IO unexpectedly ran on GUI thread");
}

// The gating task holds the whole byte budget, so the next reservation cannot be admitted.
void checkBackpressure(orbelio::QtIoExecutor& io) {
    std::promise<void> entered;
    std::promise<void> unblock;
    auto gate = unblock.get_future().share();
    post(
        io,
        [&] {
            entered.set_value();
            gate.wait();
        },
        fullBudgetReservationBytes,
        "Blocking test task rejected");
    entered.get_future().wait();
    const bool overBudget = io.tryPost([] {}, overflowReservationBytes);
    unblock.set_value();
    io.drain();
    require(!overBudget, "Byte budget did not apply backpressure");
}

void checkExceptionAccounting(orbelio::QtIoExecutor& io) {
    post(
        io,
        [] { throw std::runtime_error("intentional test failure"); },
        taskReservationBytes,
        "Exception task rejected");
    io.drain();
    require(io.pendingTasks() == 0, "Exception leaked pending task accounting");
}

void checkStopRejects(orbelio::QtIoExecutor& io) {
    io.stop();
    require(!io.tryPost([] {}), "Stopped executor accepted a task");
}

class ManualCompletions final : public orbelio::IExecutor {
public:
    std::deque<Task> tasks;

    bool tryPost(Task task, std::size_t) override {
        tasks.push_back(std::move(task));
        return true;
    }
};

QByteArray tinyGaussian() {
    QByteArray result;
    QDataStream stream(&result, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.setFloatingPointPrecision(QDataStream::SinglePrecision);
    for (const auto value : {0.f, 0.f, 0.f, 1.f, 1.f, 1.f}) {
        stream << value;
    }
    stream.writeRawData("\xff\xff\xff\xff\xff\x80\x80\x80", 8);
    return result;
}

void checkQueuedSceneReadsKeepSeparateMemoryBudgets(orbelio::QtIoExecutor& io) {
    constexpr std::size_t residentBudget = 100;
    QTemporaryFile asset(QDir::tempPath() + "/orbelio-io-XXXXXX.splat");
    require(asset.open() && asset.write(tinyGaussian()) == tinyGaussian().size(),
            "Could not write IO fixture");
    asset.close();
    orbelio::QtAssetReader reader;
    orbelio::BytePool pool(ioByteBudget);
    ManualCompletions completions;
    orbelio::ModelLimits limits{};
    limits.inputBytes = ioByteBudget;
    limits.residentBytes = residentBudget;
    orbelio::SceneLoader loader(io, completions, reader, pool, limits);
    std::vector<orbelio::SceneLoadResult> results;
    QSemaphore gate;
    const auto unblock = qScopeGuard([&] {
        gate.release();
        io.drain();
    });
    post(io, [&] { gate.acquire(); }, 0, "Could not hold IO worker");
    const auto path = asset.fileName().toUtf8().toStdString();
    const auto completion = [&](auto result) { results.push_back(std::move(result)); };
    const bool first = loader.load(path, completion);
    const bool second = loader.load(path, completion);
    gate.release();
    io.drain();
    require(first && second, "A queued read reserved the whole future input budget and rejected replacement");
    require(completions.tasks.size() == 2 && loader.residentBytes() <= residentBudget &&
                pool.stats().activeBytes == 0 && pool.stats().peakBytes <= ioByteBudget,
            "Queued reads exceeded resident/input budgets or retained an input lease");
    for (auto& task : completions.tasks) {
        task();
    }
    require(results.size() == 2, "Queued reads did not both complete");
    if (!results[0].scene || results[1].scene || results[1].error.empty()) {
        throw std::runtime_error("Queued results violated resident budget: first=" + results[0].error +
                                 "; second=" + results[1].error);
    }
    completions.tasks.clear();
    results.clear();
    require(loader.residentBytes() == 0, "Consumed queued results retained resident memory");
}

void checkStreamedFduScene(orbelio::QtIoExecutor& io) {
    constexpr std::size_t count = 130;
    constexpr std::size_t headerBytes = 64;
    auto bytes = fdu_fixture::model();
    const auto row = bytes.substr(headerBytes);
    bytes.resize(headerBytes);
    std::string encodedCount;
    fdu_fixture::append(encodedCount, std::uint64_t(count));
    bytes.replace(16, encodedCount.size(), encodedCount);
    for (std::size_t i = 0; i < count; ++i) {
        bytes += row;
    }

    QTemporaryFile asset(QDir::tempPath() + "/orbelio-io-XXXXXX.FDU4D");
    require(asset.open() && asset.write(bytes.data(), bytes.size()) == qsizetype(bytes.size()),
            "Could not write streamed FDU fixture");
    asset.close();
    orbelio::QtAssetReader reader;
    orbelio::BytePool pool(ioByteBudget);
    ManualCompletions completions;
    orbelio::ModelLimits limits{};
    limits.inputBytes = ioByteBudget;
    orbelio::SceneLoader loader(io, completions, reader, pool, limits);
    auto load = [&] {
        orbelio::SceneLoadResult result;
        require(loader.load(asset.fileName().toUtf8().toStdString(),
                            [&](auto loaded) { result = std::move(loaded); }),
                "Could not queue streamed model");
        io.drain();
        require(completions.tasks.size() == 1, "Missing streamed model completion");
        completions.tasks.front()();
        completions.tasks.clear();
        return result;
    };
    auto result = load();
    require(bytes.size() > ioByteBudget && result.fdu4d && result.fdu4d->count() == count &&
                result.fdu4d->times.back() == .25f && pool.stats().peakBytes == ioByteBudget &&
                pool.stats().activeBytes == 0 && pool.stats().allocations == 1,
            "FDU file did not decode completely through one reused input block");
    result.fdu4d.reset();

    QFile corrupt(asset.fileName());
    std::string invalid;
    fdu_fixture::append(invalid, std::numeric_limits<float>::quiet_NaN());
    require(corrupt.open(QIODevice::ReadWrite) && corrupt.seek(corrupt.size() - qsizetype(invalid.size())) &&
                corrupt.write(invalid.data(), invalid.size()) == qsizetype(invalid.size()),
            "Could not corrupt the last streamed record");
    corrupt.close();
    result = load();
    require(!result.fdu4d && !result.error.empty() && !loader.residentBytes() && !pool.stats().activeBytes,
            "Failed streamed decode retained a scene or input lease");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        orbelio::QtIoExecutor io(maxQueuedTasks, ioByteBudget);
        checkSingleWorkerReuse(io);
        checkBackpressure(io);
        checkExceptionAccounting(io);
        checkQueuedSceneReadsKeepSeparateMemoryBudgets(io);
        checkStreamedFduScene(io);
        checkStopRejects(io);
        std::cout << "Single-worker reuse, backpressure, exception accounting and stop: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
