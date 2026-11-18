#include "scene/scene_serializer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <glm/geometric.hpp>

#include "core/path.h"
#include "platform/file_system.h"
#include "scene/binary_stream.h"

namespace blue::scene {
namespace {

constexpr std::array<char, 8> kMagic = {'B', 'L', 'U', 'E',
                                        'S', 'C', 'N', '\0'};
constexpr size_t kHeaderSize = kMagic.size() + 3 * sizeof(uint32_t);
constexpr size_t kChunkHeaderSize = 2 * sizeof(uint32_t);
constexpr uint32_t kMaxStringLength = 4096;
constexpr size_t kMaxSceneFileSize = 64 * 1024 * 1024;

constexpr uint32_t kEnvironmentChunk = MakeFourcc('E', 'N', 'V', 'R');
constexpr uint32_t kEntitiesChunk = MakeFourcc('E', 'N', 'T', 'S');
constexpr uint32_t kPhysicsChunk = MakeFourcc('P', 'H', 'Y', 'S');
constexpr uint32_t kSunChunk = MakeFourcc('S', 'U', 'N', ' ');
constexpr uint32_t kMaterialsChunk = MakeFourcc('M', 'A', 'T', 'S');
constexpr uint32_t kImageLightingChunk = MakeFourcc('I', 'B', 'L', ' ');

// SUN chunk size: vec3 direction, vec3 color, f32 intensity, u32 flags.
constexpr size_t kSunChunkBytes = 8 * sizeof(uint32_t);
constexpr uint32_t kSunCastsShadowsFlag = 1U << 0;

// Two empty strings, vec3 position, quat, vec3 scale: the smallest possible
// entity record.
constexpr size_t kMinEntityBytes =
    2 * sizeof(uint32_t) + (3 + 4 + 3) * sizeof(float);

// u32 entity index, u32 motion, u32 shape, f32 mass, f32 friction,
// f32 restitution.
constexpr size_t kPhysicsRecordBytes = 6 * sizeof(uint32_t);

// u32 entity index, then the smallest valid string (u32 length, one byte).
constexpr size_t kMinMaterialRecordBytes = 2 * sizeof(uint32_t) + 1;

// Highest valid enumerator values on disk.
constexpr uint32_t kMaxMotionType = static_cast<uint32_t>(MotionType::kDynamic);
constexpr uint32_t kMaxShapeType =
    static_cast<uint32_t>(ShapeType::kTriangleMesh);

using Status = core::Result<void, SceneIoError>;
using SceneResult = core::Result<Scene, SceneIoError>;

bool IsBuiltinMesh(const std::string& mesh_path) {
  return mesh_path.starts_with(kBuiltinMeshPrefix);
}

// Returns the absolute directory that contains the scene file `scene_path`.
std::string SceneDirectory(const std::string& scene_path) {
  return core::ParentPath(platform::AbsolutePath(scene_path));
}

// Returns the form of `mesh_path` that is written to disk: relative to
// `scene_dir` where possible, built-in names unchanged.
std::string StoredMeshPath(const std::string& mesh_path,
                           const std::string& scene_dir) {
  if (IsBuiltinMesh(mesh_path)) {
    return mesh_path;
  }
  const std::string absolute = platform::AbsolutePath(mesh_path);
  // No relative form exists e.g. across drives; the absolute path is stored
  // then.
  return core::RelativePath(absolute, scene_dir).value_or(absolute);
}

// Returns the in-memory mesh path for a `stored` one: the inverse of
// StoredMeshPath().
std::string ResolvedMeshPath(const std::string& stored,
                             const std::string& scene_dir) {
  if (IsBuiltinMesh(stored)) {
    return stored;
  }
  return core::JoinPath(scene_dir, stored);
}

// Like StoredMeshPath() for a file that is not a mesh; empty stays empty.
std::string StoredFilePath(const std::string& path,
                           const std::string& scene_dir) {
  return path.empty() ? std::string() : StoredMeshPath(path, scene_dir);
}

// The inverse of StoredFilePath().
std::string ResolvedFilePath(const std::string& stored,
                             const std::string& scene_dir) {
  return stored.empty() ? std::string() : core::JoinPath(scene_dir, stored);
}

void WriteVec3(BinaryWriter& writer, const glm::vec3& v) {
  writer.WriteF32(v.x);
  writer.WriteF32(v.y);
  writer.WriteF32(v.z);
}

void WriteQuat(BinaryWriter& writer, const glm::quat& q) {
  writer.WriteF32(q.x);
  writer.WriteF32(q.y);
  writer.WriteF32(q.z);
  writer.WriteF32(q.w);
}

glm::vec3 ReadVec3(BinaryReader& reader) {
  const float x = reader.ReadF32();
  const float y = reader.ReadF32();
  const float z = reader.ReadF32();
  return {x, y, z};
}

glm::quat ReadQuat(BinaryReader& reader) {
  const float x = reader.ReadF32();
  const float y = reader.ReadF32();
  const float z = reader.ReadF32();
  const float w = reader.ReadF32();
  return {w, x, y, z};
}

bool IsFinite(const glm::vec3& v) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool IsFinite(const glm::quat& q) {
  return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) &&
         std::isfinite(q.w);
}

// Maps the reader's sticky status to the scene error model.
Status ToResult(ReadStatus status) {
  switch (status) {
    case ReadStatus::kOk:
      return Status::Ok();
    case ReadStatus::kTruncated:
      return Status::Error(SceneIoError::kTruncated);
    case ReadStatus::kLimitExceeded:
      break;
  }
  return Status::Error(SceneIoError::kCorrupt);
}

bool IsEnvironmentFinite(const Scene& scene) {
  return IsFinite(scene.camera.position) && IsFinite(scene.camera.target) &&
         std::isfinite(scene.camera.vertical_fov_degrees) &&
         IsFinite(scene.light.position) && IsFinite(scene.light.color) &&
         std::isfinite(scene.light.intensity) && IsFinite(scene.ambient);
}

bool HasBody(const Entity& entity) { return entity.body.has_value(); }

// Returns whether the PHYS chunk carries information: a non-default gravity or
// at least one body. Scenes without either are written without the chunk, so
// they stay byte-identical to files from before physics existed.
bool HasPhysicsData(const Scene& scene) {
  return scene.gravity != Scene().gravity ||
         std::ranges::any_of(scene.entities, HasBody);
}

// Returns whether the SUN chunk carries information: any field that differs
// from the default sun. Scenes with the default sun are written without the
// chunk, so they stay byte-identical to files from before the sun existed.
bool HasSunData(const SunLight& sun) {
  const SunLight defaults;
  return sun.direction != defaults.direction || sun.color != defaults.color ||
         sun.intensity != defaults.intensity ||
         sun.casts_shadows != defaults.casts_shadows;
}

bool HasMaterial(const Entity& entity) { return !entity.material_path.empty(); }

// Returns whether the IBL chunk carries information: any field that differs
// from the default environment. Scenes without it stay byte-identical to
// older files.
bool HasImageLightingData(const Environment& environment) {
  const Environment defaults;
  return environment.hdri_path != defaults.hdri_path ||
         environment.intensity != defaults.intensity ||
         environment.rotation_radians != defaults.rotation_radians ||
         environment.exposure_ev != defaults.exposure_ev;
}

// Rejects scenes the loader would refuse, so a save never produces an
// unreadable file.
bool IsSerializable(const Scene& scene, const std::string& scene_dir) {
  if (!IsEnvironmentFinite(scene) || !IsFinite(scene.gravity) ||
      ValidateSunLight(scene.sun).has_value() ||
      ValidateEnvironment(scene.environment).has_value() ||
      StoredFilePath(scene.environment.hdri_path, scene_dir).size() >
          kMaxStringLength) {
    return false;
  }
  return std::ranges::all_of(scene.entities, [&](const Entity& entity) {
    return entity.name.size() <= kMaxStringLength &&
           (!entity.body.has_value() ||
            !ValidateRigidBody(*entity.body).has_value()) &&
           StoredMeshPath(entity.mesh_path, scene_dir).size() <=
               kMaxStringLength &&
           StoredFilePath(entity.material_path, scene_dir).size() <=
               kMaxStringLength &&
           IsFinite(entity.transform.position) &&
           IsFinite(entity.transform.rotation) &&
           IsFinite(entity.transform.scale);
  });
}

// Appends the PHYS chunk: gravity, then one record per entity with a body, in
// entity order.
void EncodePhysics(const Scene& scene, BinaryWriter& writer) {
  const size_t chunk = writer.BeginChunk(kPhysicsChunk);
  WriteVec3(writer, scene.gravity);
  writer.WriteU32(
      static_cast<uint32_t>(std::ranges::count_if(scene.entities, HasBody)));
  for (size_t i = 0; i < scene.entities.size(); ++i) {
    const std::optional<RigidBody>& body = scene.entities[i].body;
    if (!body.has_value()) {
      continue;
    }
    writer.WriteU32(static_cast<uint32_t>(i));
    writer.WriteU32(static_cast<uint32_t>(body->motion));
    writer.WriteU32(static_cast<uint32_t>(body->shape));
    writer.WriteF32(body->mass);
    writer.WriteF32(body->friction);
    writer.WriteF32(body->restitution);
  }
  writer.EndChunk(chunk);
}

// Appends the SUN chunk for `sun`.
void EncodeSun(const SunLight& sun, BinaryWriter& writer) {
  const size_t chunk = writer.BeginChunk(kSunChunk);
  WriteVec3(writer, sun.direction);
  WriteVec3(writer, sun.color);
  writer.WriteF32(sun.intensity);
  writer.WriteU32(sun.casts_shadows ? kSunCastsShadowsFlag : 0U);
  writer.EndChunk(chunk);
}

// Appends the MATS chunk: one record per entity with a material, in entity
// order.
void EncodeMaterials(const Scene& scene, const std::string& scene_dir,
                     BinaryWriter& writer) {
  const size_t chunk = writer.BeginChunk(kMaterialsChunk);
  writer.WriteU32(static_cast<uint32_t>(
      std::ranges::count_if(scene.entities, HasMaterial)));
  for (size_t i = 0; i < scene.entities.size(); ++i) {
    const Entity& entity = scene.entities[i];
    if (!HasMaterial(entity)) {
      continue;
    }
    writer.WriteU32(static_cast<uint32_t>(i));
    writer.WriteString(StoredFilePath(entity.material_path, scene_dir));
  }
  writer.EndChunk(chunk);
}

// Appends the IBL chunk for `environment`.
void EncodeImageLighting(const Environment& environment,
                         const std::string& scene_dir, BinaryWriter& writer) {
  const size_t chunk = writer.BeginChunk(kImageLightingChunk);
  writer.WriteString(StoredFilePath(environment.hdri_path, scene_dir));
  writer.WriteF32(environment.intensity);
  writer.WriteF32(environment.rotation_radians);
  writer.WriteF32(environment.exposure_ev);
  writer.EndChunk(chunk);
}

// Returns the chunked payload (environment, entities, then the optional
// physics, sun, materials and image-lighting chunks) for `scene`.
std::vector<std::byte> EncodePayload(const Scene& scene,
                                     const std::string& scene_dir) {
  BinaryWriter writer;

  const size_t environment = writer.BeginChunk(kEnvironmentChunk);
  WriteVec3(writer, scene.camera.position);
  WriteVec3(writer, scene.camera.target);
  writer.WriteF32(scene.camera.vertical_fov_degrees);
  WriteVec3(writer, scene.light.position);
  WriteVec3(writer, scene.light.color);
  writer.WriteF32(scene.light.intensity);
  WriteVec3(writer, scene.ambient);
  writer.EndChunk(environment);

  const size_t entities = writer.BeginChunk(kEntitiesChunk);
  writer.WriteU32(static_cast<uint32_t>(scene.entities.size()));
  for (const Entity& entity : scene.entities) {
    writer.WriteString(entity.name);
    writer.WriteString(StoredMeshPath(entity.mesh_path, scene_dir));
    WriteVec3(writer, entity.transform.position);
    WriteQuat(writer, entity.transform.rotation);
    WriteVec3(writer, entity.transform.scale);
  }
  writer.EndChunk(entities);

  if (HasPhysicsData(scene)) {
    EncodePhysics(scene, writer);
  }
  if (HasSunData(scene.sun)) {
    EncodeSun(scene.sun, writer);
  }
  if (std::ranges::any_of(scene.entities, HasMaterial)) {
    EncodeMaterials(scene, scene_dir, writer);
  }
  if (HasImageLightingData(scene.environment)) {
    EncodeImageLighting(scene.environment, scene_dir, writer);
  }
  return writer.bytes();
}

// Decodes the environment chunk `body` into the camera, light and ambient
// fields of `scene`.
Status DecodeEnvironment(std::span<const std::byte> body, Scene& scene) {
  BinaryReader reader(body);
  scene.camera.position = ReadVec3(reader);
  scene.camera.target = ReadVec3(reader);
  scene.camera.vertical_fov_degrees = reader.ReadF32();
  scene.light.position = ReadVec3(reader);
  scene.light.color = ReadVec3(reader);
  scene.light.intensity = reader.ReadF32();
  scene.ambient = ReadVec3(reader);
  if (auto status = ToResult(reader.status()); !status.ok()) {
    return status;
  }

  if (!IsEnvironmentFinite(scene)) {
    return Status::Error(SceneIoError::kCorrupt);
  }
  return Status::Ok();
}

// Decodes the entities chunk `body` and appends the entities to `scene`.
Status DecodeEntities(std::span<const std::byte> body,
                      const std::string& scene_dir, Scene& scene) {
  BinaryReader reader(body);
  const uint32_t count = reader.ReadU32();
  if (auto status = ToResult(reader.status()); !status.ok()) {
    return status;
  }
  // Validate before reserving so a hostile count cannot drive an allocation.
  if (count > reader.remaining() / kMinEntityBytes) {
    return Status::Error(SceneIoError::kCorrupt);
  }

  scene.entities.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    Entity entity;
    entity.name = reader.ReadString(kMaxStringLength);
    entity.mesh_path =
        ResolvedMeshPath(reader.ReadString(kMaxStringLength), scene_dir);
    entity.transform.position = ReadVec3(reader);
    entity.transform.rotation = ReadQuat(reader);
    entity.transform.scale = ReadVec3(reader);
    if (auto status = ToResult(reader.status()); !status.ok()) {
      return status;
    }
    if (!IsFinite(entity.transform.position) ||
        !IsFinite(entity.transform.rotation) ||
        !IsFinite(entity.transform.scale)) {
      return Status::Error(SceneIoError::kCorrupt);
    }
    scene.entities.push_back(std::move(entity));
  }
  return Status::Ok();
}

