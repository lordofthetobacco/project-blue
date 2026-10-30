#include "camera/look.h"

#include <doctest/doctest.h>

#include <cmath>

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/trigonometric.hpp>

namespace blue::camera {

namespace {

bool NearlyEqual(glm::vec3 a, glm::vec3 b, float eps = 1.0e-5f) {
  return glm::length(a - b) < eps;
}

}  // namespace

TEST_CASE("WrapYaw keeps angles in (-pi, pi]") {
  CHECK(WrapYaw(0.0f) == 0.0f);
  CHECK(WrapYaw(glm::pi<float>()) == doctest::Approx(glm::pi<float>()));
  CHECK(WrapYaw(-glm::pi<float>()) == doctest::Approx(glm::pi<float>()));
  CHECK(WrapYaw(3.0f * glm::pi<float>()) == doctest::Approx(glm::pi<float>()));
  CHECK(WrapYaw(glm::two_pi<float>() + 0.5f) == doctest::Approx(0.5f));
}

TEST_CASE("Yaw 0 looks down -Z and the flat vectors match the forward") {
  CHECK(NearlyEqual(ForwardFrom({}), {0.0f, 0.0f, -1.0f}));
  CHECK(NearlyEqual(FlatForwardFrom(0.0f), {0.0f, 0.0f, -1.0f}));
  CHECK(NearlyEqual(FlatRightFrom(0.0f), {1.0f, 0.0f, 0.0f}));

  const float left = glm::half_pi<float>();
  CHECK(NearlyEqual(ForwardFrom({.yaw = left}), {-1.0f, 0.0f, 0.0f}));
  CHECK(NearlyEqual(FlatRightFrom(left), {0.0f, 0.0f, -1.0f}));

  const LookAngles up{.yaw = 0.3f, .pitch = glm::half_pi<float>()};
  CHECK(ForwardFrom(up).y == doctest::Approx(1.0f));
  for (const float yaw : {-2.0f, 0.0f, 1.0f, 3.0f}) {
    CHECK(glm::dot(FlatForwardFrom(yaw), FlatRightFrom(yaw)) ==
          doctest::Approx(0.0f));
    CHECK(NearlyEqual(FlatForwardFrom(yaw), ForwardFrom({.yaw = yaw})));
  }
}

TEST_CASE("Mouse look turns by delta times sensitivity and clamps pitch") {
  input::InputSettings settings;
  settings.mouse_sensitivity = 0.01f;

  LookAngles look = ApplyMouseLook({}, {10.0f, 0.0f}, settings);
  CHECK(look.yaw == doctest::Approx(-0.1f));  // mouse right turns right
  CHECK(look.pitch == 0.0f);

  look = ApplyMouseLook({}, {0.0f, -10.0f}, settings);  // mouse up
  CHECK(look.pitch == doctest::Approx(0.1f));
  settings.invert_y = true;
  look = ApplyMouseLook({}, {0.0f, -10.0f}, settings);
  CHECK(look.pitch == doctest::Approx(-0.1f));

  look = ApplyMouseLook({}, {0.0f, 100000.0f}, settings);
  CHECK(look.pitch == doctest::Approx(glm::radians(89.0f)));
}

TEST_CASE("LookTowards reproduces the direction and defaults when degenerate") {
  const glm::vec3 from{1.0f, 2.0f, 3.0f};
  const glm::vec3 to{-2.0f, 4.0f, 0.5f};
  const LookAngles look = LookTowards(from, to);
  CHECK(NearlyEqual(ForwardFrom(look), glm::normalize(to - from)));

  CHECK(LookTowards(from, from) == LookAngles{});
  CHECK(LookTowards(from, {NAN, 0.0f, 0.0f}) == LookAngles{});
}

}  // namespace blue::camera
