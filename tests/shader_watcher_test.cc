#include "shader/shader_watcher.h"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "platform/file_system.h"
#include "tests/scratch_directory.h"

namespace blue::shader {
namespace {

using ScratchDirectory = testing::ScratchDirectory;
using Paths = std::vector<std::string>;

constexpr int kMaxWriteAttempts = 1000;

// Replaces the contents of `path`.
void WriteText(const std::string& path, std::string_view text) {
  std::vector<std::byte> bytes;
  for (const char c : text) {
    bytes.push_back(static_cast<std::byte>(c));
  }
  REQUIRE(platform::WriteFileAtomic(path, bytes).ok());
}

// Rewrites `path` until its modification time differs from `before`. File
// systems with coarse timestamps can give two quick writes the same time;
// retrying (bounded) beats sleeping for a guessed granularity.
void Touch(const std::string& path, std::optional<int64_t> before) {
  for (int attempt = 0; attempt < kMaxWriteAttempts; ++attempt) {
    WriteText(path, "// edit " + std::to_string(attempt) + "\n");
    if (platform::LastWriteTime(path) != before) {
      return;
    }
  }
  FAIL("modification time of " << path << " never changed");
}

void Touch(const std::string& path) {
  Touch(path, platform::LastWriteTime(path));
}

// A watched root "main.frag" that includes "common.glsl", plus an unrelated
// root "other.vert" that includes nothing.
struct Fixture {
  ScratchDirectory dir{"shader_watcher"};
  std::string main = dir.Child("main.frag");
  std::string common = dir.Child("common.glsl");
  std::string other = dir.Child("other.vert");
  ShaderWatcher watcher;

  Fixture() {
    WriteText(main, "// main\n");
    WriteText(common, "// common\n");
    WriteText(other, "// other\n");
    watcher.Watch(main, {common});
    watcher.Watch(other, {});
  }
};

TEST_CASE("untouched files are never reported") {
  Fixture f;
  for (int i = 0; i < 4; ++i) {
    CHECK(f.watcher.Poll().empty());
  }
}

TEST_CASE("a modified root is reported after the second stable poll") {
  Fixture f;
  Touch(f.main);
  CHECK(f.watcher.Poll().empty());
  CHECK(f.watcher.Poll() == Paths{f.main});
  // Reported once only.
  CHECK(f.watcher.Poll().empty());
}

TEST_CASE("a file that keeps changing is not reported") {
  Fixture f;
  Touch(f.main);
  CHECK(f.watcher.Poll().empty());
  Touch(f.main);
  CHECK(f.watcher.Poll().empty());
  CHECK(f.watcher.Poll() == Paths{f.main});
}

TEST_CASE("a modified include reports only its dependents") {
  Fixture f;
  Touch(f.common);
  CHECK(f.watcher.Poll().empty());
  CHECK(f.watcher.Poll() == Paths{f.main});
}

TEST_CASE("a deleted file is reported once it exists again and is stable") {
  Fixture f;
  const std::optional<int64_t> original = platform::LastWriteTime(f.common);
  REQUIRE(platform::RemovePath(f.common));
  CHECK(f.watcher.Poll().empty());
  CHECK(f.watcher.Poll().empty());  // Stable but still missing.

  Touch(f.common, original);
  CHECK(f.watcher.Poll().empty());  // First sighting of the new file.
  CHECK(f.watcher.Poll() == Paths{f.main});
}

TEST_CASE("Watch after a successful compile clears the pending change") {
  Fixture f;
  Touch(f.main);
  CHECK(f.watcher.Poll().empty());
  f.watcher.Watch(f.main, {f.common});
  CHECK(f.watcher.Poll().empty());
  CHECK(f.watcher.Poll().empty());
}

TEST_CASE("Watch replaces the dependency set of a root") {
  Fixture f;
  f.watcher.Watch(f.main, {});  // No longer depends on common.glsl.
  Touch(f.common);
  CHECK(f.watcher.Poll().empty());
  CHECK(f.watcher.Poll().empty());
}

TEST_CASE("a change after a failed compile is reported again") {
  Fixture f;
  Touch(f.main);
  CHECK(f.watcher.Poll().empty());
  CHECK(f.watcher.Poll() == Paths{f.main});
  // The compile failed, so the caller did not call Watch(). Fixing the file
  // is a new change.
  Touch(f.main);
  CHECK(f.watcher.Poll().empty());
  CHECK(f.watcher.Poll() == Paths{f.main});
}

TEST_CASE("several roots are reported in the order they were watched") {
  Fixture f;
  Touch(f.other);
  Touch(f.main);
  CHECK(f.watcher.Poll().empty());
  CHECK(f.watcher.Poll() == Paths{f.main, f.other});
}

}  // namespace
}  // namespace blue::shader
