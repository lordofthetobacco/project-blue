#include "weapon/dispersion.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

namespace blue::weapon {
namespace {

constexpr int kSamples = 20000;

// Angle (radians) between two unit vectors, accurate for tiny angles.
float AngleBetween(glm::vec3 a, glm::vec3 b) {
  return std::atan2(glm::length(glm::cross(a, b)), glm::dot(a, b));
}

TEST_CASE("no accuracy figure leaves the shot exactly as it was") {
  Dispersion dispersion;
  const glm::vec3 direction = glm::normalize(glm::vec3(0.3f, -0.1f, -0.9f));
  CHECK(dispersion.Apply(direction, 0.0f) == direction);
  CHECK(dispersion.Apply(direction, -2.0f) == direction);
  CHECK(dispersion.Apply(direction, NAN) == direction);
  // Nothing was drawn: the sequence continues as a fresh generator's.
  Dispersion fresh;
  CHECK(dispersion.Apply(direction, 4.0f) == fresh.Apply(direction, 4.0f));
}

TEST_CASE("the same seed spreads the same way") {
  Dispersion a(42);
  Dispersion b(42);
  Dispersion c(43);
  const glm::vec3 forward(0.0f, 0.0f, -1.0f);
  bool differs = false;
  for (int i = 0; i < 10; ++i) {
    const glm::vec3 shot = a.Apply(forward, 4.0f);
    CHECK(shot == b.Apply(forward, 4.0f));
    differs = differs || shot != c.Apply(forward, 4.0f);
  }
  CHECK(differs);
}

TEST_CASE("shots spread uniformly over a disc of the MOA diameter") {
  const float moa = 4.4f;
  const float radius = 0.5f * moa * kRadiansPerMoa;
  for (const glm::vec3 axis :
       {glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(-1.0f, 0.0f, 0.0f),
        glm::vec3(0.0f, 1.0f, 0.0f),
        glm::normalize(glm::vec3(0.2f, 0.9f, -0.4f))}) {
    Dispersion dispersion;
    double sum_square = 0.0;
    int inner = 0;
    float largest = 0.0f;
    glm::vec3 sum(0.0f);
    glm::vec3 sum_offset(0.0f);
    for (int i = 0; i < kSamples; ++i) {
      const glm::vec3 shot = dispersion.Apply(axis, moa);
      CHECK(glm::length(shot) == doctest::Approx(1.0f));
      const float angle = AngleBetween(axis, shot);
      largest = std::max(largest, angle);
      sum_square += static_cast<double>(angle) * angle;
      inner += angle < radius / std::sqrt(2.0f) ? 1 : 0;
      sum += shot;
      sum_offset += shot - axis;
    }
    // Never outside the cone.
    CHECK(largest <= radius * 1.001f);
    CHECK(largest > radius * 0.98f);
    // Uniform over the disc: E[r^2] = R^2 / 2, half the shots inside
    // R / sqrt(2).
    CHECK(sum_square / kSamples ==
          doctest::Approx(0.5 * radius * radius).epsilon(0.03));
    CHECK(static_cast<float>(inner) / kSamples ==
          doctest::Approx(0.5f).epsilon(0.03));
    // Centred on the bore.
    CHECK(glm::length(sum_offset / static_cast<float>(kSamples)) <
          0.05f * radius);
    CHECK(AngleBetween(axis, glm::normalize(sum)) < 0.05f * radius);
  }
}

TEST_CASE("a longer direction keeps its length") {
  Dispersion dispersion;
  const glm::vec3 shot = dispersion.Apply(glm::vec3(0.0f, 0.0f, -3.0f), 10.0f);
  CHECK(glm::length(shot) == doctest::Approx(3.0f));
}

}  // namespace
}  // namespace blue::weapon
