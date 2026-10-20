#include "model_sog_assets.hpp"
#include <json/json.h>
#include <webp/decode.h>
#include <optional>

namespace orbelio::detail {
namespace {
constexpr std::size_t webpWorkingFactor = 16;
constexpr unsigned jsonStackLimit = 32;
constexpr std::size_t codecWorkspaceBytes = 1024 * 1024;
constexpr std::size_t paletteColumns = 64;
constexpr std::size_t codebookSize = 256;
constexpr float byteMaximum = 255.f;
constexpr float wordMaximum = 65535.f;
constexpr unsigned quaternionTagBase = 252;

double number(const Json::Value& value) {
    require(value.isNumeric() && std::isfinite(value.asDouble()), "Invalid SOG numeric metadata");
    return value.asDouble();
}

std::size_t integer(const Json::Value& value) {
    require(value.isUInt64() && value.asUInt64() <= std::numeric_limits<std::size_t>::max(),
            "Invalid SOG integer metadata");
    return static_cast<std::size_t>(value.asUInt64());
}

void numericArray(const Json::Value& value, std::size_t size) {
    require(value.isArray() && value.size() == size, "Invalid SOG metadata array length");
    for (const auto& item : value) {
        number(item);
    }
}

Json::Value readMeta(SogAssets& assets) {
    const auto asset = assets.load("meta.json");
    require(asset.bytes.size() <= headerByteLimit, "Oversized SOG metadata");
    Json::CharReaderBuilder builder;
    builder["rejectDupKeys"] = true;
    builder["failIfExtra"] = true;
    builder["stackLimit"] = jsonStackLimit;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    Json::Value meta;
    std::string error;
    const auto* begin = reinterpret_cast<const char*>(asset.bytes.data());
    require(reader->parse(begin, begin + asset.bytes.size(), &meta, &error) && meta.isObject(),
            "Invalid SOG JSON metadata");
    const auto version = meta.get("version", 1);
    require(version.isUInt() && (version.asUInt() == 1 || version.asUInt() == 2), "Unsupported SOG version");
    require(meta.get("model", "default").asString() != "2dgs", "2D Gaussian scenes are not supported");
    for (const auto* attribute : {"means", "scales", "quats", "sh0"}) {
        require(meta[attribute].isObject(), "Missing SOG attribute");
    }
    numericArray(meta["means"]["mins"], 3);
    numericArray(meta["means"]["maxs"], 3);
    return meta;
}

struct Texture {
    WorkingBytes pixels;
    std::size_t width;
    std::size_t height;

