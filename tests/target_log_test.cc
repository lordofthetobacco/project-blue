#include "weapon/target_log.h"

#include <doctest/doctest.h>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include "weapon/projectile_system.h"

namespace blue::weapon {
namespace {

// A 0.5 x 1.0 m plate 100 m down -Z, facing the shooter at the origin.
Target Plate() {
  return {
      .center = glm::vec3(0.0f, 1.5f, -100.0f),
      .normal = glm::vec3(0.0f, 0.0f, 1.0f),
      .up = glm::vec3(0.0f, 1.0f, 0.0f),
      .half_extents = glm::vec2(0.25f, 0.5f),
      .range_m = 100.0f,
  };
}

Impact HitAt(glm::vec3 point, int shot_number = 1) {
  return {
      .point = point,
      .normal = glm::vec3(0.0f, 0.0f, 1.0f),
      .velocity = glm::vec3(0.0f, -1.0f, -850.0f),
      .mass = 0.004f,
      .flight_time = 0.11f,
      .distance = 100.0f,
      .shot_number = shot_number,
  };
}

TEST_CASE("a hit records its offset as the shooter sees the plate") {
  TargetLog log;
  const Target plate = Plate();
  // 10 cm right and 20 cm low, on the plate's face.
  REQUIRE(log.Record(2, plate, HitAt(glm::vec3(0.1f, 1.3f, -100.0f), 5)));
  REQUIRE(log.size() == 1);
  const TargetHit& hit = log.hit(0);
  CHECK(hit.target == 2);
  CHECK(hit.range_m == 100.0f);
  CHECK(hit.offset_m.x == doctest::Approx(0.1f));
  CHECK(hit.offset_m.y == doctest::Approx(-0.2f));
  CHECK(hit.miss_m == doctest::Approx(0.2236f).epsilon(1.0e-3));
  CHECK(hit.on_plate);
  CHECK(hit.flight_time == 0.11f);
  CHECK(hit.impact_speed == doctest::Approx(850.0f).epsilon(1.0e-5));
  // E = m v^2 / 2 = 0.004 * 850^2 / 2 = 1445 J.
  CHECK(hit.energy_j == doctest::Approx(1445.0f).epsilon(1.0e-3));
  CHECK(hit.shot_number == 5);
}

TEST_CASE("offsets follow a turned target and flag misses off the plate") {
  TargetLog log;
  Target plate = Plate();
  // Facing +X (a shooter at +X looks along -X): its right is world -Z.
  plate.center = glm::vec3(0.0f, 1.5f, 0.0f);
  plate.normal = glm::vec3(1.0f, 0.0f, 0.0f);
  plate.up = glm::vec3(0.2f, 1.0f, 0.0f);  // not in the plane: projected
  REQUIRE(log.Record(0, plate, HitAt(glm::vec3(0.0f, 1.6f, -0.3f))));
  const TargetHit& hit = log.hit(0);
  CHECK(hit.offset_m.x == doctest::Approx(0.3f));
  CHECK(hit.offset_m.y == doctest::Approx(0.1f));
  CHECK_FALSE(hit.on_plate);  // 0.3 m right of a 0.25 m half width
}

TEST_CASE("the log iterates newest first and overwrites the oldest") {
  TargetLog log(/*capacity=*/3, /*target_count=*/2);
  const Target plate = Plate();
  for (int shot = 1; shot <= 5; ++shot) {
    REQUIRE(log.Record(shot % 2, plate,
                       HitAt(glm::vec3(0.0f, 1.5f, -100.0f), shot)));
  }
  REQUIRE(log.size() == 3);
  CHECK(log.capacity() == 3);
  CHECK(log.hit(0).shot_number == 5);
  CHECK(log.hit(1).shot_number == 4);
  CHECK(log.hit(2).shot_number == 3);
  // Counts include the hits that left the ring.
  CHECK(log.hits_on(1) == 3);  // shots 1, 3, 5
  CHECK(log.hits_on(0) == 2);
  CHECK(log.total_hits() == 5);
}

TEST_CASE("out-of-range targets are refused and Clear forgets everything") {
  TargetLog log(/*capacity=*/4, /*target_count=*/2);
  const Target plate = Plate();
  CHECK_FALSE(log.Record(2, plate, HitAt(plate.center)));
  CHECK_FALSE(log.Record(-1, plate, HitAt(plate.center)));
  CHECK(log.size() == 0);
  CHECK(log.hits_on(7) == 0);
  REQUIRE(log.Record(1, plate, HitAt(plate.center)));
  log.Clear();
  CHECK(log.size() == 0);
  CHECK(log.total_hits() == 0);
  CHECK(log.hits_on(1) == 0);
  REQUIRE(log.Record(0, plate, HitAt(plate.center, 9)));
  CHECK(log.hit(0).shot_number == 9);
  CHECK(log.size() == 1);
}

}  // namespace
}  // namespace blue::weapon
