#pragma once
#include "asset_reader.hpp"
#include "static_scene.hpp"
#include <functional>
#include <string_view>

namespace orbelio {
using ModelAssetResolver = std::function<BytePool::Lease(std::string_view)>;
StaticScene decodeModel(std::span<const std::byte> bytes,
                        std::string_view filename,
                        const ModelLimits& limits = {});
StaticScene decodeModel(std::span<const std::byte> bytes,
                        std::string_view filename,
                        const ModelLimits& limits,
                        const MemoryBudget& residentBudget,
                        ModelAssetResolver resolver = {});
}
