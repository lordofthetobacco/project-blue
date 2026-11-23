#include "weapon/parts/weapon_build.h"

#include <doctest/doctest.h>

#include <cstddef>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tests/part_catalog_samples.h"
#include "weapon/parts/part_catalog.h"

namespace blue::weapon::parts {
namespace {

using testing::GlockCatalog;
using testing::Id;

WeaponBuild StockGlock(const PartCatalog& catalog) {
  const PresetDef* preset = catalog.FindPreset("glock17");
  REQUIRE(preset != nullptr);
  auto build = MakeBuild(catalog, *preset);
  REQUIRE_MESSAGE(build.ok(), build.error().message);
  return std::move(build).value();
}

bool Contains(const std::string& text, std::string_view part) {
  return text.find(part) != std::string::npos;
}

// The part installed at `path`, or an invalid id when the slot is empty.
PartId InstalledAt(const PartCatalog& catalog, const WeaponBuild& build,
                   const std::string& path) {
  for (const SlotEntry& entry : ListSlots(catalog, build)) {
    if (entry.path == path) {
      return entry.installed;
    }
  }
  FAIL("no slot " << path);
  return PartId{};
}

TEST_CASE("an empty build has no part and no slots") {
  const PartCatalog catalog = GlockCatalog();
  const WeaponBuild empty;
  CHECK(empty.empty());
  CHECK_FALSE(empty.part().valid());
  CHECK(empty.slot_count() == 0);
  CHECK(empty.installed(0) == nullptr);
  CHECK(ListSlots(catalog, empty).empty());
  CHECK(MissingRequired(catalog, empty).empty());
  WeaponBuild copy = empty;
  CHECK_FALSE(Remove(catalog, copy, "slide").ok());
  CHECK(Remove(catalog, copy, "slide").error().message == "the build is empty");
}

TEST_CASE("a bare part has one empty slot per catalogue slot") {
  const PartCatalog catalog = GlockCatalog();
  const WeaponBuild frame(catalog, Id(catalog, "glock17_frame"));
  CHECK(frame.part() == Id(catalog, "glock17_frame"));
  CHECK(frame.slot_count() == 4);
  for (int i = 0; i < frame.slot_count(); ++i) {
    CHECK(frame.installed(i) == nullptr);
  }
  CHECK(frame.installed(-1) == nullptr);
  CHECK(frame.installed(4) == nullptr);
  CHECK(MissingRequired(catalog, frame) ==
        std::vector<std::string>{"slide", "barrel"});
}

TEST_CASE("MakeBuild installs a preset, parents before children") {
  const PartCatalog catalog = GlockCatalog();
  const WeaponBuild build = StockGlock(catalog);
  CHECK(build.part() == Id(catalog, "glock17_frame"));
  CHECK(InstalledAt(catalog, build, "slide") == Id(catalog, "glock17_slide"));
  CHECK(InstalledAt(catalog, build, "slide.sight_rear") ==
        Id(catalog, "glock_sight_rear"));
  CHECK(InstalledAt(catalog, build, "slide.sight_front") ==
        Id(catalog, "glock_sight_front"));
  CHECK(InstalledAt(catalog, build, "barrel") == Id(catalog, "glock17_barrel"));
  CHECK(InstalledAt(catalog, build, "magazine") == Id(catalog, "glock_mag_17"));
  CHECK_FALSE(InstalledAt(catalog, build, "tactical").valid());
  CHECK(MissingRequired(catalog, build).empty());

  const PresetDef* m16 = catalog.FindPreset("m16");
  REQUIRE(m16 != nullptr);
  const auto rifle = MakeBuild(catalog, *m16);
  REQUIRE(rifle.ok());
  CHECK(rifle.value().part() == Id(catalog, "m16"));
  CHECK(rifle.value().slot_count() == 0);
}

TEST_CASE("MakeBuild rejects bad presets") {
  const PartCatalog catalog = GlockCatalog();
  PresetDef preset{.id = "p", .root = Id(catalog, "glock17_slide")};
  CHECK(Contains(MakeBuild(catalog, preset).error().message,
                 "not a lower_receiver or complete part"));
  preset.root = Id(catalog, "glock17_frame");
  preset.entries = {{"barrel", Id(catalog, "glock17_barrel"), 0},
                    {"barrel", Id(catalog, "glock17_barrel_threaded"), 0}};
  CHECK(MakeBuild(catalog, preset).error().message ==
        "preset 'p': slot 'barrel' is set twice");
  preset.entries = {{"barrel", Id(catalog, "glock_mag_17"), 0}};
  CHECK(MakeBuild(catalog, preset).error().message ==
        "preset 'p': slot 'barrel' does not accept part 'glock_mag_17'");
}

TEST_CASE("ListSlots walks the tree depth first with paths and depths") {
  const PartCatalog catalog = GlockCatalog();
  const std::vector<SlotEntry> slots = ListSlots(catalog, StockGlock(catalog));
  REQUIRE(slots.size() == 6);
  const std::vector<std::string> paths = {
      "slide",  "slide.sight_rear", "slide.sight_front",
      "barrel", "magazine",         "tactical"};
  const std::vector<int> depths = {0, 1, 1, 0, 0, 0};
  for (size_t i = 0; i < slots.size(); ++i) {
    CHECK(slots[i].path == paths[i]);
    CHECK(slots[i].depth == depths[i]);
  }
  CHECK(slots[1].owner == Id(catalog, "glock17_slide"));
  CHECK(slots[1].slot == 0);
  const PartDef& owner = catalog.part(slots[2].owner);
  CHECK(owner.slots[static_cast<size_t>(slots[2].slot)].name == "sight_front");
  CHECK(slots[3].owner == Id(catalog, "glock17_frame"));
  CHECK(slots[3].slot == 1);
}

TEST_CASE("CanInstall checks the path and the filter") {
  const PartCatalog catalog = GlockCatalog();
  const WeaponBuild build = StockGlock(catalog);
  CHECK(
      CanInstall(catalog, build, "magazine", Id(catalog, "glock_mag_33")).ok());
  // An occupied slot is fine: installing replaces.
  CHECK(CanInstall(catalog, build, "barrel",
                   Id(catalog, "glock17_barrel_threaded"))
            .ok());
  CHECK(CanInstall(catalog, build, "magazine", Id(catalog, "red_dot"))
            .error()
            .message == "slot 'magazine' does not accept part 'red_dot'");
  // The stock slide's rear-sight slot takes only iron sights.
  CHECK_FALSE(
      CanInstall(catalog, build, "slide.sight_rear", Id(catalog, "red_dot"))
          .ok());
  CHECK(CanInstall(catalog, build, "stock", Id(catalog, "red_dot"))
            .error()
            .message ==
        "part 'glock17_frame' has no slot 'stock' (path 'stock')");
  CHECK(
      CanInstall(catalog, build, "barrel.muzzle", Id(catalog, "suppressor_9mm"))
          .error()
          .message ==
      "part 'glock17_barrel' has no slot 'muzzle' (path 'barrel.muzzle')");
  CHECK(
      CanInstall(catalog, build, "", Id(catalog, "red_dot")).error().message ==
      "empty slot path");
  CHECK(CanInstall(catalog, build, "slide.", Id(catalog, "red_dot"))
            .error()
            .message == "bad slot path 'slide.'");
  CHECK(CanInstall(catalog, build, "magazine", PartId{99}).error().message ==
        "unknown part");
  const WeaponBuild frame(catalog, Id(catalog, "glock17_frame"));
  CHECK(CanInstall(catalog, frame, "slide.sight_rear",
                   Id(catalog, "glock_sight_rear"))
            .error()
            .message == "slot 'slide' is empty (path 'slide.sight_rear')");
}

TEST_CASE("Install replaces and returns the previous sub-build") {
  const PartCatalog catalog = GlockCatalog();
  WeaponBuild build = StockGlock(catalog);
  auto replaced = Install(catalog, build, "magazine",
                          WeaponBuild(catalog, Id(catalog, "glock_mag_33")));
  REQUIRE(replaced.ok());
  CHECK(replaced.value().part() == Id(catalog, "glock_mag_17"));
  CHECK(InstalledAt(catalog, build, "magazine") == Id(catalog, "glock_mag_33"));

  auto into_empty = Install(catalog, build, "tactical",
                            WeaponBuild(catalog, Id(catalog, "olight_pl2")));
  REQUIRE(into_empty.ok());
  CHECK(into_empty.value().empty());
}

TEST_CASE("Install leaves the build unchanged when refused") {
  const PartCatalog catalog = GlockCatalog();
  WeaponBuild build = StockGlock(catalog);
  const WeaponBuild before = build;
  CHECK_FALSE(Install(catalog, build, "magazine",
                      WeaponBuild(catalog, Id(catalog, "red_dot")))
                  .ok());
  CHECK(Install(catalog, build, "magazine", WeaponBuild()).error().message ==
        "nothing to install");
  CHECK(build == before);
}

TEST_CASE("nesting: a threaded barrel takes a suppressor") {
  const PartCatalog catalog = GlockCatalog();
  WeaponBuild build = StockGlock(catalog);
  WeaponBuild barrel(catalog, Id(catalog, "glock17_barrel_threaded"));
  REQUIRE(Install(catalog, barrel, "muzzle",
                  WeaponBuild(catalog, Id(catalog, "suppressor_9mm")))
              .ok());
  REQUIRE(Install(catalog, build, "barrel", barrel).ok());
  CHECK(InstalledAt(catalog, build, "barrel.muzzle") ==
        Id(catalog, "suppressor_9mm"));
  CHECK(ListSlots(catalog, build).size() == 7);
}

TEST_CASE("Remove returns the sub-build with its own children") {
  const PartCatalog catalog = GlockCatalog();
  WeaponBuild build = StockGlock(catalog);
  auto slide = Remove(catalog, build, "slide");
  REQUIRE(slide.ok());
  CHECK(slide.value().part() == Id(catalog, "glock17_slide"));
  REQUIRE(slide.value().installed(0) != nullptr);
  CHECK(slide.value().installed(0)->part() == Id(catalog, "glock_sight_rear"));
  CHECK(slide.value().installed(1)->part() == Id(catalog, "glock_sight_front"));
  CHECK_FALSE(InstalledAt(catalog, build, "slide").valid());
  CHECK(MissingRequired(catalog, build) == std::vector<std::string>{"slide"});

  CHECK(Remove(catalog, build, "slide").error().message ==
        "slot 'slide' is empty");
  CHECK_FALSE(Remove(catalog, build, "slide.sight_rear").ok());

  // Putting the sub-assembly back restores the original build.
  REQUIRE(Install(catalog, build, "slide", slide.value()).ok());
  CHECK(build == StockGlock(catalog));
}

TEST_CASE("MissingRequired reports nested required slots") {
  const PartCatalog catalog = GlockCatalog();
  WeaponBuild build = StockGlock(catalog);
  REQUIRE(Remove(catalog, build, "slide.sight_front").ok());
  REQUIRE(Remove(catalog, build, "barrel").ok());
  REQUIRE(Remove(catalog, build, "magazine").ok());  // optional
  CHECK(MissingRequired(catalog, build) ==
        std::vector<std::string>{"slide.sight_front", "barrel"});

  // The optics-cut slide has only optional sight slots.
  REQUIRE(Install(catalog, build, "slide",
                  WeaponBuild(catalog, Id(catalog, "glock17_slide_mos")))
              .ok());
  CHECK(MissingRequired(catalog, build) == std::vector<std::string>{"barrel"});
  CHECK(CanInstall(catalog, build, "slide.sight_rear", Id(catalog, "red_dot"))
            .ok());
}

TEST_CASE("builds are values: copies are independent and compare equal") {
  const PartCatalog catalog = GlockCatalog();
  const WeaponBuild original = StockGlock(catalog);
  WeaponBuild copy = original;
  CHECK(copy == original);
  REQUIRE(Remove(catalog, copy, "slide.sight_rear").ok());
  CHECK_FALSE(copy == original);
  CHECK(InstalledAt(catalog, original, "slide.sight_rear") ==
        Id(catalog, "glock_sight_rear"));
}

}  // namespace
}  // namespace blue::weapon::parts
