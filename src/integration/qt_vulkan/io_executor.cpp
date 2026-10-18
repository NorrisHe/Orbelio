#include "io_executor.hpp"
#include <QDebug>
#include <exception>

namespace orbelio {
QtIoExecutor::QtIoExecutor(std::size_t maxTasks, std::size_t maxBytes)
    : maxTasks_(maxTasks), maxBytes_(maxBytes) {
    pool_.setMaxThreadCount(1);
    pool_.setExpiryTimeout(-1);
    pool_.setObjectName("Orbelio.IO");
}

QtIoExecutor::~QtIoExecutor() { stop(); }

bool QtIoExecutor::tryPost(Task task, std::size_t retainedBytes) {
    if (!task) {
        return false;
    }
    {
        std::lock_guard lock(mutex_);
        if (stopping_ || pending_ >= maxTasks_ || retainedBytes > maxBytes_ - bytes_) {
            return false;
        }
        ++pending_;
        bytes_ += retainedBytes;
    }
    try {
        pool_.start(
            [this, task = std::move(task), retainedBytes] { runTask(std::move(task), retainedBytes); });
    } catch (...) {
        retire(retainedBytes);
        throw;
    }
    return true;
}

void QtIoExecutor::runTask(Task task, std::size_t retainedBytes) {
    bool execute;
    {
        std::lock_guard lock(mutex_);
        execute = !stopping_;
    }
    if (execute) {
        try {
            task();
        } catch (const std::exception& error) {
            qWarning() << "IO task failed:" << error.what();
        } catch (...) {
            qWarning() << "IO task failed with an unknown exception";
        }
    }
    retire(retainedBytes);
}

void QtIoExecutor::retire(std::size_t retainedBytes) {
    std::lock_guard lock(mutex_);
    --pending_;
    bytes_ -= retainedBytes;
    idle_.notify_all();
}

void QtIoExecutor::drain() {
    // QThreadPool::waitForDone() destroys worker threads; use it only at shutdown.
    std::unique_lock lock(mutex_);
    idle_.wait(lock, [this] { return pending_ == 0; });
}

void QtIoExecutor::stop() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    // Queued wrappers still run so their retained-byte accounting is released.
    pool_.waitForDone();
}

std::size_t QtIoExecutor::pendingTasks() const {
    std::lock_guard lock(mutex_);
    return pending_;
}
} // namespace orbelio
