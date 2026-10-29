#include <orbelio/core/scene_loader.hpp>
#include <orbelio/core/model_decoder.hpp>
#include <chrono>
#include <exception>
#include <stdexcept>
#include <utility>

namespace orbelio {
namespace {
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

SceneLoadResult readScene(IAssetReader& reader,
                          BytePool& pool,
                          std::string_view asset,
                          const ModelLimits& limits,
                          const MemoryBudget& resident) {
    const auto start = std::chrono::steady_clock::now();
    SceneLoadResult result{};
    try {
        auto bytes = reader.read(asset, pool, limits.inputBytes);
        if (bytes.bytes().size() > limits.inputBytes) {
            throw std::runtime_error("Asset reader exceeded the model byte budget");
        }
        std::string frameAsset(asset);
        if (isPlySequence(bytes.bytes(), asset)) {
            result.sequence =
                std::make_shared<PlySequence>(decodePlySequence(bytes.bytes(), asset, resident));
            frameAsset = result.sequence->frames.front().asset;
            bytes = {};
            bytes = reader.read(frameAsset, pool, limits.inputBytes);
        }
        auto resolver = [&](std::string_view name) {
            return reader.readRelated(asset, name, pool, limits.inputBytes);
        };
        assignScene(result, decodeScene(bytes.bytes(), frameAsset, limits, resident, resolver));
        if (result.sequence && result.scene->memory.bytes() > limits.sequenceCacheBytes) {
            throw std::runtime_error("Sequence frame exceeds the configured cache budget");
        }
    } catch (const std::exception& error) {
        result.scene.reset();
        result.sequence.reset();
        result.fdu4d.reset();
        result.error = error.what();
    }
    result.ioMilliseconds =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return result;
}
}

SceneLoader::SceneLoader(IExecutor& worker,
                         IExecutor& completions,
                         IAssetReader& reader,
                         BytePool& pool,
                         ModelLimits limits)
    : limits_(limits), resident_(limits.residentBytes), worker_(worker), completions_(completions),
      reader_(reader), pool_(pool) {}

bool SceneLoader::load(std::string asset, Completion completion) {
    if (!completion) {
        return false;
    }
    auto* reader = &reader_;
    auto* pool = &pool_;
    auto* completions = &completions_;
    const auto limits = limits_;
    const auto resident = resident_;
    // Admission estimates owned metadata; input leases and decoded results have separate budgets.
    const auto retainedMetadataBytes = sizeof(asset) + asset.capacity() + 1 + sizeof(completion) +
                                       sizeof(limits) + sizeof(resident) + 3 * sizeof(void*) +
                                       sizeof(IExecutor::Task);
    // No loader capture: accepted work stays valid if this facade is destroyed first.
    return worker_.tryPost(
        [reader,
         pool,
         completions,
         limits,
         resident,
         asset = std::move(asset),
         completion = std::move(completion)]() mutable {
            auto result = readScene(*reader, *pool, asset, limits, resident);
            const bool posted = completions->tryPost(
                [completion = std::move(completion), result = std::move(result)]() mutable {
                    completion(std::move(result));
                });
            if (!posted) {
                throw std::runtime_error("Scene completion executor rejected the result");
            }
        },
        retainedMetadataBytes);
}
}
