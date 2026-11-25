#include "sim/weapon_table.h"

#include <doctest/doctest.h>

#include <cstddef>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <glm/vec3.hpp>

#include "asset/mesh_data.h"
#include "core/result.h"
#include "platform/file_system.h"
#include "prefab/prefab.h"
#include "prefab/prefab_file.h"
#include "tests/scratch_directory.h"
#include "weapon/parts/part_catalog.h"
#include "weapon/parts/part_catalog_file.h"
#include "weapon/weapon_def.h"
#include "world/core/prefab_id.h"
#include "world/core/prefab_source.h"
#include "world/core/prefab_store.h"

namespace blue::sim {
namespace {

namespace parts = weapon::parts;

constexpr std::string_view kCatalogText = R"([part frame]
category = lower_receiver
prefab = parts/frame.pbo
weight = 0.3
ergonomics = 91
weapon_kind = pistol
recoil_vertical = 311
recoil_horizontal = 260
fire_modes = semi
fire_rate = 1100
slot.slide = required slide

[part slide]
category = slide
prefab = parts/slide.pbo
weight = 0.3
slot.sight_rear = optional rear

[part rear]
category = sight_rear
prefab = parts/rear.pbo
sighting_range = 50

[part whole]
category = complete
fire_modes = semi
fire_rate = 60
legacy_type = spr_a3g

[preset pistol]
name = Test pistol
root = frame
slide = slide
slide.sight_rear = rear

[preset bare]
name = Bare frame
root = frame

[preset whole]
root = whole
)";

// A PrefabSource like the game's PrefabCache (loads files, keys them by
// absolute path) that hands out other ids than a PrefabStore does: ids are
// process-local and must not leak into keys or hashes.
class ShiftedSource final : public world::PrefabSource {
 public:
  ShiftedSource() {
    for (int i = 0; i < 5; ++i) {
      entries_.push_back({.key = "<unused>", .prefab = {}});
    }
  }

  core::Result<world::PrefabId, prefab::PrefabError> Get(
      std::string_view path) override {
    using GetResult = core::Result<world::PrefabId, prefab::PrefabError>;
    std::string key = platform::AbsolutePath(path);
    if (auto found = by_key_.find(key); found != by_key_.end()) {
      return GetResult::Ok(found->second);
    }
    auto loaded = prefab::LoadPrefab(key);
    if (!loaded.ok()) {
      return GetResult::Error(loaded.error());
    }
    const auto id = static_cast<world::PrefabId>(entries_.size());
    entries_.push_back({.key = key, .prefab = std::move(loaded).value()});
    by_key_.emplace(std::move(key), id);
    return GetResult::Ok(id);
  }
  const prefab::Prefab& prefab(world::PrefabId id) const override {
    return entries_[static_cast<size_t>(id)].prefab;
  }
  const std::string& key(world::PrefabId id) const override {
    return entries_[static_cast<size_t>(id)].key;
  }

 private:
  struct Entry {
    std::string key;
    prefab::Prefab prefab;
  };
  std::deque<Entry> entries_;
  std::unordered_map<std::string, world::PrefabId> by_key_;
};

prefab::Socket MakeSocket(std::string name, glm::vec3 position) {
  prefab::Socket socket;
  socket.name = std::move(name);
  socket.local.translation = position;
  return socket;
}

// A unit cube model on one node with `sockets`.
prefab::Prefab MakeModel(std::vector<prefab::Socket> sockets) {
  prefab::Prefab prefab;
  prefab.hierarchy.nodes = {{.name = "root", .parent = -1, .rest = {}}};
  const asset::MeshData cube = asset::MakeCube();
  prefab::Mesh mesh{.name = "cube", .node = 0, .bounds = cube.bounds};
  mesh.vertices =
      prefab.storage.StoreArray<asset::Vertex>(std::span(cube.vertices));
  mesh.indices =
      prefab.storage.StoreArray<asset::Index>(std::span(cube.indices));
  mesh.submeshes.push_back(
      {.first_index = 0,
       .index_count = static_cast<uint32_t>(cube.indices.size()),
       .material = 0});
  prefab.materials.push_back({.name = "steel"});
  prefab.meshes.push_back(std::move(mesh));
  prefab.bounds = cube.bounds;
  prefab.sockets = std::move(sockets);
  return prefab;
}

// Returns a single-piece weapon model tagged `weapon = type`.
prefab::Prefab MakeWholeWeapon(std::string type) {
  prefab::Prefab prefab = MakeModel({MakeSocket("muzzle", {0.0f, 0.0f, 0.5f}),
                                     MakeSocket("sight", {0.0f, 0.1f, 0.0f}),
                                     MakeSocket("grip", {0.0f, -0.3f, 0.0f})});
  prefab.tags.push_back({.key = "weapon", .value = std::move(type)});
  return prefab;
}

parts::PartCatalog ParseCatalog(std::string_view text) {
  auto file = parts::ParsePartCatalog(text, "test.cfg");
  REQUIRE(file.ok());
  const std::vector<parts::PartCatalogFile> files{std::move(file).value()};
  auto catalog = parts::MergePartCatalogs(files);
  REQUIRE(catalog.ok());
  return std::move(catalog).value();
}

// An asset root with three part models, a single-piece rifle and a whole
// pistol model tagged as the "pistol" preset.
struct Assets {
  testing::ScratchDirectory dir{"weapon_table"};
  parts::PartCatalog catalog = ParseCatalog(kCatalogText);
  const std::vector<std::string> weapon_prefabs = {"weapons/rifle.pbo",
                                                   "weapons\\pistol.pbo"};

