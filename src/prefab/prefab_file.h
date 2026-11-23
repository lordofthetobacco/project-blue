#ifndef BLUE_PREFAB_PREFAB_FILE_H_
#define BLUE_PREFAB_PREFAB_FILE_H_

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"
#include "prefab/prefab.h"

// Reading and writing .pbo prefab files.
//
// File layout (little-endian, all offsets from the start of the file):
//
//   header   32 bytes: magic "BLUEPBO\0", u32 version, u32 file size,
//            u32 chunk count, u32 vertex stride (sizeof(asset::Vertex)),
//            u32 flags (0), u32 CRC-32 of the header's first 28 bytes
//            followed by the table of contents.
//   TOC      chunk count entries of { u32 fourcc, u32 offset, u32 size,
//            u32 CRC-32 of the chunk body }, sorted by offset.
//   chunks   each starting at a multiple of 16, zero padding between.
//
// Metadata chunks (parsed into vectors and strings, each at most once):
//   'INFO'  prefab bounds (always written).
//   'NODE'  hierarchy: count, per node name, parent, rest transform.
//   'MESH'  meshes: name, node, bounds, vertex and index ranges in 'BULK',
//           submeshes.
//   'MATL'  materials: scalar parameters, texture index and UV set per slot.
//   'TEXR'  textures: name, format, size, then one 'BULK' range per mip.
//   'CLIP'  clips: name, duration, loop, channels, events.
//   'SOCK'  sockets.  'LGHT'  point lights.  'CHLD'  child prefabs.
//   'PHYS'  physics body; its cooked shape is a 'BULK' range.
//   'EDIT'  source model, import settings, texture sources.
//   'TAGS'  tags: count, per tag key and value. Written only when the prefab
//           has tags; added without a version bump (an older reader skips
//           it, a file without it loads with no tags).
// Bulk chunk:
//   'BULK'  vertex arrays, index arrays, texture mips and cooked shapes, each
//           at a multiple of 16 from the file start, referenced by
//           { u32 offset, u32 size } relative to the chunk start. Loaded
//           prefabs view it in place.
//
// Checksums: the header CRC covers the header and TOC; each chunk has its own
// CRC. LoadPrefab() verifies every metadata chunk and, by default, not 'BULK':
// checksumming tens of megabytes would dominate the load (see LoadOptions),
// and corrupt bulk bytes cannot make the loader misbehave: all bulk ranges
// are bounds-checked and every index is checked against its vertex count.
// The cooked shape is checksummed (verify_bulk) before physics restores it if
// the caller wants that guarantee.
//
// Versioning: kPrefabFormatVersion is bumped when the layout of an existing
// chunk or the meaning of a field changes; readers reject newer versions.
// New chunks do not bump it: unknown chunks are skipped. A vertex stride that
// differs from sizeof(asset::Vertex) is reported as kUnsupportedVersion (the
// file was cooked for another vertex layout and must be re-cooked).
//
// Paths (child prefabs, edit info) are stored relative to the .pbo where
// possible and resolved against its directory on load. Output is
// deterministic: the same prefab saved to the same path gives the same bytes,
// and saving a loaded prefab to its own path reproduces the file.

namespace blue::prefab {

inline constexpr uint32_t kPrefabFormatVersion = 1;

// Files above this size are rejected before reading (offsets are 32-bit).
inline constexpr size_t kMaxPrefabFileSize = size_t{1} << 31;

enum class PrefabError {
  kCannotOpen,          // The file cannot be opened or read.
  kWriteFailed,         // The file could not be written.
  kBadMagic,            // The file is not a prefab file.
  kUnsupportedVersion,  // Newer version or another vertex layout.
  kTruncated,           // The file ends before its declared contents.
  kChecksumMismatch,    // A CRC-32 does not match.
  kCorrupt,             // The contents are malformed or out of range.
  kInvalidPrefab,       // The prefab cannot be saved (ValidatePrefab()).
};

// Returns a short human-readable name for `error`, for log messages.
[[nodiscard]] const char* ToString(PrefabError error);

struct LoadOptions {
  // Also verifies the CRC of the 'BULK' chunk (about 40 ms for 40 MB in
  // Release). Off by default; blue-pack info turns it on.
  bool verify_bulk_checksum = false;
};

// Returns the .pbo bytes for `prefab`, with paths stored relative to
// `base_dir` (the directory the file will live in). Returns kInvalidPrefab
// if ValidatePrefab() fails or the file would exceed kMaxPrefabFileSize.
[[nodiscard]] core::Result<std::vector<std::byte>, PrefabError> EncodePrefab(
    const Prefab& prefab, std::string_view base_dir);

// Parses .pbo bytes, resolving stored paths against `base_dir`. Takes the
// bytes over: the returned prefab's spans view them (one storage block). Never
// reads out of bounds, whatever the input.
[[nodiscard]] core::Result<Prefab, PrefabError> DecodePrefab(
    std::vector<std::byte> file, std::string_view base_dir,
    const LoadOptions& options = {});

// Writes `prefab` to `path` (missing directories created, existing file
// replaced atomically: written to a temporary file, then renamed over it).
// Saving over a file that is loaded (mapped) is safe, also when `prefab`
// views that very file: the old mapping keeps the old contents. Returns
// kInvalidPrefab without touching the file system if the prefab fails
// validation, kWriteFailed on I/O failure.
[[nodiscard]] core::Result<void, PrefabError> SavePrefab(
    const Prefab& prefab, const std::string& path);

// Loads the .pbo at `path`: maps the file read-only (platform::MappedFile;
// falls back to reading it when mapping fails) and validates it; logs the
// time taken. The returned prefab's storage holds the mapping as its one
// block (BlockKind::kMapped), so bulk data costs no private memory and is
// only read from disk when used; validation reads the metadata chunks and
// mesh indices, not the texture data. The file stays mapped while any copy of
// the prefab lives; see platform/mapped_file.h for what may happen to the
// file meanwhile (replace by rename and delete are safe). Returns kCannotOpen
// if the file cannot be opened or exceeds kMaxPrefabFileSize, and the
// matching error for malformed contents.
[[nodiscard]] core::Result<Prefab, PrefabError> LoadPrefab(
    const std::string& path, const LoadOptions& options = {});

}  // namespace blue::prefab

#endif  // BLUE_PREFAB_PREFAB_FILE_H_
