#include "weapon/parts/part_catalog.h"

#include <doctest/doctest.h>

#include <array>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/text.h"
#include "platform/file_system.h"
#include "tests/part_catalog_samples.h"
#include "tests/scratch_directory.h"
#include "weapon/firearm.h"
#include "weapon/parts/part_catalog_file.h"

namespace blue::weapon::parts {
namespace {

using testing::GlockCatalog;
using testing::Id;
using testing::kGlockCatalogText;

// The error ParsePartCatalog reports for `text`; fails if it succeeds.
CatalogError ParseError(std::string_view text) {
  auto parsed = ParsePartCatalog(text, "bad.cfg");
  REQUIRE_FALSE(parsed.ok());
  return parsed.error();
}

// The error merging `texts` (one file each, named a.cfg, b.cfg, ...) reports;
// fails if parsing fails or merging succeeds.
CatalogError MergeError(std::span<const std::string_view> texts) {
  std::vector<PartCatalogFile> files;
  char name = 'a';
  for (const std::string_view text : texts) {
    auto parsed = ParsePartCatalog(text, std::string(1, name++) + ".cfg");
    REQUIRE_MESSAGE(parsed.ok(), parsed.error().ToString());
    files.push_back(std::move(parsed).value());
  }
  auto merged = MergePartCatalogs(files);
  REQUIRE_FALSE(merged.ok());
  return merged.error();
}

bool Contains(const std::string& text, std::string_view part) {
  return text.find(part) != std::string::npos;
}

constexpr std::string_view kMinimalRoot = R"([part frame]
category = lower_receiver
fire_modes = semi
fire_rate = 600
)";

TEST_CASE("the shared text helpers parse numbers strictly") {
  CHECK(core::Trim("  a b \t\r") == "a b");
  CHECK(core::ParseFiniteFloat("-2.9") == -2.9f);
  CHECK_FALSE(core::ParseFiniteFloat("2.9x").has_value());
  CHECK_FALSE(core::ParseFiniteFloat("inf").has_value());
  CHECK(core::ParseInt("33") == 33);
  CHECK_FALSE(core::ParseInt("3.5").has_value());
  CHECK_FALSE(core::ParseInt("").has_value());
  CHECK(core::ParseBool("TRUE") == true);
  CHECK(core::ParseBool("0") == false);
  CHECK_FALSE(core::ParseBool("yes").has_value());
}

TEST_CASE("categories and fire modes round-trip through their names") {
  for (const std::string_view name :
       {"lower_receiver", "slide", "barrel", "magazine", "sight_rear",
        "sight_front", "optic", "muzzle", "tactical", "complete"}) {
    const auto category = ParsePartCategory(name);
    REQUIRE(category.has_value());
    CHECK(ToString(*category) == name);
  }
  CHECK_FALSE(ParsePartCategory("stock").has_value());
  CHECK(IsRootCategory(PartCategory::kLowerReceiver));
  CHECK(IsRootCategory(PartCategory::kComplete));
  CHECK_FALSE(IsRootCategory(PartCategory::kSlide));
  CHECK(ParseFireMode("double_action") == FireMode::kDoubleAction);
  CHECK(ToString(FireMode::kAuto) == "auto");
  CHECK_FALSE(ParseFireMode("burst").has_value());
}

TEST_CASE("the sample catalogue reads every key") {
  const PartCatalog catalog = GlockCatalog();
  CHECK(catalog.part_count() == 14);

  const PartDef& frame = catalog.part(Id(catalog, "glock17_frame"));
  CHECK(frame.id == "glock17_frame");
  CHECK(frame.name == "Glock 17 frame");
  CHECK(frame.category == PartCategory::kLowerReceiver);
  CHECK(frame.prefab == "weapons/glock17/parts/frame.pbo");
  CHECK(frame.weight == doctest::Approx(0.21f));
  CHECK(frame.ergonomics == 90.0f);
  CHECK(frame.weapon_kind == WeaponKind::kPistol);
  CHECK(frame.recoil_vertical == 311.0f);
  CHECK(frame.recoil_horizontal == 260.0f);
  CHECK(frame.fire_modes == std::vector<FireMode>{FireMode::kSemi});
  CHECK(frame.fire_rate == 1100.0f);
  REQUIRE(frame.slots.size() == 4);
  CHECK(frame.slots[0].name == "slide");
  CHECK(frame.slots[0].required);
  CHECK(frame.slots[0].accepted ==
        std::vector<PartId>{Id(catalog, "glock17_slide"),
                            Id(catalog, "glock17_slide_mos")});
  CHECK(frame.slots[1].name == "barrel");
  CHECK(frame.slots[2].name == "magazine");
  CHECK_FALSE(frame.slots[2].required);
  CHECK(frame.slots[2].accepted.size() == 3);
  CHECK(frame.slots[3].name == "tactical");
  CHECK(frame.FindSlot("magazine") == 2);
  CHECK(frame.FindSlot("stock") == -1);
  CHECK(frame.slots[0].Accepts(Id(catalog, "glock17_slide_mos")));
  CHECK_FALSE(frame.slots[0].Accepts(Id(catalog, "glock17_barrel")));

  const PartDef& slide = catalog.part(Id(catalog, "glock17_slide_mos"));
  CHECK(slide.name == "glock17_slide_mos");  // no name: the id
  CHECK(slide.prefab.empty());

  const PartDef& barrel = catalog.part(Id(catalog, "glock17_barrel_threaded"));
  CHECK(barrel.recoil == doctest::Approx(-3.5f));
  CHECK(barrel.accuracy_moa == doctest::Approx(4.1f));
  CHECK(barrel.muzzle_velocity == 375.0f);
  CHECK(barrel.velocity == doctest::Approx(1.02f));

  const PartDef& suppressor = catalog.part(Id(catalog, "suppressor_9mm"));
  CHECK(suppressor.accuracy == -5.0f);
  CHECK(suppressor.category == PartCategory::kMuzzle);
  CHECK(catalog.part(Id(catalog, "glock_mag_33")).capacity == 33);
  CHECK(catalog.part(Id(catalog, "red_dot")).sighting_range == 100.0f);

  const PartDef& m16 = catalog.part(Id(catalog, "m16"));
  CHECK(m16.category == PartCategory::kComplete);
  CHECK(m16.weapon_kind == WeaponKind::kRifle);
  CHECK(m16.fire_modes ==
        std::vector<FireMode>{FireMode::kSemi, FireMode::kAuto});
  CHECK(m16.legacy_type == "m16");
  CHECK(m16.capacity == 30);
  CHECK(m16.slots.empty());
}

TEST_CASE("presets resolve their root and entries") {
  const PartCatalog catalog = GlockCatalog();
  REQUIRE(catalog.presets().size() == 2);
  const PresetDef* glock = catalog.FindPreset("glock17");
  REQUIRE(glock != nullptr);
  CHECK(glock->name == "Glock 17");
  CHECK(glock->root == Id(catalog, "glock17_frame"));
  REQUIRE(glock->entries.size() == 5);
  CHECK(glock->entries[0].path == "slide.sight_rear");
  CHECK(glock->entries[0].part == Id(catalog, "glock_sight_rear"));
  CHECK(glock->file == "glock.cfg");
  const PresetDef* m16 = catalog.FindPreset("m16");
  REQUIRE(m16 != nullptr);
  CHECK(m16->name == "m16");
  CHECK(m16->entries.empty());
  CHECK(catalog.FindPreset("ak") == nullptr);
  CHECK_FALSE(catalog.Find("ak").has_value());
}

TEST_CASE("comments, blank lines, CRLF and a BOM are accepted") {
  const std::string text =
      "\xEF\xBB\xBF# header\r\n\r\n[part frame]  # the frame\r\n"
      "category = lower_receiver\r\nfire_modes = semi,double_action\r\n"
      "fire_rate = 600\r\n";
  auto parsed = ParsePartCatalog(text, "crlf.cfg");
  REQUIRE_MESSAGE(parsed.ok(), parsed.error().ToString());
  REQUIRE(parsed.value().parts.size() == 1);
  CHECK(parsed.value().parts[0].def.fire_modes ==
        std::vector<FireMode>{FireMode::kSemi, FireMode::kDoubleAction});
  CHECK(parsed.value().parts[0].line == 3);
}

TEST_CASE("parse errors name the file and line") {
  struct Case {
    std::string_view text;
    int line;
    std::string_view message;
  };
  const Case cases[] = {
      {"category = slide\n", 1,
       "outside a [part], [preset] or [extend] section"},
      {"[part a]\ncategory slide\n", 2, "expected 'key = value'"},
      {"[part a]\n= slide\n", 2, "missing key"},
      {"[part a\n", 1, "bad section header"},
      {"[part A]\n", 1, "bad id 'A'"},
      {"[part]\n", 1, "bad id ''"},
      {"[gadget a]\n", 1, "unknown section 'gadget'"},
      {"[part a]\ncategory = stock\n", 2, "unknown category 'stock'"},
      {"[part a]\ncategory = slide\ncolour = red\n", 3, "unknown key 'colour'"},
      {"[part a]\ncategory = slide\nweight = 1\nweight = 2\n", 4,
       "'weight' is set twice"},
      {"[part a]\ncategory = slide\nweight = heavy\n", 3,
       "'weight' needs a number, got 'heavy'"},
      {"[part a]\ncategory = slide\nweight = -1\n", 3, "zero or more"},
      {"[part a]\ncategory = muzzle\nvelocity = 0\n", 3, "must be positive"},
      {"[part a]\ncategory = magazine\ncapacity = 3.5\n", 3, "whole number"},
      {"[part a]\ncategory = magazine\ncapacity = -1\n", 3, "whole number"},
      {"[part a]\nweight = 1\n", 1, "part 'a' has no category"},
      {"[part a]\ncategory = lower_receiver\nfire_rate = 600\n", 1,
       "needs fire_modes and fire_rate"},
      {"[part a]\ncategory = lower_receiver\nfire_modes = semi\n", 1,
       "needs fire_modes and fire_rate"},
      {"[part a]\ncategory = lower_receiver\nfire_modes = burst\n", 3,
       "unknown fire mode 'burst'"},
      {"[part a]\ncategory = lower_receiver\nfire_modes = semi, semi\n", 3,
       "listed twice"},
      {"[part a]\ncategory = lower_receiver\nfire_modes =\n", 3,
       "lists no mode"},
      {"[part a]\ncategory = lower_receiver\nweapon_kind = tank\n", 3,
       "unknown weapon_kind"},
      {"[part a]\ncategory = lower_receiver\nfire_rate = 0\n", 3,
       "must be positive"},
      // Category-specific keys, checked against a category set later on.
      {"[part a]\nrecoil_vertical = 10\ncategory = slide\n", 2,
       "'recoil_vertical' applies to lower_receiver and complete parts only"},
      {"[part a]\nweapon_kind = pistol\ncategory = barrel\n", 2,
       "'weapon_kind' applies to"},
      {"[part a]\ncategory = slide\nfire_modes = semi\n", 3,
       "'fire_modes' applies to"},
      {"[part a]\ncategory = slide\nfire_rate = 500\n", 3,
       "'fire_rate' applies to"},
      {"[part a]\ncategory = lower_receiver\nlegacy_type = m16\n", 3,
       "'legacy_type' applies to complete parts only"},
      {"[part a]\ncategory = lower_receiver\nrecoil = -3\n", 3,
       "'recoil' applies to parts other than"},
      {"[part a]\ncategory = complete\naccuracy = -3\n", 3,
       "'accuracy' applies to parts other than"},
      {"[part a]\ncategory = slide\naccuracy_moa = 3\n", 3,
       "'accuracy_moa' applies to barrel and complete parts only"},
      {"[part a]\ncategory = slide\nmuzzle_velocity = 300\n", 3,
       "'muzzle_velocity' applies to barrel"},
      {"[part a]\ncategory = barrel\ncapacity = 3\n", 3,
       "'capacity' applies to magazine and complete parts only"},
      // Slots.
      {"[part a]\ncategory = slide\nslot.Sight = required b\n", 3,
       "bad slot name 'Sight'"},
      {"[part a]\ncategory = slide\nslot.sight = maybe b\n", 3,
       "must start with 'required' or 'optional'"},
      {"[part a]\ncategory = slide\nslot.sight = optional b, , c\n", 3,
       "bad part id ''"},
      {"[part a]\ncategory = slide\nslot.s = optional b\nslot.s = optional "
       "c\n",
       4, "'slot.s' is set twice"},
      // Extensions.
      {"[extend a]\nweight = 1\n", 2, "an [extend] section takes"},
      {"[extend a]\nslot.S += b\n", 2, "bad slot key 'slot.S'"},
      {"[extend a]\nsight += b\n", 2, "bad slot key 'sight'"},
      {"[extend a]\nslot.s +=\n", 2, "adds no part"},
      {"[extend a]\nslot.s += b, , c\n", 2, "bad part id ''"},
      {"[extend A]\n", 1, "bad id 'A'"},
      // Presets.
      {"[preset p]\nname = P\n", 1, "preset 'p' has no root"},
      {"[preset p]\nroot = Frame\n", 2, "'root' needs a part id"},
      {"[preset p]\nroot = a\nslide..sight = b\n", 3, "unknown key"},
  };
  for (const Case& c : cases) {
    CAPTURE(c.text);
    const CatalogError error = ParseError(c.text);
    CHECK(error.file == "bad.cfg");
    CHECK(error.line == c.line);
    CHECK_MESSAGE(Contains(error.message, c.message), error.message);
  }
}

TEST_CASE("a slot may accept nothing until an extension adds parts") {
  const std::array<std::string_view, 2> texts = {
      R"([part frame]
category = lower_receiver
fire_modes = semi
fire_rate = 600
slot.tactical = optional
slot.barrel = required barrel_a
[part barrel_a]
category = barrel
[extend frame]
slot.barrel += barrel_b, barrel_a   # barrel_a is accepted already
)",
      R"([part barrel_b]
category = barrel
[part light]
category = tactical
[extend frame]
slot.tactical += light
[preset p]
root = frame
barrel = barrel_b
tactical = light
)"};
  std::vector<PartCatalogFile> files;
  for (const std::string_view text : texts) {
    auto parsed = ParsePartCatalog(text, "x.cfg");
    REQUIRE_MESSAGE(parsed.ok(), parsed.error().ToString());
    files.push_back(std::move(parsed).value());
  }
  CHECK(files[0].extensions.size() == 1);
  CHECK(files[0].extensions[0].part == "frame");
  CHECK(files[0].extensions[0].line == 9);
  auto merged = MergePartCatalogs(files);
  REQUIRE_MESSAGE(merged.ok(), merged.error().ToString());
  const PartCatalog& catalog = merged.value();
  const PartDef& frame = catalog.part(*catalog.Find("frame"));
  // Extensions apply in file order; an id already accepted is not repeated.
  CHECK(frame.slots[0].accepted == std::vector<PartId>{*catalog.Find("light")});
  CHECK(frame.slots[1].accepted ==
        std::vector<PartId>{*catalog.Find("barrel_a"),
                            *catalog.Find("barrel_b")});
  CHECK(catalog.FindPreset("p") != nullptr);

