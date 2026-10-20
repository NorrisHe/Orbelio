#pragma once
#include "model_decode_internal.hpp"
#include <memory>

namespace orbelio::detail {
class IModelDecoder {
public:
    virtual ~IModelDecoder() = default;
    virtual StaticScene decode(std::span<const std::byte> bytes, DecodeContext& context) const = 0;
};

class ModelDecoderFactory {
public:
    static std::unique_ptr<IModelDecoder> create(std::span<const std::byte> bytes, std::string_view filename);
};
}
