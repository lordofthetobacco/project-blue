// Tests of the death records of net::Replication (net/replication.h): a Death
// message is recorded with its hit data and the zone of the killing Damage.

#include <doctest/doctest.h>

#include <cstdint>
#include <optional>

#include <glm/vec3.hpp>

#include "net/messages.h"
#include "net/replication.h"

namespace blue::net {
namespace {

constexpr uint8_t kVictim = 3;
constexpr uint8_t kKiller = 4;

Damage DamageBy(std::optional<uint8_t> attacker, uint8_t zone,
                uint8_t victim = kVictim) {
  return Damage{.victim = victim,
                .attacker = attacker,
                .amount = 40,
                .zone = zone,
                .health_after = 0};
}

DeathHit Hit() {
  return DeathHit{.offset = glm::vec3(0.25f, 1.5f, -0.5f),
                  .velocity = glm::vec3(-300.0f, 2.0f, 40.0f)};
}

Death KilledBy(uint8_t killer, std::optional<DeathHit> hit = Hit()) {
  return Death{
      .victim = kVictim, .killer = killer, .weapon_key = 0xABCD, .hit = hit};
}

TEST_CASE(
    "a Death after a Damage of the same attacker takes its zone and hit") {
  Replication replication;
  replication.Reset(std::nullopt);
  replication.Apply(DamageBy(kKiller, hit_zone::kHead), 10.0);
  replication.Apply(KilledBy(kKiller), 10.25);

  REQUIRE(replication.deaths().end() == 1);
  const DeathRecord& record = replication.deaths().Get(0);
  CHECK(record.victim == kVictim);
  CHECK(record.killer == kKiller);
  CHECK(record.weapon_key == 0xABCD);
  CHECK(record.zone == hit_zone::kHead);
  CHECK(record.has_hit);
  CHECK(record.hit_offset == glm::vec3(0.25f, 1.5f, -0.5f));
  CHECK(record.hit_velocity == glm::vec3(-300.0f, 2.0f, 40.0f));
  CHECK(record.time_seconds == 10.25);
  // The kill feed still gets its line.
  CHECK(replication.kills().end() == 1);
}

TEST_CASE("the zone comes from the last Damage of the victim") {
  Replication replication;
  replication.Reset(std::nullopt);
  replication.Apply(DamageBy(kKiller, hit_zone::kHead), 10.0);
  replication.Apply(DamageBy(kKiller, hit_zone::kLegs), 10.1);
  // Damage to someone else does not disturb it.
  replication.Apply(DamageBy(kKiller, hit_zone::kTorso, 9), 10.15);
  replication.Apply(KilledBy(kKiller), 10.2);
  CHECK(replication.deaths().Get(0).zone == hit_zone::kLegs);
}

TEST_CASE("a Death without a fitting Damage has the other zone") {
  SUBCASE("another attacker") {
    Replication replication;
    replication.Reset(std::nullopt);
    replication.Apply(DamageBy(kKiller + 1, hit_zone::kHead), 10.0);
    replication.Apply(KilledBy(kKiller), 10.1);
    CHECK(replication.deaths().Get(0).zone == hit_zone::kOther);
  }
  SUBCASE("more than a second earlier") {
    Replication replication;
    replication.Reset(std::nullopt);
    replication.Apply(DamageBy(kKiller, hit_zone::kHead), 10.0);
    replication.Apply(KilledBy(kKiller), 11.5);
    CHECK(replication.deaths().Get(0).zone == hit_zone::kOther);
  }
  SUBCASE("exactly a second earlier still counts") {
    Replication replication;
    replication.Reset(std::nullopt);
    replication.Apply(DamageBy(kKiller, hit_zone::kHead), 10.0);
    replication.Apply(KilledBy(kKiller), 11.0);
    CHECK(replication.deaths().Get(0).zone == hit_zone::kHead);
  }
  SUBCASE("no damage at all") {
    Replication replication;
    replication.Reset(std::nullopt);
    replication.Apply(KilledBy(kKiller), 10.0);
    CHECK(replication.deaths().Get(0).zone == hit_zone::kOther);
  }
  SUBCASE("the damage was already used by an earlier death") {
    Replication replication;
    replication.Reset(std::nullopt);
    replication.Apply(DamageBy(kKiller, hit_zone::kHead), 10.0);
    replication.Apply(KilledBy(kKiller), 10.1);
    replication.Apply(KilledBy(kKiller), 10.2);
    REQUIRE(replication.deaths().end() == 2);
    CHECK(replication.deaths().Get(1).zone == hit_zone::kOther);
  }
}

TEST_CASE("a kill plane Death has no hit") {
  Replication replication;
  replication.Reset(std::nullopt);
  replication.Apply(Death{.victim = kVictim}, 5.0);
  REQUIRE(replication.deaths().end() == 1);
  const DeathRecord& record = replication.deaths().Get(0);
  CHECK_FALSE(record.killer.has_value());
  CHECK(record.zone == hit_zone::kOther);
  CHECK_FALSE(record.has_hit);
  CHECK(record.hit_offset == glm::vec3(0.0f));
  CHECK(record.hit_velocity == glm::vec3(0.0f));
}

TEST_CASE("world damage then a world Death keeps the other zone") {
  Replication replication;
  replication.Reset(std::nullopt);
  replication.Apply(DamageBy(std::nullopt, hit_zone::kOther), 5.0);
  replication.Apply(Death{.victim = kVictim}, 5.0);
  CHECK(replication.deaths().Get(0).zone == hit_zone::kOther);
  CHECK_FALSE(replication.deaths().Get(0).has_hit);
}

TEST_CASE("death records keep arrival order and the newest sixteen") {
  Replication replication;
  replication.Reset(std::nullopt);
  const uint64_t total = Replication::kDeathHistory + 5;
  for (uint64_t i = 0; i < total; ++i) {
    replication.Apply(
        Death{.victim = kVictim, .weapon_key = static_cast<uint32_t>(i)},
        static_cast<double>(i));
  }
  CHECK(replication.deaths().end() == total);
  CHECK(replication.deaths().first() == total - Replication::kDeathHistory);
  for (uint64_t n = replication.deaths().first();
       n < replication.deaths().end(); ++n) {
    CHECK(replication.deaths().Get(n).weapon_key == n);
  }
}

TEST_CASE("Reset clears the death records and the remembered damage") {
  Replication replication;
  replication.Reset(std::nullopt);
  replication.Apply(DamageBy(kKiller, hit_zone::kHead), 10.0);
  replication.Apply(KilledBy(kKiller), 10.1);
  replication.Apply(DamageBy(kKiller, hit_zone::kHead), 20.0);
  REQUIRE(replication.deaths().end() == 1);

  replication.Reset(std::nullopt);
  CHECK(replication.deaths().end() == 0);
  CHECK(replication.deaths().first() == 0);
  replication.Apply(KilledBy(kKiller), 20.1);
  REQUIRE(replication.deaths().end() == 1);
  CHECK(replication.deaths().Get(0).zone == hit_zone::kOther);
}

}  // namespace
}  // namespace blue::net