  // Without the extension the empty slot accepts nothing.
  files[1].extensions.clear();
  CHECK_FALSE(MergePartCatalogs(files).ok());
}

TEST_CASE("extension errors name the file and line") {
  SUBCASE("an unknown part") {
    const std::array<std::string_view, 2> texts = {
        kMinimalRoot, "[extend nope]\nslot.barrel += frame\n"};
    const CatalogError error = MergeError(texts);
    CHECK(error.file == "b.cfg");
    CHECK(error.line == 1);
    CHECK(error.message == "unknown part 'nope'");
  }
  SUBCASE("an unknown slot") {
    const std::array<std::string_view, 2> texts = {
        kMinimalRoot, "\n[extend frame]\nslot.stock += frame\n"};
    const CatalogError error = MergeError(texts);
    CHECK(error.line == 3);
    CHECK(error.message == "part 'frame' has no slot 'stock'");
  }
  SUBCASE("an unknown added part") {
    const std::array<std::string_view, 2> texts = {
        std::string_view("[part frame]\ncategory = lower_receiver\n"
                         "fire_modes = semi\nfire_rate = 600\n"
                         "slot.barrel = optional\n"),
        "[extend frame]\nslot.barrel += nope\n"};
    const CatalogError error = MergeError(texts);
    CHECK(error.line == 2);
    CHECK(error.message == "unknown part 'nope'");
  }
}

