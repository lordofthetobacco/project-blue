#ifndef BLUE_PLATFORM_FILE_SYSTEM_H_
#define BLUE_PLATFORM_FILE_SYSTEM_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"

// File system access built on SDL3's filesystem and IOStream APIs, which
// take UTF-8 paths on every platform (unlike narrow-path std::ifstream on
// Windows). None of these functions needs SDL_Init. Paths are UTF-8 and may
// use '/' or '\'; returned paths always use '/'. Failures are reported
// through return values; nothing here throws or terminates.

namespace blue::platform {

enum class FileError {
  kNotFound,     // The file does not exist or cannot be opened.
  kTooLarge,     // The file is larger than the caller's limit.
  kReadFailed,   // The file exists but reading it failed.
  kWriteFailed,  // Creating, writing or replacing a file failed.
};

// Returns a short human-readable name for `error`, for log messages.
[[nodiscard]] const char* ToString(FileError error);

// Reads the whole file at `path`. Returns kNotFound if it cannot be opened
// and does not exist, kTooLarge if it is bigger than `max_bytes` (nothing is
// read in that case), and kReadFailed if it exists but cannot be read
// completely (for example when `path` is a directory).
core::Result<std::vector<std::byte>, FileError> ReadFile(
    const std::string& path, size_t max_bytes);

// Replaces the contents of `path` with `bytes`. Missing parent directories
// are created. The data is written to "<path>.tmp" and renamed over `path`,
// so readers see either the old or the new file. Overwrites an existing
// file. On any failure the temporary file is removed, `path` is left as it
// was, and kWriteFailed is returned.
core::Result<void, FileError> WriteFileAtomic(const std::string& path,
                                              std::span<const std::byte> bytes);

// Creates the directory `path` and any missing parents. Returns true if the
// directory exists afterwards (including when it already existed) and false
// on failure or for an empty path.
bool CreateDirectories(const std::string& path);

// Returns true if a file or directory exists at `path`. Symlinks are
// followed.
bool PathExists(const std::string& path);

// Returns true if `path` exists and is a regular file (not a directory).
bool IsRegularFile(const std::string& path);

// Returns the last modification time of the file or directory at `path` in
// nanoseconds since the Unix epoch, or nullopt if it does not exist or its
// metadata cannot be read. Only meaningful for comparing two readings of the
// same path: the file system decides the granularity.
std::optional<int64_t> LastWriteTime(const std::string& path);

// Removes a file or an empty directory. Returns true on success, and also
// when nothing exists at `path`; false if the removal failed (for example a
// non-empty directory).
bool RemovePath(const std::string& path);

// Removes `path` and everything below it. Meant for trees the caller created
// itself, such as test temp directories: symlinked directories are followed
// like any other directory. Returns true if `path` is gone afterwards
// (including when it never existed).
bool RemoveDirectoryRecursive(const std::string& path);

// Returns the current working directory without a trailing separator.
// Returns an empty string on failure.
std::string CurrentDirectory();

// Returns `path` made absolute against the current directory and
// normalized; purely lexical, symlinks are not resolved. An absolute `path`
// is only normalized.
std::string AbsolutePath(std::string_view path);

// Returns the directory containing the executable, without a trailing
// separator, or an empty string on failure.
std::string BasePath();

// Returns the per-user writable directory for the given organization and
// application, without a trailing separator. The directory is created if
// needed. Returns an empty string on failure.
std::string PrefPath(const char* organization, const char* application);

// Returns the temporary directory from the TEMP, TMP or TMPDIR environment
// variable (first non-empty one wins), falling back to "/tmp".
std::string TempDirectory();

}  // namespace blue::platform

#endif  // BLUE_PLATFORM_FILE_SYSTEM_H_