  Assets() {
    Save(MakeModel({MakeSocket("slot.slide", {0.0f, 0.5f, 0.0f}),
                    MakeSocket("grip", {0.0f, -0.3f, 0.0f})}),
         "parts/frame.pbo");
    Save(MakeModel({MakeSocket("slot.sight_rear", {0.0f, 0.5f, -0.4f}),
                    MakeSocket("muzzle", {0.0f, 0.0f, 0.5f})}),
         "parts/slide.pbo");
    Save(MakeModel({MakeSocket("sight", {0.0f, 0.1f, 0.0f})}),
         "parts/rear.pbo");
    Save(MakeWholeWeapon("m16"), "weapons/rifle.pbo");
    Save(MakeWholeWeapon("pistol"), "weapons/pistol.pbo");
  }

  void Save(const prefab::Prefab& prefab, std::string_view path) const {
    REQUIRE(prefab::SavePrefab(prefab, dir.Child(path)).ok());
  }
};

TEST_CASE("Weapon keys are FNV-1a of stable names") {
  CHECK(Fnv1a32("") == 0x811c9dc5u);
  CHECK(PresetWeaponName("glock17") == "preset:glock17");
  CHECK(KeyOfWeaponName(PresetWeaponName("glock17")) == 0x7d0d49beu);
  CHECK(KeyOfWeaponName(PresetWeaponName("pistol")) == 0x134ca8dbu);
  CHECK(PrefabWeaponName("weapons\\m16.pbo") == "prefab:weapons/m16.pbo");
  CHECK(KeyOfWeaponName(PrefabWeaponName("weapons/m16.pbo")) == 0xdfc49222u);
}

TEST_CASE("Both prefab sources give the same weapon table") {
  Assets assets;
  world::PrefabStore store;
  weapon::WeaponLibrary store_library;
  auto from_store = BuildWeaponTable(&assets.catalog, assets.weapon_prefabs,
                                     store, assets.dir.path(), store_library);
  REQUIRE(from_store.ok());

  ShiftedSource shifted;
  weapon::WeaponLibrary shifted_library;
  auto from_shifted =
      BuildWeaponTable(&assets.catalog, assets.weapon_prefabs, shifted,
                       assets.dir.path(), shifted_library);
  REQUIRE(from_shifted.ok());

  const WeaponTable& a = from_store.value();
  const WeaponTable& b = from_shifted.value();
  // pistol, bare, the rifle and the whole pistol ("whole" has no model).
  REQUIRE(a.size() == 4);
  REQUIRE(b.size() == a.size());
  for (size_t i = 0; i < a.size(); ++i) {
    CAPTURE(i);
    CHECK(a.rows()[i].key == b.rows()[i].key);
    CHECK(a.rows()[i].name == b.rows()[i].name);
    CHECK(a.rows()[i].key == KeyOfWeaponName(a.rows()[i].name));
    if (i > 0) {
      CHECK(a.rows()[i - 1].key < a.rows()[i].key);
    }
  }
  CHECK(a.content_hash() == b.content_hash());
  CHECK(a.content_hash() != 0);

  // The whole pistol is the preset's definition; the smallest key names it.
  const WeaponKey preset = KeyOfWeaponName("preset:pistol");
  const WeaponKey prefab = KeyOfWeaponName("prefab:weapons/pistol.pbo");
  REQUIRE(a.Find(preset).has_value());
  CHECK(a.Find(preset) == a.Find(prefab));
  CHECK(a.KeyOf(*a.Find(preset)) == std::min(preset, prefab));
  const std::optional<weapon::WeaponDefId> rifle =
      a.Find(KeyOfWeaponName("prefab:weapons/rifle.pbo"));
  REQUIRE(rifle.has_value());
  CHECK(store_library.def(*rifle).type.id == "m16");
  CHECK_FALSE(a.Find(KeyOfWeaponName("preset:whole")).has_value());
  CHECK_FALSE(a.KeyOf(static_cast<weapon::WeaponDefId>(99)).has_value());
}

TEST_CASE("The content hash follows the simulated spec") {
  Assets assets;
  world::PrefabStore store;
  weapon::WeaponLibrary library;
  auto table =
      BuildWeaponTable(&assets.catalog, {}, store, assets.dir.path(), library);
  REQUIRE(table.ok());

  std::string faster(kCatalogText);
  faster.replace(faster.find("fire_rate = 1100"), 16, "fire_rate = 1200");
  const parts::PartCatalog faster_catalog = ParseCatalog(faster);
  weapon::WeaponLibrary faster_library;
  auto faster_table = BuildWeaponTable(&faster_catalog, {}, store,
                                       assets.dir.path(), faster_library);
  REQUIRE(faster_table.ok());
  REQUIRE(faster_table.value().size() == table.value().size());
  CHECK(faster_table.value().rows()[0].key == table.value().rows()[0].key);
  CHECK(faster_table.value().content_hash() != table.value().content_hash());
}

TEST_CASE("Weapon key collisions are build errors") {
  Assets assets;
  world::PrefabStore store;
  weapon::WeaponLibrary library;
  auto table =
      BuildWeaponTable(&assets.catalog, {}, store, assets.dir.path(), library);
  REQUIRE(table.ok());
  REQUIRE(library.size() >= 2);
  const auto first = static_cast<weapon::WeaponDefId>(0);
  const auto second = static_cast<weapon::WeaponDefId>(1);

  // These two names have the same 32-bit FNV-1a hash.
  REQUIRE(KeyOfWeaponName("preset:w1588") == KeyOfWeaponName("preset:w416922"));
  const std::vector<WeaponTableEntry> colliding = {
      {.name = "preset:w1588", .def = first},
      {.name = "preset:w416922", .def = second}};
  auto collided = WeaponTable::Build(colliding, library);
  REQUIRE_FALSE(collided.ok());
  CHECK(collided.error() ==
        "weapons 'preset:w1588' and 'preset:w416922' have the same key "
        "0xce95c747");

  const std::vector<WeaponTableEntry> twice = {
      {.name = "preset:a", .def = first}, {.name = "preset:a", .def = second}};
  auto ambiguous = WeaponTable::Build(twice, library);
  REQUIRE_FALSE(ambiguous.ok());
  CHECK(ambiguous.error() == "weapon 'preset:a' is listed for two definitions");

  const std::vector<WeaponTableEntry> repeated = {
      {.name = "preset:a", .def = first}, {.name = "preset:a", .def = first}};
  auto deduplicated = WeaponTable::Build(repeated, library);
  REQUIRE(deduplicated.ok());
  CHECK(deduplicated.value().size() == 1);
}

TEST_CASE("A listed prefab that is no weapon fails the table") {
  Assets assets;
  world::PrefabStore store;
  weapon::WeaponLibrary library;
  const std::vector<std::string> paths = {"parts/frame.pbo"};
  auto table = BuildWeaponTable(&assets.catalog, paths, store,
                                assets.dir.path(), library);
  REQUIRE_FALSE(table.ok());
  CHECK(table.error() == "weapon prefab parts/frame.pbo is no usable weapon");

  const std::vector<std::string> missing = {"weapons/none.pbo"};
  CHECK_FALSE(
      BuildWeaponTable(nullptr, missing, store, assets.dir.path(), library)
          .ok());
}

}  // namespace
}  // namespace blue::sim
