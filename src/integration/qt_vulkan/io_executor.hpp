#pragma once

#include <orbelio/core/executor.hpp>
#include <QThreadPool>
#include <condition_variable>
#include <mutex>

namespace orbelio {
class QtIoExecutor final : public IExecutor {
public:
    explicit QtIoExecutor(std::size_t maxTasks = 16, std::size_t maxBytes = 256 * 1024 * 1024);
    ~QtIoExecutor() override;
    bool tryPost(Task task, std::size_t retainedBytes = 0) override;
    void drain();
    void stop();
    std::size_t pendingTasks() const;

private:
    // Runs one queued task, then always hands its reservation back.
    void runTask(Task task, std::size_t retainedBytes);
    void retire(std::size_t retainedBytes);

    QThreadPool pool_;
    mutable std::mutex mutex_;
    std::condition_variable idle_;
    bool stopping_ = false;
    std::size_t pending_ = 0;
    std::size_t bytes_ = 0;
    std::size_t maxTasks_ = 0;
    std::size_t maxBytes_ = 0;
};
} // namespace orbelio
