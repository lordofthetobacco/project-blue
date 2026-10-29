#ifndef BLUE_SCENE_SCENE_SERIALIZER_H_
#define BLUE_SCENE_SCENE_SERIALIZER_H_

#include <cstdint>
#include <string>

#include "core/result.h"
#include "scene/scene.h"

namespace blue::scene {

enum class SceneIoError {
  kCannotOpen,          // The file cannot be opened or read.
  kWriteFailed,         // The file could not be written.
  kBadMagic,            // The file is not a scene file.
  kUnsupportedVersion,  // The file was written by a newer version.
  kTruncated,           // The file ends before its declared contents.
  kChecksumMismatch,    // The payload does not match its CRC-32.
  kCorrupt,             // The contents are malformed or out of range.
  kInvalidScene,        // The scene cannot be serialized.
};

// Returns a short human-readable name for `error`, for log messages.
[[nodiscard]] const char* ToString(SceneIoError error);

// File layout: magic "BLUESCN\0", u32 version, u32 payload size, u32 CRC-32 of
// the payload, then the payload: a sequence of chunks (u32 fourcc, u32 size,
// body). Unknown chunks are skipped, so the version stays 1 when chunks are
// added. Chunks:
//   'ENVR'  camera, light and ambient (required, once).
//   'ENTS'  u32 count, then per entity: name, mesh path, vec3 position, quat,
//           vec3 scale (required, once).
//   'PHYS'  optional, once: vec3 gravity, u32 count, then count records of
//           { u32 entity_index, u32 motion, u32 shape, f32 mass, f32 friction,
//           f32 restitution } sorted by entity index. Written only if the
//           gravity differs from the default or any entity has a body; a file
//           without it loads with no bodies and the default gravity. May
//           precede ENTS. motion and shape hold the MotionType / ShapeType
//           enumerator values.
inline constexpr uint32_t kSceneFormatVersion = 1;

// Writes `scene` to the file at `path`, creating missing directories and
// replacing an existing file atomically. Mesh paths are stored relative to the
// scene file so a scene folder can be moved. Returns kInvalidScene, without
// touching the file system, if the scene holds an oversized string or a
// non-finite value, an invalid rigid body (see ValidateRigidBody()) or a
// non-finite gravity, and kWriteFailed if the file cannot be written.
[[nodiscard]] core::Result<void, SceneIoError> SaveScene(
    const Scene& scene, const std::string& path);

// Loads the scene file at `path`. Mesh paths are resolved against the scene
// file's directory and returned in generic ('/') form. Returns kCannotOpen if
// the file cannot be read, kCorrupt if it exceeds the size limit, and the
// matching error for any malformed contents, including physics records that
// refer to missing entities or fail ValidateRigidBody(); hostile sizes are
// rejected before they can drive an allocation.
[[nodiscard]] core::Result<Scene, SceneIoError> LoadScene(
    const std::string& path);

}  // namespace blue::scene

#endif  // BLUE_SCENE_SCENE_SERIALIZER_H_
