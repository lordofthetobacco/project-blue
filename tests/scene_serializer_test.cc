#include "scene/scene_serializer.h"

#include <SDL3/SDL_filesystem.h>
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

#include "core/path.h"
#include "platform/file_system.h"
#include "scene/binary_stream.h"
#include "scene/scene.h"
#include "tests/scratch_directory.h"

namespace blue::scene {

namespace {

constexpr std::array<char, 8> kMagic = {'B', 'L', 'U', 'E',
                                        'S', 'C', 'N', '\0'};
constexpr uint32_t kEnvr = MakeFourcc('E', 'N', 'V', 'R');
constexpr uint32_t kEnts = MakeFourcc('E', 'N', 'T', 'S');
constexpr uint32_t kPhys = MakeFourcc('P', 'H', 'Y', 'S');
constexpr uint32_t kSun = MakeFourcc('S', 'U', 'N', ' ');
constexpr uint32_t kMats = MakeFourcc('M', 'A', 'T', 'S');
constexpr uint32_t kIbl = MakeFourcc('I', 'B', 'L', ' ');
constexpr uint32_t kNanBits = 0x7FC00000U;

std::vector<std::byte> ReadWholeFile(const std::string& path) {
  const auto bytes = platform::ReadFile(path, 128 * 1024 * 1024);
  REQUIRE(bytes.ok());
  return bytes.value();
}

void WriteWholeFile(const std::string& path, std::span<const std::byte> bytes) {
  REQUIRE(platform::WriteFileAtomic(path, bytes).ok());
}

bool SameBits(float a, float b) {
  return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b);
}

bool SameBits(const glm::vec3& a, const glm::vec3& b) {
  return SameBits(a.x, b.x) && SameBits(a.y, b.y) && SameBits(a.z, b.z);
}

bool SameBits(const glm::quat& a, const glm::quat& b) {
  return SameBits(a.w, b.w) && SameBits(a.x, b.x) && SameBits(a.y, b.y) &&
         SameBits(a.z, b.z);
}

void ExpectSameScene(const Scene& a, const Scene& b) {
  CHECK(SameBits(a.camera.position, b.camera.position));
  CHECK(SameBits(a.camera.target, b.camera.target));
  CHECK(SameBits(a.camera.vertical_fov_degrees, b.camera.vertical_fov_degrees));
  CHECK(SameBits(a.light.position, b.light.position));
  CHECK(SameBits(a.light.color, b.light.color));
  CHECK(SameBits(a.light.intensity, b.light.intensity));
  CHECK(SameBits(a.ambient, b.ambient));
  CHECK(SameBits(a.gravity, b.gravity));
  // The loader normalizes a stored direction; applying the same operation to
  // the original makes the comparison bit-exact. A default sun is not stored.
  CHECK((SameBits(a.sun.direction, b.sun.direction) ||
         SameBits(glm::normalize(a.sun.direction), b.sun.direction)));
  CHECK(SameBits(a.sun.color, b.sun.color));
  CHECK(SameBits(a.sun.intensity, b.sun.intensity));
  CHECK(a.sun.casts_shadows == b.sun.casts_shadows);
  CHECK(a.environment.hdri_path == b.environment.hdri_path);
  CHECK(SameBits(a.environment.intensity, b.environment.intensity));
  CHECK(
      SameBits(a.environment.rotation_radians, b.environment.rotation_radians));
  CHECK(SameBits(a.environment.exposure_ev, b.environment.exposure_ev));

  REQUIRE(a.entities.size() == b.entities.size());
  for (size_t i = 0; i < a.entities.size(); ++i) {
    REQUIRE(a.entities[i].body.has_value() == b.entities[i].body.has_value());
    if (a.entities[i].body.has_value()) {
      const RigidBody& expected = *a.entities[i].body;
      const RigidBody& actual = *b.entities[i].body;
      CHECK(expected.motion == actual.motion);
      CHECK(expected.shape == actual.shape);
      CHECK(SameBits(expected.mass, actual.mass));
      CHECK(SameBits(expected.friction, actual.friction));
      CHECK(SameBits(expected.restitution, actual.restitution));
    }
    CHECK(a.entities[i].name == b.entities[i].name);
    CHECK(a.entities[i].mesh_path == b.entities[i].mesh_path);
    CHECK(a.entities[i].material_path == b.entities[i].material_path);
    CHECK(SameBits(a.entities[i].transform.position,
                   b.entities[i].transform.position));
    CHECK(SameBits(a.entities[i].transform.rotation,
                   b.entities[i].transform.rotation));
    CHECK(
        SameBits(a.entities[i].transform.scale, b.entities[i].transform.scale));
  }
}

void WriteVec3(BinaryWriter& writer, float x, float y, float z) {
  writer.WriteF32(x);
  writer.WriteF32(y);
  writer.WriteF32(z);
}

// ENVR chunk with default camera/light/ambient values.
void WriteEnvironmentChunk(BinaryWriter& writer) {
  const size_t chunk = writer.BeginChunk(kEnvr);
  WriteVec3(writer, 2.2f, 1.6f, 2.8f);
  WriteVec3(writer, 0.0f, 0.0f, 0.0f);
  writer.WriteF32(60.0f);
  WriteVec3(writer, 2.5f, 3.5f, 1.5f);
  WriteVec3(writer, 1.0f, 0.96f, 0.9f);
  writer.WriteF32(18.0f);
  WriteVec3(writer, 0.025f, 0.025f, 0.025f);
  writer.EndChunk(chunk);
}

std::vector<std::byte> MakeFile(std::span<const std::byte> payload,
                                uint32_t version = kSceneFormatVersion) {
  BinaryWriter file;
  file.WriteBytes(std::as_bytes(std::span(kMagic)));
  file.WriteU32(version);
  file.WriteU32(static_cast<uint32_t>(payload.size()));
  file.WriteU32(Crc32(payload));
  file.WriteBytes(payload);
  return file.bytes();
}

std::vector<std::byte> MakeEmptySceneFile(
    const std::vector<std::byte>& extra_chunks = {}) {
  BinaryWriter payload;
  WriteEnvironmentChunk(payload);
  payload.WriteBytes(extra_chunks);
  const size_t entities = payload.BeginChunk(kEnts);
  payload.WriteU32(0);
  payload.EndChunk(entities);
  return MakeFile(payload.bytes());
}

// One record of a PHYS chunk, field by field, so tests can write invalid ones.
struct PhysRecord {
  uint32_t index = 0;
  uint32_t motion = static_cast<uint32_t>(MotionType::kDynamic);
  uint32_t shape = static_cast<uint32_t>(ShapeType::kBox);
  float mass = 1.0f;
  float friction = 0.5f;
  float restitution = 0.0f;
};

// Appends a PHYS chunk with `gravity`, a record count of `count` (which may
// disagree with `records` on purpose) and `records`.
void WritePhysicsChunk(BinaryWriter& writer, const glm::vec3& gravity,
                       uint32_t count, const std::vector<PhysRecord>& records) {
  const size_t chunk = writer.BeginChunk(kPhys);
  WriteVec3(writer, gravity.x, gravity.y, gravity.z);
  writer.WriteU32(count);
  for (const PhysRecord& record : records) {
    writer.WriteU32(record.index);
    writer.WriteU32(record.motion);
    writer.WriteU32(record.shape);
    writer.WriteF32(record.mass);
    writer.WriteF32(record.friction);
    writer.WriteF32(record.restitution);
  }
  writer.EndChunk(chunk);
}

void WritePhysicsChunk(BinaryWriter& writer,
                       const std::vector<PhysRecord>& records) {
  WritePhysicsChunk(writer, {0.0f, -9.81f, 0.0f},
                    static_cast<uint32_t>(records.size()), records);
}

// ENTS chunk with `count` default-named built-in cube entities.
void WriteEntitiesChunk(BinaryWriter& writer, uint32_t count) {
  const size_t chunk = writer.BeginChunk(kEnts);
  writer.WriteU32(count);
  for (uint32_t i = 0; i < count; ++i) {
    writer.WriteString("e");
    writer.WriteString("builtin:cube");
    WriteVec3(writer, 0.0f, 0.0f, 0.0f);
    writer.WriteF32(0.0f);
    writer.WriteF32(0.0f);
    writer.WriteF32(0.0f);
    writer.WriteF32(1.0f);
    WriteVec3(writer, 1.0f, 1.0f, 1.0f);
  }
  writer.EndChunk(chunk);
}

// Payload with ENVR, `entity_count` entities and the given PHYS chunk.
std::vector<std::byte> MakePhysicsPayload(
    uint32_t entity_count, const std::vector<std::byte>& physics_chunk,
    bool physics_first = false) {
  BinaryWriter payload;
  WriteEnvironmentChunk(payload);
  if (physics_first) {
    payload.WriteBytes(physics_chunk);
  }
  WriteEntitiesChunk(payload, entity_count);
  if (!physics_first) {
    payload.WriteBytes(physics_chunk);
  }
  return payload.bytes();
}

Scene MakeSampleScene() {
  Scene scene;
  scene.camera = {{1.5f, 2.5f, -3.5f}, {0.1f, 0.2f, 0.3f}, 72.5f};
  scene.light = {{-4.0f, 5.0f, 6.0f}, {0.3f, 0.6f, 0.9f}, 7.25f};
  scene.ambient = {0.01f, 0.02f, 0.03f};
  scene.sun = {{0.3f, -0.9f, 0.1f}, {0.8f, 0.7f, 0.6f}, 4.5f, false};
  scene.sun = {{0.3f, -0.9f, 0.1f}, {0.8f, 0.7f, 0.6f}, 4.5f, false};

  scene.entities.push_back({"Cube", "builtin:cube", {}, {}});
  scene.entities.push_back({"Zażółć gęślą jaźń \xF0\x9F\x9A\x80",
                            "models/teapot.obj",
                            {{1.0f, -2.0f, 3.5f},
                             glm::normalize(glm::quat(0.9f, 0.1f, -0.3f, 0.2f)),
                             {2.0f, 0.5f, 1.25f}},
                            {}});
  scene.entities.push_back({"",
                            "builtin:plane",
                            {{0.0f, 0.1f, 0.0f},
                             glm::angleAxis(1.1f, glm::vec3(0.0f, 1.0f, 0.0f)),
                             {10.0f, 1.0f, 10.0f}},
                            {}});
  scene.gravity = {0.5f, -3.711f, 0.25f};
  scene.entities[1].body = RigidBody{.motion = MotionType::kDynamic,
                                     .shape = ShapeType::kConvexHull,
                                     .mass = 12.5f,
                                     .friction = 0.35f,
                                     .restitution = 0.8f};
  scene.entities[2].body = RigidBody{.motion = MotionType::kStatic,
                                     .shape = ShapeType::kTriangleMesh,
                                     .mass = 1.0f,
                                     .friction = 1.25f,
                                     .restitution = 0.0f};
  return scene;
}

}  // namespace

