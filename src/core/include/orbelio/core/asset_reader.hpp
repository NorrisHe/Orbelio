#pragma once

#include "byte_pool.hpp"
#include <string_view>
#include <stdexcept>

namespace orbelio {
class IAssetStream {
public:
    static constexpr std::size_t blockBytes = 4 * 1024 * 1024;
    virtual ~IAssetStream() = default;
    virtual std::size_t size() const = 0;
    virtual void read(std::span<std::byte> destination) = 0;
};

class IAssetReader {
public:
    virtual ~IAssetReader() = default;

    // UTF-8 asset identifiers may be filesystem paths or platform resource names.
    virtual BytePool::Lease read(std::string_view asset, BytePool& pool, std::size_t limitBytes) = 0;

    virtual std::unique_ptr<IAssetStream> openStream(std::string_view) { return {}; }

    virtual BytePool::Lease readRelated(std::string_view, std::string_view, BytePool&, std::size_t) {
        throw std::runtime_error("Asset reader does not support related model assets");
    }
};
}
