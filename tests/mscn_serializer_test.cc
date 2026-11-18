#include "scene/mscn_serializer.h"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "core/path.h"
#include "platform/file_system.h"
#include "scene/binary_stream.h"
#include "scene/scene_data.h"
#include "tests/scratch_directory.h"

namespace blue::scene {
namespace {

constexpr size_t kHeaderSize = 8 + 3 * sizeof(uint32_t);
constexpr uint32_t kInst = MakeFourcc('I', 'N', 'S', 'T');

std::vector<std::byte> ReadWholeFile(const std::string& path) {
  const auto bytes = platform::ReadFile(path, 64 * 1024 * 1024);
  REQUIRE(bytes.ok());
  return bytes.value();
}

void WriteWholeFile(const std::string& path, std::span<const std::byte> bytes) {
  REQUIRE(platform::WriteFileAtomic(path, bytes).ok());
}

void PutU32(std::vector<std::byte>& bytes, size_t offset, uint32_t value) {
  for (size_t i = 0; i < 4; ++i) {
    bytes[offset + i] = static_cast<std::byte>((value >> (8 * i)) & 0xFFU);
  }
}

uint32_t GetU32(std::span<const std::byte> bytes, size_t offset) {
  uint32_t value = 0;
  for (size_t i = 0; i < 4; ++i) {
    value |= std::to_integer<uint32_t>(bytes[offset + i]) << (8 * i);
  }
  return value;
}

// Returns `file` with its payload replaced by `payload` and the header's size
// and CRC fixed up, so decoders see the edited bytes.
std::vector<std::byte> WithPayload(std::span<const std::byte> file,
                                   std::span<const std::byte> payload) {
  std::vector<std::byte> result(file.begin(), file.begin() + kHeaderSize);
  result.insert(result.end(), payload.begin(), payload.end());
  PutU32(result, 12, static_cast<uint32_t>(payload.size()));
  PutU32(result, 16, Crc32(payload));
  return result;
}

std::vector<std::byte> PayloadOf(std::span<const std::byte> file) {
  return {file.begin() + kHeaderSize, file.end()};
}

// Returns the offset of the chunk header tagged `fourcc` in `payload`.
size_t FindChunk(std::span<const std::byte> payload, uint32_t fourcc) {
  size_t offset = 0;
  while (offset + 8 <= payload.size()) {
    if (GetU32(payload, offset) == fourcc) {
      return offset;
    }
    offset += 8 + GetU32(payload, offset + 4);
  }
  FAIL("chunk not found");
  return 0;
}

// A scene that uses every field: nested instances, both override kinds,
// physics overrides, lights, a non-default environment.
SceneData MakeRichScene(const testing::ScratchDirectory& dir) {
  SceneData scene;
  const int gun = AddPrefab(scene, dir.Child("assets/glock.pbo"));
  const int library = AddPrefab(scene, dir.Child("assets/materials.pbo"));
  const int crate = AddPrefab(scene, dir.Child("assets/props/crate.pbo"));

  Instance group;
  group.name = "props";
  group.transform.position = {1.0f, 2.0f, 3.0f};
  const InstanceId group_id = AddInstance(scene, group);

  Instance pistol;
  pistol.name = "pistol";
  pistol.prefab = gun;
  pistol.transform.rotation = glm::normalize(glm::quat(0.9f, 0.1f, 0.2f, 0.3f));
  pistol.transform.scale = {1.5f, 1.5f, 1.5f};
  MaterialOverride replace;
  replace.material_name = "frame";
  replace.source_prefab = library;
  replace.source_material = "steel";
  replace.roughness = 0.25f;
  MaterialOverride tweak;
  tweak.material_name = "slide";
  tweak.base_color = glm::vec4(0.5f, 0.4f, 0.3f, 1.0f);
  tweak.metallic = 1.0f;
  tweak.emissive = glm::vec3(2.0f, 0.0f, 0.5f);
  tweak.emissive_strength = 3.0f;
  tweak.uv_scale = glm::vec2(2.0f, 4.0f);
  pistol.materials = {replace, tweak};
  pistol.physics = PhysicsOverride{.motion = MotionType::kStatic};
  AddInstance(scene, pistol);

  Instance box;
  box.name = "crate";
  box.prefab = crate;
  box.parent = group_id;
  box.visible = false;
  box.physics = PhysicsOverride{.disabled = true,
                                .motion = MotionType::kDynamic,
                                .mass = 25.0f,
                                .friction = 0.9f,
                                .restitution = 0.3f};
  AddInstance(scene, box);

  scene.sun.direction = glm::normalize(glm::vec3(0.2f, -1.0f, 0.1f));
  scene.sun.color = {0.9f, 0.8f, 0.7f};
  scene.sun.intensity = 5.0f;
  scene.sun.casts_shadows = false;
  scene.point_lights.push_back(
      {"lamp", {0.0f, 3.0f, 0.0f}, {1.0f, 0.5f, 0.2f}, 40.0f, 8.0f});
  scene.point_lights.push_back({"fill", {4.0f, 1.0f, -2.0f}, {}, 5.0f, 3.0f});
  scene.environment.hdri_path = dir.Child("hdri/sky.hdr");
  scene.environment.intensity = 0.8f;
  scene.environment.rotation_radians = 1.0f;
  scene.environment.exposure_ev = -0.5f;
  scene.player_start = {{0.0f, 1.7f, 5.0f}, 3.0f, -0.1f};
  scene.gravity = {0.0f, -3.7f, 0.0f};
  return scene;
}

}  // namespace

TEST_CASE("mscn round trip preserves every field") {
  const testing::ScratchDirectory dir("mscn");
  const SceneData scene = MakeRichScene(dir);
  const std::string path = dir.Child("scenes/level.mscn");
  REQUIRE(SaveMscn(scene, path).ok());

  const auto loaded = LoadMscn(path);
  REQUIRE(loaded.ok());
  CHECK(loaded.value() == scene);
}

TEST_CASE("mscn round trip of an empty scene") {
  const testing::ScratchDirectory dir("mscn");
  const std::string path = dir.Child("empty.mscn");
  REQUIRE(SaveMscn(SceneData{}, path).ok());
  const auto loaded = LoadMscn(path);
  REQUIRE(loaded.ok());
  CHECK(loaded.value() == SceneData{});
}

TEST_CASE("EncodeMscn and DecodeMscn round trip in memory") {
  const testing::ScratchDirectory dir("mscn");
  const SceneData scene = MakeRichScene(dir);
  const std::string path = dir.Child("scenes/level.mscn");
  const auto encoded = EncodeMscn(scene, path);
  REQUIRE(encoded.ok());
  CHECK_FALSE(platform::PathExists(path));
  const auto decoded = DecodeMscn(encoded.value(), path);
  REQUIRE(decoded.ok());
  CHECK(decoded.value() == scene);
}

TEST_CASE("mscn output is deterministic") {
  const testing::ScratchDirectory dir("mscn");
  const SceneData scene = MakeRichScene(dir);
  REQUIRE(SaveMscn(scene, dir.Child("a.mscn")).ok());
  REQUIRE(SaveMscn(scene, dir.Child("b.mscn")).ok());
  const std::vector<std::byte> first = ReadWholeFile(dir.Child("a.mscn"));
  CHECK(first == ReadWholeFile(dir.Child("b.mscn")));

  // Load and save again: byte-identical.
  const auto loaded = LoadMscn(dir.Child("a.mscn"));
  REQUIRE(loaded.ok());
  REQUIRE(SaveMscn(loaded.value(), dir.Child("c.mscn")).ok());
  CHECK(first == ReadWholeFile(dir.Child("c.mscn")));
}

TEST_CASE("mscn stores paths relative to the scene file") {
  const testing::ScratchDirectory dir("mscn");
  SceneData scene;
  AddPrefab(scene, dir.Child("assets/crate.pbo"));
  scene.environment.hdri_path = dir.Child("hdri/sky.hdr");
  REQUIRE(SaveMscn(scene, dir.Child("levels/one/scene.mscn")).ok());

  const std::vector<std::byte> bytes =
      ReadWholeFile(dir.Child("levels/one/scene.mscn"));
  const std::string text(reinterpret_cast<const char*>(bytes.data()),
                         bytes.size());
  CHECK(text.find("../../assets/crate.pbo") != std::string::npos);
  CHECK(text.find("../../hdri/sky.hdr") != std::string::npos);
  CHECK(text.find(dir.path()) == std::string::npos);

  // Moving the whole tree keeps the references valid.
  const std::string moved = dir.Child("copy/levels/one/scene.mscn");
  WriteWholeFile(moved, bytes);
  const auto loaded = LoadMscn(moved);
  REQUIRE(loaded.ok());
  REQUIRE(loaded.value().prefabs.size() == 1);
  CHECK(loaded.value().prefabs[0] == dir.Child("copy/assets/crate.pbo"));
  CHECK(loaded.value().environment.hdri_path == dir.Child("copy/hdri/sky.hdr"));
}

TEST_CASE("mscn keeps instance ids across removal, save and load") {
  const testing::ScratchDirectory dir("mscn");
  SceneData scene = MakeRichScene(dir);
  REQUIRE(RemoveInstance(scene, 1));  // The group and its crate.
  REQUIRE(SaveMscn(scene, dir.Child("s.mscn")).ok());

  auto loaded = LoadMscn(dir.Child("s.mscn"));
  REQUIRE(loaded.ok());
  SceneData& reloaded = loaded.value();
  REQUIRE(reloaded.instances.size() == 1);
  CHECK(reloaded.instances[0].id == 2);
  CHECK(reloaded.instances[0].name == "pistol");
  // The crate prefab was compacted away.
  CHECK(reloaded.prefabs.size() == 2);
  Instance another;
  another.prefab = 0;
  CHECK(AddInstance(reloaded, another) == 4);
}

TEST_CASE("mscn refuses invalid scenes without writing") {
  const testing::ScratchDirectory dir("mscn");
  SceneData scene = MakeRichScene(dir);
  scene.instances[1].materials[0].roughness = 2.0f;
  const std::string path = dir.Child("bad.mscn");
  const std::optional<std::string> reason = CheckMscnSerializable(scene, path);
  REQUIRE(reason.has_value());
  CHECK(reason->find("roughness") != std::string::npos);
  const auto saved = SaveMscn(scene, path);
  REQUIRE_FALSE(saved.ok());
  CHECK(saved.error() == MscnError::kInvalidScene);
  CHECK_FALSE(platform::PathExists(path));

  SceneData long_name;
  Instance instance;
  instance.name = std::string(5000, 'x');
  AddInstance(long_name, instance);
  CHECK(CheckMscnSerializable(long_name, path).has_value());
}

TEST_CASE("mscn header errors") {
  const testing::ScratchDirectory dir("mscn");
  const std::string path = dir.Child("s.mscn");
  REQUIRE(SaveMscn(MakeRichScene(dir), path).ok());
  std::vector<std::byte> bytes = ReadWholeFile(path);

  CHECK(LoadMscn(dir.Child("missing.mscn")).error() == MscnError::kCannotOpen);

  SUBCASE("bad magic") {
    bytes[0] = std::byte{'X'};
    WriteWholeFile(path, bytes);
    CHECK(LoadMscn(path).error() == MscnError::kBadMagic);
  }
  SUBCASE("newer version") {
    PutU32(bytes, 8, kMscnFormatVersion + 1);
    WriteWholeFile(path, bytes);
    CHECK(LoadMscn(path).error() == MscnError::kUnsupportedVersion);
  }
  SUBCASE("checksum") {
    bytes.back() ^= std::byte{0x01};
    WriteWholeFile(path, bytes);
    CHECK(LoadMscn(path).error() == MscnError::kChecksumMismatch);
  }
  SUBCASE("every truncation fails cleanly") {
    for (size_t size = 0; size < bytes.size(); ++size) {
      CHECK_FALSE(DecodeMscn(std::span(bytes).first(size), path).ok());
    }
    WriteWholeFile(path, std::span(bytes).first(bytes.size() / 2));
    CHECK(LoadMscn(path).error() == MscnError::kTruncated);
  }
}

TEST_CASE("mscn skips unknown chunks and rejects duplicate or missing ones") {
  const testing::ScratchDirectory dir("mscn");
  const SceneData scene = MakeRichScene(dir);
  const std::string path = dir.Child("s.mscn");
  REQUIRE(SaveMscn(scene, path).ok());
  const std::vector<std::byte> file = ReadWholeFile(path);
  std::vector<std::byte> payload = PayloadOf(file);

  SUBCASE("unknown chunk") {
    BinaryWriter extra;
    const size_t chunk = extra.BeginChunk(MakeFourcc('Z', 'Z', 'Z', 'Z'));
    extra.WriteU32(123);
    extra.WriteString("from the future");
    extra.EndChunk(chunk);
    payload.insert(payload.begin(), extra.bytes().begin(), extra.bytes().end());
    WriteWholeFile(path, WithPayload(file, payload));
    const auto loaded = LoadMscn(path);
    REQUIRE(loaded.ok());
    CHECK(loaded.value() == scene);
  }
  SUBCASE("duplicate chunk") {
    const size_t inst = FindChunk(payload, kInst);
    const size_t size = 8 + GetU32(payload, inst + 4);
    const std::vector<std::byte> copy(payload.begin() + inst,
                                      payload.begin() + inst + size);
    payload.insert(payload.end(), copy.begin(), copy.end());
    WriteWholeFile(path, WithPayload(file, payload));
    CHECK(LoadMscn(path).error() == MscnError::kCorrupt);
  }
  SUBCASE("missing required chunk") {
    const size_t inst = FindChunk(payload, kInst);
    const size_t size = 8 + GetU32(payload, inst + 4);
    payload.erase(payload.begin() + inst, payload.begin() + inst + size);
    WriteWholeFile(path, WithPayload(file, payload));
    CHECK(LoadMscn(path).error() == MscnError::kCorrupt);
  }
}

TEST_CASE("mscn survives corrupted payload bytes") {
  // Flips every bit pattern of every payload byte with a valid CRC, so the
  // decoders (not the checksum) must catch the damage. Loads may succeed
  // when a value stays valid, but must never crash or produce an invalid
  // scene.
  const testing::ScratchDirectory dir("mscn");
  const std::string path = dir.Child("s.mscn");
  REQUIRE(SaveMscn(MakeRichScene(dir), path).ok());
  const std::vector<std::byte> file = ReadWholeFile(path);
  const std::vector<std::byte> payload = PayloadOf(file);

  int failures = 0;
  for (size_t i = 0; i < payload.size(); ++i) {
    for (const std::byte mask :
         {std::byte{0x01}, std::byte{0x80}, std::byte{0xFF}}) {
      std::vector<std::byte> damaged = payload;
      damaged[i] ^= mask;
      const auto loaded = DecodeMscn(WithPayload(file, damaged), path);
      if (loaded.ok()) {
        CHECK_FALSE(ValidateSceneData(loaded.value()).has_value());
      } else {
        ++failures;
      }
    }
  }
  CHECK(failures > 0);
}

}  // namespace blue::scene
