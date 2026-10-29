#ifndef BLUE_TESTS_SCRATCH_DIRECTORY_H_
#define BLUE_TESTS_SCRATCH_DIRECTORY_H_

#include <SDL3/SDL_timer.h>

#include <atomic>
#include <string>
#include <string_view>

#include "core/path.h"
#include "platform/file_system.h"

namespace blue::testing {

// A unique directory under the system temp directory, removed together with
// everything inside it on destruction, so failed checks leave nothing behind.
class ScratchDirectory {
 public:
  explicit ScratchDirectory(std::string_view label = "test")
      : path_(core::JoinPath(platform::TempDirectory(), UniqueName(label))) {
    platform::CreateDirectories(path_);
  }
  ScratchDirectory(const ScratchDirectory&) = delete;
  ScratchDirectory& operator=(const ScratchDirectory&) = delete;
  ~ScratchDirectory() { platform::RemoveDirectoryRecursive(path_); }

  const std::string& path() const { return path_; }

  // Returns `relative` resolved against the scratch directory, normalized.
  std::string Child(std::string_view relative) const {
    return core::JoinPath(path_, relative);
  }

 private:
  static std::string UniqueName(std::string_view label) {
    static std::atomic<unsigned> counter{0};
    return "blue_" + std::string(label) + "_" +
           std::to_string(SDL_GetTicksNS()) + "_" + std::to_string(counter++);
  }

  std::string path_;
};

}  // namespace blue::testing

#endif  // BLUE_TESTS_SCRATCH_DIRECTORY_H_