// Decodes the physics chunk `body`: sets the gravity of `scene` and attaches a
// body to each listed entity. Must run after the entities are decoded because
// records refer to entities by index.
Status DecodePhysics(std::span<const std::byte> body, Scene& scene) {
  BinaryReader reader(body);
  scene.gravity = ReadVec3(reader);
  const uint32_t count = reader.ReadU32();
  if (auto status = ToResult(reader.status()); !status.ok()) {
    return status;
  }
  // Checked before any record is read so a hostile count cannot drive work.
  if (!IsFinite(scene.gravity) ||
      reader.remaining() / kPhysicsRecordBytes != count ||
      reader.remaining() % kPhysicsRecordBytes != 0) {
    return Status::Error(SceneIoError::kCorrupt);
  }

  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t index = reader.ReadU32();
    const uint32_t motion = reader.ReadU32();
    const uint32_t shape = reader.ReadU32();
    RigidBody rigid_body;
    rigid_body.mass = reader.ReadF32();
    rigid_body.friction = reader.ReadF32();
    rigid_body.restitution = reader.ReadF32();
    if (auto status = ToResult(reader.status()); !status.ok()) {
      return status;
    }
    if (index >= scene.entities.size() || motion > kMaxMotionType ||
        shape > kMaxShapeType || scene.entities[index].body.has_value()) {
      return Status::Error(SceneIoError::kCorrupt);
    }
    rigid_body.motion = static_cast<MotionType>(motion);
    rigid_body.shape = static_cast<ShapeType>(shape);
    if (ValidateRigidBody(rigid_body).has_value()) {
      return Status::Error(SceneIoError::kCorrupt);
    }
    scene.entities[index].body = rigid_body;
  }
  return Status::Ok();
}

