#include "cook/generated_file.h"

#include <doctest/doctest.h>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"
#include "platform/file_system.h"
#include "tests/scratch_directory.h"

namespace blue::cook {
namespace {

constexpr size_t kMaxTestFileBytes = 1024;

std::vector<std::byte> Bytes(std::string_view text) {
  const auto view = std::as_bytes(std::span(text.data(), text.size()));
  return {view.begin(), view.end()};
}

std::vector<std::byte> ReadAll(const std::string& path) {
  const auto bytes = platform::ReadFile(path, kMaxTestFileBytes);
  REQUIRE(bytes.ok());
  return bytes.value();
}

GeneratedWrite Write(const std::string& path, std::string_view text) {
  core::Result<GeneratedWrite, std::string> written =
      WriteGeneratedFile(path, Bytes(text));
  REQUIRE(written.ok());
  return written.value();
}

TEST_CASE("Generated file names") {
  CHECK(FingerprintPath("a/range.mscn") == "a/range.mscn.fingerprint");
  CHECK(KeptAsidePath("a/range.mscn") == "a/range.generated.mscn");
  CHECK(KeptAsidePath("a.b/notes") == "a.b/notes.generated");
  CHECK(KeptAsidePath("a/.hidden") == "a/.hidden.generated");
  CHECK(KeptAsidePath("x.tar.gz") == "x.tar.generated.gz");
  CHECK(Fingerprint(Bytes("abc")) != Fingerprint(Bytes("abd")));
}

TEST_CASE("A generated file is written into a fresh directory") {
  testing::ScratchDirectory dir("generated_fresh");
  const std::string path = dir.Child("range.mscn");
  CHECK(ClassifyGeneratedFile(path) == GeneratedFileState::kAbsent);
  const GeneratedWrite written = Write(path, "v1");
  CHECK(written.written_path == path);
  CHECK_FALSE(written.kept_user_file);
  CHECK(ReadAll(path) == Bytes("v1"));
  CHECK(platform::IsRegularFile(FingerprintPath(path)));
  CHECK(ClassifyGeneratedFile(path) == GeneratedFileState::kUnedited);
}

TEST_CASE("An unedited generated file is regenerated") {
  testing::ScratchDirectory dir("generated_unedited");
  const std::string path = dir.Child("range.mscn");
  Write(path, "v1");
  // A copy set aside by an earlier run is stale once the file is replaced.
  REQUIRE(platform::WriteFileAtomic(KeptAsidePath(path), Bytes("old")).ok());
  const GeneratedWrite written = Write(path, "version 2");
  CHECK(written.written_path == path);
  CHECK_FALSE(written.kept_user_file);
  CHECK(ReadAll(path) == Bytes("version 2"));
  CHECK(ClassifyGeneratedFile(path) == GeneratedFileState::kUnedited);
  CHECK_FALSE(platform::PathExists(KeptAsidePath(path)));
}

TEST_CASE("An edited generated file is kept") {
  testing::ScratchDirectory dir("generated_edited");
  const std::string path = dir.Child("range.mscn");
  Write(path, "v1");
  const std::vector<std::byte> fingerprint = ReadAll(FingerprintPath(path));
  // Same size, different bytes: the user saved the scene from the editor.
  REQUIRE(platform::WriteFileAtomic(path, Bytes("v9")).ok());
  CHECK(ClassifyGeneratedFile(path) == GeneratedFileState::kEdited);

  const GeneratedWrite written = Write(path, "v2");
  CHECK(written.kept_user_file);
  CHECK(written.written_path == KeptAsidePath(path));
  CHECK(ReadAll(path) == Bytes("v9"));
  CHECK(ReadAll(KeptAsidePath(path)) == Bytes("v2"));
  CHECK(ReadAll(FingerprintPath(path)) == fingerprint);
  // And again on the next run: still kept.
  CHECK(Write(path, "v3").kept_user_file);
  CHECK(ReadAll(path) == Bytes("v9"));

  // Deleting the kept file lets it be generated again.
  REQUIRE(platform::RemovePath(path));
  CHECK_FALSE(Write(path, "v4").kept_user_file);
  CHECK(ReadAll(path) == Bytes("v4"));
}

TEST_CASE("An existing file without a sidecar counts as edited") {
  testing::ScratchDirectory dir("generated_no_sidecar");
  const std::string path = dir.Child("range.mscn");
  // Written by an older cook (no sidecar), possibly edited since.
  REQUIRE(platform::WriteFileAtomic(path, Bytes("v1")).ok());
  CHECK(ClassifyGeneratedFile(path) == GeneratedFileState::kEdited);
  const GeneratedWrite written = Write(path, "v1");
  CHECK(written.kept_user_file);
  CHECK(ReadAll(path) == Bytes("v1"));
  CHECK_FALSE(platform::PathExists(FingerprintPath(path)));

  // A corrupt sidecar too.
  REQUIRE(platform::WriteFileAtomic(FingerprintPath(path), Bytes("junk")).ok());
  CHECK(ClassifyGeneratedFile(path) == GeneratedFileState::kEdited);
}

}  // namespace
}  // namespace blue::cook