TEST_CASE("round trip preserves every field bit-exactly") {
  const testing::ScratchDirectory dir("scene");
  const std::string file = dir.Child("scene.bscn");

  Scene scene = MakeSampleScene();
  // Make the relative path absolute so it survives the round trip unchanged.
  scene.entities[1].mesh_path = dir.Child("models/teapot.obj");

  REQUIRE(SaveScene(scene, file).ok());
  const auto loaded = LoadScene(file);
  REQUIRE(loaded.ok());
  ExpectSameScene(scene, loaded.value());
}

TEST_CASE("mesh paths are stored relative to the scene file") {
  const testing::ScratchDirectory dir("scene");
  const std::string asset_file = dir.Child("assets/teapot.obj");
  const std::string scene_file = dir.Child("scenes/a.bscn");

  Scene scene;
  scene.entities.push_back({"Teapot", asset_file, {}, {}});
  scene.entities.push_back({"Cube", "builtin:cube", {}, {}});

  REQUIRE(SaveScene(scene, scene_file).ok());

  const std::vector<std::byte> raw = ReadWholeFile(scene_file);
  constexpr std::string_view kRelative = "../assets/teapot.obj";
  const auto found =
      std::ranges::search(raw, std::as_bytes(std::span(kRelative)));
  CHECK_FALSE(found.empty());

  const auto loaded = LoadScene(scene_file);
  REQUIRE(loaded.ok());
  CHECK(loaded.value().entities[0].mesh_path == asset_file);
  CHECK(loaded.value().entities[1].mesh_path == "builtin:cube");

  SUBCASE("moving the whole folder keeps the reference valid") {
    const std::string moved = dir.Child("moved");
    REQUIRE(platform::CreateDirectories(moved));
    REQUIRE(platform::CreateDirectories(dir.Child("assets")));
    REQUIRE(SDL_RenamePath(dir.Child("assets").c_str(),
                           core::JoinPath(moved, "assets").c_str()));
    REQUIRE(SDL_RenamePath(dir.Child("scenes").c_str(),
                           core::JoinPath(moved, "scenes").c_str()));

    const auto reloaded = LoadScene(core::JoinPath(moved, "scenes/a.bscn"));
    REQUIRE(reloaded.ok());
    CHECK(reloaded.value().entities[0].mesh_path ==
          core::JoinPath(moved, "assets/teapot.obj"));
  }
}

