#include <io_executor.hpp>
#include <QCoreApplication>
#include <QThread>
#include <atomic>
#include <functional>
#include <future>
#include <iostream>
#include <mutex>
#include <set>
#include <stdexcept>

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
    std::promise<void> entered, unblock;
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
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        orbelio::QtIoExecutor io(maxQueuedTasks, ioByteBudget);
        checkSingleWorkerReuse(io);
        checkBackpressure(io);
        checkExceptionAccounting(io);
        checkStopRejects(io);
        std::cout << "Single-worker reuse, backpressure, exception accounting and stop: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
