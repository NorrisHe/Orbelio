#include "model_decoder_factory.hpp"
#include <cctype>

namespace orbelio {
namespace {
using Reader = StaticScene (*)(std::span<const std::byte>, detail::DecodeContext&);

class FormatDecoder final : public detail::IModelDecoder {
public:
    explicit FormatDecoder(Reader reader) : reader_(reader) {}

    DecodedScene decode(std::span<const std::byte> bytes, detail::DecodeContext& context) const override {
        auto scene = reader_(bytes, context);
        detail::finishDecodedScene(scene);
        return scene;
    }

private:
    Reader reader_;
};

class FduDecoder final : public detail::IModelDecoder {
public:
    DecodedScene decode(std::span<const std::byte> bytes, detail::DecodeContext& context) const override {
        return detail::readFdu4d(bytes, context);
    }
};

std::string extensionOf(std::string_view filename) {
    const auto dot = filename.find_last_of('.');
    std::string extension(dot == std::string_view::npos ? "" : filename.substr(dot));
    for (auto& character : extension) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return extension;
}
}

std::unique_ptr<detail::IModelDecoder> detail::ModelDecoderFactory::create(std::span<const std::byte> bytes,
                                                                           std::string_view filename) {
    const auto extension = extensionOf(filename);
    const std::string_view prefix(reinterpret_cast<const char*>(bytes.data()), std::min(bytes.size(), 4uz));
    if (prefix == "FDU4" || extension == ".fdu4d") {
        return std::make_unique<FduDecoder>();
    }
    Reader reader = nullptr;
    if (prefix.starts_with("ply") || extension == ".ply") {
        reader = readPly;
    } else if (extension == ".sog" || extension == ".json") {
        reader = readSog;
    } else if (extension == ".spz" || prefix == "NGSP") {
        reader = readSpz;
    } else if (extension == ".ksplat") {
        reader = readKsplat;
    } else if (extension == ".splat") {
        reader = readSplat;
    }
    require(reader != nullptr, "Unsupported model format (PLY, SOG, SPZ, SPLAT, KSPLAT, FDU4D)");
    return std::make_unique<FormatDecoder>(reader);
}

StaticScene decodeModel(std::span<const std::byte> bytes,
                        std::string_view filename,
                        const ModelLimits& limits) {
    return decodeModel(bytes, filename, limits, MemoryBudget(limits.residentBytes));
}

StaticScene decodeModel(std::span<const std::byte> bytes,
                        std::string_view filename,
                        const ModelLimits& limits,
                        const MemoryBudget& resident,
                        ModelAssetResolver resolver) {
    auto decoded = decodeScene(bytes, filename, limits, resident, std::move(resolver));
    detail::require(std::holds_alternative<StaticScene>(decoded), "Dynamic model requires decodeScene");
    return std::move(std::get<StaticScene>(decoded));
}

DecodedScene decodeScene(std::span<const std::byte> bytes,
                         std::string_view filename,
                         const ModelLimits& limits,
                         const MemoryBudget& resident,
                         ModelAssetResolver resolver) {
    detail::require(!bytes.empty() && bytes.size() <= limits.inputBytes, "Input exceeds model byte budget");
    detail::DecodeContext context{limits, resident, std::move(resolver)};
    return detail::ModelDecoderFactory::create(bytes, filename)->decode(bytes, context);
}
}
