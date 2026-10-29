#pragma once
#include "asset_reader.hpp"
#include "static_scene.hpp"
#include "fdu4d_scene.hpp"
#include <variant>
#include <functional>
#include <string_view>

namespace orbelio {
using ModelAssetResolver = std::function<BytePool::Lease(std::string_view)>;
using DecodedScene = std::variant<StaticScene, Fdu4dScene>;
DecodedScene decodeScene(std::span<const std::byte> bytes,
                         std::string_view filename,
                         const ModelLimits& limits,
                         const MemoryBudget& residentBudget,
                         ModelAssetResolver resolver = {});
StaticScene decodeModel(std::span<const std::byte> bytes,
                        std::string_view filename,
                        const ModelLimits& limits = {});
StaticScene decodeModel(std::span<const std::byte> bytes,
                        std::string_view filename,
                        const ModelLimits& limits,
                        const MemoryBudget& residentBudget,
                        ModelAssetResolver resolver = {});
}