// Decodes the SUN chunk `body` into `scene.sun`, normalizing the direction.
Status DecodeSun(std::span<const std::byte> body, Scene& scene) {
  if (body.size() != kSunChunkBytes) {
    return Status::Error(SceneIoError::kCorrupt);
  }
  BinaryReader reader(body);
  SunLight sun;
  sun.direction = ReadVec3(reader);
  sun.color = ReadVec3(reader);
  sun.intensity = reader.ReadF32();
  const uint32_t flags = reader.ReadU32();
  if (auto status = ToResult(reader.status()); !status.ok()) {
    return status;
  }
  if ((flags & ~kSunCastsShadowsFlag) != 0 ||
      ValidateSunLight(sun).has_value()) {
    return Status::Error(SceneIoError::kCorrupt);
  }
  sun.casts_shadows = (flags & kSunCastsShadowsFlag) != 0;
  sun.direction = glm::normalize(sun.direction);
  scene.sun = sun;
  return Status::Ok();
}

// Decodes the MATS chunk `body`: assigns a material file to each listed
// entity. Must run after the entities are decoded.
Status DecodeMaterials(std::span<const std::byte> body,
                       const std::string& scene_dir, Scene& scene) {
  BinaryReader reader(body);
  const uint32_t count = reader.ReadU32();
  if (auto status = ToResult(reader.status()); !status.ok()) {
    return status;
  }
  // Checked before any record is read so a hostile count cannot drive work.
  if (count > reader.remaining() / kMinMaterialRecordBytes) {
    return Status::Error(SceneIoError::kCorrupt);
  }
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t index = reader.ReadU32();
    const std::string stored = reader.ReadString(kMaxStringLength);
    if (auto status = ToResult(reader.status()); !status.ok()) {
      return status;
    }
    if (index >= scene.entities.size() || stored.empty() ||
        HasMaterial(scene.entities[index])) {
      return Status::Error(SceneIoError::kCorrupt);
    }
    scene.entities[index].material_path = ResolvedFilePath(stored, scene_dir);
  }
  if (reader.remaining() != 0) {
    return Status::Error(SceneIoError::kCorrupt);
  }
  return Status::Ok();
}

