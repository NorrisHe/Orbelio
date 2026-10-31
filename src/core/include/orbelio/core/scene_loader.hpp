#pragma once

#include "asset_reader.hpp"
#include "executor.hpp"
#include "static_scene.hpp"
#include "ply_sequence.hpp"
#include "fdu4d_scene.hpp"
#include <functional>
#include <memory>
#include <string>
#include <chrono>
#include <optional>

namespace orbelio {
struct SceneLoadTimings {
    std::optional<double> workerQueueMs;
    std::optional<double> readMs;
    std::optional<double> decodeMs;
    std::optional<double> completionQueueMs;
    std::optional<double> totalMs;
};

struct SceneLoadResult {
    std::shared_ptr<const StaticScene> scene;
    std::string error;
    double ioMilliseconds = 0;
    SceneLoadTimings timings;
    std::shared_ptr<const PlySequence> sequence;
    std::shared_ptr<const Fdu4dScene> fdu4d;
};

class SceneLoader {
public:
    static constexpr std::size_t modelByteBudget = ModelLimits{}.inputBytes;
    using Completion = std::function<void(SceneLoadResult)>;
    using Clock = std::chrono::steady_clock;
    using Now = std::function<Clock::time_point()>;

    // Reader, pool and completion executor must outlive queued worker tasks.
    SceneLoader(IExecutor& worker,
                IExecutor& completions,
                IAssetReader& reader,
                BytePool& pool,
                ModelLimits limits = {},
                Now now = Clock::now);

    const ModelLimits& limits() const { return limits_; }

    std::size_t residentBytes() const { return resident_.usedBytes(); }

    bool load(std::string asset, Completion completion);

private:
    ModelLimits limits_;
    MemoryBudget resident_;
    IExecutor& worker_;
    IExecutor& completions_;
    IAssetReader& reader_;
    BytePool& pool_;
    Now now_;
};
}