TEST_CASE("CatalogError formats file, line and message") {
  CHECK(CatalogError{"a.cfg", 3, "boom"}.ToString() == "a.cfg:3: boom");
  CHECK(CatalogError{"dir", 0, "boom"}.ToString() == "dir: boom");
}

TEST_CASE("merge resolves references across files") {
  const std::array<std::string_view, 2> texts = {
      R"([part frame]
category = lower_receiver
fire_modes = semi
fire_rate = 600
slot.barrel = required barrel_b
[preset p]
root = frame
barrel = barrel_b
)",
      R"([part barrel_b]
category = barrel
accuracy_moa = 3
)"};
  std::vector<PartCatalogFile> files;
  for (const std::string_view text : texts) {
    files.push_back(ParsePartCatalog(text, "x.cfg").value());
  }
  auto merged = MergePartCatalogs(files);
  REQUIRE_MESSAGE(merged.ok(), merged.error().ToString());
  const PartCatalog& catalog = merged.value();
  // Parts keep the file order: frame first, then the second file's barrel.
  CHECK(catalog.part(PartId{0}).id == "frame");
  CHECK(catalog.part(PartId{1}).id == "barrel_b");
  CHECK(catalog.part(PartId{0}).slots[0].accepted ==
        std::vector<PartId>{PartId{1}});
  CHECK(catalog.FindPreset("p") != nullptr);
}

