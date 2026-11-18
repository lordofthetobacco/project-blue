#include "prefab/prefab.h"

#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "asset/mesh_data.h"
#include "tests/prefab_samples.h"

namespace blue::prefab {
namespace {

bool IsAligned(const void* pointer) {
  return std::bit_cast<uintptr_t>(pointer) % kBulkAlignment == 0;
}

// Returns the first problem ValidatePrefab() reports, or "" if valid.
std::string Problem(const Prefab& prefab) {
  return ValidatePrefab(prefab).value_or("");
}

}  // namespace

TEST_CASE("BulkStorage stores aligned blocks that copies share") {
  BulkStorage storage;
  const std::vector<std::byte> bytes = testing::PatternBytes(33, 1);
  const std::span<const std::byte> stored = storage.Store(bytes);
  REQUIRE(stored.size() == bytes.size());
  CHECK(IsAligned(stored.data()));
  CHECK(std::equal(stored.begin(), stored.end(), bytes.begin()));
  CHECK(storage.Store({}).empty());
  CHECK(storage.block_count() == 1);

  const std::vector<float> floats = {1.0f, 2.0f, 3.0f};
  const std::span<const float> typed = storage.StoreArray<float>(floats);
  CHECK(typed.size() == 3);
  CHECK(typed[2] == 3.0f);
  CHECK(storage.byte_size() == 33 + 12);

  // A copy keeps the blocks alive after the original is gone.
  std::optional<BulkStorage> original(std::move(storage));
  const BulkStorage copy = *original;
  original.reset();
  CHECK(copy.block_count() == 2);
  CHECK(stored[5] == bytes[5]);
}

TEST_CASE("Texture format sizes") {
  CHECK(IsBlockCompressed(TextureFormat::kBc7Srgb));
  CHECK(IsBlockCompressed(TextureFormat::kBc5Unorm));
  CHECK_FALSE(IsBlockCompressed(TextureFormat::kRgba8Srgb));
  // BC levels round up to whole 4x4 blocks of 16 bytes.
  CHECK(MipByteSize(TextureFormat::kBc7Unorm, 4, 4) == 16);
  CHECK(MipByteSize(TextureFormat::kBc7Unorm, 5, 1) == 32);
  CHECK(MipByteSize(TextureFormat::kBc5Unorm, 1, 1) == 16);
  CHECK(MipByteSize(TextureFormat::kRgba8Unorm, 3, 2) == 24);
  CHECK(FullMipCount(1, 1) == 1);
  CHECK(FullMipCount(2048, 2048) == 12);
  CHECK(FullMipCount(20, 12) == 5);
  CHECK(FullMipCount(1, 300) == 9);
}

TEST_CASE("The sample prefab is valid and lookups find its parts") {
  const Prefab prefab = testing::MakeSamplePrefab();
  CHECK(Problem(prefab).empty());
  CHECK(FindSocket(prefab, "muzzle") == 0);
  CHECK(FindSocket(prefab, "sight") == -1);
  CHECK(FindClip(prefab, "fire") == 0);
  CHECK(FindMaterial(prefab, "grip") == 1);
  CHECK(FindMaterial(prefab, "wood") == -1);
  CHECK(prefab.materials[0].texture(TextureSlot::kNormal) == 1);
  CHECK(Problem(Prefab{}).empty());
}

TEST_CASE("ValidatePrefab reports broken references and values") {
  const Prefab sample = testing::MakeSamplePrefab();

  SUBCASE("hierarchy order") {
    Prefab prefab = sample;
    prefab.hierarchy.nodes[0].parent = 1;
    CHECK(Problem(prefab).find("hierarchy") != std::string::npos);
  }
  SUBCASE("mesh node") {
    Prefab prefab = sample;
    prefab.meshes[0].node = 2;
    CHECK(Problem(prefab).find("mesh") != std::string::npos);
  }
  SUBCASE("index out of range") {
    Prefab prefab = sample;
    std::vector<asset::Index> indices(prefab.meshes[0].indices.begin(),
                                      prefab.meshes[0].indices.end());
    indices[4] = static_cast<asset::Index>(prefab.meshes[0].vertices.size());
    prefab.meshes[0].indices =
        prefab.storage.StoreArray<asset::Index>(std::span(indices));
    CHECK(Problem(prefab).find("index out of range") != std::string::npos);
  }
  SUBCASE("submesh past the indices") {
    Prefab prefab = sample;
    prefab.meshes[0].submeshes[1].index_count += 3;
    CHECK(Problem(prefab).find("submesh") != std::string::npos);
  }
  SUBCASE("submesh material") {
    Prefab prefab = sample;
    prefab.meshes[0].submeshes[0].material = 2;
    CHECK_FALSE(Problem(prefab).empty());
  }
  SUBCASE("material texture and UV set") {
    Prefab prefab = sample;
    prefab.materials[0].textures[1] = 2;
    CHECK(Problem(prefab).find("steel") != std::string::npos);
    prefab = sample;
    prefab.materials[0].uv_sets[0] = 2;
    CHECK(Problem(prefab).find("UV set") != std::string::npos);
  }
  SUBCASE("non-finite material value") {
    Prefab prefab = sample;
    prefab.materials[1].roughness = std::nanf("");
    CHECK(Problem(prefab).find("grip") != std::string::npos);
  }
  SUBCASE("texture mip size") {
    Prefab prefab = sample;
    prefab.textures[0].mips[1].data = prefab.textures[0].mips[0].data;
    CHECK(Problem(prefab).find("mip 1") != std::string::npos);
  }
  SUBCASE("too many mips") {
    Prefab prefab = sample;
    TextureMip extra = prefab.textures[1].mips.back();
    prefab.textures[1].mips.push_back(extra);
    CHECK(Problem(prefab).find("mip count") != std::string::npos);
  }
  SUBCASE("clip channel node") {
    Prefab prefab = sample;
    prefab.clips[0].channels[0].node = 7;
    CHECK(Problem(prefab).find("fire") != std::string::npos);
  }
  SUBCASE("clip events unsorted") {
    Prefab prefab = sample;
    std::swap(prefab.clips[0].events[0], prefab.clips[0].events[1]);
    CHECK(Problem(prefab).find("fire") != std::string::npos);
  }
  SUBCASE("socket, light and child") {
    Prefab prefab = sample;
    prefab.sockets[0].node = -2;
    CHECK(Problem(prefab).find("muzzle") != std::string::npos);
    prefab = sample;
    prefab.lights[0].range = 0.0f;
    CHECK(Problem(prefab).find("lamp") != std::string::npos);
    prefab = sample;
    prefab.children[0].path.clear();
    CHECK(Problem(prefab).find("child") != std::string::npos);
  }
  SUBCASE("physics") {
    Prefab prefab = sample;
    prefab.physics.cooked_shape = {};
    CHECK(Problem(prefab).find("cooked shape") != std::string::npos);
    prefab = sample;
    prefab.physics.kind = ColliderKind::kTriangleMesh;
    CHECK(Problem(prefab).find("dynamic") != std::string::npos);
    prefab = sample;
    prefab.physics.restitution = 2.0f;
    CHECK(Problem(prefab).find("physics") != std::string::npos);
  }
  SUBCASE("string length") {
    Prefab prefab = sample;
    prefab.sockets[0].name.assign(kMaxStringLength + 1, 'x');
    CHECK_FALSE(Problem(prefab).empty());
  }
}

}  // namespace blue::prefab
