#include "game/part_stash.h"

#include <doctest/doctest.h>

#include <array>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "tests/part_catalog_samples.h"
#include "weapon/parts/part_catalog.h"
#include "weapon/parts/weapon_build.h"

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

std::vector<std::string> TopIds(const parts::PartCatalog& catalog,
                                const PartStash& stash) {
  std::vector<std::string> ids;
  for (int i = 0; i < stash.size(); ++i) {
    ids.push_back(catalog.part(stash.entry(i).part()).id);
  }
  return ids;
}

TEST_CASE("the starter set: every loose non-root part plus every magazine") {
  const parts::PartCatalog catalog = GlockCatalog();
  const std::array<parts::WeaponBuild, 1> starting = {StockGlock(catalog)};
  PartStash stash;
  stash.Seed(catalog, starting);
  // Catalogue order; the stock pistol's slide, sights and barrel are not
  // in it, its magazine is; roots (frame, the complete M16) never are.
  CHECK(TopIds(catalog, stash) ==
        std::vector<std::string>{"glock17_slide_mos", "red_dot",
                                 "glock17_barrel_threaded", "suppressor_9mm",
                                 "glock_mag_17", "glock_mag_21", "glock_mag_33",
                                 "olight_pl2"});
  for (int i = 0; i < stash.size(); ++i) {
    // Bare parts: nothing installed on them.
    const parts::WeaponBuild& entry = stash.entry(i);
    for (int slot = 0; slot < entry.slot_count(); ++slot) {
      CHECK(entry.installed(slot) == nullptr);
    }
  }
}

TEST_CASE("seeding again starts over") {
  const parts::PartCatalog catalog = GlockCatalog();
  PartStash stash;
  stash.Seed(catalog, {});
  const int full = stash.size();
  (void)stash.Take(0);
  stash.Seed(catalog, {});
  CHECK(stash.size() == full);
  stash.Clear();
  CHECK(stash.empty());
}

TEST_CASE("take and put back keep sub-assemblies whole") {
  const parts::PartCatalog catalog = GlockCatalog();
  parts::WeaponBuild glock = StockGlock(catalog);
  auto slide = parts::Remove(catalog, glock, "slide");
  REQUIRE(slide.ok());
  PartStash stash;
  stash.PutBack(parts::WeaponBuild{});  // ignored
  CHECK(stash.empty());
  stash.PutBack(parts::WeaponBuild(catalog, Id(catalog, "red_dot")));
  stash.PutBack(slide.value());
  REQUIRE(stash.size() == 2);
  CHECK(stash.entry(1) == slide.value());
  const parts::WeaponBuild taken = stash.Take(0);
  CHECK(taken.part() == Id(catalog, "red_dot"));
  REQUIRE(stash.size() == 1);
  CHECK(stash.entry(0) == slide.value());  // the sights came along
}

TEST_CASE("Fitting lists the entries a slot accepts, in stash order") {
  const parts::PartCatalog catalog = GlockCatalog();
  const parts::WeaponBuild glock = StockGlock(catalog);
  PartStash stash;
  stash.Seed(catalog, {});
  const std::vector<int> magazines = stash.Fitting(catalog, glock, "magazine");
  std::vector<std::string> ids;
  for (const int index : magazines) {
    ids.push_back(catalog.part(stash.entry(index).part()).id);
  }
  CHECK(ids == std::vector<std::string>{"glock_mag_17", "glock_mag_21",
                                        "glock_mag_33"});
  // The stock slide offers no optic slot; a slot that does not exist fits
  // nothing.
  CHECK(stash.Fitting(catalog, glock, "barrel.muzzle").empty());
  CHECK(stash.Fitting(catalog, glock, "nope").empty());
}

}  // namespace
}  // namespace blue::game
