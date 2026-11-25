#ifndef BLUE_CORE_LOG_H_
#define BLUE_CORE_LOG_H_

#include <cstdio>
#include <cstdlib>
#include <format>
#include <mutex>
#include <string_view>
#include <utility>

namespace blue::core::log {

// Severity of a logged line.
enum class Level : int { kInfo = 0, kWarn = 1, kError = 2, kFatal = 3 };

// Receives every logged line (without the level prefix or a newline) when
// set with SetSink(). `user` is the pointer given to SetSink().
using Sink = void (*)(Level level, std::string_view message, void* user);

namespace internal {

// The process-wide sink; trivially destructible, written by SetSink() only.
struct SinkSlot {
  Sink fn = nullptr;
  void* user = nullptr;
};
inline constinit SinkSlot g_sink;

// Serialises calls into the sink so it never runs on two threads at once.
// Leaked on purpose: logging may happen during static destruction.
inline std::mutex& SinkMutex() {
  static std::mutex& mutex = *new std::mutex;
  return mutex;
}

inline std::string_view LevelName(Level level) {
  switch (level) {
    case Level::kInfo:
      return "info";
    case Level::kWarn:
      return "warn";
    case Level::kError:
      return "error";
    case Level::kFatal:
      return "fatal";
  }
  return "?";
}

// Delivers one line: to the sink when one is set, otherwise as
// "[level] message" to stdout (info) or stderr (the rest).
inline void Write(Level level, std::string_view message) {
  if (const SinkSlot sink = g_sink; sink.fn != nullptr) {
    const std::lock_guard<std::mutex> lock(SinkMutex());
    sink.fn(level, message, sink.user);
    return;
  }
  std::FILE* stream = level == Level::kInfo ? stdout : stderr;
  const std::string_view name = LevelName(level);
  std::fprintf(stream, "[%.*s] %.*s\n", static_cast<int>(name.size()),
               name.data(), static_cast<int>(message.size()), message.data());
}

}  // namespace internal

// Routes every following line through `sink` (called with `user`; never on
// two threads at once), or restores the default stdout/stderr output when
// `sink` is null. Not synchronised with logging: call it before other
// threads may log (Jolt workers, ...). The sink must not log itself.
inline void SetSink(Sink sink, void* user) {
  internal::g_sink = {.fn = sink, .user = sink != nullptr ? user : nullptr};
}

// Logs an informational message to stdout. `fmt` is a std::format string.
template <typename... Args>
void Info(std::format_string<Args...> fmt, Args&&... args) {
  internal::Write(Level::kInfo, std::format(fmt, std::forward<Args>(args)...));
}

// Logs a recoverable problem to stderr.
template <typename... Args>
void Warn(std::format_string<Args...> fmt, Args&&... args) {
  internal::Write(Level::kWarn, std::format(fmt, std::forward<Args>(args)...));
}

// Logs an error to stderr. The caller keeps running; use Fatal() when there is
// no sensible way to continue.
template <typename... Args>
void Error(std::format_string<Args...> fmt, Args&&... args) {
  internal::Write(Level::kError, std::format(fmt, std::forward<Args>(args)...));
}

// Reports an unrecoverable error (through the sink, if set) and terminates
// the process. Used for startup failures where there is no sensible way to
// continue (no GPU, missing shaders, ...).
template <typename... Args>
[[noreturn]] void Fatal(std::format_string<Args...> fmt, Args&&... args) {
  internal::Write(Level::kFatal, std::format(fmt, std::forward<Args>(args)...));
  std::fflush(stdout);
  std::fflush(stderr);
  std::abort();
}

}  // namespace blue::core::log

#endif  // BLUE_CORE_LOG_H_
