#pragma once

#include "byte_pool.hpp"
#include <string_view>
#include <stdexcept>

namespace orbelio {
class IAssetReader {
public:
    virtual ~IAssetReader() = default;

    // UTF-8 asset identifiers may be filesystem paths or platform resource names.
    virtual BytePool::Lease read(std::string_view asset, BytePool& pool, std::size_t limitBytes) = 0;

    virtual BytePool::Lease readRelated(std::string_view, std::string_view, BytePool&, std::size_t) {
        throw std::runtime_error("Asset reader does not support related model assets");
    }
};
}
