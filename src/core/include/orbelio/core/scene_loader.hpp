#pragma once

#include "asset_reader.hpp"
#include "executor.hpp"
#include "static_scene.hpp"
#include <functional>
#include <memory>
#include <string>

namespace orbelio {
struct SceneLoadResult {
    std::shared_ptr<const StaticScene> scene;
    std::string error;
    double ioMilliseconds = 0;
};

class SceneLoader {
public:
    static constexpr std::size_t modelByteBudget = 8 * 1024 * 1024;
    using Completion = std::function<void(SceneLoadResult)>;

    // Reader, pool and completion executor must outlive queued worker tasks.
    SceneLoader(IExecutor& worker, IExecutor& completions, IAssetReader& reader, BytePool& pool);
    bool load(std::string asset, Completion completion);

private:
    IExecutor& worker_;
    IExecutor& completions_;
    IAssetReader& reader_;
    BytePool& pool_;
};
}
