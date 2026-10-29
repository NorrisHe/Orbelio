#pragma once
#include <orbelio/core/model_decoder.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

namespace orbelio::detail {
inline constexpr float shDc = .28209479177387814f;
inline constexpr float alphaEpsilon = 1e-6f;
inline constexpr std::size_t headerByteLimit = 64 * 1024;
inline constexpr std::size_t baseFloatCount = 14;
inline constexpr std::size_t framingFloatCount = 3;

inline void require(bool valid, const char* message) {
    if (!valid) {
        throw std::runtime_error(message);
    }
}

inline std::size_t checkedProduct(std::size_t count, std::size_t stride) {
    require(!stride || count <= std::numeric_limits<std::size_t>::max() / stride, "Model size overflow");
    return count * stride;
}

inline float logit(float alpha) {
    alpha = std::clamp(alpha, alphaEpsilon, 1.f - alphaEpsilon);
    return std::log(alpha / (1.f - alpha));
}

inline unsigned shDegreeForRest(std::size_t values) {
    for (unsigned degree = 0; degree <= 3; ++degree) {
        if (values == degree * (degree + 2) * 3) {
            return degree;
        }
    }
    throw std::runtime_error("Invalid SH layout: expected SH0, SH1, SH2 or SH3");
}

class Bytes {
public:
    explicit Bytes(std::span<const std::byte> data) : data_(data) {}

    std::size_t size() const { return data_.size(); }

    std::span<const std::byte> slice(std::size_t offset, std::size_t size) const {
        require(offset <= data_.size() && size <= data_.size() - offset, "Truncated model payload");
        return data_.subspan(offset, size);
    }

    template <class T> T read(std::size_t offset, bool littleEndian = true) const {
        const auto bytes = slice(offset, sizeof(T));
        std::array<std::byte, sizeof(T)> storage{};
        std::copy(bytes.begin(), bytes.end(), storage.begin());
        if (littleEndian != (std::endian::native == std::endian::little)) {
            std::reverse(storage.begin(), storage.end());
        }
        return std::bit_cast<T>(storage);
    }

    std::uint8_t u8(std::size_t offset) const { return read<std::uint8_t>(offset); }

    std::uint16_t u16(std::size_t offset) const { return read<std::uint16_t>(offset); }

    std::uint32_t u32(std::size_t offset) const { return read<std::uint32_t>(offset); }

    std::uint64_t u64(std::size_t offset) const { return read<std::uint64_t>(offset); }

    float f32(std::size_t offset) const { return read<float>(offset); }

    float f16(std::size_t offset) const;

private:
    std::span<const std::byte> data_;
};

struct DecodeContext {
    const ModelLimits& limits;
    const MemoryBudget& resident;
    ModelAssetResolver resolver;
    std::size_t outputBytes = 0;
    std::size_t temporaryBytes = 0;
    void checkWorking(std::size_t additional) const;
    std::size_t sceneBytes(std::size_t count, unsigned degree) const;
    StaticScene createScene(std::size_t count, unsigned degree, std::string format);
};

class WorkingReservation {
public:
    WorkingReservation(DecodeContext& context, std::size_t bytes);
    ~WorkingReservation();

    WorkingReservation(WorkingReservation&& other) noexcept
        : context_(std::exchange(other.context_, nullptr)), memory_(std::move(other.memory_)) {}

    WorkingReservation(const WorkingReservation&) = delete;
    WorkingReservation& operator=(const WorkingReservation&) = delete;

private:
    DecodeContext* context_ = nullptr;
    MemoryBudget::Reservation memory_;
};

struct WorkingBytes {
    WorkingReservation memory;
    std::vector<std::byte> data;

    WorkingBytes(DecodeContext& context, std::size_t size) : memory(context, size), data(size) {}

    WorkingBytes(WorkingBytes&&) noexcept = default;
};

void finishDecodedScene(StaticScene& scene);
void validateDecodedScene(StaticScene& scene);
void normalizeRotation(StaticScene& scene, std::size_t index);
StaticScene readPly(std::span<const std::byte> data, DecodeContext& context);
Fdu4dScene readFdu4d(std::span<const std::byte> data, DecodeContext& context);
StaticScene readSplat(std::span<const std::byte> data, DecodeContext& context);
StaticScene readKsplat(std::span<const std::byte> data, DecodeContext& context);
StaticScene readSpz(std::span<const std::byte> data, DecodeContext& context);
StaticScene readSog(std::span<const std::byte> data, DecodeContext& context);
}
