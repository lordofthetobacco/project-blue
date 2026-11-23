#include "interact/interaction_focus.h"

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/mat3x3.hpp>
#include <glm/vec3.hpp>

namespace blue::interact {
namespace {

const glm::vec3 kForward(0.0f, 0.0f, -1.0f);

// An axis-aligned candidate.
Interactable Box(uint32_t id, glm::vec3 center, glm::vec3 half) {
  return {.id = id,
          .kind = InteractableKind::kDroppedWeapon,
          .shape = {.center = center, .half_extents = half}};
}

// A 4 cm cube, about a pistol's thickness.
Interactable Small(uint32_t id, glm::vec3 center) {
  return Box(id, center, glm::vec3(0.02f));
}

bool AllVisible(const Interactable& /*candidate*/, glm::vec3 /*aim_point*/) {
  return true;
}

// Focuses from the origin looking along `direction`.
std::optional<Focus> Look(InteractionFocus& focus,
                          const std::vector<Interactable>& candidates,
                          glm::vec3 direction = kForward,
                          glm::vec3 eye = glm::vec3(0.0f)) {
  return focus.Update({.eye = eye, .direction = direction}, candidates,
                      AllVisible);
}

// The unit direction from the origin towards `point`.
glm::vec3 Towards(glm::vec3 point) { return glm::normalize(point); }

std::optional<uint32_t> Id(const std::optional<Focus>& focus) {
  return focus ? std::optional<uint32_t>(focus->id) : std::nullopt;
}

TEST_CASE("a ray hitting the box measures no miss and the entry distance") {
  const AimMeasure aim = MeasureAim(
      glm::vec3(0.0f), kForward,
      {.center = {0.0f, 0.0f, -2.0f}, .half_extents = glm::vec3(0.1f)});
  CHECK(aim.miss_distance == 0.0f);
  CHECK(aim.miss_angle == 0.0f);
  CHECK(aim.along == doctest::Approx(1.9f));
  CHECK(aim.reach_distance == doctest::Approx(1.9f));
  CHECK(aim.aim_point.z == doctest::Approx(-1.9f));
}

TEST_CASE("a ray passing beside the box measures the gap and its angle") {
  const AimMeasure aim = MeasureAim(
      glm::vec3(0.0f), kForward,
      {.center = {0.3f, 0.0f, -2.0f}, .half_extents = glm::vec3(0.1f)});
  CHECK(aim.miss_distance == doctest::Approx(0.2f).epsilon(1.0e-3));
  CHECK(aim.along >= 1.9f - 1.0e-3f);
  CHECK(aim.along <= 2.1f + 1.0e-3f);
  CHECK(aim.miss_angle == doctest::Approx(std::atan2(0.2f, aim.along)));
  CHECK(aim.aim_point.x == doctest::Approx(0.2f).epsilon(1.0e-3));
}

TEST_CASE("an oriented box is measured in its own frame") {
  // A 2 m long, 10 cm thick bar turned to lie along the view, 10 cm to the
  // right of it: an axis-aligned box of its unturned size would be hit.
  const glm::mat3 turned(glm::vec3(0.0f, 0.0f, 1.0f),
                         glm::vec3(0.0f, 1.0f, 0.0f),
                         glm::vec3(-1.0f, 0.0f, 0.0f));
  const OrientedBox bar{.center = {0.1f, 0.0f, -3.0f},
                        .axes = turned,
                        .half_extents = {1.0f, 0.05f, 0.05f}};
  const AimMeasure aim = MeasureAim(glm::vec3(0.0f), kForward, bar);
  CHECK(aim.miss_distance == doctest::Approx(0.05f).epsilon(1.0e-3));
  CHECK(
      aim.reach_distance ==
      doctest::Approx(std::sqrt(2.0f * 2.0f + 0.05f * 0.05f)).epsilon(1.0e-3));
}

TEST_CASE("only shapes within reach are focusable") {
  InteractionFocus focus;
  // The closest point 2.4 m away: in reach.
  CHECK(Id(Look(focus, {Box(1, {0.0f, 0.0f, -2.5f}, glm::vec3(0.1f))})) == 1u);
  InteractionFocus fresh;
  // 2.6 m: out of reach.
  CHECK_FALSE(
      Look(fresh, {Box(1, {0.0f, 0.0f, -2.7f}, glm::vec3(0.1f))}).has_value());
}

TEST_CASE("the aim radius rules far away and the angle close by") {
  // At 2 m a ray 10 cm beside a small item is close enough, 15 cm is not.
  {
    InteractionFocus focus;
    CHECK(Id(Look(focus, {Small(1, {0.12f, 0.0f, -2.0f})})) == 1u);
  }
  {
    InteractionFocus focus;
    CHECK_FALSE(Look(focus, {Small(1, {0.17f, 0.0f, -2.0f})}).has_value());
  }
  // At 0.5 m the same 10 cm is too wide an angle; 6 cm is within it.
  {
    InteractionFocus focus;
    CHECK_FALSE(Look(focus, {Small(1, {0.12f, 0.0f, -0.5f})}).has_value());
  }
  {
    InteractionFocus focus;
    CHECK(Id(Look(focus, {Small(1, {0.08f, 0.0f, -0.5f})})) == 1u);
  }
}

TEST_CASE("the candidate the ray passes closest wins, ties by distance") {
  InteractionFocus focus;
  const std::vector<Interactable> side_by_side{Small(1, {0.10f, 0.0f, -2.0f}),
                                               Small(2, {0.03f, 0.0f, -2.0f}),
                                               Small(3, {-0.09f, 0.0f, -2.0f})};
  CHECK(Id(Look(focus, side_by_side)) == 2u);

  InteractionFocus in_line;
  const std::vector<Interactable> one_behind_the_other{
      Small(1, {0.0f, 0.0f, -2.0f}), Small(2, {0.0f, 0.0f, -1.2f})};
  CHECK(Id(Look(in_line, one_behind_the_other)) == 2u);
}

TEST_CASE("the focus does not flicker between two neighbours") {
  InteractionFocus focus;
  const std::vector<Interactable> pair{Small(1, {-0.06f, 0.0f, -2.0f}),
                                       Small(2, {0.06f, 0.0f, -2.0f})};
  REQUIRE(Id(Look(focus, pair, Towards({-0.06f, 0.0f, -2.0f}))) == 1u);
  // Slightly nearer the right one: not clearly better, the left one stays.
  for (const float x : {0.0f, 0.01f, -0.01f, 0.015f, 0.0f}) {
    CHECK(Id(Look(focus, pair, Towards({x, 0.0f, -2.0f}))) == 1u);
  }
  // Straight at the right one: clearly better.
  CHECK(Id(Look(focus, pair, Towards({0.06f, 0.0f, -2.0f}))) == 2u);
  CHECK(Id(Look(focus, pair, Towards({-0.01f, 0.0f, -2.0f}))) == 2u);
}

TEST_CASE("the focus is kept past the entry limits and released beyond") {
  const std::vector<Interactable> item{
      Box(1, {0.0f, 0.0f, -2.5f}, glm::vec3(0.1f))};
  InteractionFocus focus;
  // Closest point 2.4 m: focused.
  REQUIRE(Id(Look(focus, item)) == 1u);
  // Backing off to 2.6 m keeps it (a fresh look would not take it)...
  const glm::vec3 back_0_2(0.0f, 0.0f, 0.2f);
  CHECK(Id(Look(focus, item, kForward, back_0_2)) == 1u);
  InteractionFocus fresh;
  CHECK_FALSE(Look(fresh, item, kForward, back_0_2).has_value());
  // ...but 2.8 m is beyond the release reach.
  CHECK_FALSE(
      Look(focus, item, kForward, glm::vec3(0.0f, 0.0f, 0.4f)).has_value());
  CHECK_FALSE(focus.focus().has_value());

  // Sideways at 2 m: a 15 cm miss keeps a focus, 20 cm releases it.
  const std::vector<Interactable> small{Small(2, {0.0f, 0.0f, -2.0f})};
  InteractionFocus aimed;
  REQUIRE(Id(Look(aimed, small)) == 2u);
  CHECK(Id(Look(aimed, small, Towards({0.17f, 0.0f, -2.0f}))) == 2u);
  InteractionFocus not_yet;
  CHECK_FALSE(Look(not_yet, small, Towards({0.17f, 0.0f, -2.0f})).has_value());
  CHECK_FALSE(Look(aimed, small, Towards({0.22f, 0.0f, -2.0f})).has_value());
}

TEST_CASE("hidden candidates are skipped in preference order") {
  InteractionFocus focus;
  const std::vector<Interactable> pair{Small(1, {0.0f, 0.0f, -2.0f}),
                                       Small(2, {0.05f, 0.0f, -2.0f})};
  std::vector<uint32_t> asked;
  const auto hide_first = [&](const Interactable& candidate, glm::vec3) {
    asked.push_back(candidate.id);
    return candidate.id != 1;
  };
  CHECK(Id(focus.Update({.direction = kForward}, pair, hide_first)) == 2u);
  CHECK(asked == std::vector<uint32_t>{1, 2});
  const auto hide_all = [](const Interactable&, glm::vec3) { return false; };
  CHECK_FALSE(
      focus.Update({.direction = kForward}, pair, hide_all).has_value());
  CHECK_FALSE(focus.focus().has_value());
}

TEST_CASE("the line of sight is asked about the point aimed at") {
  InteractionFocus focus;
  glm::vec3 asked(0.0f);
  const auto record = [&](const Interactable&, glm::vec3 aim_point) {
    asked = aim_point;
    return true;
  };
  (void)focus.Update(
      {.direction = kForward},
      std::vector<Interactable>{Box(1, {0.0f, 0.0f, -2.0f}, glm::vec3(0.1f))},
      record);
  CHECK(asked.z == doctest::Approx(-1.9f));
  CHECK(asked.x == doctest::Approx(0.0f));
}

TEST_CASE("no candidates and degenerate views focus nothing") {
  InteractionFocus focus;
  const std::vector<Interactable> item{Small(1, {0.0f, 0.0f, -1.0f})};
  REQUIRE(Look(focus, item).has_value());
  CHECK_FALSE(Look(focus, {}).has_value());
  CHECK_FALSE(focus.focus().has_value());

  CHECK_FALSE(Look(focus, item, glm::vec3(0.0f)).has_value());
  const float nan = std::numeric_limits<float>::quiet_NaN();
  CHECK_FALSE(Look(focus, item, glm::vec3(nan, 0.0f, -1.0f)).has_value());
  CHECK_FALSE(
      Look(focus, item, kForward, glm::vec3(nan, 0.0f, 0.0f)).has_value());
  // A direction need not be normalised.
  CHECK(Id(Look(focus, item, kForward * 7.0f)) == 1u);
}

TEST_CASE("an eye inside the shape focuses it, wherever it looks") {
  InteractionFocus focus;
  const std::vector<Interactable> around{
      Box(1, glm::vec3(0.0f), glm::vec3(0.5f))};
  CHECK(Id(Look(focus, around, glm::vec3(0.0f, 1.0f, 0.0f))) == 1u);
  const AimMeasure aim =
      MeasureAim(glm::vec3(0.0f), kForward, around.front().shape);
  CHECK(aim.reach_distance == 0.0f);
  CHECK(aim.along == 0.0f);
  CHECK(aim.miss_distance == 0.0f);
}

TEST_CASE("a shape behind the eye is not focused") {
  InteractionFocus focus;
  CHECK_FALSE(Look(focus, {Small(1, {0.0f, 0.0f, 1.0f})}).has_value());
}

}  // namespace
}  // namespace blue::interact