TEST_CASE("merge errors name the file and line") {
  SUBCASE("a part defined in two files names both") {
    const std::array<std::string_view, 2> texts = {kMinimalRoot,
                                                   "\n\n"
                                                   "[part frame]\n"
                                                   "category = slide\n"};
    const CatalogError error = MergeError(texts);
    CHECK(error.file == "b.cfg");
    CHECK(error.line == 3);
    CHECK(
        Contains(error.message, "part 'frame' is already defined at a.cfg:1"));
  }
  SUBCASE("a preset defined twice") {
    const std::array<std::string_view, 2> texts = {"[preset p]\nroot = frame\n",
                                                   R"([part frame]
category = lower_receiver
fire_modes = semi
fire_rate = 600
[preset p]
root = frame
)"};
    const CatalogError error = MergeError(texts);
    CHECK(error.file == "b.cfg");
    CHECK(error.line == 5);
    CHECK(Contains(error.message, "preset 'p' is already defined at a.cfg:1"));
  }
  SUBCASE("an unknown part in a slot filter") {
    const std::array<std::string_view, 1> texts = {
        "[part frame]\ncategory = lower_receiver\nfire_modes = semi\n"
        "fire_rate = 600\nslot.barrel = required nope\n"};
    const CatalogError error = MergeError(texts);
    CHECK(error.file == "a.cfg");
    CHECK(error.line == 5);
    CHECK(error.message == "unknown part 'nope'");
  }
  SUBCASE("an unknown preset root") {
    const std::array<std::string_view, 1> texts = {
        "[preset p]\nname = P\nroot = nope\n"};
    const CatalogError error = MergeError(texts);
    CHECK(error.line == 3);
    CHECK(error.message == "unknown part 'nope'");
  }
  SUBCASE("an unknown preset part") {
    const std::array<std::string_view, 2> texts = {
        kMinimalRoot, "[preset p]\nroot = frame\nbarrel = nope\n"};
    const CatalogError error = MergeError(texts);
    CHECK(error.file == "b.cfg");
    CHECK(error.line == 3);
    CHECK(error.message == "unknown part 'nope'");
  }
  SUBCASE("a preset root that is not a root part") {
    const std::array<std::string_view, 1> texts = {
        "[part s]\ncategory = slide\n[preset p]\nroot = s\n"};
    const CatalogError error = MergeError(texts);
    CHECK(error.line == 4);
    CHECK(Contains(error.message, "is not a lower_receiver or complete part"));
  }
  SUBCASE("a preset part its slot's filter rejects") {
    const std::array<std::string_view, 1> texts = {
        R"([part frame]
category = lower_receiver
fire_modes = semi
fire_rate = 600
slot.barrel = required barrel_a
[part barrel_a]
category = barrel
[part barrel_b]
category = barrel
[preset p]
root = frame
barrel = barrel_b
)"};
    const CatalogError error = MergeError(texts);
    CHECK(error.line == 12);
    CHECK(error.message == "slot 'barrel' does not accept part 'barrel_b'");
  }
  SUBCASE("a preset path through an empty or missing slot") {
    const std::array<std::string_view, 1> texts = {
        R"([part frame]
category = lower_receiver
fire_modes = semi
fire_rate = 600
slot.slide = required slide
[part slide]
category = slide
slot.sight = optional sight
[part sight]
category = sight_rear
[preset p]
root = frame
slide.sight = sight
[preset q]
root = frame
stock = sight
)"};
    const CatalogError error = MergeError(texts);
    CHECK(error.line == 13);
    CHECK(Contains(error.message, "slot 'slide' is empty"));
  }
  SUBCASE("a preset slot the part does not have") {
    const std::array<std::string_view, 2> texts = {
        kMinimalRoot, "[preset q]\nroot = frame\nstock = frame\n"};
    const CatalogError error = MergeError(texts);
    CHECK(error.line == 3);
    CHECK(Contains(error.message, "part 'frame' has no slot 'stock'"));
  }
}

