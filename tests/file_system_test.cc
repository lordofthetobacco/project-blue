#include "platform/file_system.h"

#include <doctest/doctest.h>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/path.h"
#include "tests/scratch_directory.h"

namespace blue::platform {
namespace {

// UTF-8 for "modele_" followed by the Polish letters z-dot, o-acute, l-stroke,
// w.
constexpr char kNonAsciiDirectory[] = "modele_\xC5\xBC\xC3\xB3\xC5\x82w";

using ScratchDirectory = testing::ScratchDirectory;

std::vector<std::byte> Bytes(std::string_view text) {
  std::vector<std::byte> bytes;
  for (const char c : text) {
    bytes.push_back(static_cast<std::byte>(c));
  }
  return bytes;
}

std::string Text(const std::vector<std::byte>& bytes) {
  std::string text;
  for (const std::byte b : bytes) {
    text.push_back(static_cast<char>(b));
  }
  return text;
}

constexpr size_t kLimit = 1024 * 1024;

TEST_CASE("file system: write then read round trip") {
  const ScratchDirectory dir;
  const std::string file = dir.Child("data.bin");
  const std::vector<std::byte> payload =
      Bytes(std::string_view("hello\0world", 11));

  REQUIRE(WriteFileAtomic(file, payload).ok());
  CHECK(IsRegularFile(file));
  CHECK_FALSE(PathExists(file + ".tmp"));

  const auto read = ReadFile(file, kLimit);
  REQUIRE(read.ok());
  CHECK(read.value() == payload);
}

TEST_CASE("file system: empty file round trip") {
  const ScratchDirectory dir;
  const std::string file = dir.Child("empty.bin");

  REQUIRE(WriteFileAtomic(file, {}).ok());
  const auto read = ReadFile(file, kLimit);
  REQUIRE(read.ok());
  CHECK(read.value().empty());
}

TEST_CASE("file system: write overwrites an existing file") {
  const ScratchDirectory dir;
  const std::string file = dir.Child("data.txt");

  REQUIRE(WriteFileAtomic(file, Bytes("first version, longer")).ok());
  REQUIRE(WriteFileAtomic(file, Bytes("second")).ok());

  const auto read = ReadFile(file, kLimit);
  REQUIRE(read.ok());
  CHECK(Text(read.value()) == "second");
  CHECK_FALSE(PathExists(file + ".tmp"));
}

TEST_CASE("file system: read reports kTooLarge above max_bytes") {
  const ScratchDirectory dir;
  const std::string file = dir.Child("big.bin");
  REQUIRE(WriteFileAtomic(file, Bytes("0123456789")).ok());

  const auto too_small = ReadFile(file, 9);
  REQUIRE_FALSE(too_small.ok());
  CHECK(too_small.error() == FileError::kTooLarge);

  CHECK(ReadFile(file, 10).ok());
}

TEST_CASE("file system: read reports kNotFound for a missing file") {
  const ScratchDirectory dir;
  const auto read = ReadFile(dir.Child("missing.bin"), kLimit);
  REQUIRE_FALSE(read.ok());
  CHECK(read.error() == FileError::kNotFound);
}

TEST_CASE("file system: read reports kReadFailed for a directory") {
  const ScratchDirectory dir;
  const auto read = ReadFile(dir.path(), kLimit);
  REQUIRE_FALSE(read.ok());
  CHECK(read.error() != FileError::kNotFound);
}

TEST_CASE("file system: write creates nested directories") {
  const ScratchDirectory dir;
  const std::string file = dir.Child("a/b/c/scene.bin");

  REQUIRE(WriteFileAtomic(file, Bytes("x")).ok());
  CHECK(IsRegularFile(file));
  CHECK(PathExists(dir.Child("a/b")));
  CHECK_FALSE(IsRegularFile(dir.Child("a/b")));
}

TEST_CASE("file system: write fails when the parent is a file") {
  const ScratchDirectory dir;
  REQUIRE(WriteFileAtomic(dir.Child("blocker"), Bytes("x")).ok());

  const auto written = WriteFileAtomic(dir.Child("blocker/child"), Bytes("y"));
  REQUIRE_FALSE(written.ok());
  CHECK(written.error() == FileError::kWriteFailed);
}

TEST_CASE("file system: CreateDirectories is idempotent") {
  const ScratchDirectory dir;
  const std::string nested = dir.Child("x/y/z");

  CHECK(CreateDirectories(nested));
  CHECK(CreateDirectories(nested));
  CHECK(PathExists(nested));
  CHECK_FALSE(IsRegularFile(nested));
  CHECK_FALSE(CreateDirectories(""));
}

TEST_CASE("file system: non-ASCII directory names work") {
  const ScratchDirectory dir;
  const std::string nested = dir.Child(kNonAsciiDirectory);
  const std::string file = core::JoinPath(nested, "plik.txt");

  REQUIRE(CreateDirectories(nested));
  REQUIRE(
      WriteFileAtomic(file, Bytes("za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87")).ok());

  const auto read = ReadFile(file, kLimit);
  REQUIRE(read.ok());
  CHECK(Text(read.value()) == "za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87");
  CHECK(core::FileName(core::ParentPath(file)) == kNonAsciiDirectory);
}

TEST_CASE("file system: RemovePath removes a file and an empty directory") {
  const ScratchDirectory dir;
  const std::string file = dir.Child("f.txt");
  const std::string sub = dir.Child("sub");
  REQUIRE(WriteFileAtomic(file, Bytes("x")).ok());
  REQUIRE(CreateDirectories(sub));

  CHECK(RemovePath(file));
  CHECK(RemovePath(sub));
  CHECK_FALSE(PathExists(file));
  CHECK_FALSE(PathExists(sub));
  CHECK(RemovePath(file));  // Already gone counts as success.
}

TEST_CASE("file system: RemovePath refuses a non-empty directory") {
  const ScratchDirectory dir;
  REQUIRE(WriteFileAtomic(dir.Child("sub/f.txt"), Bytes("x")).ok());

  CHECK_FALSE(RemovePath(dir.Child("sub")));
  CHECK(PathExists(dir.Child("sub/f.txt")));
}

TEST_CASE("file system: RemoveDirectoryRecursive cleans up a tree") {
  std::string root;
  {
    const ScratchDirectory dir;
    root = dir.Child("tree");
    REQUIRE(WriteFileAtomic(dir.Child("tree/a.txt"), Bytes("a")).ok());
    REQUIRE(WriteFileAtomic(dir.Child("tree/b/c.txt"), Bytes("c")).ok());
    REQUIRE(CreateDirectories(dir.Child("tree/b/empty")));

    CHECK(RemoveDirectoryRecursive(root));
    CHECK_FALSE(PathExists(root));
    CHECK(PathExists(dir.path()));
  }
  CHECK(RemoveDirectoryRecursive(root));  // Missing path is fine.
}

TEST_CASE("file system: RemoveDirectoryRecursive removes the scratch itself") {
  std::string path;
  {
    const ScratchDirectory dir;
    path = dir.path();
    REQUIRE(WriteFileAtomic(dir.Child("f"), Bytes("x")).ok());
  }
  CHECK_FALSE(PathExists(path));
}

TEST_CASE("file system: directory queries") {
  const std::string current = CurrentDirectory();
  CHECK_FALSE(current.empty());
  CHECK(current.find('\\') == std::string::npos);
  CHECK(core::IsAbsolutePath(current));
  CHECK(PathExists(current));

  const std::string base = BasePath();
  CHECK(core::IsAbsolutePath(base));
  CHECK(base.find('\\') == std::string::npos);

  const std::string temp = TempDirectory();
  CHECK_FALSE(temp.empty());
  CHECK(PathExists(temp));
}

TEST_CASE("file system: AbsolutePath resolves against the current directory") {
  const std::string current = CurrentDirectory();
  CHECK(AbsolutePath("a/../b.txt") == core::JoinPath(current, "b.txt"));
  CHECK(AbsolutePath("") == current);
  CHECK(AbsolutePath(current + "/x/./y") == current + "/x/y");
}

}  // namespace
}  // namespace blue::platform
