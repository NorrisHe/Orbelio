#include <orbelio/core/ply_sequence.hpp>
#include <json/json.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <stdexcept>

namespace orbelio {
namespace {
constexpr std::size_t manifestByteLimit = 4 * ModelLimits::mebibyte;
constexpr Json::ArrayIndex maximumFrames = 100000;

Json::Value manifestJson(std::span<const std::byte> bytes) {
    if (bytes.empty() || bytes.size() > manifestByteLimit) {
        throw std::runtime_error("Sequence manifest exceeds the 4 MiB limit");
    }
    Json::CharReaderBuilder builder;
    builder["rejectDupKeys"] = true;
    builder["failIfExtra"] = true;
    builder["stackLimit"] = 32;
    Json::Value root;
    std::string error;
    const auto* begin = reinterpret_cast<const char*>(bytes.data());
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    if (!reader->parse(begin, begin + bytes.size(), &root, &error)) {
        throw std::runtime_error("Invalid sequence JSON: " + error);
    }
    return root;
}

double finiteTime(const Json::Value& value) {
    if (!value.isNumeric() || !std::isfinite(value.asDouble()) || value.asDouble() < 0) {
        throw std::runtime_error("Sequence timestamps must be finite non-negative seconds");
    }
    return value.asDouble();
}

std::filesystem::path utf8Path(std::string_view value) {
    return std::filesystem::path(
        std::u8string_view(reinterpret_cast<const char8_t*>(value.data()), value.size()));
}

std::string frameAsset(std::string_view asset, const Json::Value& value) {
    if (!value.isString()) {
        throw std::runtime_error("Sequence frame path must be a relative PLY file");
    }
    const auto name = value.asString();
    const auto relative = utf8Path(name);
    if (name.empty() || name.find_first_of(":\\") != std::string::npos ||
        name.find('\0') != std::string::npos || relative.has_root_path() || relative.extension() != ".ply") {
        throw std::runtime_error("Sequence frame path must be a relative .ply file");
    }
    for (const auto& component : relative) {
        if (component == "..") {
            throw std::runtime_error("Sequence frame path escapes its directory");
        }
    }
    const auto path = (utf8Path(asset).parent_path() / relative).lexically_normal();
    const auto utf8 = path.generic_u8string();
    return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}
}

bool isPlySequence(std::span<const std::byte> bytes, std::string_view asset) {
    if (asset.ends_with(".gsseq")) {
        return true;
    }
    if (!asset.ends_with(".json") || bytes.size() > manifestByteLimit) {
        return false;
    }
    const auto root = manifestJson(bytes);
    return root["schema"].isString() && root["schema"].asString() == "orbelio.ply-sequence";
}

PlySequence decodePlySequence(std::span<const std::byte> bytes,
                              std::string_view asset,
                              const MemoryBudget& resident) {
    const auto root = manifestJson(bytes);
    if (!root.isObject() || root["schema"] != "orbelio.ply-sequence" || !root["version"].isInt() ||
        root["version"].asInt() != 1) {
        throw std::runtime_error("Unsupported PLY sequence schema or version");
    }
    const auto& entries = root["frames"];
    if (!entries.isArray() || entries.empty() || entries.size() > maximumFrames) {
        throw std::runtime_error("Sequence requires 1 to 100000 frames");
    }
    PlySequence result{};
    result.memory =
        resident.reserve(bytes.size() + entries.size() * (sizeof(SequenceFrame) + asset.size() + 1));
    result.endTimeSeconds = finiteTime(root["endTimeSeconds"]);
    result.frames.reserve(entries.size());
    double previous = -1;
    for (const auto& entry : entries) {
        const auto timestamp = finiteTime(entry["timestampSeconds"]);
        if (timestamp <= previous) {
            throw std::runtime_error("Sequence timestamps must be strictly increasing");
        }
        result.frames.push_back({timestamp, frameAsset(asset, entry["path"])});
        previous = timestamp;
    }
    if (result.endTimeSeconds <= previous) {
        throw std::runtime_error("Sequence end time must be after its last frame");
    }
    return result;
}

double PlySequence::clampTime(double time) const {
    if (!std::isfinite(time) || frames.empty()) {
        throw std::invalid_argument("Sequence time must be finite and the sequence non-empty");
    }
    return std::clamp(time, frames.front().timestampSeconds, endTimeSeconds);
}

std::size_t PlySequence::frameAt(double time) const {
    time = clampTime(time);
    const auto next =
        std::upper_bound(frames.begin(), frames.end(), time, [](double value, const auto& frame) {
            return value < frame.timestampSeconds;
        });
    return next == frames.begin() ? 0 : std::size_t(next - frames.begin() - 1);
}

double PlySequence::advance(double time, double seconds, bool loop) const {
    if (!std::isfinite(seconds) || seconds < 0) {
        throw std::invalid_argument("Playback delta must be finite and non-negative");
    }
    time = clampTime(time);
    const auto start = frames.front().timestampSeconds;
    if (!loop) {
        return seconds >= endTimeSeconds - time ? endTimeSeconds : time + seconds;
    }
    const auto duration = endTimeSeconds - start;
    const auto delta = std::fmod(seconds, duration);
    const auto remaining = endTimeSeconds - time;
    return delta >= remaining ? start + (delta - remaining) : time + delta;
}
}
