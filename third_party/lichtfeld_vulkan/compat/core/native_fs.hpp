#pragma once
// Orbelio standalone filesystem adapter; paths at this boundary are UTF-8.
#include <filesystem>
#include <string_view>
namespace lfs::core::native_fs {
inline std::filesystem::path from_utf8(std::string_view value) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(value.data()),value.size()));
}
inline std::filesystem::path native_path(const std::filesystem::path& value) { return value; }
}
