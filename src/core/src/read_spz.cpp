#include "model_decode_internal.hpp"
#include <zlib.h>
#include <zstd.h>

namespace orbelio::detail {
namespace {
constexpr std::uint32_t magic = 0x5053474e;
constexpr std::size_t headerBytes = 16;
constexpr std::size_t v4HeaderBytes = 32;
constexpr std::size_t tocEntryBytes = 16;
constexpr std::size_t codecWorkspaceBytes = 1024 * 1024;
constexpr unsigned gzipWindowBits = 31;
constexpr float byteMaximum = 255.f;
constexpr float colorScale = .15f;
constexpr float scaleQuantization = 16.f;
constexpr float scaleBias = 10.f;
constexpr unsigned rotationMask = 1023;
constexpr unsigned rotationSign = 512;
constexpr unsigned rotationMagnitude = 511;
constexpr unsigned rotationSelectorShift = 30;
constexpr unsigned fixedSign = 0x800000;
constexpr unsigned fixedSignExtension = 0xff000000;

namespace field {
constexpr std::size_t magic = 0;
constexpr std::size_t version = 4;
constexpr std::size_t count = 8;
constexpr std::size_t degree = 12;
constexpr std::size_t fraction = 13;
constexpr std::size_t flags = 14;
constexpr std::size_t streams = 15;
constexpr std::size_t toc = 16;
}

constexpr unsigned rotationComponentBits = 10;
constexpr std::size_t tocPlainSizeOffset = 8;
constexpr std::uint8_t gzipFirstByte = 0x1f;
constexpr std::uint8_t gzipSecondByte = 0x8b;

struct Header {
    unsigned version = 0;
    std::size_t count = 0;
    unsigned degree = 0;
    unsigned fractionalBits = 0;
    bool antialiased = false;
    std::array<std::size_t, 6> sizes{};

    std::size_t payloadBytes() const {
        std::size_t total = 0;
        for (const auto size : sizes) {
            total += size;
        }
        return total;
    }
};

Header readHeader(const Bytes& bytes, const ModelLimits& limits) {
    bytes.slice(0, headerBytes);
    require(bytes.u32(field::magic) == magic, "Invalid SPZ magic");
    Header header{};
    header.version = bytes.u32(field::version);
    header.count = bytes.u32(field::count);
    header.degree = bytes.u8(field::degree);
    header.fractionalBits = bytes.u8(field::fraction);
    header.antialiased = (bytes.u8(field::flags) & 1) != 0;
    require(header.version >= 2 && header.version <= 4 && header.degree <= 3 && header.count &&
                header.count <= limits.maxGaussians,
            "Unsupported SPZ version/SH degree or invalid count/fixed-point precision");
    const auto count = header.count;
    header.sizes = {checkedProduct(count, 9),
                    count,
                    checkedProduct(count, 3),
                    checkedProduct(count, 3),
                    checkedProduct(count, header.version == 2 ? 3 : 4),
                    checkedProduct(count, header.degree * (header.degree + 2) * 3)};
    return header;
}

class Inflater {
public:
    explicit Inflater(std::span<const std::byte> data) {
        require(data.size() <= std::numeric_limits<uInt>::max(), "SPZ gzip input too large");
        stream_.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(data.data()));
        stream_.avail_in = static_cast<uInt>(data.size());
        require(inflateInit2(&stream_, gzipWindowBits) == Z_OK, "Cannot initialize SPZ gzip decoder");
    }

    ~Inflater() { inflateEnd(&stream_); }

    Inflater(const Inflater&) = delete;
    Inflater& operator=(const Inflater&) = delete;

    void read(std::span<std::byte> output) {
        require(output.size() <= std::numeric_limits<uInt>::max(), "SPZ gzip output too large");
        stream_.next_out = reinterpret_cast<Bytef*>(output.data());
        stream_.avail_out = static_cast<uInt>(output.size());
        while (stream_.avail_out) {
            const auto before = stream_.avail_in;
            const auto result = inflate(&stream_, Z_NO_FLUSH);
            require(result == Z_OK || (result == Z_STREAM_END && !stream_.avail_out),
                    "Corrupt/truncated SPZ gzip stream");
            require(before != stream_.avail_in || !stream_.avail_out, "Truncated SPZ gzip stream");
        }
    }

