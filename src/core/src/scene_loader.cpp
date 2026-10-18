#include <orbelio/core/scene_loader.hpp>
#include <chrono>
#include <exception>
#include <stdexcept>
#include <utility>

namespace orbelio {
namespace {
SceneLoadResult readScene(IAssetReader& reader, BytePool& pool, std::string_view asset) {
    const auto start = std::chrono::steady_clock::now();
    SceneLoadResult result{};
    try {
        const auto bytes = reader.read(asset, pool, SceneLoader::modelByteBudget);
        if (bytes.bytes().size() > SceneLoader::modelByteBudget) {
            throw std::runtime_error("Asset reader exceeded the model byte budget");
        }
        result.scene = std::make_shared<StaticScene>(decodeCompressedPly(bytes.bytes()));
    } catch (const std::exception& error) {
        result.error = error.what();
    }
    result.ioMilliseconds =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return result;
}
}

SceneLoader::SceneLoader(IExecutor& worker, IExecutor& completions, IAssetReader& reader, BytePool& pool)
    : worker_(worker), completions_(completions), reader_(reader), pool_(pool) {}

bool SceneLoader::load(std::string asset, Completion completion) {
    if (!completion) {
        return false;
    }
    auto* reader = &reader_;
    auto* pool = &pool_;
    auto* completions = &completions_;
    // No loader capture: accepted work stays valid if this facade is destroyed first.
    return worker_.tryPost(
        [reader, pool, completions, asset = std::move(asset), completion = std::move(completion)]() mutable {
            auto result = readScene(*reader, *pool, asset);
            const bool posted = completions->tryPost(
                [completion = std::move(completion), result = std::move(result)]() mutable {
                    completion(std::move(result));
                });
            if (!posted) {
                throw std::runtime_error("Scene completion executor rejected the result");
            }
        },
        modelByteBudget);
}
}