TEST_CASE("empty scene round trip") {
  const testing::ScratchDirectory dir("scene");
  const std::string file = dir.Child("empty.bscn");

  const Scene scene;
  REQUIRE(SaveScene(scene, file).ok());
  const auto loaded = LoadScene(file);
  REQUIRE(loaded.ok());
  CHECK(loaded.value().entities.empty());
  ExpectSameScene(scene, loaded.value());
}

TEST_CASE("header and payload validation") {
  const testing::ScratchDirectory dir("scene");
  const std::string file = dir.Child("bad.bscn");

  SUBCASE("bad magic") {
    std::vector<std::byte> bytes = MakeEmptySceneFile();
    bytes[0] = static_cast<std::byte>('X');
    WriteWholeFile(file, bytes);
    const auto loaded = LoadScene(file);
    REQUIRE_FALSE(loaded.ok());
    CHECK((loaded.error() == SceneIoError::kBadMagic));
  }

  SUBCASE("newer version") {
    BinaryWriter payload;
    WriteEnvironmentChunk(payload);
    WriteWholeFile(file, MakeFile(payload.bytes(), kSceneFormatVersion + 1));
    const auto loaded = LoadScene(file);
    REQUIRE_FALSE(loaded.ok());
    CHECK((loaded.error() == SceneIoError::kUnsupportedVersion));
  }

  SUBCASE("flipped payload byte fails the checksum") {
    std::vector<std::byte> bytes = MakeEmptySceneFile();
    bytes.back() ^= std::byte{0x01};
    WriteWholeFile(file, bytes);
    const auto loaded = LoadScene(file);
    REQUIRE_FALSE(loaded.ok());
    CHECK((loaded.error() == SceneIoError::kChecksumMismatch));
  }

  SUBCASE("unknown chunks are skipped") {
    BinaryWriter unknown;
    const size_t chunk = unknown.BeginChunk(MakeFourcc('X', 'T', 'R', 'A'));
    unknown.WriteString("future data");
    unknown.EndChunk(chunk);

    WriteWholeFile(file, MakeEmptySceneFile(unknown.bytes()));
    const auto loaded = LoadScene(file);
    REQUIRE(loaded.ok());
    CHECK(loaded.value().entities.empty());
  }

  SUBCASE("missing required chunk") {
    BinaryWriter payload;
    WriteEnvironmentChunk(payload);
    WriteWholeFile(file, MakeFile(payload.bytes()));
    const auto loaded = LoadScene(file);
    REQUIRE_FALSE(loaded.ok());
    CHECK((loaded.error() == SceneIoError::kCorrupt));
  }

  SUBCASE("non-finite float") {
    BinaryWriter payload;
    const size_t chunk = payload.BeginChunk(kEnvr);
    for (int i = 0; i < 17; ++i) {
      payload.WriteF32(i == 6 ? std::bit_cast<float>(0x7FC00000U) : 1.0f);
    }
    payload.EndChunk(chunk);
    const size_t entities = payload.BeginChunk(kEnts);
    payload.WriteU32(0);
    payload.EndChunk(entities);
    WriteWholeFile(file, MakeFile(payload.bytes()));
    const auto loaded = LoadScene(file);
    REQUIRE_FALSE(loaded.ok());
    CHECK((loaded.error() == SceneIoError::kCorrupt));
  }
}

TEST_CASE("every truncated prefix is rejected") {
  const testing::ScratchDirectory dir("scene");
  const std::string file = dir.Child("full.bscn");
  const std::string prefix = dir.Child("prefix.bscn");

  REQUIRE(SaveScene(MakeSampleScene(), file).ok());
  const std::vector<std::byte> bytes = ReadWholeFile(file);
  REQUIRE(bytes.size() > 20);

  for (size_t length = 0; length < bytes.size(); ++length) {
    WriteWholeFile(prefix, std::span(bytes).first(length));
    const auto loaded = LoadScene(prefix);
    CHECK_FALSE(loaded.ok());
  }
}

