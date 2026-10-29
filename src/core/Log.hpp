#pragma once

#include <cstdio>
#include <cstdlib>
#include <format>
#include <string_view>
#include <utility>

namespace blue::log {

namespace detail {
inline void write(std::FILE* stream, std::string_view level, std::string_view message)
{
    std::fprintf(stream, "[%.*s] %.*s\n",
                 static_cast<int>(level.size()), level.data(),
                 static_cast<int>(message.size()), message.data());
}
} // namespace detail

template <typename... Args>
void info(std::format_string<Args...> fmt, Args&&... args)
{
    detail::write(stdout, "info", std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void warn(std::format_string<Args...> fmt, Args&&... args)
{
    detail::write(stderr, "warn", std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void error(std::format_string<Args...> fmt, Args&&... args)
{
    detail::write(stderr, "error", std::format(fmt, std::forward<Args>(args)...));
}

// Unrecoverable error: report and terminate. Used for startup failures where
// there is no sensible way to continue (no GPU, missing shaders, ...).
template <typename... Args>
[[noreturn]] void fatal(std::format_string<Args...> fmt, Args&&... args)
{
    detail::write(stderr, "fatal", std::format(fmt, std::forward<Args>(args)...));
    std::fflush(stderr);
    std::abort();
}

} // namespace blue::log
