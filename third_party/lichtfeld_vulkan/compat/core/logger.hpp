#pragma once
// Orbelio standalone logging adapter; no logging worker thread.
#include <cstdio>
#include <format>
namespace orbelio::lfs_adapter {
template<class... Args> void log(const char* level, std::format_string<Args...> format, Args&&... args) {
    const auto text = std::format(format, std::forward<Args>(args)...);
    std::fprintf(stderr, "LichtFeld [%s] %s\n", level, text.c_str());
}
}
#define LOG_ERROR(...) ::orbelio::lfs_adapter::log("error", __VA_ARGS__)
#define LOG_WARN(...) ::orbelio::lfs_adapter::log("warn", __VA_ARGS__)
#define LOG_INFO(...) ::orbelio::lfs_adapter::log("info", __VA_ARGS__)
#define LOG_DEBUG(...) ((void)0)
#define LOG_TRACE(...) ((void)0)
#define LOG_PERF(...) ((void)0)
#define LOG_TIMER(...) ((void)0)
