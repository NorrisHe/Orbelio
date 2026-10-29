#pragma once

#include <cstddef>
#include <functional>

namespace orbelio {
class IExecutor {
public:
    using Task = std::function<void()>;
    virtual ~IExecutor() = default;

    // False means closed, over budget, or invalid; caller must handle backpressure.
    // retainedBytes estimates already-owned payload, excluding allocations made when the task runs.
    virtual bool tryPost(Task task, std::size_t retainedBytes = 0) = 0;
};
}
