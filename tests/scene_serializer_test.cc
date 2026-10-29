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

  scene.entities.push_back({"Cube", "builtin:cube", {}});
  scene.entities.push_back({"Zażółć gęślą jaźń \xF0\x9F\x9A\x80",
                            "models/teapot.obj",
                            {{1.0f, -2.0f, 3.5f},
                             glm::normalize(glm::quat(0.9f, 0.1f, -0.3f, 0.2f)),
                             {2.0f, 0.5f, 1.25f}}});
  scene.entities.push_back({"",
                            "builtin:plane",
                            {{0.0f, 0.1f, 0.0f},
                             glm::angleAxis(1.1f, glm::vec3(0.0f, 1.0f, 0.0f)),
                             {10.0f, 1.0f, 10.0f}}});
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
  scene.entities.push_back({"Teapot", asset_file, {}});
  scene.entities.push_back({"Cube", "builtin:cube", {}});

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
    scene.entities.push_back({std::string(4097, 'a'), "builtin:cube", {}});
    const auto saved = SaveScene(scene, file);
    REQUIRE_FALSE(saved.ok());
    CHECK((saved.error() == SceneIoError::kInvalidScene));
    CHECK_FALSE(platform::PathExists(file));
    CHECK_FALSE(platform::PathExists(file + ".tmp"));
  }

  SUBCASE("non-finite position") {
    Scene scene;
    scene.entities.push_back({"Bad", "builtin:cube", {}});
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
  scene.entities.push_back({"Body", "builtin:cube", {}});
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
