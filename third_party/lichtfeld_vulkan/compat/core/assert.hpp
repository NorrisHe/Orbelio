#pragma once
// Orbelio standalone diagnostic adapter. Not the full LichtFeld core.
#include "source_site.hpp"
#include <cassert>
#include <stdexcept>
#include <string>
#include <string_view>
namespace lfs::core::detail {
[[noreturn]] inline void assertion_failed(std::string_view title, std::string_view condition,
                                          std::string_view detail, SourceSite) {
    throw std::runtime_error(std::string(title) + ": " + std::string(condition) + ": " + std::string(detail));
}
}
