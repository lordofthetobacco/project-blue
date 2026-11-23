#include "cook/generated_file.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/path.h"
#include "platform/file_system.h"
#include "scene/binary_stream.h"

namespace blue::cook {
namespace {

using WriteResult = core::Result<GeneratedWrite, std::string>;

constexpr std::string_view kFingerprintSuffix = ".fingerprint";
constexpr std::string_view kKeptAsideSuffix = ".generated";

// Generated files are scenes and similar small files; anything bigger is
// not compared (and so treated as edited).
constexpr size_t kMaxComparedBytes = size_t{256} * 1024 * 1024;
// A sidecar is one short line.
constexpr size_t kMaxFingerprintBytes = 256;

std::span<const std::byte> AsBytes(const std::string& text) {
  return std::as_bytes(std::span(text.data(), text.size()));
}

}  // namespace

std::string FingerprintPath(const std::string& path) {
  return path + std::string(kFingerprintSuffix);
}

std::string KeptAsidePath(const std::string& path) {
  const std::string name = core::FileName(path);
  const size_t dot = name.rfind('.');
  // A leading dot ("."-files) is part of the stem, not an extension.
  const size_t stem_end =
      dot == std::string::npos || dot == 0 ? name.size() : dot;
  const std::string renamed = name.substr(0, stem_end) +
                              std::string(kKeptAsideSuffix) +
                              name.substr(stem_end);
  return path.substr(0, path.size() - name.size()) + renamed;
}

std::string Fingerprint(std::span<const std::byte> bytes) {
  return std::format("size {} crc32 {:08x}\n", bytes.size(),
                     scene::Crc32(bytes));
}

GeneratedFileState ClassifyGeneratedFile(const std::string& path) {
  if (!platform::PathExists(path)) {
    return GeneratedFileState::kAbsent;
  }
  const auto recorded =
      platform::ReadFile(FingerprintPath(path), kMaxFingerprintBytes);
  if (!recorded.ok()) {
    return GeneratedFileState::kEdited;
  }
  const auto contents = platform::ReadFile(path, kMaxComparedBytes);
  if (!contents.ok()) {
    return GeneratedFileState::kEdited;
  }
  const std::string expected = Fingerprint(contents.value());
  const std::vector<std::byte>& actual = recorded.value();
  const bool same = std::ranges::equal(actual, AsBytes(expected));
  return same ? GeneratedFileState::kUnedited : GeneratedFileState::kEdited;
}

WriteResult WriteGeneratedFile(const std::string& path,
                               std::span<const std::byte> bytes) {
  if (ClassifyGeneratedFile(path) == GeneratedFileState::kEdited) {
    const std::string aside = KeptAsidePath(path);
    if (!platform::WriteFileAtomic(aside, bytes).ok()) {
      return WriteResult::Error(std::format("cannot write {}", aside));
    }
    return WriteResult::Ok({.written_path = aside, .kept_user_file = true});
  }
  if (!platform::WriteFileAtomic(path, bytes).ok()) {
    return WriteResult::Error(std::format("cannot write {}", path));
  }
  const std::string fingerprint = Fingerprint(bytes);
  if (!platform::WriteFileAtomic(FingerprintPath(path), AsBytes(fingerprint))
           .ok()) {
    return WriteResult::Error(
        std::format("cannot write {}", FingerprintPath(path)));
  }
  // The file is generated again, so a copy set aside earlier is stale.
  platform::RemovePath(KeptAsidePath(path));
  return WriteResult::Ok({.written_path = path, .kept_user_file = false});
}

}  // namespace blue::cook
