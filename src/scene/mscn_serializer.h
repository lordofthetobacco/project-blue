#ifndef BLUE_SCENE_MSCN_SERIALIZER_H_
#define BLUE_SCENE_MSCN_SERIALIZER_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/result.h"
#include "scene/scene_data.h"

namespace blue::scene {

enum class MscnError {
  kCannotOpen,          // The file cannot be opened or read.
  kWriteFailed,         // The file could not be written.
  kBadMagic,            // The file is not a .mscn file.
  kUnsupportedVersion,  // The file was written by a newer version.
  kTruncated,           // The file ends before its declared contents.
  kChecksumMismatch,    // The payload does not match its CRC-32.
  kCorrupt,             // The contents are malformed or out of range.
  kInvalidScene,        // The scene cannot be serialized.
};

// Returns a short human-readable name for `error`, for log messages.
[[nodiscard]] const char* ToString(MscnError error);

// File layout: magic "BLUEMSCN", u32 version, u32 payload size, u32 CRC-32 of
// the payload, then the payload: a sequence of chunks (u32 fourcc, u32 size,
// body). All values are little-endian; strings are a u32 byte length and the
// bytes. Unknown chunks are skipped, so adding a chunk keeps the version;
// changing the layout of an existing chunk bumps it (older readers then
// report kUnsupportedVersion). The writer emits every chunk once, in the
// order below, so equal scenes give equal bytes. Chunks:
//   'INFO'  required: u32 next_instance_id, vec3 gravity, vec3 player start
//           position, f32 yaw, f32 pitch.
//   'PFBS'  required: u32 count, then count prefab paths (relative to the
//           scene file where possible, '/' separators).
//   'INST'  required: u32 count, then per instance: u32 id, string name, u32
//           prefab index (0xFFFFFFFF = none), u32 parent id (0 = root), vec3
//           position, quat (x, y, z, w), vec3 scale, u32 flags (bit 0
//           visible, bit 1 has physics override), u32 material override
//           count, the material overrides, then the physics override if
//           flagged.
//           Material override: string material_name, u32 source prefab index
//           (0xFFFFFFFF = none), string source_material, u32 field mask
//           (bit 0 base_color vec4, 1 metallic f32, 2 roughness f32,
//           3 emissive vec3, 4 emissive_strength f32, 5 uv_scale vec2), then
//           the present fields in bit order.
//           Physics override: u32 field mask (bit 0 disabled, 1 motion,
//           2 mass, 3 friction, 4 restitution), then u32 motion, f32 mass,
//           f32 friction, f32 restitution for the present fields in order.
//   'SUN '  optional (default sun if absent): vec3 direction, vec3 color,
//           f32 intensity, u32 flags (bit 0 casts shadows). A direction
//           that is not unit length is normalized on load.
//   'PLIT'  optional (none if absent): u32 count, then per light: string
//           name, vec3 position, vec3 color, f32 intensity, f32 range.
//   'ENVI'  optional (default if absent): string hdri path (stored like
//           prefab paths; empty = default sky), f32 intensity, f32
//           rotation_radians, f32 exposure_ev.
inline constexpr uint32_t kMscnFormatVersion = 1;

// Returns a readable reason why `scene` cannot be saved to `path`, or
// std::nullopt if SaveMscn() would accept it: ValidateSceneData() plus the
// on-disk limits (string lengths after making paths relative). Touches no
// file.
[[nodiscard]] std::optional<std::string> CheckMscnSerializable(
    const SceneData& scene, const std::string& path);

// Writes `scene` to `path`, creating missing directories and replacing an
// existing file atomically. Prefab and HDRI paths are stored relative to the
// scene file's directory where possible. Returns kInvalidScene without
// touching the file system if CheckMscnSerializable() fails, kWriteFailed if
// the file cannot be written.
[[nodiscard]] core::Result<void, MscnError> SaveMscn(const SceneData& scene,
                                                     const std::string& path);

// Returns the bytes SaveMscn() would write for `scene` at `scene_path` (paths
// are made relative to its directory; nothing is read or written), or
// kInvalidScene if CheckMscnSerializable() fails. Does not log; meant also
// for in-memory snapshots such as undo history.
[[nodiscard]] core::Result<std::vector<std::byte>, MscnError> EncodeMscn(
    const SceneData& scene, const std::string& scene_path);

// Decodes .mscn `bytes` as if they were read from `scene_path` (relative
// paths resolve against its directory). Same checks as LoadMscn(), but no
// file access and no logging. `bytes` need not outlive the call.
[[nodiscard]] core::Result<SceneData, MscnError> DecodeMscn(
    std::span<const std::byte> bytes, const std::string& scene_path);

// Loads the .mscn file at `path`. Paths are resolved against the scene file's
// directory and returned normalized in '/' form. Any malformed content,
// including everything ValidateSceneData() rejects, is an error; hostile
// sizes are rejected before they can drive an allocation. Logs the load time.
[[nodiscard]] core::Result<SceneData, MscnError> LoadMscn(
    const std::string& path);

}  // namespace blue::scene

#endif  // BLUE_SCENE_MSCN_SERIALIZER_H_