TEST_CASE(
    "hostile sizes inside a valid checksum are rejected without allocating") {
  const testing::ScratchDirectory dir("scene");
  const std::string file = dir.Child("hostile.bscn");

  SUBCASE("entity count") {
    BinaryWriter payload;
    WriteEnvironmentChunk(payload);
    const size_t entities = payload.BeginChunk(kEnts);
    payload.WriteU32(1'000'000'000U);
    payload.EndChunk(entities);
    WriteWholeFile(file, MakeFile(payload.bytes()));

    const auto loaded = LoadScene(file);
    REQUIRE_FALSE(loaded.ok());
    CHECK((loaded.error() == SceneIoError::kCorrupt ||
           loaded.error() == SceneIoError::kTruncated));
  }

  SUBCASE("string length") {
    BinaryWriter payload;
    WriteEnvironmentChunk(payload);
    const size_t entities = payload.BeginChunk(kEnts);
    payload.WriteU32(1);
    payload.WriteU32(1'000'000'000U);
    payload.WriteU32(0);
    payload.WriteU32(0);
    payload.WriteU32(0);
    payload.WriteU32(0);
    payload.WriteU32(0);
    payload.WriteU32(0);
    payload.WriteU32(0);
    payload.WriteU32(0);
    payload.WriteU32(0);
    payload.WriteU32(0);
    payload.EndChunk(entities);
    WriteWholeFile(file, MakeFile(payload.bytes()));

    const auto loaded = LoadScene(file);
    REQUIRE_FALSE(loaded.ok());
    CHECK((loaded.error() == SceneIoError::kCorrupt ||
           loaded.error() == SceneIoError::kTruncated));
  }
}

TEST_CASE("save creates missing directories and load reports missing files") {
  const testing::ScratchDirectory dir("scene");
  const std::string nested = dir.Child("a/b/c/scene.bscn");

  REQUIRE(SaveScene(Scene{}, nested).ok());
  CHECK(platform::PathExists(nested));
  CHECK_FALSE(platform::PathExists(nested + ".tmp"));

  const auto missing = LoadScene(dir.Child("nope.bscn"));
  REQUIRE_FALSE(missing.ok());
  CHECK((missing.error() == SceneIoError::kCannotOpen));
}

TEST_CASE("save rejects scenes the loader would refuse") {
  const testing::ScratchDirectory dir("scene");
  const std::string file = dir.Child("invalid.bscn");

  SUBCASE("oversized entity name") {
    Scene scene;
    scene.entities.push_back({std::string(4097, 'a'), "builtin:cube", {}, {}});
    const auto saved = SaveScene(scene, file);
    REQUIRE_FALSE(saved.ok());
    CHECK((saved.error() == SceneIoError::kInvalidScene));
    CHECK_FALSE(platform::PathExists(file));
    CHECK_FALSE(platform::PathExists(file + ".tmp"));
  }

  SUBCASE("non-finite position") {
    Scene scene;
    scene.entities.push_back({"Bad", "builtin:cube", {}, {}});
    scene.entities[0].transform.position.x = std::bit_cast<float>(0x7FC00000U);
    const auto saved = SaveScene(scene, file);
    REQUIRE_FALSE(saved.ok());
    CHECK((saved.error() == SceneIoError::kInvalidScene));
    CHECK_FALSE(platform::PathExists(file));
  }
}

TEST_CASE("physics data round trips and is written only when present") {
  const testing::ScratchDirectory dir("scene");
  const std::string file = dir.Child("physics.bscn");

  SUBCASE("sample scene with bodies and custom gravity") {
    Scene scene = MakeSampleScene();
    scene.entities[1].mesh_path = dir.Child("models/teapot.obj");
    REQUIRE(SaveScene(scene, file).ok());
    const auto loaded = LoadScene(file);
    REQUIRE(loaded.ok());
    ExpectSameScene(scene, loaded.value());
    CHECK_FALSE(loaded.value().entities[0].body.has_value());
    REQUIRE(loaded.value().entities[1].body.has_value());
    CHECK(loaded.value().entities[1].body->mass == 12.5f);
    CHECK((loaded.value().entities[2].body->shape == ShapeType::kTriangleMesh));
  }

  SUBCASE("custom gravity alone") {
    Scene scene;
    scene.gravity = {0.0f, 0.0f, 0.0f};
    REQUIRE(SaveScene(scene, file).ok());
    const auto loaded = LoadScene(file);
    REQUIRE(loaded.ok());
    CHECK(loaded.value().gravity == glm::vec3(0.0f));
  }

  SUBCASE("a scene without physics data has no PHYS chunk") {
    REQUIRE(SaveScene(Scene{}, file).ok());
    const std::vector<std::byte> raw = ReadWholeFile(file);
    constexpr std::string_view kFourcc = "PHYS";
    CHECK(std::ranges::search(raw, std::as_bytes(std::span(kFourcc))).empty());
  }
}

TEST_CASE("files without a PHYS chunk load with no bodies") {
  const testing::ScratchDirectory dir("scene");
  const std::string file = dir.Child("legacy.bscn");
  WriteWholeFile(file, MakeEmptySceneFile());
  const auto loaded = LoadScene(file);
  REQUIRE(loaded.ok());
  CHECK(loaded.value().gravity == Scene().gravity);

  BinaryWriter payload;
  WriteEnvironmentChunk(payload);
  WriteEntitiesChunk(payload, 2);
  WriteWholeFile(file, MakeFile(payload.bytes()));
  const auto with_entities = LoadScene(file);
  REQUIRE(with_entities.ok());
  REQUIRE(with_entities.value().entities.size() == 2);
  CHECK_FALSE(with_entities.value().entities[0].body.has_value());
  CHECK_FALSE(with_entities.value().entities[1].body.has_value());
  CHECK(with_entities.value().gravity == Scene().gravity);
}

TEST_CASE("PHYS may precede ENTS in the payload") {
  const testing::ScratchDirectory dir("scene");
  const std::string file = dir.Child("order.bscn");

  BinaryWriter physics;
  WritePhysicsChunk(physics, {0.0f, -1.5f, 0.0f}, 1,
                    {{.index = 1, .mass = 4.0f}});
  WriteWholeFile(file, MakeFile(MakePhysicsPayload(2, physics.bytes(),
                                                   /*physics_first=*/true)));
  const auto loaded = LoadScene(file);
  REQUIRE(loaded.ok());
  CHECK(loaded.value().gravity == glm::vec3(0.0f, -1.5f, 0.0f));
  CHECK_FALSE(loaded.value().entities[0].body.has_value());
  REQUIRE(loaded.value().entities[1].body.has_value());
  CHECK(loaded.value().entities[1].body->mass == 4.0f);
}

TEST_CASE("malformed PHYS chunks are rejected as corrupt") {
  const testing::ScratchDirectory dir("scene");
  const std::string file = dir.Child("bad_physics.bscn");

  // Loads a file with two entities and `physics`; expects kCorrupt.
  const auto expect_corrupt = [&](const BinaryWriter& physics) {
    WriteWholeFile(file, MakeFile(MakePhysicsPayload(2, physics.bytes())));
    const auto loaded = LoadScene(file);
    REQUIRE_FALSE(loaded.ok());
    CHECK((loaded.error() == SceneIoError::kCorrupt));
  };
  const auto expect_record_corrupt = [&](const PhysRecord& record) {
    BinaryWriter physics;
    WritePhysicsChunk(physics, {record});
    expect_corrupt(physics);
  };

  SUBCASE("entity index out of range") { expect_record_corrupt({.index = 2}); }

  SUBCASE("duplicate entity index") {
    BinaryWriter physics;
    WritePhysicsChunk(physics, {{.index = 1}, {.index = 1}});
    expect_corrupt(physics);
  }

  SUBCASE("motion out of range") { expect_record_corrupt({.motion = 2}); }

  SUBCASE("shape out of range") { expect_record_corrupt({.shape = 4}); }

  SUBCASE("non-positive dynamic mass") {
    expect_record_corrupt({.mass = 0.0f});
  }

  SUBCASE("non-finite mass") {
    expect_record_corrupt({.mass = std::bit_cast<float>(kNanBits)});
  }

  SUBCASE("negative friction") { expect_record_corrupt({.friction = -0.1f}); }

  SUBCASE("restitution above one") {
    expect_record_corrupt({.restitution = 1.5f});
  }

  SUBCASE("dynamic triangle mesh") {
    expect_record_corrupt(
        {.shape = static_cast<uint32_t>(ShapeType::kTriangleMesh)});
  }

  SUBCASE("non-finite gravity") {
    BinaryWriter physics;
    WritePhysicsChunk(physics, {std::bit_cast<float>(kNanBits), 0.0f, 0.0f}, 0,
                      {});
    expect_corrupt(physics);
  }

  SUBCASE("count larger than the chunk") {
    BinaryWriter physics;
    WritePhysicsChunk(physics, {0.0f, -9.81f, 0.0f}, 1'000'000'000U,
                      {{.index = 0}});
    expect_corrupt(physics);
  }

  SUBCASE("count smaller than the records") {
    BinaryWriter physics;
    WritePhysicsChunk(physics, {0.0f, -9.81f, 0.0f}, 1,
                      {{.index = 0}, {.index = 1}});
    expect_corrupt(physics);
  }

  SUBCASE("duplicate PHYS chunk") {
    BinaryWriter physics;
    WritePhysicsChunk(physics, {{.index = 0}});
    WritePhysicsChunk(physics, {{.index = 1}});
    expect_corrupt(physics);
  }
}

TEST_CASE("save rejects invalid physics data without writing anything") {
  const testing::ScratchDirectory dir("scene");
  const std::string file = dir.Child("invalid_physics.bscn");

  Scene scene;
  scene.entities.push_back({"Body", "builtin:cube", {}, {}});
  scene.entities[0].body = RigidBody{};

  SUBCASE("non-positive dynamic mass") { scene.entities[0].body->mass = 0.0f; }

  SUBCASE("non-finite friction") {
    scene.entities[0].body->friction = std::bit_cast<float>(kNanBits);
  }

  SUBCASE("restitution out of range") {
    scene.entities[0].body->restitution = -0.5f;
  }

  SUBCASE("dynamic triangle mesh") {
    scene.entities[0].body->shape = ShapeType::kTriangleMesh;
  }

  SUBCASE("non-finite gravity") {
    scene.gravity.y = std::bit_cast<float>(kNanBits);
  }

  const auto saved = SaveScene(scene, file);
  REQUIRE_FALSE(saved.ok());
  CHECK((saved.error() == SceneIoError::kInvalidScene));
  CHECK_FALSE(platform::PathExists(file));
  CHECK_FALSE(platform::PathExists(file + ".tmp"));
}

TEST_CASE("load rejects oversized files before reading them") {
  const testing::ScratchDirectory dir("scene");
  const std::string file = dir.Child("huge.bscn");
  const std::vector<std::byte> oversized(64ULL * 1024 * 1024 + 1);
  WriteWholeFile(file, oversized);
  const auto loaded = LoadScene(file);
  REQUIRE_FALSE(loaded.ok());
  CHECK((loaded.error() == SceneIoError::kCorrupt));
}

}  // namespace blue::scene

namespace blue::scene {
namespace {

// Raw fields of a SUN chunk, for building valid and malformed ones.
struct SunFields {
  glm::vec3 direction{0.0f, -1.0f, 0.0f};
  glm::vec3 color{1.0f, 0.5f, 0.25f};
  float intensity = 2.0f;
  uint32_t flags = 1;
};

void WriteSunChunk(BinaryWriter& writer, const SunFields& fields) {
  const size_t chunk = writer.BeginChunk(kSun);
  WriteVec3(writer, fields.direction.x, fields.direction.y, fields.direction.z);
  WriteVec3(writer, fields.color.x, fields.color.y, fields.color.z);
  writer.WriteF32(fields.intensity);
  writer.WriteU32(fields.flags);
  writer.EndChunk(chunk);
}

// Payload with ENVR, no entities and the given extra chunks.
std::vector<std::byte> MakeSunPayload(std::span<const std::byte> sun_chunks) {
  BinaryWriter payload;
  WriteEnvironmentChunk(payload);
  WriteEntitiesChunk(payload, 0);
  payload.WriteBytes(sun_chunks);
  return payload.bytes();
}

}  // namespace

TEST_CASE("sun round trips and is written only when it is not the default") {
  const testing::ScratchDirectory dir("scene");
  const std::string file = dir.Child("sun.bscn");
  constexpr std::string_view kFourcc = "SUN ";

  SUBCASE("custom sun") {
    Scene scene;
    scene.sun = {{0.3f, -0.9f, 0.1f}, {0.8f, 0.7f, 0.6f}, 4.5f, false};
    REQUIRE(SaveScene(scene, file).ok());
    const auto loaded = LoadScene(file);
    REQUIRE(loaded.ok());
    ExpectSameScene(scene, loaded.value());
    CHECK(glm::length(loaded.value().sun.direction) ==
          doctest::Approx(1.0f).epsilon(1e-6));
    CHECK_FALSE(loaded.value().sun.casts_shadows);
  }

  SUBCASE("a non-normalized direction is normalized on load") {
    Scene scene;
    scene.sun.direction = {0.0f, -5.0f, 0.0f};
    REQUIRE(SaveScene(scene, file).ok());
    const auto loaded = LoadScene(file);
    REQUIRE(loaded.ok());
    CHECK(loaded.value().sun.direction == glm::vec3(0.0f, -1.0f, 0.0f));
  }

  SUBCASE("the default sun has no SUN chunk") {
    REQUIRE(SaveScene(Scene{}, file).ok());
    const std::vector<std::byte> raw = ReadWholeFile(file);
    CHECK(std::ranges::search(raw, std::as_bytes(std::span(kFourcc))).empty());
  }

  SUBCASE("a sun differing only in the shadow flag is written") {
    Scene scene;
    scene.sun.casts_shadows = false;
    REQUIRE(SaveScene(scene, file).ok());
    const auto loaded = LoadScene(file);
    REQUIRE(loaded.ok());
    CHECK_FALSE(loaded.value().sun.casts_shadows);
  }
}

TEST_CASE("files without a SUN chunk load with the default sun") {
  const testing::ScratchDirectory dir("scene");
  const std::string file = dir.Child("no_sun.bscn");
  WriteWholeFile(file, MakeEmptySceneFile());
  const auto loaded = LoadScene(file);
  REQUIRE(loaded.ok());
  const SunLight defaults;
  CHECK(loaded.value().sun.direction == defaults.direction);
  CHECK(loaded.value().sun.color == defaults.color);
  CHECK(loaded.value().sun.intensity == defaults.intensity);
  CHECK(loaded.value().sun.casts_shadows == defaults.casts_shadows);
}

TEST_CASE("a valid hand-built SUN chunk loads") {
  const testing::ScratchDirectory dir("scene");
  const std::string file = dir.Child("hand_sun.bscn");
  BinaryWriter sun;
  WriteSunChunk(sun, {});
  WriteWholeFile(file, MakeFile(MakeSunPayload(sun.bytes())));
  const auto loaded = LoadScene(file);
  REQUIRE(loaded.ok());
  CHECK(loaded.value().sun.color == glm::vec3(1.0f, 0.5f, 0.25f));
  CHECK(loaded.value().sun.intensity == 2.0f);
  CHECK(loaded.value().sun.casts_shadows);
}

TEST_CASE("malformed SUN chunks are rejected as corrupt") {
  const testing::ScratchDirectory dir("scene");
  const std::string file = dir.Child("bad_sun.bscn");
  const float nan = std::bit_cast<float>(kNanBits);

  const auto expect_corrupt = [&](const BinaryWriter& sun) {
    WriteWholeFile(file, MakeFile(MakeSunPayload(sun.bytes())));
    const auto loaded = LoadScene(file);
    REQUIRE_FALSE(loaded.ok());
    CHECK((loaded.error() == SceneIoError::kCorrupt));
  };
  const auto expect_fields_corrupt = [&](const SunFields& fields) {
    BinaryWriter sun;
    WriteSunChunk(sun, fields);
    expect_corrupt(sun);
  };

  SUBCASE("non-finite direction") {
    expect_fields_corrupt({.direction = {nan, -1.0f, 0.0f}});
  }

  SUBCASE("zero direction") {
    expect_fields_corrupt({.direction = {0.0f, 0.0f, 0.0f}});
  }

  SUBCASE("negative color") {
    expect_fields_corrupt({.color = {1.0f, -0.1f, 1.0f}});
  }

  SUBCASE("non-finite color") {
    expect_fields_corrupt({.color = {1.0f, nan, 1.0f}});
  }

  SUBCASE("negative intensity") { expect_fields_corrupt({.intensity = -1.0f}); }

  SUBCASE("non-finite intensity") { expect_fields_corrupt({.intensity = nan}); }

  SUBCASE("unknown flag bits") {
    expect_fields_corrupt({.flags = 2});
    expect_fields_corrupt({.flags = 0x80000001U});
  }

  SUBCASE("chunk too short") {
    BinaryWriter sun;
    const size_t chunk = sun.BeginChunk(kSun);
    WriteVec3(sun, 0.0f, -1.0f, 0.0f);
    sun.EndChunk(chunk);
    expect_corrupt(sun);
  }

  SUBCASE("chunk too long") {
    BinaryWriter sun;
    const size_t chunk = sun.BeginChunk(kSun);
    WriteVec3(sun, 0.0f, -1.0f, 0.0f);
    WriteVec3(sun, 1.0f, 1.0f, 1.0f);
    sun.WriteF32(1.0f);
    sun.WriteU32(1);
    sun.WriteU32(0);
    sun.EndChunk(chunk);
    expect_corrupt(sun);
  }

  SUBCASE("duplicate SUN chunk") {
    BinaryWriter sun;
    WriteSunChunk(sun, {});
    WriteSunChunk(sun, {});
    expect_corrupt(sun);
  }
}

TEST_CASE("save rejects an invalid sun without writing anything") {
  const testing::ScratchDirectory dir("scene");
  const std::string file = dir.Child("invalid_sun.bscn");
  const float nan = std::bit_cast<float>(kNanBits);

  Scene scene;
  SUBCASE("non-finite direction") { scene.sun.direction.x = nan; }
  SUBCASE("zero direction") { scene.sun.direction = glm::vec3(0.0f); }
  SUBCASE("negative color") { scene.sun.color.z = -0.5f; }
  SUBCASE("non-finite color") { scene.sun.color.y = nan; }
  SUBCASE("negative intensity") { scene.sun.intensity = -2.0f; }
  SUBCASE("non-finite intensity") { scene.sun.intensity = nan; }

  const auto saved = SaveScene(scene, file);
  REQUIRE_FALSE(saved.ok());
  CHECK((saved.error() == SceneIoError::kInvalidScene));
  CHECK_FALSE(platform::PathExists(file));
  CHECK_FALSE(platform::PathExists(file + ".tmp"));
}

namespace {

// Appends a MATS chunk with a record count of `count` and the given
// (entity index, stored path) records.
void WriteMaterialsChunk(
    BinaryWriter& writer, uint32_t count,
    const std::vector<std::pair<uint32_t, std::string>>& records) {
  const size_t chunk = writer.BeginChunk(kMats);
  writer.WriteU32(count);
  for (const auto& [index, path] : records) {
    writer.WriteU32(index);
    writer.WriteString(path);
  }
  writer.EndChunk(chunk);
}

// Raw fields of an IBL chunk.
struct IblFields {
  std::string hdri_path = "sky.hdr";
  float intensity = 1.5f;
  float rotation = 0.25f;
  float exposure = -1.0f;
};

void WriteIblChunk(BinaryWriter& writer, const IblFields& fields) {
  const size_t chunk = writer.BeginChunk(kIbl);
  writer.WriteString(fields.hdri_path);
  writer.WriteF32(fields.intensity);
  writer.WriteF32(fields.rotation);
  writer.WriteF32(fields.exposure);
  writer.EndChunk(chunk);
}

// Payload with ENVR, `entity_count` entities and `extra` chunks before or
// after ENTS.
std::vector<std::byte> MakeExtraPayload(uint32_t entity_count,
                                        std::span<const std::byte> extra,
                                        bool extra_first = false) {
  BinaryWriter payload;
  WriteEnvironmentChunk(payload);
  if (extra_first) {
    payload.WriteBytes(extra);
  }
  WriteEntitiesChunk(payload, entity_count);
  if (!extra_first) {
    payload.WriteBytes(extra);
  }
  return payload.bytes();
}

}  // namespace

TEST_CASE("material paths and the environment round trip") {
  const testing::ScratchDirectory dir("scene");
  const std::string file = dir.Child("scenes/lit.bscn");

  Scene scene = MakeSampleScene();
  scene.entities[1].mesh_path = dir.Child("models/teapot.obj");
  scene.entities[0].material_path = dir.Child("materials/steel.bmat");
  scene.entities[2].material_path = dir.Child("materials/ground.bmat");
  scene.environment = {.hdri_path = dir.Child("env/sky.hdr"),
                       .intensity = 0.75f,
                       .rotation_radians = 1.25f,
                       .exposure_ev = -0.5f};

  REQUIRE(SaveScene(scene, file).ok());

  const std::vector<std::byte> raw = ReadWholeFile(file);
  for (const std::string_view stored :
       {"../materials/steel.bmat", "../env/sky.hdr"}) {
    const std::string text(stored);
    CAPTURE(text);
    CHECK_FALSE(
        std::ranges::search(raw, std::as_bytes(std::span(stored))).empty());
  }
  const auto loaded = LoadScene(file);
  REQUIRE(loaded.ok());
  ExpectSameScene(scene, loaded.value());
}

TEST_CASE("MATS and IBL chunks are written only when not default") {
  const testing::ScratchDirectory dir("scene");
  const std::string file = dir.Child("plain.bscn");
  Scene scene = MakeSampleScene();
  REQUIRE(SaveScene(scene, file).ok());
  const std::vector<std::byte> raw = ReadWholeFile(file);
  for (const std::string_view fourcc : {"MATS", "IBL "}) {
    const std::string text(fourcc);
    CAPTURE(text);
    CHECK(std::ranges::search(raw, std::as_bytes(std::span(fourcc))).empty());
  }

  SUBCASE("an environment differing only in exposure is written") {
    scene.environment.exposure_ev = 1.0f;
    REQUIRE(SaveScene(scene, file).ok());
    const auto loaded = LoadScene(file);
    REQUIRE(loaded.ok());
    CHECK(loaded.value().environment.exposure_ev == 1.0f);
    CHECK(loaded.value().environment.hdri_path.empty());
  }
}

TEST_CASE("files without MATS and IBL load with defaults") {
  const testing::ScratchDirectory dir("scene");
  const std::string file = dir.Child("old.bscn");
  WriteWholeFile(file, MakeFile(MakeExtraPayload(2, {})));
  const auto loaded = LoadScene(file);
  REQUIRE(loaded.ok());
  for (const Entity& entity : loaded.value().entities) {
    CHECK(entity.material_path.empty());
  }
  const Environment defaults;
  CHECK(loaded.value().environment.hdri_path.empty());
  CHECK(loaded.value().environment.intensity == defaults.intensity);
  CHECK(loaded.value().environment.rotation_radians ==
        defaults.rotation_radians);
  CHECK(loaded.value().environment.exposure_ev == defaults.exposure_ev);
}

TEST_CASE("hand-built MATS and IBL chunks load, MATS before ENTS too") {
  const testing::ScratchDirectory dir("scene");
  const std::string file = dir.Child("hand.bscn");
  BinaryWriter extra;
  WriteMaterialsChunk(extra, 2, {{0, "a.bmat"}, {2, "sub/b.bmat"}});
  WriteIblChunk(extra, {});

  for (const bool first : {false, true}) {
    CAPTURE(first);
    WriteWholeFile(file, MakeFile(MakeExtraPayload(3, extra.bytes(), first)));
    const auto loaded = LoadScene(file);
    REQUIRE(loaded.ok());
    const std::string scene_dir =
        core::ParentPath(platform::AbsolutePath(file));
    CHECK(loaded.value().entities[0].material_path ==
          core::JoinPath(scene_dir, "a.bmat"));
    CHECK(loaded.value().entities[1].material_path.empty());
    CHECK(loaded.value().entities[2].material_path ==
          core::JoinPath(scene_dir, "sub/b.bmat"));
    CHECK(loaded.value().environment.hdri_path ==
          core::JoinPath(scene_dir, "sky.hdr"));
    CHECK(loaded.value().environment.intensity == 1.5f);
    CHECK(loaded.value().environment.rotation_radians == 0.25f);
    CHECK(loaded.value().environment.exposure_ev == -1.0f);
  }
}

TEST_CASE("malformed MATS and IBL chunks are rejected as corrupt") {
  const testing::ScratchDirectory dir("scene");
  const std::string file = dir.Child("bad.bscn");
  const float nan = std::bit_cast<float>(kNanBits);

  const auto expect_corrupt = [&](const BinaryWriter& extra) {
    WriteWholeFile(file, MakeFile(MakeExtraPayload(2, extra.bytes())));
    const auto loaded = LoadScene(file);
    REQUIRE_FALSE(loaded.ok());
    CHECK((loaded.error() == SceneIoError::kCorrupt ||
           loaded.error() == SceneIoError::kTruncated));
  };
  BinaryWriter extra;

  SUBCASE("MATS entity index out of range") {
    WriteMaterialsChunk(extra, 1, {{2, "a.bmat"}});
  }
  SUBCASE("MATS duplicate entity") {
    WriteMaterialsChunk(extra, 2, {{1, "a.bmat"}, {1, "b.bmat"}});
  }
  SUBCASE("MATS empty path") { WriteMaterialsChunk(extra, 1, {{0, ""}}); }
  SUBCASE("MATS hostile count") {
    WriteMaterialsChunk(extra, 0xFFFFFFFFU, {{0, "a.bmat"}});
  }
  SUBCASE("MATS count below the records") {
    WriteMaterialsChunk(extra, 1, {{0, "a.bmat"}, {1, "b.bmat"}});
  }
  SUBCASE("duplicate MATS chunk") {
    WriteMaterialsChunk(extra, 1, {{0, "a.bmat"}});
    WriteMaterialsChunk(extra, 1, {{1, "b.bmat"}});
  }
  SUBCASE("IBL negative intensity") {
    WriteIblChunk(extra, {.intensity = -1.0f});
  }
  SUBCASE("IBL non-finite values") { WriteIblChunk(extra, {.rotation = nan}); }
  SUBCASE("IBL truncated") {
    const size_t chunk = extra.BeginChunk(kIbl);
    extra.WriteString("sky.hdr");
    extra.WriteF32(1.0f);
    extra.EndChunk(chunk);
  }
  SUBCASE("IBL too long") {
    const size_t chunk = extra.BeginChunk(kIbl);
    extra.WriteString("");
    for (int i = 0; i < 4; ++i) {
      extra.WriteF32(1.0f);
    }
    extra.EndChunk(chunk);
  }
  SUBCASE("duplicate IBL chunk") {
    WriteIblChunk(extra, {});
    WriteIblChunk(extra, {});
  }
  expect_corrupt(extra);
}

TEST_CASE("save rejects an invalid environment without writing anything") {
  const testing::ScratchDirectory dir("scene");
  const std::string file = dir.Child("invalid_env.bscn");
  const float nan = std::bit_cast<float>(kNanBits);

  Scene scene;
  SUBCASE("negative intensity") { scene.environment.intensity = -0.5f; }
  SUBCASE("non-finite rotation") { scene.environment.rotation_radians = nan; }
  SUBCASE("non-finite exposure") { scene.environment.exposure_ev = nan; }
  SUBCASE("oversized material path") {
    scene.entities.push_back(
        {.name = "e",
         .mesh_path = "builtin:cube",
         .material_path = dir.Child(std::string(5000, 'm'))});
  }

  const auto saved = SaveScene(scene, file);
  REQUIRE_FALSE(saved.ok());
  CHECK((saved.error() == SceneIoError::kInvalidScene));
  CHECK_FALSE(platform::PathExists(file));
}

}  // namespace blue::scene