TEST_CASE("LoadPartCatalog merges the .cfg files of a directory by name") {
  blue::testing::ScratchDirectory dir("part_catalog");
  const auto write = [&dir](std::string_view name, std::string_view text) {
    REQUIRE(platform::WriteFileAtomic(dir.Child(name),
                                      std::as_bytes(std::span(text)))
                .ok());
  };
  // b.cfg sorts after a.cfg, so the barrel gets index 1.
  write("b.cfg", "[part barrel_b]\ncategory = barrel\n");
  write("a.cfg",
        std::string(kMinimalRoot) + "slot.barrel = optional barrel_b\n");
  write("notes.txt", "this is not part of the catalogue [");
  write("a.generated.cfg", "[part frame]\nthe fresh copy is skipped [");
  REQUIRE(platform::CreateDirectories(dir.Child("sub.cfg")));

  auto loaded = LoadPartCatalog(dir.path());
  REQUIRE_MESSAGE(loaded.ok(), loaded.error().ToString());
  CHECK(loaded.value().part_count() == 2);
  CHECK(loaded.value().part(PartId{0}).id == "frame");
  CHECK(loaded.value().part(PartId{1}).id == "barrel_b");

  write("c.cfg", "[part x]\ncategory = slide\nweight = ?\n");
  auto broken = LoadPartCatalog(dir.path());
  REQUIRE_FALSE(broken.ok());
  CHECK(broken.error().file == dir.Child("c.cfg"));
  CHECK(broken.error().line == 3);
}

