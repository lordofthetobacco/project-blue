#include "scene/mscn_serializer.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>

#include "core/log.h"
#include "core/path.h"
#include "platform/file_system.h"
#include "scene/binary_stream.h"

namespace blue::scene {
namespace {

constexpr std::array<char, 8> kMagic = {'B', 'L', 'U', 'E', 'M', 'S', 'C', 'N'};
constexpr size_t kHeaderSize = kMagic.size() + 3 * sizeof(uint32_t);
constexpr size_t kChunkHeaderSize = 2 * sizeof(uint32_t);
constexpr uint32_t kMaxStringLength = 4096;
constexpr size_t kMaxSceneFileSize = 64 * 1024 * 1024;

constexpr uint32_t kInfoChunk = MakeFourcc('I', 'N', 'F', 'O');
constexpr uint32_t kPrefabsChunk = MakeFourcc('P', 'F', 'B', 'S');
constexpr uint32_t kInstancesChunk = MakeFourcc('I', 'N', 'S', 'T');
constexpr uint32_t kSunChunk = MakeFourcc('S', 'U', 'N', ' ');
constexpr uint32_t kPointLightsChunk = MakeFourcc('P', 'L', 'I', 'T');
constexpr uint32_t kEnvironmentChunk = MakeFourcc('E', 'N', 'V', 'I');

// On-disk "none" for prefab indices.
constexpr uint32_t kNoIndex = 0xFFFFFFFFU;

// Instance flags.
constexpr uint32_t kVisibleFlag = 1U << 0;
constexpr uint32_t kPhysicsOverrideFlag = 1U << 1;
constexpr uint32_t kInstanceFlags = kVisibleFlag | kPhysicsOverrideFlag;

// Material override field mask.
constexpr uint32_t kBaseColorBit = 1U << 0;
constexpr uint32_t kMetallicBit = 1U << 1;
constexpr uint32_t kRoughnessBit = 1U << 2;
constexpr uint32_t kEmissiveBit = 1U << 3;
constexpr uint32_t kEmissiveStrengthBit = 1U << 4;
constexpr uint32_t kUvScaleBit = 1U << 5;
constexpr uint32_t kMaterialFields = (1U << 6) - 1;

// Physics override field mask.
constexpr uint32_t kDisabledBit = 1U << 0;
constexpr uint32_t kMotionBit = 1U << 1;
constexpr uint32_t kMassBit = 1U << 2;
constexpr uint32_t kFrictionBit = 1U << 3;
constexpr uint32_t kRestitutionBit = 1U << 4;
constexpr uint32_t kPhysicsFields = (1U << 5) - 1;

constexpr uint32_t kSunCastsShadowsFlag = 1U << 0;
// SUN chunk size: vec3 direction, vec3 color, f32 intensity, u32 flags.
constexpr size_t kSunChunkBytes = 8 * sizeof(uint32_t);

// Smallest records, used to reject hostile counts before reserving.
// Instance: id, empty name, prefab, parent, 10 transform floats, flags,
// override count.
constexpr size_t kMinInstanceBytes = (6 + 10) * sizeof(uint32_t);
// Point light: empty name, 8 floats.
constexpr size_t kMinPointLightBytes = 9 * sizeof(uint32_t);
// Material override: empty name, index, empty material, mask.
constexpr size_t kMinMaterialOverrideBytes = 4 * sizeof(uint32_t);

using Status = core::Result<void, MscnError>;
using SceneResult = core::Result<SceneData, MscnError>;

// Returns the absolute directory that contains the scene file `scene_path`.
std::string SceneDirectory(const std::string& scene_path) {
  return core::ParentPath(platform::AbsolutePath(scene_path));
}

// Returns the form of `path` written to disk: relative to `scene_dir` where
// possible (not e.g. across drives), absolute otherwise; empty stays empty.
std::string StoredPath(const std::string& path, const std::string& scene_dir) {
  if (path.empty()) {
    return {};
  }
  const std::string absolute = platform::AbsolutePath(path);
  return core::RelativePath(absolute, scene_dir).value_or(absolute);
}

// The inverse of StoredPath().
std::string ResolvedPath(const std::string& stored,
                         const std::string& scene_dir) {
  return stored.empty() ? std::string() : core::JoinPath(scene_dir, stored);
}

uint32_t ToStoredIndex(int index) {
  return index == kNoPrefab ? kNoIndex : static_cast<uint32_t>(index);
}

void WriteVec2(BinaryWriter& writer, const glm::vec2& v) {
  writer.WriteF32(v.x);
  writer.WriteF32(v.y);
}

void WriteVec3(BinaryWriter& writer, const glm::vec3& v) {
  writer.WriteF32(v.x);
  writer.WriteF32(v.y);
  writer.WriteF32(v.z);
}

void WriteVec4(BinaryWriter& writer, const glm::vec4& v) {
  writer.WriteF32(v.x);
  writer.WriteF32(v.y);
  writer.WriteF32(v.z);
  writer.WriteF32(v.w);
}

void WriteQuat(BinaryWriter& writer, const glm::quat& q) {
  writer.WriteF32(q.x);
  writer.WriteF32(q.y);
  writer.WriteF32(q.z);
  writer.WriteF32(q.w);
}

glm::vec2 ReadVec2(BinaryReader& reader) {
  const float x = reader.ReadF32();
  const float y = reader.ReadF32();
  return {x, y};
}

glm::vec3 ReadVec3(BinaryReader& reader) {
  const float x = reader.ReadF32();
  const float y = reader.ReadF32();
  const float z = reader.ReadF32();
  return {x, y, z};
}

glm::vec4 ReadVec4(BinaryReader& reader) {
  const float x = reader.ReadF32();
  const float y = reader.ReadF32();
  const float z = reader.ReadF32();
  const float w = reader.ReadF32();
  return {x, y, z, w};
}

glm::quat ReadQuat(BinaryReader& reader) {
  const float x = reader.ReadF32();
  const float y = reader.ReadF32();
  const float z = reader.ReadF32();
  const float w = reader.ReadF32();
  return {w, x, y, z};
}

// Maps the reader's sticky status to the error model.
Status ToResult(ReadStatus status) {
  switch (status) {
    case ReadStatus::kOk:
      return Status::Ok();
    case ReadStatus::kTruncated:
      return Status::Error(MscnError::kTruncated);
    case ReadStatus::kLimitExceeded:
      break;
  }
  return Status::Error(MscnError::kCorrupt);
}

// Returns `stored` as an in-memory prefab index, or nullopt if it is neither
// kNoIndex nor below `prefab_count`.
std::optional<int> ToPrefabIndex(uint32_t stored, size_t prefab_count) {
  if (stored == kNoIndex) {
    return kNoPrefab;
  }
  if (stored >= prefab_count) {
    return std::nullopt;
  }
  return static_cast<int>(stored);
}

// ---------------------------------------------------------------------------
// Encoding.

void EncodeInfo(const SceneData& scene, BinaryWriter& writer) {
  const size_t chunk = writer.BeginChunk(kInfoChunk);
  writer.WriteU32(scene.next_instance_id);
  WriteVec3(writer, scene.gravity);
  WriteVec3(writer, scene.player_start.position);
  writer.WriteF32(scene.player_start.yaw_radians);
  writer.WriteF32(scene.player_start.pitch_radians);
  writer.EndChunk(chunk);
}

void EncodePrefabs(const SceneData& scene, const std::string& scene_dir,
                   BinaryWriter& writer) {
  const size_t chunk = writer.BeginChunk(kPrefabsChunk);
  writer.WriteU32(static_cast<uint32_t>(scene.prefabs.size()));
  for (const std::string& path : scene.prefabs) {
    writer.WriteString(StoredPath(path, scene_dir));
  }
  writer.EndChunk(chunk);
}

void EncodeMaterialOverride(const MaterialOverride& material,
                            BinaryWriter& writer) {
  writer.WriteString(material.material_name);
  writer.WriteU32(ToStoredIndex(material.source_prefab));
  writer.WriteString(material.source_material);
  uint32_t mask = 0;
  mask |= material.base_color.has_value() ? kBaseColorBit : 0U;
  mask |= material.metallic.has_value() ? kMetallicBit : 0U;
  mask |= material.roughness.has_value() ? kRoughnessBit : 0U;
  mask |= material.emissive.has_value() ? kEmissiveBit : 0U;
  mask |= material.emissive_strength.has_value() ? kEmissiveStrengthBit : 0U;
  mask |= material.uv_scale.has_value() ? kUvScaleBit : 0U;
  writer.WriteU32(mask);
  if (material.base_color.has_value()) {
    WriteVec4(writer, *material.base_color);
  }
  if (material.metallic.has_value()) {
    writer.WriteF32(*material.metallic);
  }
  if (material.roughness.has_value()) {
    writer.WriteF32(*material.roughness);
  }
  if (material.emissive.has_value()) {
    WriteVec3(writer, *material.emissive);
  }
  if (material.emissive_strength.has_value()) {
    writer.WriteF32(*material.emissive_strength);
  }
  if (material.uv_scale.has_value()) {
    WriteVec2(writer, *material.uv_scale);
  }
}

void EncodePhysicsOverride(const PhysicsOverride& physics,
                           BinaryWriter& writer) {
  uint32_t mask = physics.disabled ? kDisabledBit : 0U;
  mask |= physics.motion.has_value() ? kMotionBit : 0U;
  mask |= physics.mass.has_value() ? kMassBit : 0U;
  mask |= physics.friction.has_value() ? kFrictionBit : 0U;
  mask |= physics.restitution.has_value() ? kRestitutionBit : 0U;
  writer.WriteU32(mask);
  if (physics.motion.has_value()) {
    writer.WriteU32(static_cast<uint32_t>(*physics.motion));
  }
  if (physics.mass.has_value()) {
    writer.WriteF32(*physics.mass);
  }
  if (physics.friction.has_value()) {
    writer.WriteF32(*physics.friction);
  }
  if (physics.restitution.has_value()) {
    writer.WriteF32(*physics.restitution);
  }
}

void EncodeInstances(const SceneData& scene, BinaryWriter& writer) {
  const size_t chunk = writer.BeginChunk(kInstancesChunk);
  writer.WriteU32(static_cast<uint32_t>(scene.instances.size()));
  for (const Instance& instance : scene.instances) {
    writer.WriteU32(instance.id);
    writer.WriteString(instance.name);
    writer.WriteU32(ToStoredIndex(instance.prefab));
    writer.WriteU32(instance.parent);
    WriteVec3(writer, instance.transform.position);
    WriteQuat(writer, instance.transform.rotation);
    WriteVec3(writer, instance.transform.scale);
    uint32_t flags = instance.visible ? kVisibleFlag : 0U;
    flags |= instance.physics.has_value() ? kPhysicsOverrideFlag : 0U;
    writer.WriteU32(flags);
    writer.WriteU32(static_cast<uint32_t>(instance.materials.size()));
    for (const MaterialOverride& material : instance.materials) {
      EncodeMaterialOverride(material, writer);
    }
    if (instance.physics.has_value()) {
      EncodePhysicsOverride(*instance.physics, writer);
    }
  }
  writer.EndChunk(chunk);
}

void EncodeSun(const SunLight& sun, BinaryWriter& writer) {
  const size_t chunk = writer.BeginChunk(kSunChunk);
  WriteVec3(writer, sun.direction);
  WriteVec3(writer, sun.color);
  writer.WriteF32(sun.intensity);
  writer.WriteU32(sun.casts_shadows ? kSunCastsShadowsFlag : 0U);
  writer.EndChunk(chunk);
}

void EncodePointLights(const SceneData& scene, BinaryWriter& writer) {
  const size_t chunk = writer.BeginChunk(kPointLightsChunk);
  writer.WriteU32(static_cast<uint32_t>(scene.point_lights.size()));
  for (const ScenePointLight& light : scene.point_lights) {
    writer.WriteString(light.name);
    WriteVec3(writer, light.position);
    WriteVec3(writer, light.color);
    writer.WriteF32(light.intensity);
    writer.WriteF32(light.range);
  }
  writer.EndChunk(chunk);
}

void EncodeEnvironment(const Environment& environment,
                       const std::string& scene_dir, BinaryWriter& writer) {
  const size_t chunk = writer.BeginChunk(kEnvironmentChunk);
  writer.WriteString(StoredPath(environment.hdri_path, scene_dir));
  writer.WriteF32(environment.intensity);
  writer.WriteF32(environment.rotation_radians);
  writer.WriteF32(environment.exposure_ev);
  writer.EndChunk(chunk);
}

std::vector<std::byte> EncodePayload(const SceneData& scene,
                                     const std::string& scene_dir) {
  BinaryWriter writer;
  EncodeInfo(scene, writer);
  EncodePrefabs(scene, scene_dir, writer);
  EncodeInstances(scene, writer);
  EncodeSun(scene.sun, writer);
  EncodePointLights(scene, writer);
  EncodeEnvironment(scene.environment, scene_dir, writer);
  return writer.bytes();
}

// ---------------------------------------------------------------------------
// Decoding. Values are range-checked by ValidateSceneData() once the whole
// scene is decoded; the decoders only check what they need to stay in bounds
// (counts, enumerators, flag bits, indices into the prefab table).

Status DecodeInfo(std::span<const std::byte> body, SceneData& scene) {
  BinaryReader reader(body);
  scene.next_instance_id = reader.ReadU32();
  scene.gravity = ReadVec3(reader);
  scene.player_start.position = ReadVec3(reader);
  scene.player_start.yaw_radians = reader.ReadF32();
  scene.player_start.pitch_radians = reader.ReadF32();
  if (auto status = ToResult(reader.status()); !status.ok()) {
    return status;
  }
  return reader.remaining() == 0 ? Status::Ok()
                                 : Status::Error(MscnError::kCorrupt);
}

Status DecodePrefabs(std::span<const std::byte> body,
                     const std::string& scene_dir, SceneData& scene) {
  BinaryReader reader(body);
  const uint32_t count = reader.ReadU32();
  if (auto status = ToResult(reader.status()); !status.ok()) {
    return status;
  }
  if (count > reader.remaining() / sizeof(uint32_t)) {
    return Status::Error(MscnError::kCorrupt);
  }
  scene.prefabs.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    std::string stored = reader.ReadString(kMaxStringLength);
    if (auto status = ToResult(reader.status()); !status.ok()) {
      return status;
    }
    if (stored.empty()) {
      return Status::Error(MscnError::kCorrupt);
    }
    scene.prefabs.push_back(ResolvedPath(stored, scene_dir));
  }
  return reader.remaining() == 0 ? Status::Ok()
                                 : Status::Error(MscnError::kCorrupt);
}

Status DecodeMaterialOverride(BinaryReader& reader, size_t prefab_count,
                              MaterialOverride& material) {
  material.material_name = reader.ReadString(kMaxStringLength);
  const uint32_t source = reader.ReadU32();
  material.source_material = reader.ReadString(kMaxStringLength);
  const uint32_t mask = reader.ReadU32();
  if (auto status = ToResult(reader.status()); !status.ok()) {
    return status;
  }
  const std::optional<int> source_index = ToPrefabIndex(source, prefab_count);
  if (!source_index.has_value() || (mask & ~kMaterialFields) != 0) {
    return Status::Error(MscnError::kCorrupt);
  }
  material.source_prefab = *source_index;
  if ((mask & kBaseColorBit) != 0) {
    material.base_color = ReadVec4(reader);
  }
  if ((mask & kMetallicBit) != 0) {
    material.metallic = reader.ReadF32();
  }
  if ((mask & kRoughnessBit) != 0) {
    material.roughness = reader.ReadF32();
  }
  if ((mask & kEmissiveBit) != 0) {
    material.emissive = ReadVec3(reader);
  }
  if ((mask & kEmissiveStrengthBit) != 0) {
    material.emissive_strength = reader.ReadF32();
  }
  if ((mask & kUvScaleBit) != 0) {
    material.uv_scale = ReadVec2(reader);
  }
  return ToResult(reader.status());
}

Status DecodePhysicsOverride(BinaryReader& reader, PhysicsOverride& physics) {
  const uint32_t mask = reader.ReadU32();
  if (auto status = ToResult(reader.status()); !status.ok()) {
    return status;
  }
  if ((mask & ~kPhysicsFields) != 0) {
    return Status::Error(MscnError::kCorrupt);
  }
  physics.disabled = (mask & kDisabledBit) != 0;
  if ((mask & kMotionBit) != 0) {
    const uint32_t motion = reader.ReadU32();
    if (motion > static_cast<uint32_t>(MotionType::kDynamic)) {
      return Status::Error(MscnError::kCorrupt);
    }
    physics.motion = static_cast<MotionType>(motion);
  }
  if ((mask & kMassBit) != 0) {
    physics.mass = reader.ReadF32();
  }
  if ((mask & kFrictionBit) != 0) {
    physics.friction = reader.ReadF32();
  }
  if ((mask & kRestitutionBit) != 0) {
    physics.restitution = reader.ReadF32();
  }
  return ToResult(reader.status());
}

// Decodes one instance record; prefab indices are checked against
// `prefab_count`.
Status DecodeInstance(BinaryReader& reader, size_t prefab_count,
                      Instance& instance) {
  instance.id = reader.ReadU32();
  instance.name = reader.ReadString(kMaxStringLength);
  const uint32_t prefab = reader.ReadU32();
  instance.parent = reader.ReadU32();
  instance.transform.position = ReadVec3(reader);
  instance.transform.rotation = ReadQuat(reader);
  instance.transform.scale = ReadVec3(reader);
  const uint32_t flags = reader.ReadU32();
  const uint32_t material_count = reader.ReadU32();
  if (auto status = ToResult(reader.status()); !status.ok()) {
    return status;
  }
  const std::optional<int> prefab_index = ToPrefabIndex(prefab, prefab_count);
  if (!prefab_index.has_value() || (flags & ~kInstanceFlags) != 0 ||
      material_count > reader.remaining() / kMinMaterialOverrideBytes) {
    return Status::Error(MscnError::kCorrupt);
  }
  instance.prefab = *prefab_index;
  instance.visible = (flags & kVisibleFlag) != 0;
  instance.materials.resize(material_count);
  for (MaterialOverride& material : instance.materials) {
    if (auto status = DecodeMaterialOverride(reader, prefab_count, material);
        !status.ok()) {
      return status;
    }
  }
  if ((flags & kPhysicsOverrideFlag) != 0) {
    instance.physics.emplace();
    return DecodePhysicsOverride(reader, *instance.physics);
  }
  return Status::Ok();
}

// Decodes the INST chunk. Must run after PFBS so prefab indices can be
// checked.
Status DecodeInstances(std::span<const std::byte> body, SceneData& scene) {
  BinaryReader reader(body);
  const uint32_t count = reader.ReadU32();
  if (auto status = ToResult(reader.status()); !status.ok()) {
    return status;
  }
  if (count > reader.remaining() / kMinInstanceBytes) {
    return Status::Error(MscnError::kCorrupt);
  }
  scene.instances.resize(count);
  for (Instance& instance : scene.instances) {
    if (auto status = DecodeInstance(reader, scene.prefabs.size(), instance);
        !status.ok()) {
      return status;
    }
  }
  return reader.remaining() == 0 ? Status::Ok()
                                 : Status::Error(MscnError::kCorrupt);
}

Status DecodeSun(std::span<const std::byte> body, SceneData& scene) {
  if (body.size() != kSunChunkBytes) {
    return Status::Error(MscnError::kCorrupt);
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
  if ((flags & ~kSunCastsShadowsFlag) != 0) {
    return Status::Error(MscnError::kCorrupt);
  }
  sun.casts_shadows = (flags & kSunCastsShadowsFlag) != 0;
  scene.sun = sun;
  return Status::Ok();
}

Status DecodePointLights(std::span<const std::byte> body, SceneData& scene) {
  BinaryReader reader(body);
  const uint32_t count = reader.ReadU32();
  if (auto status = ToResult(reader.status()); !status.ok()) {
    return status;
  }
  if (count > reader.remaining() / kMinPointLightBytes) {
    return Status::Error(MscnError::kCorrupt);
  }
  scene.point_lights.resize(count);
  for (ScenePointLight& light : scene.point_lights) {
    light.name = reader.ReadString(kMaxStringLength);
    light.position = ReadVec3(reader);
    light.color = ReadVec3(reader);
    light.intensity = reader.ReadF32();
    light.range = reader.ReadF32();
  }
  if (auto status = ToResult(reader.status()); !status.ok()) {
    return status;
  }
  return reader.remaining() == 0 ? Status::Ok()
                                 : Status::Error(MscnError::kCorrupt);
}

Status DecodeEnvironment(std::span<const std::byte> body,
                         const std::string& scene_dir, SceneData& scene) {
  BinaryReader reader(body);
  Environment environment;
  environment.hdri_path =
      ResolvedPath(reader.ReadString(kMaxStringLength), scene_dir);
  environment.intensity = reader.ReadF32();
  environment.rotation_radians = reader.ReadF32();
  environment.exposure_ev = reader.ReadF32();
  if (auto status = ToResult(reader.status()); !status.ok()) {
    return status;
  }
  if (reader.remaining() != 0) {
    return Status::Error(MscnError::kCorrupt);
  }
  scene.environment = std::move(environment);
  return Status::Ok();
}

// The bodies of the known chunks of one file, each present at most once.
struct ChunkBodies {
  std::optional<std::span<const std::byte>> info;
  std::optional<std::span<const std::byte>> prefabs;
  std::optional<std::span<const std::byte>> instances;
  std::optional<std::span<const std::byte>> sun;
  std::optional<std::span<const std::byte>> point_lights;
  std::optional<std::span<const std::byte>> environment;
};

// Splits `payload` into chunks. Unknown chunks are skipped; a repeated known
// chunk is an error.
Status SplitChunks(std::span<const std::byte> payload, ChunkBodies& bodies) {
  BinaryReader chunks(payload);
  while (chunks.remaining() > 0) {
    if (chunks.remaining() < kChunkHeaderSize) {
      return Status::Error(MscnError::kTruncated);
    }
    const uint32_t fourcc = chunks.ReadU32();
    const uint32_t size = chunks.ReadU32();
    if (size > chunks.remaining()) {
      return Status::Error(MscnError::kTruncated);
    }
    const std::span<const std::byte> body = chunks.ReadBytes(size);

    std::optional<std::span<const std::byte>>* slot = nullptr;
    switch (fourcc) {
      case kInfoChunk:
        slot = &bodies.info;
        break;
      case kPrefabsChunk:
        slot = &bodies.prefabs;
        break;
      case kInstancesChunk:
        slot = &bodies.instances;
        break;
      case kSunChunk:
        slot = &bodies.sun;
        break;
      case kPointLightsChunk:
        slot = &bodies.point_lights;
        break;
      case kEnvironmentChunk:
        slot = &bodies.environment;
        break;
      default:
        continue;  // Unknown chunk: written by a newer version.
    }
    if (slot->has_value()) {
      return Status::Error(MscnError::kCorrupt);
    }
    *slot = body;
  }
  return Status::Ok();
}

// Decodes the chunks in dependency order (PFBS before INST) and validates the
// result.
SceneResult DecodeChunks(const ChunkBodies& bodies,
                         const std::string& scene_dir) {
  if (!bodies.info.has_value() || !bodies.prefabs.has_value() ||
      !bodies.instances.has_value()) {
    return SceneResult::Error(MscnError::kCorrupt);
  }
  SceneData scene;
  Status status = DecodeInfo(*bodies.info, scene);
  if (status.ok()) {
    status = DecodePrefabs(*bodies.prefabs, scene_dir, scene);
  }
  if (status.ok()) {
    status = DecodeInstances(*bodies.instances, scene);
  }
  if (status.ok() && bodies.sun.has_value()) {
    status = DecodeSun(*bodies.sun, scene);
  }
  if (status.ok() && bodies.point_lights.has_value()) {
    status = DecodePointLights(*bodies.point_lights, scene);
  }
  if (status.ok() && bodies.environment.has_value()) {
    status = DecodeEnvironment(*bodies.environment, scene_dir, scene);
  }
  if (!status.ok()) {
    return SceneResult::Error(status.error());
  }
  if (ValidateSceneData(scene).has_value()) {
    return SceneResult::Error(MscnError::kCorrupt);
  }
  // Directions that are already unit length keep their exact bits, so a
  // saved scene loads back unchanged.
  if (std::abs(glm::length(scene.sun.direction) - 1.0f) > 1e-5f) {
    scene.sun.direction = glm::normalize(scene.sun.direction);
  }
  return SceneResult::Ok(std::move(scene));
}

// Validates the header and checksum of the file `file` and decodes it.
SceneResult DecodeFile(std::span<const std::byte> file,
                       const std::string& scene_dir) {
  if (file.size() < kHeaderSize) {
    return SceneResult::Error(MscnError::kTruncated);
  }
  BinaryReader header(file);
  const std::span<const std::byte> magic = header.ReadBytes(kMagic.size());
  if (!std::equal(
          magic.begin(), magic.end(), kMagic.begin(), [](std::byte a, char b) {
            return std::to_integer<int>(a) == static_cast<unsigned char>(b);
          })) {
    return SceneResult::Error(MscnError::kBadMagic);
  }
  const uint32_t version = header.ReadU32();
  const uint32_t payload_size = header.ReadU32();
  const uint32_t payload_crc = header.ReadU32();
  if (version == 0) {
    return SceneResult::Error(MscnError::kCorrupt);
  }
  if (version > kMscnFormatVersion) {
    return SceneResult::Error(MscnError::kUnsupportedVersion);
  }
  if (payload_size > header.remaining()) {
    return SceneResult::Error(MscnError::kTruncated);
  }
  if (payload_size < header.remaining()) {
    return SceneResult::Error(MscnError::kCorrupt);
  }
  const std::span<const std::byte> payload = header.ReadBytes(payload_size);
  if (Crc32(payload) != payload_crc) {
    return SceneResult::Error(MscnError::kChecksumMismatch);
  }

  ChunkBodies bodies;
  if (Status split = SplitChunks(payload, bodies); !split.ok()) {
    return SceneResult::Error(split.error());
  }
  return DecodeChunks(bodies, scene_dir);
}

}  // namespace

const char* ToString(MscnError error) {
  switch (error) {
    case MscnError::kCannotOpen:
      return "cannot open file";
    case MscnError::kWriteFailed:
      return "write failed";
    case MscnError::kBadMagic:
      return "not a .mscn file (bad magic)";
    case MscnError::kUnsupportedVersion:
      return "unsupported .mscn version";
    case MscnError::kTruncated:
      return "file truncated";
    case MscnError::kChecksumMismatch:
      return "checksum mismatch";
    case MscnError::kCorrupt:
      return "corrupt scene data";
    case MscnError::kInvalidScene:
      return "scene cannot be serialized (see CheckMscnSerializable)";
  }
  return "unknown error";
}

std::optional<std::string> CheckMscnSerializable(const SceneData& scene,
                                                 const std::string& path) {
  if (auto error = ValidateSceneData(scene)) {
    return error;
  }
  const std::string scene_dir = SceneDirectory(path);
  const auto too_long = [](size_t length) { return length > kMaxStringLength; };
  for (const std::string& prefab : scene.prefabs) {
    if (too_long(StoredPath(prefab, scene_dir).size())) {
      return std::format("prefab path longer than {} bytes: '{}'",
                         kMaxStringLength, prefab);
    }
  }
  if (too_long(StoredPath(scene.environment.hdri_path, scene_dir).size())) {
    return std::format("HDRI path longer than {} bytes", kMaxStringLength);
  }
  for (const Instance& instance : scene.instances) {
    if (too_long(instance.name.size())) {
      return std::format("instance {}: name longer than {} bytes", instance.id,
                         kMaxStringLength);
    }
    for (const MaterialOverride& material : instance.materials) {
      if (too_long(material.material_name.size()) ||
          too_long(material.source_material.size())) {
        return std::format("instance {}: material name longer than {} bytes",
                           instance.id, kMaxStringLength);
      }
    }
  }
  for (const ScenePointLight& light : scene.point_lights) {
    if (too_long(light.name.size())) {
      return std::format("point light name longer than {} bytes",
                         kMaxStringLength);
    }
  }
  return std::nullopt;
}

core::Result<std::vector<std::byte>, MscnError> EncodeMscn(
    const SceneData& scene, const std::string& scene_path) {
  using BytesResult = core::Result<std::vector<std::byte>, MscnError>;
  if (CheckMscnSerializable(scene, scene_path).has_value()) {
    return BytesResult::Error(MscnError::kInvalidScene);
  }
  const std::vector<std::byte> payload =
      EncodePayload(scene, SceneDirectory(scene_path));

  BinaryWriter file;
  file.WriteBytes(std::as_bytes(std::span(kMagic)));
  file.WriteU32(kMscnFormatVersion);
  file.WriteU32(static_cast<uint32_t>(payload.size()));
  file.WriteU32(Crc32(payload));
  file.WriteBytes(payload);
  return BytesResult::Ok(file.bytes());
}

core::Result<SceneData, MscnError> DecodeMscn(std::span<const std::byte> bytes,
                                              const std::string& scene_path) {
  return DecodeFile(bytes, SceneDirectory(scene_path));
}

core::Result<void, MscnError> SaveMscn(const SceneData& scene,
                                       const std::string& path) {
  if (auto error = CheckMscnSerializable(scene, path)) {
    core::log::Warn("Cannot save scene {}: {}", path, *error);
    return Status::Error(MscnError::kInvalidScene);
  }
  const auto encoded = EncodeMscn(scene, path);
  if (!encoded.ok()) {
    return Status::Error(encoded.error());
  }
  if (!platform::WriteFileAtomic(path, encoded.value()).ok()) {
    return Status::Error(MscnError::kWriteFailed);
  }
  return Status::Ok();
}

core::Result<SceneData, MscnError> LoadMscn(const std::string& path) {
  const auto start = std::chrono::steady_clock::now();
  const auto bytes = platform::ReadFile(path, kMaxSceneFileSize);
  if (!bytes.ok()) {
    return SceneResult::Error(bytes.error() == platform::FileError::kTooLarge
                                  ? MscnError::kCorrupt
                                  : MscnError::kCannotOpen);
  }
  SceneResult scene = DecodeMscn(bytes.value(), path);
  if (!scene.ok()) {
    core::log::Warn("Cannot load scene {}: {}", path, ToString(scene.error()));
  } else {
    const std::chrono::duration<double, std::milli> elapsed =
        std::chrono::steady_clock::now() - start;
    core::log::Info("Loaded scene {} ({} bytes, {} instances) in {:.2f} ms",
                    path, bytes.value().size(), scene.value().instances.size(),
                    elapsed.count());
  }
  return scene;
}

}  // namespace blue::scene
