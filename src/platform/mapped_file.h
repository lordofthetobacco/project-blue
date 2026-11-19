#ifndef BLUE_PLATFORM_MAPPED_FILE_H_
#define BLUE_PLATFORM_MAPPED_FILE_H_

#include <cstddef>
#include <memory>
#include <span>
#include <string>

#include "core/result.h"
#include "platform/file_system.h"

// Read-only memory mapping of a whole file (Windows file mappings, POSIX
// mmap). Mapped pages are backed by the file, not by the page file: they do
// not count as private commit, are shared with the OS file cache and are only
// read from disk when first touched.
//
// Lifetime guarantees while a MappedFile lives:
//   - Replacing the file by renaming another file over it (what
//     platform::WriteFileAtomic does) or deleting it is allowed; the mapping
//     keeps showing the old contents until it is destroyed.
//   - Opening the file for writing in place fails on Windows (the file is
//     opened without write sharing and the OS refuses to truncate a mapped
//     file). On POSIX nothing prevents another process from truncating or
//     rewriting the file in place; touching pages past a truncation raises
//     SIGBUS. Every writer in this project replaces files by rename instead.
//   - A file on removable or network media that disappears may make page
//     faults fail (EXCEPTION_IN_PAGE_ERROR / SIGBUS); there is no recovery.

namespace blue::platform {

class MappedFile {
 public:
  MappedFile(const MappedFile&) = delete;
  MappedFile& operator=(const MappedFile&) = delete;
  ~MappedFile();

  // Maps the file at `path` (UTF-8) read-only. Returns kNotFound if it cannot
  // be opened, kTooLarge if it is bigger than `max_bytes` (nothing is mapped),
  // kReadFailed if mapping fails (the caller may fall back to ReadFile). An
  // empty file gives an empty mapping.
  static core::Result<std::unique_ptr<MappedFile>, FileError> Open(
      const std::string& path, size_t max_bytes);

  // The file's bytes, valid for the lifetime of this object.
  [[nodiscard]] std::span<const std::byte> bytes() const {
    return {static_cast<const std::byte*>(data_), size_};
  }

 private:
  MappedFile(const void* data, size_t size) : data_(data), size_(size) {}

  const void* data_;
  size_t size_;
};

}  // namespace blue::platform

#endif  // BLUE_PLATFORM_MAPPED_FILE_H_
