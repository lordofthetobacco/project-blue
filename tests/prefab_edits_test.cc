#include "cook/prefab_edits.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/ext/quaternion_float.hpp>
#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include "animation/clip.h"
#include "core/result.h"
#include "platform/file_system.h"
#include "prefab/prefab.h"
#include "prefab/prefab_file.h"
#include "tests/prefab_samples.h"
#include "tests/scratch_directory.h"

namespace blue::cook {
namespace {

using Kind = PrefabEdit::Kind;

PrefabEdit Parse(std::string_view option, std::string_view value) {
  core::Result<PrefabEdit, std::string> edit = ParsePrefabEdit(option, value);
  REQUIRE(edit.ok());
  return std::move(edit).value();
}

bool ParseFails(std::string_view option, std::string_view value) {
  return !ParsePrefabEdit(option, value).ok();
}

// Applies `option value` to `prefab`; returns the error, if any.
std::optional<std::string> Apply(prefab::Prefab& prefab,
                                 std::string_view option,
                                 std::string_view value) {
  core::Result<void, std::string> applied =
      ApplyPrefabEdit(prefab, Parse(option, value));
  if (applied.ok()) {
    return std::nullopt;
  }
  return applied.error();
}

// Whether every bulk array of `a` holds the same bytes as `b`'s.
bool SameBulk(const prefab::Prefab& a, const prefab::Prefab& b) {
  std::vector<std::span<const std::byte>> views_a;
  std::vector<std::span<const std::byte>> views_b;
  prefab::CollectBulkViews(a, views_a);
  prefab::CollectBulkViews(b, views_b);
  if (views_a.size() != views_b.size()) {
    return false;
  }
  for (size_t i = 0; i < views_a.size(); ++i) {
    if (!std::ranges::equal(views_a[i], views_b[i])) {
      return false;
    }
  }
  return true;
}

TEST_CASE("Prefab edit options parse") {
  CHECK(IsPrefabEditOption("--tag"));
  CHECK(IsPrefabEditOption("--rename-clip"));
  CHECK_FALSE(IsPrefabEditOption("--scale"));

  const PrefabEdit tag = Parse("--tag", "weapon=glock17");
  CHECK(tag.kind == Kind::kSetTag);
  CHECK(tag.target == "weapon");
  CHECK(tag.text == "glock17");
  // The value is everything after the first '='.
  CHECK(Parse("--tag", "note=a=b").text == "a=b");

  const PrefabEdit socket = Parse("--socket", "sight=Slide@0,0.1398,-0.0865");
  CHECK(socket.kind == Kind::kSetSocket);
  CHECK(socket.target == "sight");
  CHECK(socket.text == "Slide");
  CHECK(socket.position == glm::vec3(0.0f, 0.1398f, -0.0865f));
  CHECK_FALSE(socket.aim.has_value());
  const PrefabEdit aimed = Parse("--socket", "sight=-@1,2,3/0,0.1,1");
  REQUIRE(aimed.aim.has_value());
  CHECK(aimed.aim->y == doctest::Approx(0.1f));

  const PrefabEdit event = Parse("--event", "Fire=slide_lock@0.6833");
  CHECK(event.kind == Kind::kAddEvent);
  CHECK(event.target == "Fire");
  CHECK(event.text == "slide_lock");
  CHECK(event.time == doctest::Approx(0.6833f));

  const PrefabEdit rename = Parse("--rename-clip", "Fire=fire");
  CHECK(rename.kind == Kind::kRenameClip);
  CHECK(Parse("--remove-tag", "weapon").kind == Kind::kRemoveTag);
  CHECK(Parse("--remove-socket", "lens").kind == Kind::kRemoveSocket);

  CHECK(ParseFails("--tag", "no-equals"));
  CHECK(ParseFails("--socket", "sight=Slide@0,1"));
  CHECK(ParseFails("--socket", "sight=Slide@0,1,2/0,1"));
  CHECK(ParseFails("--socket", "=Slide@0,1,2"));
  CHECK(ParseFails("--event", "Fire=shot@soon"));
  CHECK(ParseFails("--rename-clip", "Fire="));
  CHECK(ParseFails("--remove-tag", ""));
  CHECK(ParseFails("--bogus", "x"));
}

TEST_CASE("Tag edits validate keys and values") {
  prefab::Prefab prefab = testing::MakeSamplePrefab();
  CHECK_FALSE(Apply(prefab, "--tag", "weapon=glock17"));
  CHECK(prefab::FindTag(prefab, "weapon") == "glock17");
  CHECK_FALSE(Apply(prefab, "--tag", "weapon=m16"));  // Replaced.
  CHECK(prefab::FindTag(prefab, "weapon") == "m16");
  CHECK(Apply(prefab, "--tag", "Weapon=x").has_value());
  CHECK(Apply(prefab, "--tag",
              "long=" + std::string(prefab::kMaxTagValueLength + 1, 'v'))
            .has_value());
  CHECK_FALSE(Apply(prefab, "--remove-tag", "weapon"));
  CHECK(Apply(prefab, "--remove-tag", "weapon").has_value());
  CHECK_FALSE(prefab::FindTag(prefab, "weapon").has_value());
}

TEST_CASE("Socket edits add, replace, aim and remove sockets") {
  prefab::Prefab prefab = testing::MakeSamplePrefab();
  const size_t before = prefab.sockets.size();
  CHECK_FALSE(Apply(prefab, "--socket", "grip=slide@0,-0.05,0.1"));
  REQUIRE(prefab.sockets.size() == before + 1);
  const prefab::Socket& grip = prefab.sockets.back();
  CHECK(grip.node == 1);
  CHECK(grip.local.translation == glm::vec3(0.0f, -0.05f, 0.1f));

  // Replacing keeps the count; '-' is the root; the direction aims +Z.
  CHECK_FALSE(Apply(prefab, "--socket", "grip=-@1,2,3/0,1,0"));
  REQUIRE(prefab.sockets.size() == before + 1);
  CHECK(prefab.sockets.back().node == -1);
  const glm::vec3 z =
      prefab.sockets.back().local.rotation * glm::vec3(0.0f, 0.0f, 1.0f);
  CHECK(glm::length(z - glm::vec3(0.0f, 1.0f, 0.0f)) < 1.0e-5f);
  // Straight backwards turns about +Y.
  CHECK_FALSE(Apply(prefab, "--socket", "back=-@0,0,0/0,0,-2"));
  const glm::vec3 back =
      prefab.sockets.back().local.rotation * glm::vec3(0.0f, 0.0f, 1.0f);
  CHECK(glm::length(back - glm::vec3(0.0f, 0.0f, -1.0f)) < 1.0e-5f);

  CHECK(Apply(prefab, "--socket", "x=NoSuchNode@0,0,0").has_value());
  CHECK(Apply(prefab, "--socket", "x=-@0,0,0/0,0,0").has_value());
  CHECK_FALSE(Apply(prefab, "--remove-socket", "grip"));
  CHECK(Apply(prefab, "--remove-socket", "grip").has_value());
}

TEST_CASE("Clip edits add events and rename clips") {
  prefab::Prefab prefab = testing::MakeSamplePrefab();
  CHECK_FALSE(Apply(prefab, "--event", "fire=eject@0.1"));
  REQUIRE(prefab::FindClip(prefab, "fire") >= 0);
  CHECK(animation::FindEvent(prefab.clips[0], "eject") != nullptr);
  CHECK(Apply(prefab, "--event", "fire=late@9").has_value());
  CHECK(Apply(prefab, "--event", "nope=shot@0").has_value());
  CHECK_FALSE(Apply(prefab, "--rename-clip", "fire=Fire"));
  CHECK(prefab::FindClip(prefab, "Fire") >= 0);
  CHECK(Apply(prefab, "--rename-clip", "fire=x").has_value());
}

TEST_CASE("A list of edits applies all or nothing") {
  const prefab::Prefab prefab = testing::MakeSamplePrefab();
  const std::vector<PrefabEdit> edits{Parse("--tag", "weapon=glock17"),
                                      Parse("--remove-socket", "missing")};
  core::Result<prefab::Prefab, std::string> edited =
      ApplyPrefabEdits(prefab, edits);
  REQUIRE_FALSE(edited.ok());
  CHECK(edited.error().starts_with("edit 2:"));
  CHECK(prefab.tags == testing::MakeSamplePrefab().tags);
}

TEST_CASE("EditPrefabFile edits a cooked file and keeps everything else") {
  testing::ScratchDirectory scratch("prefab_edit");
  const std::string input = scratch.Child("in/weapon.pbo");
  const std::string output = scratch.Child("out/edited.pbo");
  REQUIRE(prefab::SavePrefab(testing::MakeSamplePrefab(), input).ok());
  core::Result<prefab::Prefab, prefab::PrefabError> original =
      prefab::LoadPrefab(input);
  REQUIRE(original.ok());

  const std::vector<PrefabEdit> edits{
      Parse("--tag", "weapon=glock17"),
      Parse("--socket", "sight=slide@0,0.14,-0.09"),
      Parse("--event", "fire=eject@0.1"),
      Parse("--rename-clip", "fire=Fire"),
  };
  REQUIRE(EditPrefabFile(input, output, edits).ok());
  core::Result<prefab::Prefab, prefab::PrefabError> edited =
      prefab::LoadPrefab(output);
  REQUIRE(edited.ok());
  const prefab::Prefab& a = original.value();
  const prefab::Prefab& b = edited.value();
  CHECK(prefab::FindTag(b, "weapon") == "glock17");
  CHECK(prefab::FindSocket(b, "sight") >= 0);
  CHECK(prefab::FindClip(b, "Fire") >= 0);
  // Untouched: bulk bytes, physics, materials, paths (resolved again
  // against the new directory), import settings.
  CHECK(SameBulk(a, b));
  CHECK(b.meshes.size() == a.meshes.size());
  CHECK(b.materials.size() == a.materials.size());
  CHECK(b.physics.kind == a.physics.kind);
  CHECK(b.physics.mass == a.physics.mass);
  CHECK(b.edit.source_model == a.edit.source_model);
  CHECK(b.edit.import_settings == a.edit.import_settings);
  CHECK(b.edit.texture_sources == a.edit.texture_sources);
  REQUIRE(b.children.size() == a.children.size());
  CHECK(b.children[0].path == a.children[0].path);

  // In place works too (the input stays mapped while it is replaced).
  REQUIRE(EditPrefabFile(output, output,
                         std::vector<PrefabEdit>{Parse("--tag", "kind=x")})
              .ok());
  core::Result<prefab::Prefab, prefab::PrefabError> again =
      prefab::LoadPrefab(output);
  REQUIRE(again.ok());
  CHECK(prefab::FindTag(again.value(), "kind") == "x");
  CHECK(prefab::FindTag(again.value(), "weapon") == "glock17");
}

TEST_CASE("A failed edit leaves the file untouched") {
  testing::ScratchDirectory scratch("prefab_edit_fail");
  const std::string path = scratch.Child("weapon.pbo");
  REQUIRE(prefab::SavePrefab(testing::MakeSamplePrefab(), path).ok());
  const auto before = platform::ReadFile(path, prefab::kMaxPrefabFileSize);
  REQUIRE(before.ok());
  const std::vector<PrefabEdit> edits{Parse("--tag", "weapon=glock17"),
                                      Parse("--event", "fire=late@9")};
  CHECK_FALSE(EditPrefabFile(path, path, edits).ok());
  const auto after = platform::ReadFile(path, prefab::kMaxPrefabFileSize);
  REQUIRE(after.ok());
  CHECK(before.value() == after.value());
  CHECK_FALSE(EditPrefabFile(scratch.Child("missing.pbo"), path, edits).ok());
}

}  // namespace
}  // namespace blue::cook