TEST_CASE(
    "LoadPartCatalog: a missing directory is an error, an empty one "
    "is an empty catalogue") {
  blue::testing::ScratchDirectory dir("part_catalog_empty");
  auto empty = LoadPartCatalog(dir.path());
  REQUIRE(empty.ok());
  CHECK(empty.value().part_count() == 0);
  CHECK_FALSE(LoadPartCatalog(dir.Child("missing")).ok());
}

TEST_CASE("ListDirectory lists names sorted") {
  blue::testing::ScratchDirectory dir("list_directory");
  const std::string_view text = "x";
  for (const std::string_view name : {"b", "a", "c"}) {
    REQUIRE(platform::WriteFileAtomic(dir.Child(name),
                                      std::as_bytes(std::span(text)))
                .ok());
  }
  const auto names = platform::ListDirectory(dir.path());
  REQUIRE(names.has_value());
  CHECK(*names == std::vector<std::string>{"a", "b", "c"});
  CHECK_FALSE(platform::ListDirectory(dir.Child("a")).has_value());
  CHECK_FALSE(platform::ListDirectory(dir.Child("none")).has_value());
}

TEST_CASE("the contract's sample text parses") {
  CHECK(ParsePartCatalog(kGlockCatalogText, "glock.cfg").ok());
}

}  // namespace
}  // namespace blue::weapon::parts
