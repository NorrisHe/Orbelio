#pragma once
#include "native_fs.hpp"
#include <string>
namespace lfs::core {
inline std::filesystem::path utf8_to_path(std::string_view value) { return native_fs::from_utf8(value); }
inline std::string path_to_utf8(const std::filesystem::path& value) {
    const auto utf8 = value.u8string();
    return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}
}