// Decodes the IBL chunk `body` into `scene.environment`.
Status DecodeImageLighting(std::span<const std::byte> body,
                           const std::string& scene_dir, Scene& scene) {
  BinaryReader reader(body);
  Environment environment;
  environment.hdri_path =
      ResolvedFilePath(reader.ReadString(kMaxStringLength), scene_dir);
  environment.intensity = reader.ReadF32();
  environment.rotation_radians = reader.ReadF32();
  environment.exposure_ev = reader.ReadF32();
  if (auto status = ToResult(reader.status()); !status.ok()) {
    return status;
  }
  if (reader.remaining() != 0 || ValidateEnvironment(environment).has_value()) {
    return Status::Error(SceneIoError::kCorrupt);
  }
  scene.environment = std::move(environment);
  return Status::Ok();
}

// Validates the header and checksum of the scene file `file` and decodes its
// chunks. Unknown chunks are skipped.
SceneResult DecodeFile(std::span<const std::byte> file,
                       const std::string& scene_dir) {
  if (file.size() < kHeaderSize) {
    return SceneResult::Error(SceneIoError::kTruncated);
  }

  BinaryReader header(file);
  const std::span<const std::byte> magic = header.ReadBytes(kMagic.size());
  if (!std::equal(
          magic.begin(), magic.end(), kMagic.begin(), [](std::byte a, char b) {
            return std::to_integer<int>(a) == static_cast<unsigned char>(b);
          })) {
    return SceneResult::Error(SceneIoError::kBadMagic);
  }
  const uint32_t version = header.ReadU32();
  const uint32_t payload_size = header.ReadU32();
  const uint32_t payload_crc = header.ReadU32();

  if (version == 0) {
    return SceneResult::Error(SceneIoError::kCorrupt);
  }
  if (version > kSceneFormatVersion) {
    return SceneResult::Error(SceneIoError::kUnsupportedVersion);
  }
  if (payload_size > header.remaining()) {
    return SceneResult::Error(SceneIoError::kTruncated);
  }
  const std::span<const std::byte> payload = header.ReadBytes(payload_size);
  if (Crc32(payload) != payload_crc) {
    return SceneResult::Error(SceneIoError::kChecksumMismatch);
  }

  Scene scene;
  bool has_environment = false;
  bool has_entities = false;
  bool has_sun = false;
  bool has_image_lighting = false;
  std::optional<std::span<const std::byte>> physics;
  std::optional<std::span<const std::byte>> materials;
  BinaryReader chunks(payload);
  while (chunks.remaining() > 0) {
    if (chunks.remaining() < kChunkHeaderSize) {
      return SceneResult::Error(SceneIoError::kTruncated);
    }
    const uint32_t fourcc = chunks.ReadU32();
    const uint32_t size = chunks.ReadU32();
    if (size > chunks.remaining()) {
      return SceneResult::Error(SceneIoError::kTruncated);
    }
    const std::span<const std::byte> body = chunks.ReadBytes(size);

    if (fourcc == kPhysicsChunk) {
      // Entity indices are only checkable once ENTS is known, whatever the
      // chunk order, so decoding is deferred.
      if (physics.has_value()) {
        return SceneResult::Error(SceneIoError::kCorrupt);
      }
      physics = body;
    } else if (fourcc == kMaterialsChunk) {
      // Like PHYS: records refer to entities by index.
      if (materials.has_value()) {
        return SceneResult::Error(SceneIoError::kCorrupt);
      }
      materials = body;
    } else if (fourcc == kImageLightingChunk) {
      if (has_image_lighting) {
        return SceneResult::Error(SceneIoError::kCorrupt);
      }
      has_image_lighting = true;
      if (auto decoded = DecodeImageLighting(body, scene_dir, scene);
          !decoded.ok()) {
        return SceneResult::Error(decoded.error());
      }
    } else if (fourcc == kSunChunk) {
      if (has_sun) {
        return SceneResult::Error(SceneIoError::kCorrupt);
      }
      has_sun = true;
      if (auto decoded = DecodeSun(body, scene); !decoded.ok()) {
        return SceneResult::Error(decoded.error());
      }
    } else if (fourcc == kEnvironmentChunk || fourcc == kEntitiesChunk) {
      bool& seen = fourcc == kEnvironmentChunk ? has_environment : has_entities;
      if (seen) {
        return SceneResult::Error(SceneIoError::kCorrupt);
      }
      seen = true;
      const auto decoded = fourcc == kEnvironmentChunk
                               ? DecodeEnvironment(body, scene)
                               : DecodeEntities(body, scene_dir, scene);
      if (!decoded.ok()) {
        return SceneResult::Error(decoded.error());
      }
    }
  }

  if (!has_environment || !has_entities) {
    return SceneResult::Error(SceneIoError::kCorrupt);
  }
  if (physics.has_value()) {
    if (auto decoded = DecodePhysics(*physics, scene); !decoded.ok()) {
      return SceneResult::Error(decoded.error());
    }
  }
  if (materials.has_value()) {
    if (auto decoded = DecodeMaterials(*materials, scene_dir, scene);
        !decoded.ok()) {
      return SceneResult::Error(decoded.error());
    }
  }
  return SceneResult::Ok(std::move(scene));
}

}  // namespace

