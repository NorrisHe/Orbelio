#pragma once

#include <orbelio/core/asset_reader.hpp>

namespace orbelio {
class QtAssetReader final : public IAssetReader {
public:
    BytePool::Lease read(std::string_view asset, BytePool& pool, std::size_t limitBytes) override;
    BytePool::Lease readRelated(std::string_view asset,
                                std::string_view relative,
                                BytePool& pool,
                                std::size_t limitBytes) override;
};
}
