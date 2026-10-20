#include "model_sog_assets.hpp"
#include <archive.h>
#include <archive_entry.h>
#include <unordered_set>

namespace orbelio::detail {
namespace {
constexpr std::size_t maxArchiveEntries = 32;
constexpr std::size_t zipEndBytes = 22;
constexpr std::size_t zipMaxCommentBytes = 65535;
constexpr std::size_t zip64LocatorBytes = 20;
constexpr std::uint32_t zipEndSignature = 0x06054b50;
constexpr std::uint32_t zip64EndSignature = 0x06064b50;
constexpr std::uint32_t zip64LocatorSignature = 0x07064b50;

bool localAssetName(std::string_view name) {
    if (name.empty() || name.front() == '/' || name.find_first_of("\\:\0", 0, 3) != std::string_view::npos) {
        return false;
    }
    while (!name.empty()) {
        const auto slash = name.find('/');
        const auto component = name.substr(0, slash);
        if (component.empty() || component == "." || component == "..") {
            return false;
        }
        if (slash == std::string_view::npos) {
            return true;
        }
        name.remove_prefix(slash + 1);
    }
    return true;
}

std::size_t zipEndOffset(const Bytes& bytes) {
    require(bytes.size() >= zipEndBytes, "Truncated SOG ZIP container");
    const auto first =
        bytes.size() > zipEndBytes + zipMaxCommentBytes ? bytes.size() - zipEndBytes - zipMaxCommentBytes : 0;
    for (auto at = bytes.size() - zipEndBytes;; --at) {
        if (bytes.u32(at) == zipEndSignature && bytes.u16(at + 20) == bytes.size() - at - zipEndBytes) {
            return at;
        }
        if (at == first) {
            break;
        }
    }
    throw std::runtime_error("Missing SOG ZIP central directory");
}

void validateZipDirectory(std::span<const std::byte> data) {
    const Bytes bytes(data);
    const auto at = zipEndOffset(bytes);
    require(bytes.u16(at + 4) == 0 && bytes.u16(at + 6) == 0, "Multi-disk SOG ZIP is not supported");
    std::uint64_t entries = bytes.u16(at + 10);
    std::uint64_t size = bytes.u32(at + 12);
    std::uint64_t offset = bytes.u32(at + 16);
    if (entries == std::numeric_limits<std::uint16_t>::max() ||
        size == std::numeric_limits<std::uint32_t>::max() ||
        offset == std::numeric_limits<std::uint32_t>::max()) {
        require(at >= zip64LocatorBytes && bytes.u32(at - zip64LocatorBytes) == zip64LocatorSignature,
                "Missing SOG ZIP64 locator");
        const auto end = bytes.u64(at - zip64LocatorBytes + 8);
        bytes.slice(end, 56);
        require(bytes.u32(end) == zip64EndSignature && bytes.u32(end + 16) == 0 && bytes.u32(end + 20) == 0,
                "Invalid SOG ZIP64 directory");
        entries = bytes.u64(end + 32);
        size = bytes.u64(end + 40);
        offset = bytes.u64(end + 48);
    }
    // Bound central-directory allocation before libarchive parses entry names and extra fields.
    require(entries && entries <= maxArchiveEntries && size <= headerByteLimit && offset <= at &&
                size <= at - offset,
            "Oversized/invalid SOG ZIP directory");
    bytes.slice(offset, size);
}

struct ArchiveDeleter {
    void operator()(archive* value) const { archive_read_free(value); }
};

SogAsset readEntry(archive* reader, archive_entry* entry, DecodeContext& context, std::string_view name) {
    const auto size = archive_entry_size(entry);
    require(size > 0 && std::uint64_t(size) <= context.limits.inputBytes &&
                (name != "meta.json" || std::uint64_t(size) <= headerByteLimit),
            "Oversized SOG ZIP entry");
    SogAsset result{};
    result.owned.emplace(context, static_cast<std::size_t>(size));
    std::size_t at = 0;
    while (at < result.owned->data.size()) {
        const auto read =
            archive_read_data(reader, result.owned->data.data() + at, result.owned->data.size() - at);
        require(read > 0, "Corrupt/truncated SOG ZIP entry");
        at += static_cast<std::size_t>(read);
    }
    std::byte extra{};
    require(archive_read_data(reader, &extra, 1) == 0, "Corrupt SOG ZIP entry/CRC");
    result.bytes = result.owned->data;
    return result;
}
}

SogAssets::SogAssets(std::span<const std::byte> data, DecodeContext& context)
    : data_(data), context_(context), bundled_(data.size() >= 4 && std::memcmp(data.data(), "PK", 2) == 0) {
    if (bundled_) {
        validateZipDirectory(data);
    }
}

SogAsset SogAssets::load(std::string_view name) {
    require(localAssetName(name), "SOG texture must be a relative local path");
    if (bundled_) {
        return fromArchive(name);
    }
    SogAsset result{};
    if (name == "meta.json") {
        result.bytes = data_;
    } else {
        require(bool(context_.resolver), "Unpacked SOG requires a local texture resolver");
        result.lease = context_.resolver(name);
        result.bytes = result.lease.bytes();
        require(result.bytes.size() <= context_.limits.inputBytes, "SOG texture exceeds input budget");
    }
    return result;
}

SogAsset SogAssets::fromArchive(std::string_view name) {
    std::unique_ptr<archive, ArchiveDeleter> reader(archive_read_new());
    require(bool(reader), "Cannot initialize SOG ZIP decoder");
    require(archive_read_support_format_zip_seekable(reader.get()) == ARCHIVE_OK &&
                archive_read_open_memory(reader.get(), data_.data(), data_.size()) == ARCHIVE_OK,
            "Invalid SOG ZIP container");
    std::optional<SogAsset> result;
    archive_entry* entry = nullptr;
    std::unordered_set<std::string> names;
    int status = ARCHIVE_OK;
    while ((status = archive_read_next_header(reader.get(), &entry)) == ARCHIVE_OK) {
        const auto* path = archive_entry_pathname_utf8(entry);
        require(path && localAssetName(path) && names.size() < maxArchiveEntries && names.insert(path).second,
                "Invalid/duplicate SOG ZIP entry");
        require(!archive_entry_symlink(entry) && !archive_entry_hardlink(entry) &&
                    (archive_entry_filetype(entry) == AE_IFREG || archive_entry_filetype(entry) == AE_IFDIR),
                "SOG ZIP entries must be regular files");
        if (name == path) {
            result.emplace(readEntry(reader.get(), entry, context_, name));
        }
    }
    require(status == ARCHIVE_EOF && result.has_value(), "Missing/corrupt SOG ZIP asset");
    return std::move(*result);
}
}
