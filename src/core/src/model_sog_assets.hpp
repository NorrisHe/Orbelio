#pragma once
#include "model_decode_internal.hpp"
#include <optional>

namespace orbelio::detail {
struct SogAsset {
    std::optional<WorkingBytes> owned;
    BytePool::Lease lease;
    std::span<const std::byte> bytes;
};

class SogAssets {
public:
    SogAssets(std::span<const std::byte> data, DecodeContext& context);
    SogAsset load(std::string_view name);

private:
    SogAsset fromArchive(std::string_view name);
    std::span<const std::byte> data_;
    DecodeContext& context_;
    bool bundled_;
};
}
