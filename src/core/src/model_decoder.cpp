#include "model_decode_internal.hpp"
#include <cctype>

namespace orbelio {
namespace {
using Reader = StaticScene (*)(std::span<const std::byte>, detail::DecodeContext&);
}

std::string detail::extensionOf(std::string_view filename) {
    const auto dot = filename.find_last_of('.');
    std::string extension(dot == std::string_view::npos ? "" : filename.substr(dot));
    for (auto& character : extension) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return extension;
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
    const auto extension = detail::extensionOf(filename);
    const std::string_view prefix(reinterpret_cast<const char*>(bytes.data()), std::min(bytes.size(), 4uz));
    if (prefix == "FDU4" || extension == ".fdu4d") {
        return detail::readFdu4d(bytes, context);
    }
    Reader reader = nullptr;
    if (prefix.starts_with("ply") || extension == ".ply") {
        reader = detail::readPly;
    } else if (extension == ".sog" || extension == ".json") {
        reader = detail::readSog;
    } else if (extension == ".spz" || prefix == "NGSP") {
        reader = detail::readSpz;
    } else if (extension == ".ksplat") {
        reader = detail::readKsplat;
    } else if (extension == ".splat") {
        reader = detail::readSplat;
    }
    detail::require(reader != nullptr, "Unsupported model format (PLY, SOG, SPZ, SPLAT, KSPLAT, FDU4D)");
    auto scene = reader(bytes, context);
    detail::finishDecodedScene(scene);
    return scene;
}
}
