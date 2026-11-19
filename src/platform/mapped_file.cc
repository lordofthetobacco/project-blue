#include "platform/mapped_file.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "core/result.h"
#include "platform/file_system.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace blue::platform {
namespace {

using OpenResult = core::Result<std::unique_ptr<MappedFile>, FileError>;

#ifdef _WIN32

std::wstring Widen(const std::string& utf8) {
  const int length = MultiByteToWideChar(
      CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  std::wstring wide(static_cast<size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                      wide.data(), length);
  return wide;
}

// Closes a Win32 handle on scope exit; the view outlives both handles.
class HandleCloser {
 public:
  explicit HandleCloser(HANDLE handle) : handle_(handle) {}
  HandleCloser(const HandleCloser&) = delete;
  HandleCloser& operator=(const HandleCloser&) = delete;
  ~HandleCloser() {
    if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
      CloseHandle(handle_);
    }
  }

 private:
  HANDLE handle_;
};

#endif

}  // namespace

#ifdef _WIN32

// Shared read and delete, not write: renaming over or deleting the file
// works, rewriting it in place does not.
OpenResult MappedFile::Open(const std::string& path, size_t max_bytes) {
  const HANDLE file = CreateFileW(
      Widen(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return OpenResult::Error(FileError::kNotFound);
  }
  HandleCloser file_closer(file);
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file, &size) || size.QuadPart < 0) {
    return OpenResult::Error(FileError::kReadFailed);
  }
  if (static_cast<uint64_t>(size.QuadPart) > max_bytes) {
    return OpenResult::Error(FileError::kTooLarge);
  }
  if (size.QuadPart == 0) {  // CreateFileMapping rejects empty files.
    return OpenResult::Ok(
        std::unique_ptr<MappedFile>(new MappedFile(nullptr, 0)));
  }
  const HANDLE mapping =
      CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
  if (mapping == nullptr) {
    return OpenResult::Error(FileError::kReadFailed);
  }
  HandleCloser mapping_closer(mapping);
  const void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
  if (view == nullptr) {
    return OpenResult::Error(FileError::kReadFailed);
  }
  return OpenResult::Ok(std::unique_ptr<MappedFile>(
      new MappedFile(view, static_cast<size_t>(size.QuadPart))));
}

MappedFile::~MappedFile() {
  if (data_ != nullptr) {
    UnmapViewOfFile(data_);
  }
}

#else

OpenResult MappedFile::Open(const std::string& path, size_t max_bytes) {
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return OpenResult::Error(FileError::kNotFound);
  }
  struct stat info{};
  const bool stated = fstat(fd, &info) == 0 && S_ISREG(info.st_mode);
  if (!stated) {
    close(fd);
    return OpenResult::Error(FileError::kReadFailed);
  }
  const auto size = static_cast<uint64_t>(info.st_size);
  if (size > max_bytes) {
    close(fd);
    return OpenResult::Error(FileError::kTooLarge);
  }
  if (size == 0) {
    close(fd);
    return OpenResult::Ok(
        std::unique_ptr<MappedFile>(new MappedFile(nullptr, 0)));
  }
  void* view =
      mmap(nullptr, static_cast<size_t>(size), PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);  // The mapping keeps the file referenced.
  if (view == MAP_FAILED) {
    return OpenResult::Error(FileError::kReadFailed);
  }
  return OpenResult::Ok(std::unique_ptr<MappedFile>(
      new MappedFile(view, static_cast<size_t>(size))));
}

MappedFile::~MappedFile() {
  if (data_ != nullptr) {
    munmap(const_cast<void*>(data_), size_);
  }
}

#endif

}  // namespace blue::platform
