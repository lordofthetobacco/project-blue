#ifndef BLUE_COOK_GENERATED_FILE_H_
#define BLUE_COOK_GENERATED_FILE_H_

#include <cstddef>
#include <span>
#include <string>

#include "core/result.h"

// Writes a generated file (e.g. the sample cook's range.mscn) without
// destroying the user's edits to an earlier copy. Every write also records a
// fingerprint of exactly the bytes written in a sidecar, "<file>.fingerprint".
// On the next write the existing file is compared against that fingerprint:
//   - no file:                         written (with its sidecar);
//   - file matches its fingerprint:    unedited, replaced (with its sidecar);
//   - mismatch, no sidecar or the file cannot be read: treated as edited by
//     the user and kept; the fresh contents go to the kept-aside path
//     ("range.mscn" -> "range.generated.mscn") in the same directory, so
//     paths stored relative to the directory stay valid there.
// Deleting a kept file (or its sidecar and the file) lets the next write
// regenerate it.

namespace blue::cook {

enum class GeneratedFileState {
  kAbsent,    // No file at the path.
  kUnedited,  // The file holds exactly the bytes its sidecar records.
  kEdited,    // Anything else: the file must be kept.
};

// "<path>.fingerprint".
[[nodiscard]] std::string FingerprintPath(const std::string& path);

// Where fresh contents go while the file at `path` is kept: the stem gets a
// ".generated" suffix before the extension ("a/range.mscn" ->
// "a/range.generated.mscn"; "a/notes" -> "a/notes.generated").
[[nodiscard]] std::string KeptAsidePath(const std::string& path);

// The sidecar text recorded for `bytes`: their size and CRC-32, one line.
[[nodiscard]] std::string Fingerprint(std::span<const std::byte> bytes);

// Classifies the file at `path` against its sidecar (see above).
[[nodiscard]] GeneratedFileState ClassifyGeneratedFile(const std::string& path);

struct GeneratedWrite {
  std::string written_path;  // `path`, or KeptAsidePath(path) if kept.
  bool kept_user_file = false;
};

// Writes `bytes` to `path` with its sidecar unless the file there is edited
// (see ClassifyGeneratedFile()); then writes them to KeptAsidePath(path)
// instead and leaves `path` and its sidecar untouched. After a write to
// `path` a stale kept-aside file is removed. Files are replaced atomically;
// the file is written before its sidecar, so an interrupted write reads as
// edited next time. Returns an error message if a write fails.
[[nodiscard]] core::Result<GeneratedWrite, std::string> WriteGeneratedFile(
    const std::string& path, std::span<const std::byte> bytes);

}  // namespace blue::cook

#endif  // BLUE_COOK_GENERATED_FILE_H_