    void finish() {
        std::byte extra{};
        stream_.next_out = reinterpret_cast<Bytef*>(&extra);
        stream_.avail_out = 1;
        const auto result = inflate(&stream_, Z_FINISH);
        require(result == Z_STREAM_END && stream_.avail_out == 1 && stream_.avail_in == 0,
                "Corrupt/trailing SPZ gzip payload");
    }

private:
    z_stream stream_{};
};

void decodeRotation(const Bytes& bytes,
                    std::size_t at,
                    const Header& header,
                    StaticScene& scene,
                    std::size_t i) {
    std::array<float, 4> xyzw{};
    float squared = 0;
    if (header.version == 2) {
        for (std::size_t axis = 0; axis < 3; ++axis) {
            xyzw[axis] = bytes.u8(at + i * 3 + axis) / (byteMaximum * .5f) - 1.f;
            squared += xyzw[axis] * xyzw[axis];
        }
        xyzw[3] = std::sqrt(std::max(0.f, 1.f - squared));
    } else {
        auto packed = bytes.u32(at + i * 4);
        const auto largest = packed >> rotationSelectorShift;
        for (int axis = 3; axis >= 0; --axis) {
            if (static_cast<unsigned>(axis) == largest) {
                continue;
            }
            const auto bits = packed & rotationMask;
            xyzw[axis] = float(bits & rotationMagnitude) / rotationMagnitude * std::sqrt(.5f) *
                         (bits & rotationSign ? -1.f : 1.f);
            squared += xyzw[axis] * xyzw[axis];
            packed >>= rotationComponentBits;
        }
        xyzw[largest] = std::sqrt(std::max(0.f, 1.f - squared));
    }
    scene.rotations[i * 4] = xyzw[3];
    for (std::size_t axis = 0; axis < 3; ++axis) {
        scene.rotations[i * 4 + axis + 1] = xyzw[axis];
    }
}

StaticScene decodePayload(std::span<const std::byte> data, const Header& header, DecodeContext& context) {
    const Bytes bytes(data);
    require(data.size() == header.payloadBytes(), "Truncated/trailing SPZ attribute payload");
    auto scene = context.createScene(header.count, header.degree, "SPZ v" + std::to_string(header.version));
    scene.antialiased = header.antialiased;
    std::array<std::size_t, 6> offsets{};
    for (std::size_t j = 1; j < offsets.size(); ++j) {
        offsets[j] = offsets[j - 1] + header.sizes[j - 1];
    }
    const auto fixedScale = std::ldexp(1.f, -static_cast<int>(header.fractionalBits));
    const auto rest = scene.shRestCoefficients() * 3;
    for (std::size_t i = 0; i < header.count; ++i) {
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const auto at = i * 9 + axis * 3;
            auto fixed = std::uint32_t(bytes.u8(at)) | (std::uint32_t(bytes.u8(at + 1)) << 8) |
                         (std::uint32_t(bytes.u8(at + 2)) << 16);
            if (fixed & fixedSign) {
                fixed |= fixedSignExtension;
            }
            scene.positions[i * 3 + axis] = static_cast<std::int32_t>(fixed) * fixedScale;
            scene.sh0[i * 3 + axis] = (bytes.u8(offsets[2] + i * 3 + axis) / byteMaximum - .5f) / colorScale;
            scene.logScales[i * 3 + axis] =
                bytes.u8(offsets[3] + i * 3 + axis) / scaleQuantization - scaleBias;
        }
        scene.opacityLogits[i] = logit(bytes.u8(offsets[1] + i) / byteMaximum);
        decodeRotation(bytes, offsets[4], header, scene, i);
        for (std::size_t j = 0; j < rest; ++j) {
            scene.shRest[i * rest + j] = (float(bytes.u8(offsets[5] + i * rest + j)) - 128.f) / 128.f;
        }
    }
    return scene;
}

StaticScene readGzip(std::span<const std::byte> data, DecodeContext& context) {
    WorkingReservation codecMemory(context, codecWorkspaceBytes);
    Inflater inflater(data);
    std::array<std::byte, headerBytes> prefix{};
    inflater.read(prefix);
    const auto header = readHeader(Bytes(prefix), context.limits);
    require(header.version < 4, "SPZ v4 must use its plaintext header/Zstd container");
    context.checkWorking(context.sceneBytes(header.count, header.degree) + header.payloadBytes());
    WorkingBytes payload(context, header.payloadBytes());
    inflater.read(payload.data);
    inflater.finish();
    return decodePayload(payload.data, header, context);
}

StaticScene readV4(std::span<const std::byte> data, const Header& header, DecodeContext& context) {
    const Bytes bytes(data);
    bytes.slice(0, v4HeaderBytes);
    const auto streams = header.degree ? 6uz : 5uz;
    const auto toc = bytes.u32(field::toc);
    require(bytes.u8(field::streams) == streams && toc >= v4HeaderBytes, "Invalid SPZ v4 stream table");
    bytes.slice(toc, streams * tocEntryBytes);
    auto at = toc + streams * tocEntryBytes;
    context.checkWorking(context.sceneBytes(header.count, header.degree) + header.payloadBytes());
    WorkingBytes payload(context, header.payloadBytes());
    WorkingReservation codecMemory(context, codecWorkspaceBytes);
    std::size_t outputAt = 0;
    for (std::size_t i = 0; i < streams; ++i) {
        const auto compressed = bytes.u64(toc + i * tocEntryBytes);
        require(bytes.u64(toc + i * tocEntryBytes + tocPlainSizeOffset) == header.sizes[i],
                "SPZ v4 attribute size mismatch");
        const auto source = bytes.slice(at, compressed);
        require(ZSTD_findFrameCompressedSize(source.data(), source.size()) == source.size(),
                "Invalid SPZ Zstd frame boundary");
        const auto size =
            ZSTD_decompress(payload.data.data() + outputAt, header.sizes[i], source.data(), source.size());
        require(!ZSTD_isError(size) && size == header.sizes[i], "Corrupt SPZ Zstd attribute");
        outputAt += size;
        at += compressed;
    }
    require(at == data.size(), "Trailing SPZ v4 payload");
    return decodePayload(payload.data, header, context);
}
}

StaticScene readSpz(std::span<const std::byte> data, DecodeContext& context) {
    const Bytes bytes(data);
    if (bytes.u8(0) == gzipFirstByte && bytes.u8(1) == gzipSecondByte) {
        return readGzip(data, context);
    }
    const auto header = readHeader(bytes, context.limits);
    return header.version == 4 ? readV4(data, header, context)
                               : decodePayload(data.subspan(headerBytes), header, context);
}
}
