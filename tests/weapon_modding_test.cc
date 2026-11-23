#include "game/weapon_modding.h"

#include <doctest/doctest.h>

#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "game/part_stash.h"
#include "tests/part_catalog_samples.h"
#include "weapon/parts/part_catalog.h"
#include "weapon/parts/weapon_build.h"
#include "weapon/parts/weapon_stats.h"

namespace blue::game {
namespace {

namespace parts = weapon::parts;
using parts::testing::GlockCatalog;
using parts::testing::Id;

parts::WeaponBuild StockGlock(const parts::PartCatalog& catalog) {
  const parts::PresetDef* preset = catalog.FindPreset("glock17");
  REQUIRE(preset != nullptr);
  auto build = parts::MakeBuild(catalog, *preset);
  REQUIRE_MESSAGE(build.ok(), build.error().message);
  return std::move(build).value();
}

// Selects the row of slot `path`; fails the test if there is none.
void SelectPath(WeaponModding& modding, std::string_view path) {
  for (size_t i = 0; i < modding.rows().size(); ++i) {
    if (modding.rows()[i].path == path) {
      modding.Select(static_cast<int>(i));
      return;
    }
  }
  FAIL("no slot " << path);
}

// The stash index of the first entry whose top part is `id`.
int StashIndex(const parts::PartCatalog& catalog, const PartStash& stash,
               std::string_view id) {
  for (int i = 0; i < stash.size(); ++i) {
    if (catalog.part(stash.entry(i).part()).id == id) {
      return i;
    }
  }
  FAIL("not in the stash: " << id);
  return -1;
}

const StatLine& Line(const std::vector<StatLine>& lines,
                     std::string_view label) {
  for (const StatLine& line : lines) {
    if (line.label == label) {
      return line;
    }
  }
  FAIL("no stat " << label);
  return lines.front();
}

// A session on the stock pistol with the starter stash, and an apply step
// that records the builds it is given and can refuse them.
struct Fixture {
  parts::PartCatalog catalog = GlockCatalog();
  WeaponModding modding{catalog};
  PartStash stash;
  std::vector<parts::WeaponBuild> applied;
  bool accept = true;
  ApplyBuild apply = [this](const parts::WeaponBuild& build) {
    applied.push_back(build);
    return accept;
  };