    std::uint8_t at(std::size_t pixel, std::size_t channel) const {
        require(pixel < width * height, "SOG texture index outside dimensions");
        return std::to_integer<std::uint8_t>(pixels.data[pixel * 4 + channel]);
    }
};

Texture readTexture(SogAssets& assets,
                    DecodeContext& context,
                    const Json::Value& attribute,
                    unsigned index,
                    std::size_t minimumPixels) {
    const auto& files = attribute["files"];
    require(files.isArray() && index < files.size() && files[index].isString(), "Missing SOG texture file");
    const auto input = assets.load(files[index].asString());
    int width = 0;
    int height = 0;
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(input.bytes.data());
    require(WebPGetInfo(bytes, input.bytes.size(), &width, &height) && width > 0 && height > 0,
            "Invalid SOG WebP texture");
    const auto pixels = checkedProduct(static_cast<std::size_t>(width), static_cast<std::size_t>(height));
    require(pixels >= minimumPixels, "SOG texture too small for count");
    const auto size = checkedProduct(pixels, 4);
    // WebP's entropy/transform storage scales with image dimensions even with an external output buffer.
    WorkingReservation codecMemory(context, checkedProduct(size, webpWorkingFactor) + codecWorkspaceBytes);
    Texture texture{
        WorkingBytes(context, size), static_cast<std::size_t>(width), static_cast<std::size_t>(height)};
    require(WebPDecodeRGBAInto(bytes,
                               input.bytes.size(),
                               reinterpret_cast<std::uint8_t*>(texture.pixels.data.data()),
                               size,
                               width * 4) != nullptr,
            "Corrupt SOG WebP texture");
    return texture;
}

float lerpMetadata(const Json::Value& attribute, unsigned axis, float fraction, bool means = false) {
    const auto low = number(attribute["mins"][axis]);
    auto range = number(attribute["maxs"][axis]) - low;
    if (means && range == 0) {
        range = 1;
    }
    return static_cast<float>(low + range * fraction);
}

void decodeMeans(SogAssets& assets, DecodeContext& context, const Json::Value& meta, StaticScene& scene) {
    const auto low = readTexture(assets, context, meta["means"], 0, scene.count());
    const auto high = readTexture(assets, context, meta["means"], 1, scene.count());
    for (std::size_t i = 0; i < scene.count(); ++i) {
        for (unsigned axis = 0; axis < 3; ++axis) {
            const auto quantized = low.at(i, axis) | (unsigned(high.at(i, axis)) << 8);
            const auto value = lerpMetadata(meta["means"], axis, quantized / wordMaximum, true);
            scene.positions[i * 3 + axis] = std::copysign(std::expm1(std::abs(value)), value);
        }
    }
}

void decodeQuaternions(SogAssets& assets,
                       DecodeContext& context,
                       const Json::Value& meta,
                       StaticScene& scene) {
    const auto texture = readTexture(assets, context, meta["quats"], 0, scene.count());
    for (std::size_t i = 0; i < scene.count(); ++i) {
        const auto tag = texture.at(i, 3);
        if (tag < quaternionTagBase) {
            scene.rotations[i * 4] = 1.f;
            continue;
        }
        const auto largest = tag - quaternionTagBase;
        unsigned source = 0;
        float squared = 0;
        for (unsigned axis = 0; axis < 4; ++axis) {
            if (axis == largest) {
                continue;
            }
            const auto value = (texture.at(i, source++) / byteMaximum * 2.f - 1.f) / std::sqrt(2.f);
            scene.rotations[i * 4 + axis] = value;
            squared += value * value;
        }
        scene.rotations[i * 4 + largest] = std::sqrt(std::max(0.f, 1.f - squared));
    }
}

void decodeScalesAndColor(SogAssets& assets,
                          DecodeContext& context,
                          const Json::Value& meta,
                          StaticScene& scene) {
    const auto v2 = meta.get("version", 1).asUInt() == 2;
    numericArray(meta["scales"][v2 ? "codebook" : "mins"], v2 ? codebookSize : 3);
    numericArray(meta["sh0"][v2 ? "codebook" : "mins"], v2 ? codebookSize : 4);
    if (!v2) {
        numericArray(meta["scales"]["maxs"], 3);
        numericArray(meta["sh0"]["maxs"], 4);
    }
    {
        const auto scales = readTexture(assets, context, meta["scales"], 0, scene.count());
        for (std::size_t i = 0; i < scene.count(); ++i) {
            for (unsigned axis = 0; axis < 3; ++axis) {
                const auto code = scales.at(i, axis);
                scene.logScales[i * 3 + axis] =
                    v2 ? static_cast<float>(number(meta["scales"]["codebook"][code]))
                       : lerpMetadata(meta["scales"], axis, code / byteMaximum);
            }
        }
    }
    const auto color = readTexture(assets, context, meta["sh0"], 0, scene.count());
    for (std::size_t i = 0; i < scene.count(); ++i) {
        for (unsigned axis = 0; axis < 3; ++axis) {
            const auto code = color.at(i, axis);
            scene.sh0[i * 3 + axis] = v2 ? static_cast<float>(number(meta["sh0"]["codebook"][code]))
                                         : lerpMetadata(meta["sh0"], axis, code / byteMaximum);
        }
        scene.opacityLogits[i] = v2 ? logit(color.at(i, 3) / byteMaximum)
                                    : lerpMetadata(meta["sh0"], 3, color.at(i, 3) / byteMaximum);
    }
}

void decodeSh(SogAssets& assets,
              DecodeContext& context,
              const Json::Value& attribute,
              const Texture& centroids,
              bool v2,
              StaticScene& scene) {
    const auto coefficients = scene.shRestCoefficients();
    require(centroids.width == paletteColumns * coefficients, "Invalid SOG SH centroid width");
    const auto paletteCount = v2 ? integer(attribute["count"]) : paletteColumns * centroids.height;
    require(paletteCount && paletteCount <= paletteColumns * centroids.height, "Invalid SOG palette size");
    if (v2) {
        numericArray(attribute["codebook"], codebookSize);
    } else {
        number(attribute["mins"]);
        number(attribute["maxs"]);
    }
    const auto labels = readTexture(assets, context, attribute, 1, scene.count());
    for (std::size_t i = 0; i < scene.count(); ++i) {
        const auto label = labels.at(i, 0) | (unsigned(labels.at(i, 1)) << 8);
        require(label < paletteCount, "SOG SH label outside palette");
        const auto first =
            (label / paletteColumns) * centroids.width + (label % paletteColumns) * coefficients;
        for (std::size_t coefficient = 0; coefficient < coefficients; ++coefficient) {
            for (unsigned channel = 0; channel < 3; ++channel) {
                const auto code = centroids.at(first + coefficient, channel);
                scene.shRest[(i * coefficients + coefficient) * 3 + channel] =
                    v2 ? static_cast<float>(number(attribute["codebook"][code]))
                       : static_cast<float>(std::lerp(number(attribute["mins"]),
                                                      number(attribute["maxs"]),
                                                      code / double(byteMaximum)));
            }
        }
    }
}
}

StaticScene readSog(std::span<const std::byte> data, DecodeContext& context) {
    WorkingReservation metadataMemory(context, codecWorkspaceBytes);
    SogAssets assets(data, context);
    const auto meta = readMeta(assets);
    const auto v2 = meta.get("version", 1).asUInt() == 2;
    const auto count = integer(v2 ? meta["count"] : meta["means"]["shape"][0]);
    std::optional<Texture> centroids;
    unsigned degree = 0;
    if (!meta["shN"].isNull() && (!v2 || integer(meta["shN"]["bands"]) > 0)) {
        centroids.emplace(readTexture(assets, context, meta["shN"], 0, 1));
        if (v2) {
            degree = static_cast<unsigned>(integer(meta["shN"]["bands"]));
        } else {
            degree = shDegreeForRest(centroids->width / paletteColumns * 3);
        }
        require(degree && degree <= 3, "Invalid SOG SH degree");
    }
    auto scene = context.createScene(count, degree, v2 ? "SOG v2" : "SOG v1");
    scene.antialiased = meta.get("model", "default").asString() == "antialiased";
    decodeMeans(assets, context, meta, scene);
    decodeQuaternions(assets, context, meta, scene);
    decodeScalesAndColor(assets, context, meta, scene);
    if (centroids) {
        decodeSh(assets, context, meta["shN"], *centroids, v2, scene);
    }
    return scene;
}
}
