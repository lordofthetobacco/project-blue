#ifndef BLUE_CORE_LOG_H_
#define BLUE_CORE_LOG_H_

#include <cstdio>
#include <cstdlib>
#include <format>
#include <string_view>
#include <utility>

namespace blue::core::log {

namespace internal {

// Writes one "[level] message" line to `stream`.
inline void Write(std::FILE* stream, std::string_view level,
                  std::string_view message) {
  std::fprintf(stream, "[%.*s] %.*s\n", static_cast<int>(level.size()),
               level.data(), static_cast<int>(message.size()), message.data());
}

}  // namespace internal

// Logs an informational message to stdout. `fmt` is a std::format string.
template <typename... Args>
void Info(std::format_string<Args...> fmt, Args&&... args) {
  internal::Write(stdout, "info",
                  std::format(fmt, std::forward<Args>(args)...));
}

// Logs a recoverable problem to stderr.
template <typename... Args>
void Warn(std::format_string<Args...> fmt, Args&&... args) {
  internal::Write(stderr, "warn",
                  std::format(fmt, std::forward<Args>(args)...));
}

// Logs an error to stderr. The caller keeps running; use Fatal() when there is
// no sensible way to continue.
template <typename... Args>
void Error(std::format_string<Args...> fmt, Args&&... args) {
  internal::Write(stderr, "error",
                  std::format(fmt, std::forward<Args>(args)...));
}

// Reports an unrecoverable error and terminates the process. Used for startup
// failures where there is no sensible way to continue (no GPU, missing
// shaders, ...).
template <typename... Args>
[[noreturn]] void Fatal(std::format_string<Args...> fmt, Args&&... args) {
  internal::Write(stderr, "fatal",
                  std::format(fmt, std::forward<Args>(args)...));
  std::fflush(stderr);
  std::abort();
}

}  // namespace blue::core::log

#endif  // BLUE_CORE_LOG_H_