  Fixture() {
    const parts::WeaponBuild glock = StockGlock(catalog);
    stash.Seed(catalog, std::span(&glock, 1));
    modding.SetBuild(glock);
  }
};

TEST_CASE("the slot rows follow the build, nested and marked required") {
  Fixture f;
  std::vector<std::string> paths;
  for (const SlotRow& row : f.modding.rows()) {
    paths.push_back(row.path);
  }
  CHECK(paths == std::vector<std::string>{"slide", "slide.sight_rear",
                                          "slide.sight_front", "barrel",
                                          "magazine", "tactical"});
  const SlotRow& rear = f.modding.rows()[1];
  CHECK(rear.depth == 1);
  CHECK(rear.name == "sight_rear");
  CHECK(rear.required);
  CHECK(rear.installed == Id(f.catalog, "glock_sight_rear"));
  const SlotRow& tactical = f.modding.rows()[5];
  CHECK_FALSE(tactical.required);
  CHECK_FALSE(tactical.installed.valid());
  CHECK(tactical.installed_name.empty());
  CHECK(f.modding.rows()[0].installed_name == "Glock 17 slide");
  CHECK(f.modding.selected() == 0);
  CHECK(f.modding.stats().functional);
}

TEST_CASE("selection: out of range selects nothing, nothing fits then") {
  Fixture f;
  f.modding.Select(99);
  CHECK(f.modding.selected() == -1);
  CHECK_FALSE(f.modding.CanRemove());
  CHECK(f.modding.Candidates(f.stash).empty());
  CHECK_FALSE(f.modding.Install(f.stash, 0, f.apply).ok());
  CHECK(f.applied.empty());
}

TEST_CASE("candidates are the stash entries that fit the selected slot") {
  Fixture f;
  SelectPath(f.modding, "magazine");
  const std::vector<StashLine> lines = f.modding.Candidates(f.stash);
  REQUIRE(lines.size() == 3);
  CHECK(lines[1].name == "glock_mag_21");  // no name in the file: the id
  CHECK(lines[1].category == "magazine");
  CHECK(lines[1].numbers == "0.10 kg  ergo -4  21 rds");
  CHECK(f.stash.entry(lines[1].stash_index).part() ==
        Id(f.catalog, "glock_mag_21"));
}

TEST_CASE("installing a part swaps it with the stash and applies the build") {
  Fixture f;
  SelectPath(f.modding, "magazine");
  const int before = f.stash.size();
  const int mag33 = StashIndex(f.catalog, f.stash, "glock_mag_33");
  REQUIRE(f.modding.Install(f.stash, mag33, f.apply).ok());
  REQUIRE(f.applied.size() == 1);
  CHECK(f.applied.back() == f.modding.build());
  CHECK(f.modding.stats().magazine_capacity == 33);
  // The stock magazine went back: the stash keeps its size.
  CHECK(f.stash.size() == before);
  CHECK(f.stash.entry(f.stash.size() - 1).part() ==
        Id(f.catalog, "glock_mag_17"));
  // The selection stays on the slot.
  CHECK(f.modding.rows()[static_cast<size_t>(f.modding.selected())].path ==
        "magazine");
}

TEST_CASE("installing into an empty slot puts nothing back") {
  Fixture f;
  SelectPath(f.modding, "tactical");
  const int before = f.stash.size();
  const int light = StashIndex(f.catalog, f.stash, "olight_pl2");
  REQUIRE(f.modding.Install(f.stash, light, f.apply).ok());
  CHECK(f.stash.size() == before - 1);
  CHECK(f.modding.CanRemove());
}

TEST_CASE("nested slots: a threaded barrel, then a suppressor on it") {
  Fixture f;
  SelectPath(f.modding, "barrel");
  REQUIRE(
      f.modding
          .Install(f.stash,
                   StashIndex(f.catalog, f.stash, "glock17_barrel_threaded"),
                   f.apply)
          .ok());
  SelectPath(f.modding, "barrel.muzzle");
  CHECK(f.modding.rows()[static_cast<size_t>(f.modding.selected())].depth == 1);
  REQUIRE(f.modding
              .Install(f.stash,
                       StashIndex(f.catalog, f.stash, "suppressor_9mm"),
                       f.apply)
              .ok());
  // Removing the barrel takes the suppressor along as one stash entry.
  SelectPath(f.modding, "barrel");
  REQUIRE(f.modding.Remove(f.stash, f.apply).ok());
  const parts::WeaponBuild& removed = f.stash.entry(f.stash.size() - 1);
  CHECK(removed.part() == Id(f.catalog, "glock17_barrel_threaded"));
  REQUIRE(removed.installed(0) != nullptr);
  CHECK(removed.installed(0)->part() == Id(f.catalog, "suppressor_9mm"));
  CHECK_FALSE(f.modding.stats().functional);
  CHECK(f.modding.stats().missing_required ==
        std::vector<std::string>{"barrel"});
  // The sub-assembly is listed with its extra part.
  const std::vector<StashLine> lines = f.modding.Candidates(f.stash);
  bool listed = false;
  for (const StashLine& line : lines) {
    listed = listed || line.name == "glock17_barrel_threaded (+1 part)";
  }
  CHECK(listed);
}

TEST_CASE("a refused build leaves the stash and the build exactly as before") {
  Fixture f;
  f.accept = false;
  SelectPath(f.modding, "magazine");
  const PartStash stash_before = f.stash;
  const parts::WeaponBuild build_before = f.modding.build();
  const int mag21 = StashIndex(f.catalog, f.stash, "glock_mag_21");
  CHECK_FALSE(f.modding.Install(f.stash, mag21, f.apply).ok());
  CHECK_FALSE(f.modding.Remove(f.stash, f.apply).ok());
  CHECK(f.applied.size() == 2);
  CHECK(f.modding.build() == build_before);
  REQUIRE(f.stash.size() == stash_before.size());
  for (int i = 0; i < f.stash.size(); ++i) {
    CHECK(f.stash.entry(i) == stash_before.entry(i));
  }
}

TEST_CASE("a part the slot does not take is refused without applying") {
  Fixture f;
  SelectPath(f.modding, "magazine");
  const int dot = StashIndex(f.catalog, f.stash, "red_dot");
  const auto refused = f.modding.Install(f.stash, dot, f.apply);
  CHECK_FALSE(refused.ok());
  CHECK(f.applied.empty());
  CHECK_FALSE(f.modding.PreviewInstall(f.stash, dot).has_value());
}

TEST_CASE("the root is no slot: it can be neither removed nor replaced") {
  Fixture f;
  for (const SlotRow& row : f.modding.rows()) {
    CHECK_FALSE(row.path.empty());
  }
  // Removing everything still leaves the frame.
  for (int round = 0; round < 3; ++round) {
    for (int i = 0; i < static_cast<int>(f.modding.rows().size()); ++i) {
      f.modding.Select(i);
      if (f.modding.CanRemove()) {
        REQUIRE(f.modding.Remove(f.stash, f.apply).ok());
      }
    }
  }
  CHECK(f.modding.build().part() == Id(f.catalog, "glock17_frame"));
}

TEST_CASE("previews: a hovered part and a hovered remove") {
  Fixture f;
  SelectPath(f.modding, "magazine");
  const std::optional<parts::WeaponStats> bigger = f.modding.PreviewInstall(
      f.stash, StashIndex(f.catalog, f.stash, "glock_mag_33"));
  REQUIRE(bigger.has_value());
  CHECK(bigger->magazine_capacity == 33);
  CHECK(f.modding.stats().magazine_capacity == 17);  // unchanged
  const std::optional<parts::WeaponStats> none = f.modding.PreviewRemove();
  REQUIRE(none.has_value());
  CHECK(none->magazine_capacity == 0);

  const std::vector<StatLine> lines =
      MakeStatLines(f.modding.stats(), &*bigger);
  // Heavier, less ergonomic, more recoil: worse; more rounds: better.
  CHECK(Line(lines, "Weight").change == StatChange::kWorse);
  CHECK(Line(lines, "Weight").delta.starts_with("+0."));
  CHECK(Line(lines, "Ergonomics").change == StatChange::kWorse);
  CHECK(Line(lines, "Ergonomics").delta == "-8");
  CHECK(Line(lines, "Vertical recoil").change == StatChange::kWorse);
  CHECK(Line(lines, "Magazine").change == StatChange::kBetter);
  CHECK(Line(lines, "Magazine").preview == "33 rds");
  CHECK(Line(lines, "Accuracy").change == StatChange::kNone);
  CHECK(Line(lines, "Accuracy").delta.empty());

  const std::vector<StatLine> removed =
      MakeStatLines(f.modding.stats(), &*none);
  CHECK(Line(removed, "Magazine").preview == "none");
  CHECK(Line(removed, "Magazine").change == StatChange::kWorse);
  CHECK(Line(removed, "Weight").change == StatChange::kBetter);

  // No magazine to remove from an empty slot.
  SelectPath(f.modding, "tactical");
  CHECK_FALSE(f.modding.PreviewRemove().has_value());
}

TEST_CASE("stat lines: order, formatting and lower-is-better stats") {
  parts::WeaponStats now;
  now.weight = 0.91f;
  now.ergonomics = 88.0f;
  now.accuracy_moa = 4.4f;
  now.sighting_range = 50.0f;
  now.recoil_vertical = 302.0f;
  now.recoil_horizontal = 252.0f;
  now.fire_modes = weapon::FireModeBit(weapon::FireMode::kSemi);
  now.fire_rate = 1100.0f;
  now.magazine_capacity = 17;
  now.muzzle_velocity = 375.0f;
  std::vector<StatLine> lines = MakeStatLines(now, nullptr);
  std::vector<std::string_view> labels;
  for (const StatLine& line : lines) {
    labels.push_back(line.label);
    CHECK(line.preview.empty());
    CHECK(line.change == StatChange::kNone);
  }
  CHECK(labels == std::vector<std::string_view>{
                      "Weight", "Ergonomics", "Accuracy", "Sighting range",
                      "Vertical recoil", "Horizontal recoil", "Fire modes",
                      "Fire rate", "Magazine", "Muzzle velocity"});
  CHECK(Line(lines, "Weight").value == "0.91 kg");
  CHECK(Line(lines, "Accuracy").value == "4.4 MOA");
  CHECK(Line(lines, "Sighting range").value == "50 m");
  CHECK(Line(lines, "Fire modes").value == "semi");
  CHECK(Line(lines, "Fire rate").value == "1100 rpm");
  CHECK(Line(lines, "Muzzle velocity").value == "375 m/s");

  parts::WeaponStats next = now;
  next.weight = 0.71f;
  next.accuracy_moa = 4.0f;
  next.recoil_vertical = 290.0f;
  next.recoil_horizontal = 260.0f;
  next.sighting_range = 0.0f;
  next.fire_modes |= weapon::FireModeBit(weapon::FireMode::kAuto);
  next.muzzle_velocity = 390.0f;
  lines = MakeStatLines(now, &next);
  CHECK(Line(lines, "Weight").change == StatChange::kBetter);
  CHECK(Line(lines, "Weight").delta == "-0.20");
  CHECK(Line(lines, "Accuracy").change == StatChange::kBetter);
  CHECK(Line(lines, "Accuracy").delta == "-0.4");
  CHECK(Line(lines, "Vertical recoil").change == StatChange::kBetter);
  CHECK(Line(lines, "Horizontal recoil").change == StatChange::kWorse);
  CHECK(Line(lines, "Horizontal recoil").delta == "+8");
  CHECK(Line(lines, "Sighting range").preview == "none");
  CHECK(Line(lines, "Sighting range").change == StatChange::kWorse);
  CHECK(Line(lines, "Fire modes").preview == "semi, auto");
  CHECK(Line(lines, "Fire modes").change == StatChange::kDifferent);
  CHECK(Line(lines, "Muzzle velocity").change == StatChange::kBetter);
  CHECK(Line(lines, "Ergonomics").change == StatChange::kNone);
}

TEST_CASE("values that format the same are no change") {
  parts::WeaponStats now;
  now.weight = 0.500f;
  parts::WeaponStats next = now;
  next.weight = 0.501f;
  const std::vector<StatLine> lines = MakeStatLines(now, &next);
  CHECK(Line(lines, "Weight").change == StatChange::kNone);
  CHECK(Line(lines, "Weight").delta.empty());
  CHECK(Line(lines, "Accuracy").value == "-");  // no barrel figure
}

TEST_CASE("PartKeyNumbers lists the part's non-zero figures") {
  const parts::PartCatalog catalog = GlockCatalog();
  CHECK(PartKeyNumbers(catalog.part(Id(catalog, "suppressor_9mm"))) ==
        "0.20 kg  ergo -12  recoil -15.0%  acc -5.0%  vel x0.95");
  CHECK(PartKeyNumbers(catalog.part(Id(catalog, "red_dot"))) ==
        "0.05 kg  ergo -2  100 m");
  CHECK(PartKeyNumbers(catalog.part(Id(catalog, "glock17_barrel_threaded"))) ==
        "0.19 kg  ergo -3  recoil -3.5%  4.1 MOA  vel x1.02");
}

}  // namespace
}  // namespace blue::game