const char* ToString(SceneIoError error) {
  switch (error) {
    case SceneIoError::kCannotOpen:
      return "cannot open file";
    case SceneIoError::kWriteFailed:
      return "write failed";
    case SceneIoError::kBadMagic:
      return "not a scene file (bad magic)";
    case SceneIoError::kUnsupportedVersion:
      return "unsupported scene version";
    case SceneIoError::kTruncated:
      return "file truncated";
    case SceneIoError::kChecksumMismatch:
      return "checksum mismatch";
    case SceneIoError::kCorrupt:
      return "corrupt scene data";
    case SceneIoError::kInvalidScene:
      return "scene cannot be serialized (oversized string, non-finite "
             "value, invalid rigid body, sun or environment)";
  }
  return "unknown error";
}

core::Result<void, SceneIoError> SaveScene(const Scene& scene,
                                           const std::string& path) {
  const std::string scene_dir = SceneDirectory(path);
  if (!IsSerializable(scene, scene_dir)) {
    return Status::Error(SceneIoError::kInvalidScene);
  }
  const std::vector<std::byte> payload = EncodePayload(scene, scene_dir);

  BinaryWriter file;
  file.WriteBytes(std::as_bytes(std::span(kMagic)));
  file.WriteU32(kSceneFormatVersion);
  file.WriteU32(static_cast<uint32_t>(payload.size()));
  file.WriteU32(Crc32(payload));
  file.WriteBytes(payload);

  // platform::FileError does not distinguish "cannot open" from "write
  // failed" on the write side.
  if (!platform::WriteFileAtomic(path, file.bytes()).ok()) {
    return Status::Error(SceneIoError::kWriteFailed);
  }
  return Status::Ok();
}

core::Result<Scene, SceneIoError> LoadScene(const std::string& path) {
  const auto bytes = platform::ReadFile(path, kMaxSceneFileSize);
  if (!bytes.ok()) {
    return SceneResult::Error(bytes.error() == platform::FileError::kTooLarge
                                  ? SceneIoError::kCorrupt
                                  : SceneIoError::kCannotOpen);
  }
  return DecodeFile(bytes.value(), SceneDirectory(path));
}

}  // namespace blue::scene
