#include "platform/file_system.h"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_stdinc.h>

#include <algorithm>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/path.h"
#include "core/result.h"

namespace blue::platform {
namespace {

// Closes the wrapped SDL stream on destruction. Close() reports whether the
// final flush succeeded, which matters for streams opened for writing.
class IoStreamCloser {
 public:
  explicit IoStreamCloser(SDL_IOStream* stream) : stream_(stream) {}
  IoStreamCloser(const IoStreamCloser&) = delete;
  IoStreamCloser& operator=(const IoStreamCloser&) = delete;
  ~IoStreamCloser() { Close(); }

  bool Close() {
    SDL_IOStream* stream = stream_;
    stream_ = nullptr;
    return stream == nullptr || SDL_CloseIO(stream);
  }

 private:
  SDL_IOStream* stream_;
};

// Converts an SDL-allocated string to a generic UTF-8 string and frees it.
// Returns an empty string for null.
std::string TakeSdlString(char* text) {
  if (text == nullptr) {
    return std::string();
  }
  std::string result(text);
  SDL_free(text);
  std::replace(result.begin(), result.end(), '\\', '/');
  return result;
}

// Collects the entry names of a directory, appending to `names`.
SDL_EnumerationResult CollectEntry(void* userdata, const char* /*dirname*/,
                                   const char* name) {
  static_cast<std::vector<std::string>*>(userdata)->emplace_back(name);
  return SDL_ENUM_CONTINUE;
}

}  // namespace

const char* ToString(FileError error) {
  switch (error) {
    case FileError::kNotFound:
      return "not found";
    case FileError::kTooLarge:
      return "too large";
    case FileError::kReadFailed:
      return "read failed";
    case FileError::kWriteFailed:
      return "write failed";
  }
  return "unknown";
}

core::Result<std::vector<std::byte>, FileError> ReadFile(
    const std::string& path, size_t max_bytes) {
  using ReadResult = core::Result<std::vector<std::byte>, FileError>;
  SDL_IOStream* stream = SDL_IOFromFile(path.c_str(), "rb");
  if (stream == nullptr) {
    return ReadResult::Error(PathExists(path) ? FileError::kReadFailed
                                              : FileError::kNotFound);
  }
  IoStreamCloser closer(stream);
  const Sint64 size = SDL_GetIOSize(stream);
  if (size < 0) {
    return ReadResult::Error(FileError::kReadFailed);
  }
  if (static_cast<Uint64>(size) > max_bytes) {
    return ReadResult::Error(FileError::kTooLarge);
  }
  std::vector<std::byte> bytes(static_cast<size_t>(size));
  if (!bytes.empty() &&
      SDL_ReadIO(stream, bytes.data(), bytes.size()) != bytes.size()) {
    return ReadResult::Error(FileError::kReadFailed);
  }
  return ReadResult::Ok(std::move(bytes));
}

core::Result<void, FileError> WriteFileAtomic(
    const std::string& path, std::span<const std::byte> bytes) {
  using WriteResult = core::Result<void, FileError>;
  const std::string parent = core::ParentPath(path);
  if (!parent.empty() && !CreateDirectories(parent)) {
    return WriteResult::Error(FileError::kWriteFailed);
  }

  const std::string temp = path + ".tmp";
  SDL_IOStream* stream = SDL_IOFromFile(temp.c_str(), "wb");
  if (stream == nullptr) {
    return WriteResult::Error(FileError::kWriteFailed);
  }
  IoStreamCloser closer(stream);
  const bool written =
      bytes.empty() ||
      SDL_WriteIO(stream, bytes.data(), bytes.size()) == bytes.size();
  // Close before renaming: Windows cannot move a file that is still open.
  if (!closer.Close() || !written) {
    RemovePath(temp);
    return WriteResult::Error(FileError::kWriteFailed);
  }
  // SDL_RenamePath replaces an existing target (MOVEFILE_REPLACE_EXISTING on
  // Windows, rename(2) elsewhere).
  if (!SDL_RenamePath(temp.c_str(), path.c_str())) {
    RemovePath(temp);
    return WriteResult::Error(FileError::kWriteFailed);
  }
  return WriteResult::Ok();
}

bool CreateDirectories(const std::string& path) {
  // SDL_CreateDirectory creates missing parents and succeeds if the
  // directory already exists.
  return !path.empty() && SDL_CreateDirectory(path.c_str());
}

bool PathExists(const std::string& path) {
  return SDL_GetPathInfo(path.c_str(), nullptr);
}

bool IsRegularFile(const std::string& path) {
  SDL_PathInfo info;
  return SDL_GetPathInfo(path.c_str(), &info) && info.type == SDL_PATHTYPE_FILE;
}

bool RemovePath(const std::string& path) {
  return SDL_RemovePath(path.c_str());
}

bool RemoveDirectoryRecursive(const std::string& path) {
  if (!PathExists(path)) {
    return true;
  }
  SDL_PathInfo info;
  if (SDL_GetPathInfo(path.c_str(), &info) &&
      info.type == SDL_PATHTYPE_DIRECTORY) {
    // Collect first: removing entries while enumerating is not allowed.
    std::vector<std::string> names;
    if (!SDL_EnumerateDirectory(path.c_str(), CollectEntry, &names)) {
      return false;
    }
    bool all_removed = true;
    for (const std::string& name : names) {
      all_removed =
          RemoveDirectoryRecursive(core::JoinPath(path, name)) && all_removed;
    }
    if (!all_removed) {
      return false;
    }
  }
  return RemovePath(path);
}

std::string CurrentDirectory() {
  return core::NormalizePath(TakeSdlString(SDL_GetCurrentDirectory()));
}

std::string AbsolutePath(std::string_view path) {
  return core::JoinPath(CurrentDirectory(), path);
}

std::string BasePath() {
  // The returned string is owned and cached by SDL; it must not be freed.
  const char* base = SDL_GetBasePath();
  if (base == nullptr) {
    return std::string();
  }
  return core::NormalizePath(base);
}

std::string PrefPath(const char* organization, const char* application) {
  return core::NormalizePath(
      TakeSdlString(SDL_GetPrefPath(organization, application)));
}

std::string TempDirectory() {
  for (const char* name : {"TEMP", "TMP", "TMPDIR"}) {
    const char* value = SDL_getenv(name);
    if (value != nullptr && *value != '\0') {
      return core::NormalizePath(value);
    }
  }
  return "/tmp";
}

}  // namespace blue::platform
