#include <orbelio/core/scene_loader.hpp>
#include <orbelio/core/model_decoder.hpp>
#include <chrono>
#include <exception>
#include <stdexcept>
#include <utility>

namespace orbelio {
namespace {
double milliseconds(SceneLoader::Clock::time_point begin, SceneLoader::Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

class StageTimer {
public:
    StageTimer(std::optional<double>& target, const SceneLoader::Now& now)
        : target_(target), now_(now), started_(now_()) {}

    ~StageTimer() { target_ = target_.value_or(0.) + milliseconds(started_, now_()); }

private:
    std::optional<double>& target_;
    const SceneLoader::Now& now_;
    SceneLoader::Clock::time_point started_;
};

BytePool::Lease readAsset(IAssetReader& reader,
                          BytePool& pool,
                          std::string_view asset,
                          const ModelLimits& limits,
                          SceneLoadTimings& timings,
                          const SceneLoader::Now& now) {
    StageTimer timer(timings.readMs, now);
    return reader.read(asset, pool, limits.inputBytes);
}

void assignScene(SceneLoadResult& result, DecodedScene decoded) {
    if (auto* scene = std::get_if<StaticScene>(&decoded)) {
        result.scene = std::make_shared<StaticScene>(std::move(*scene));
    } else {
        if (result.sequence) {
            throw std::runtime_error("PLY Sequence requires static frames");
        }
        result.fdu4d = std::make_shared<Fdu4dScene>(std::move(std::get<Fdu4dScene>(decoded)));
    }
}

std::string firstFrame(SceneLoadResult& result,
                       std::span<const std::byte> bytes,
                       std::string_view asset,
                       const MemoryBudget& resident,
                       const SceneLoader::Now& now) {
    if (!isPlySequence(bytes, asset)) {
        return std::string(asset);
    }
    StageTimer timer(result.timings.decodeMs, now);
    result.sequence = std::make_shared<PlySequence>(decodePlySequence(bytes, asset, resident));
    return result.sequence->frames.front().asset;
}

SceneLoadResult readScene(IAssetReader& reader,
                          BytePool& pool,
                          std::string_view asset,
                          const ModelLimits& limits,
                          const MemoryBudget& resident,
                          const SceneLoader::Now& now) {
    const auto start = now();
    SceneLoadResult result{};
    try {
        auto bytes = readAsset(reader, pool, asset, limits, result.timings, now);
        if (bytes.bytes().size() > limits.inputBytes) {
            throw std::runtime_error("Asset reader exceeded the model byte budget");
        }
        const auto frameAsset = firstFrame(result, bytes.bytes(), asset, resident, now);
        if (result.sequence) {
            bytes = {};
            bytes = readAsset(reader, pool, frameAsset, limits, result.timings, now);
        }
        auto resolver = [&](std::string_view name) {
            return reader.readRelated(asset, name, pool, limits.inputBytes);
        };
        {
            // Associated SOG reads are part of decoding until that decoder exposes separate phases.
            StageTimer timer(result.timings.decodeMs, now);
            assignScene(result, decodeScene(bytes.bytes(), frameAsset, limits, resident, resolver));
        }
        if (result.sequence && result.scene->memory.bytes() > limits.sequenceCacheBytes) {
            throw std::runtime_error("Sequence frame exceeds the configured cache budget");
        }
    } catch (const std::exception& error) {
        result.scene.reset();
        result.sequence.reset();
        result.fdu4d.reset();
        result.error = error.what();
    }
    result.ioMilliseconds = milliseconds(start, now());
    return result;
}

void postResult(IExecutor& completions,
                SceneLoader::Completion completion,
                SceneLoadResult result,
                SceneLoader::Now now,
                SceneLoader::Clock::time_point acceptedAt) {
    const auto postedAt = now();
    const bool posted = completions.tryPost([completion = std::move(completion),
                                             result = std::move(result),
                                             now = std::move(now),
                                             acceptedAt,
                                             postedAt]() mutable {
        const auto completedAt = now();
        result.timings.completionQueueMs = milliseconds(postedAt, completedAt);
        result.timings.totalMs = milliseconds(acceptedAt, completedAt);
        completion(std::move(result));
    });
    if (!posted) {
        throw std::runtime_error("Scene completion executor rejected the result");
    }
}
}

SceneLoader::SceneLoader(IExecutor& worker,
                         IExecutor& completions,
                         IAssetReader& reader,
                         BytePool& pool,
                         ModelLimits limits,
                         Now now)
    : limits_(limits), resident_(limits.residentBytes), worker_(worker), completions_(completions),
      reader_(reader), pool_(pool), now_(std::move(now)) {}

bool SceneLoader::load(std::string asset, Completion completion) {
    if (!completion) {
        return false;
    }
    auto* reader = &reader_;
    auto* pool = &pool_;
    auto* completions = &completions_;
    const auto limits = limits_;
    const auto resident = resident_;
    const auto now = now_;
    const auto acceptedAt = now();
    // Admission estimates owned metadata; input leases and decoded results have separate budgets.
    const auto retainedMetadataBytes = sizeof(asset) + asset.capacity() + 1 + sizeof(completion) +
                                       sizeof(limits) + sizeof(resident) + 3 * sizeof(void*) +
                                       sizeof(IExecutor::Task) + sizeof(now) + sizeof(acceptedAt);
    // No loader capture: accepted work stays valid if this facade is destroyed first.
    return worker_.tryPost(
        [reader,
         pool,
         completions,
         limits,
         resident,
         now,
         acceptedAt,
         asset = std::move(asset),
         completion = std::move(completion)]() mutable {
            const auto startedAt = now();
            auto result = readScene(*reader, *pool, asset, limits, resident, now);
            result.timings.workerQueueMs = milliseconds(acceptedAt, startedAt);
            postResult(*completions, std::move(completion), std::move(result), now, acceptedAt);
        },
        retainedMetadataBytes);
}
}
