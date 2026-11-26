#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include "camera/look.h"
#include "core/log.h"
#include "sim/c_api/blue_sim.h"
#include "sim/user_command.h"
#include "tests/sim/sim_test_scene.h"

namespace blue::sim {
namespace {

// Messages (and their levels) the log callback received. Not locked: the
// library must never call the callback on two threads at once.
struct LogSink {
  std::vector<std::string> messages;
  std::vector<int32_t> levels;
};

void Collect(void* user, int32_t level, const char* message) {
  auto* sink = static_cast<LogSink*>(user);
  sink->messages.emplace_back(message);
  sink->levels.push_back(level);
}

// A BlueSim on the test scene, destroyed at the end.
struct CSim {
  testing::TestScene files;
  BlueSim* sim = nullptr;

  CSim() {
    // The C ABI's own config only names the asset tree and scene; the test
    // scene's starting weapons sit where the defaults expect them.
    files.Save(testing::MakeWeaponPrefab("glock17"), "glock17.pbo");
    files.Save(testing::MakeWeaponPrefab("m16"), "m16.pbo");
    const std::string asset_dir = files.dir.path();
    const BlueSimConfig config{.asset_dir = asset_dir.c_str(),
                               .scene_path = "test.mscn",
                               .worker_threads = 0,
                               .max_players = 4};
    char err[256];
    REQUIRE(blue_sim_create(&config, &sim, err, sizeof(err)) == BLUE_SIM_OK);
    CHECK(std::string(err).empty());
    REQUIRE(sim != nullptr);
  }
  CSim(const CSim&) = delete;
  CSim& operator=(const CSim&) = delete;
  ~CSim() { CHECK(blue_sim_destroy(sim) == BLUE_SIM_OK); }

  std::string LastError() const {
    char buffer[256];
    REQUIRE(blue_sim_last_error(sim, buffer, sizeof(buffer)) >= 0);
    return buffer;
  }

  BluePlayerState Player(uint32_t id) const {
    BluePlayerState players[4];
    const int32_t count = blue_sim_read_players(sim, players, 4);
    REQUIRE(count >= 0);
    for (int32_t i = 0; i < count; ++i) {
      if (players[i].id == id) {
        return players[i];
      }
    }
    FAIL("no such player");
    return {};
  }
};

glm::vec3 ToGlm(const BlueVec3& v) { return {v.x, v.y, v.z}; }

TEST_CASE("The C ABI reports its version and struct sizes") {
  CHECK(BLUE_SIM_ABI_VERSION == 3);
  CHECK(blue_sim_abi_version() == BLUE_SIM_ABI_VERSION);
  CHECK(blue_sim_struct_size(BLUE_SIM_STRUCT_CONFIG) ==
        static_cast<int32_t>(sizeof(BlueSimConfig)));
  CHECK(blue_sim_struct_size(BLUE_SIM_STRUCT_VEC3) == 12);
  CHECK(blue_sim_struct_size(BLUE_SIM_STRUCT_QUAT) == 16);
  CHECK(blue_sim_struct_size(BLUE_SIM_STRUCT_USER_COMMAND) == 28);
  CHECK(blue_sim_struct_size(BLUE_SIM_STRUCT_FIREARM_STATE) == 44);
  CHECK(blue_sim_struct_size(BLUE_SIM_STRUCT_LOADOUT_SLOT) == 60);
  CHECK(blue_sim_struct_size(BLUE_SIM_STRUCT_PLAYER_STATE) == 200);
  CHECK(blue_sim_struct_size(BLUE_SIM_STRUCT_SEGMENT) == 72);
  CHECK(blue_sim_struct_size(BLUE_SIM_STRUCT_SHOT_HIT) == 16);
  CHECK(blue_sim_struct_size(BLUE_SIM_STRUCT_CAPSULE) == 20);
  CHECK(blue_sim_struct_size(BLUE_SIM_STRUCT_RAY_HIT) == 36);
  CHECK(blue_sim_struct_size(BLUE_SIM_STRUCT_BODY_STATE) == 60);
  CHECK(blue_sim_struct_size(BLUE_SIM_STRUCT_DROPPED_WEAPON) == 76);
  CHECK(blue_sim_struct_size(BLUE_SIM_STRUCT_PICKUP) == 40);
  CHECK(blue_sim_struct_size(BLUE_SIM_STRUCT_EVENT) == 60);
  CHECK(blue_sim_struct_size(0) == BLUE_SIM_ERR_INVALID_ARGUMENT);
  CHECK(blue_sim_struct_size(99) == BLUE_SIM_ERR_INVALID_ARGUMENT);
}

TEST_CASE("The log callback receives every engine log line") {
  LogSink sink;
  REQUIRE(blue_sim_set_log_callback(&Collect, &sink) == BLUE_SIM_OK);
  core::log::Info("info {}", 1);
  core::log::Warn("warn {}", 2);
  core::log::Error("error {}", 3);
  REQUIRE(sink.messages.size() == 3);
  CHECK(sink.messages[0] == "info 1");
  CHECK(sink.messages[1] == "warn 2");
  CHECK(sink.messages[2] == "error 3");
  CHECK(sink.levels == std::vector<int32_t>{BLUE_SIM_LOG_INFO,
                                            BLUE_SIM_LOG_WARN,
                                            BLUE_SIM_LOG_ERROR});

  // Several threads logging at once: every line arrives whole, one call at
  // a time (the unlocked sink would otherwise race).
  constexpr int kThreads = 4;
  constexpr int kLines = 200;
  sink = {};
  {
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
      threads.emplace_back([t] {
        for (int i = 0; i < kLines; ++i) {
          core::log::Warn("thread {} line {}", t, i);
        }
      });
    }
    for (std::thread& thread : threads) {
      thread.join();
    }
  }
  REQUIRE(sink.messages.size() == static_cast<size_t>(kThreads * kLines));
  for (const std::string& message : sink.messages) {
    CHECK(message.starts_with("thread "));
  }

  // NULL restores the default output: the sink hears nothing more.
  REQUIRE(blue_sim_set_log_callback(nullptr, &sink) == BLUE_SIM_OK);
  sink = {};
  core::log::Info("to stdout");
  CHECK(sink.messages.empty());
}

TEST_CASE("Every C ABI function refuses a null handle") {
  BlueSegment segment;
  BlueShotHit hit;
  BlueCapsule capsule;
  BlueRayHit ray;
  BluePlayerState player;
  BlueBodyState body;
  BlueDroppedWeapon dropped;
  BluePickup pickup;
  BlueSimEvent event;
  BlueUserCommand command{};
  const BlueVec3 v{0.0f, 0.0f, 0.0f};
  uint64_t hash = 0;
  char buffer[8];
  CHECK(blue_sim_destroy(nullptr) == BLUE_SIM_ERR_NULL);
  CHECK(blue_sim_last_error(nullptr, buffer, 8) == BLUE_SIM_ERR_NULL);
  CHECK(blue_sim_content_hash(nullptr, &hash) == BLUE_SIM_ERR_NULL);
  CHECK(blue_sim_add_player(nullptr, 1) == BLUE_SIM_ERR_NULL);
  CHECK(blue_sim_remove_player(nullptr, 1) == BLUE_SIM_ERR_NULL);
  CHECK(blue_sim_apply_command(nullptr, 1, &command) == BLUE_SIM_ERR_NULL);
  CHECK(blue_sim_step(nullptr, 1) == BLUE_SIM_ERR_NULL);
  CHECK(blue_sim_projectiles_sweep(nullptr, &segment, 1) == BLUE_SIM_ERR_NULL);
  CHECK(blue_sim_projectiles_resolve(nullptr, &hit, 1) == BLUE_SIM_ERR_NULL);
  CHECK(blue_sim_hit_capsules_now(nullptr, &hit, 1) == BLUE_SIM_ERR_NULL);
  CHECK(blue_sim_player_capsule(nullptr, 1, &capsule) == BLUE_SIM_ERR_NULL);
  CHECK(blue_sim_cast_ray(nullptr, &v, &v, 1.0f, &ray) == BLUE_SIM_ERR_NULL);
  CHECK(blue_sim_read_players(nullptr, &player, 1) == BLUE_SIM_ERR_NULL);
  CHECK(blue_sim_read_bodies(nullptr, &body, 1) == BLUE_SIM_ERR_NULL);
  CHECK(blue_sim_read_dropped_weapons(nullptr, &dropped, 1) ==
        BLUE_SIM_ERR_NULL);
  CHECK(blue_sim_read_pickups(nullptr, &pickup, 1) == BLUE_SIM_ERR_NULL);
  CHECK(blue_sim_poll_events(nullptr, &event, 1) == BLUE_SIM_ERR_NULL);
}

TEST_CASE("blue_sim_create fails cleanly") {
  const testing::TestScene files;
  const std::string asset_dir = files.dir.path();
  BlueSimConfig config{.asset_dir = asset_dir.c_str(),
                       .scene_path = "missing.mscn",
                       .worker_threads = 0,
                       .max_players = 4};
  BlueSim* sim = reinterpret_cast<BlueSim*>(&config);  // Must be cleared.
  char err[256];
  CHECK(blue_sim_create(&config, nullptr, err, sizeof(err)) ==
        BLUE_SIM_ERR_NULL);
  CHECK(blue_sim_create(nullptr, &sim, err, sizeof(err)) == BLUE_SIM_ERR_NULL);
  CHECK(sim == nullptr);

  LogSink sink;
  CHECK(blue_sim_set_log_callback(&Collect, &sink) == BLUE_SIM_OK);
  CHECK(blue_sim_create(&config, &sim, err, sizeof(err)) ==
        BLUE_SIM_ERR_CREATE_FAILED);
  CHECK(sim == nullptr);
  CHECK(std::string(err).find("missing.mscn") != std::string::npos);
  REQUIRE_FALSE(sink.messages.empty());
  CHECK(sink.messages.back().find("missing.mscn") != std::string::npos);
  CHECK(sink.levels.back() == BLUE_SIM_LOG_ERROR);
  CHECK(blue_sim_set_log_callback(nullptr, nullptr) == BLUE_SIM_OK);

  // The message is truncated to the buffer; a null buffer is fine.
  char tiny[4];
  CHECK(blue_sim_create(&config, &sim, tiny, sizeof(tiny)) ==
        BLUE_SIM_ERR_CREATE_FAILED);
  CHECK(std::string(tiny).size() == 3);
  CHECK(blue_sim_create(&config, &sim, nullptr, 0) ==
        BLUE_SIM_ERR_CREATE_FAILED);

  // The default starting weapons (glock17.pbo, m16.pbo) are missing here.
  config.scene_path = "test.mscn";
  CHECK(blue_sim_create(&config, &sim, err, sizeof(err)) ==
        BLUE_SIM_ERR_CREATE_FAILED);
  CHECK(std::string(err).find("glock17.pbo") != std::string::npos);

  config.scene_path = nullptr;
  CHECK(blue_sim_create(&config, &sim, err, sizeof(err)) == BLUE_SIM_ERR_NULL);
  config.scene_path = "test.mscn";
  config.max_players = 0;
  CHECK(blue_sim_create(&config, &sim, err, sizeof(err)) ==
        BLUE_SIM_ERR_INVALID_ARGUMENT);
  config.max_players = 4;
  config.worker_threads = -1;
  CHECK(blue_sim_create(&config, &sim, err, sizeof(err)) ==
        BLUE_SIM_ERR_INVALID_ARGUMENT);
  CHECK(sim == nullptr);
}

TEST_CASE("The C ABI rejects bad arguments on a live sim") {
  CSim c;
  CHECK(c.LastError().empty());
  CHECK(blue_sim_add_player(c.sim, 1) == BLUE_SIM_OK);
  CHECK(blue_sim_add_player(c.sim, 1) == BLUE_SIM_ERR_PLAYER_REFUSED);
  CHECK(c.LastError().find("player 1") != std::string::npos);
  for (uint32_t id = 2; id <= 4; ++id) {
    CHECK(blue_sim_add_player(c.sim, id) == BLUE_SIM_OK);
  }
  CHECK(blue_sim_add_player(c.sim, 5) == BLUE_SIM_ERR_PLAYER_REFUSED);
  CHECK(blue_sim_remove_player(c.sim, 9) == BLUE_SIM_ERR_UNKNOWN_PLAYER);
  CHECK(c.LastError() == "unknown player 9");
  const BlueUserCommand command{};
  CHECK(blue_sim_apply_command(c.sim, 9, &command) ==
        BLUE_SIM_ERR_UNKNOWN_PLAYER);
  CHECK(blue_sim_apply_command(c.sim, 1, nullptr) == BLUE_SIM_ERR_NULL);
  BlueCapsule capsule;
  CHECK(blue_sim_player_capsule(c.sim, 9, &capsule) ==
        BLUE_SIM_ERR_UNKNOWN_PLAYER);
  CHECK(blue_sim_player_capsule(c.sim, 1, nullptr) == BLUE_SIM_ERR_NULL);
  CHECK(blue_sim_content_hash(c.sim, nullptr) == BLUE_SIM_ERR_NULL);
  const BlueVec3 v{0.0f, 0.0f, 0.0f};
  CHECK(blue_sim_cast_ray(c.sim, nullptr, &v, 1.0f, nullptr) ==
        BLUE_SIM_ERR_NULL);

  // Too small buffers write nothing and say how much is needed.
  BluePlayerState players[4];
  CHECK(blue_sim_read_players(c.sim, nullptr, 0) == 4);  // The count.
  CHECK(blue_sim_read_players(c.sim, players, 3) ==
        BLUE_SIM_ERR_BUFFER_TOO_SMALL);
  CHECK(c.LastError() == "buffer of 3 entries, 4 needed");
  CHECK(blue_sim_read_players(c.sim, nullptr, 2) ==
        BLUE_SIM_ERR_INVALID_ARGUMENT);
  CHECK(blue_sim_read_players(c.sim, players, -1) ==
        BLUE_SIM_ERR_INVALID_ARGUMENT);
  CHECK(blue_sim_read_players(c.sim, players, 4) == 4);
  CHECK(blue_sim_read_bodies(c.sim, nullptr, 0) == 1);
  BlueBodyState no_room[1];
  CHECK(blue_sim_read_bodies(c.sim, no_room, 0) ==
        BLUE_SIM_ERR_BUFFER_TOO_SMALL);
  CHECK(blue_sim_read_pickups(c.sim, nullptr, 0) == 1);
  CHECK(blue_sim_read_dropped_weapons(c.sim, nullptr, 0) == 0);  // None.
  BlueShotHit hit{};
  CHECK(blue_sim_projectiles_resolve(c.sim, nullptr, 1) ==
        BLUE_SIM_ERR_INVALID_ARGUMENT);
  CHECK(blue_sim_projectiles_resolve(c.sim, &hit, -1) ==
        BLUE_SIM_ERR_INVALID_ARGUMENT);
  BlueSimEvent event;
  CHECK(blue_sim_poll_events(c.sim, nullptr, 1) ==
        BLUE_SIM_ERR_INVALID_ARGUMENT);
  CHECK(blue_sim_poll_events(c.sim, &event, 0) == 0);
  char small[6];
  CHECK(blue_sim_last_error(c.sim, small, sizeof(small)) > 5);
  CHECK(std::string(small).size() == 5);
  CHECK(blue_sim_last_error(c.sim, nullptr, 3) ==
        BLUE_SIM_ERR_INVALID_ARGUMENT);
}

TEST_CASE("A 600-tick game through the C ABI") {
  CSim c;
  uint64_t hash = 0;
  REQUIRE(blue_sim_content_hash(c.sim, &hash) == BLUE_SIM_OK);
  CHECK(hash != 0);
  REQUIRE(blue_sim_add_player(c.sim, 1) == BLUE_SIM_OK);
  REQUIRE(blue_sim_add_player(c.sim, 2) == BLUE_SIM_OK);

  BlueBodyState bodies[2];
  REQUIRE(blue_sim_read_bodies(c.sim, bodies, 2) == 1);
  CHECK(bodies[0].instance == testing::kCrateInstance);
  BluePickup pickups[2];
  REQUIRE(blue_sim_read_pickups(c.sim, pickups, 2) == 1);
  CHECK(pickups[0].instance == testing::kPickupInstance);
  CHECK(pickups[0].taken == 0);
  CHECK(pickups[0].weapon_key ==
        KeyOfWeaponName(PrefabWeaponName("weapons/rifle.pbo")));

  const BlueVec3 above{testing::kCratePosition.x, 5.0f,
                       testing::kCratePosition.z};
  const BlueVec3 down{0.0f, -1.0f, 0.0f};
  BlueRayHit ray;
  REQUIRE(blue_sim_cast_ray(c.sim, &above, &down, 10.0f, &ray) == 1);
  CHECK(ray.instance == testing::kCrateInstance);
  CHECK(ray.point.y == doctest::Approx(1.0f).epsilon(1e-3));
  const BlueVec3 up{0.0f, 1.0f, 0.0f};
  CHECK(blue_sim_cast_ray(c.sim, &above, &up, 10.0f, &ray) == 0);

  const uint32_t rifle_key = KeyOfWeaponName(PrefabWeaponName("m16.pbo"));
  int shots = 0;
  int damage = 0;
  int deaths = 0;
  int respawns = 0;
  int drops = 0;
  int segments_seen = 0;
  std::vector<BlueSegment> segments;
  std::vector<BlueShotHit> hits;
  for (uint32_t tick = 1; tick <= 600; ++tick) {
    // Player 1 fires at player 2's chest every 20 ticks; player 2 strafes.
    const BluePlayerState shooter = c.Player(1);
    BlueCapsule target{};
    REQUIRE(blue_sim_player_capsule(c.sim, 2, &target) == BLUE_SIM_OK);
    const glm::vec3 eye =
        ToGlm(shooter.position) + glm::vec3(0.0f, shooter.eye_height, 0.0f);
    const glm::vec3 chest =
        ToGlm(target.feet) + glm::vec3(0.0f, 0.6f * target.height, 0.0f);
    const camera::LookAngles look = camera::LookTowards(eye, chest);
    BlueUserCommand aim{};
    aim.tick = tick;
    aim.yaw = look.yaw;
    aim.pitch = look.pitch;
    aim.select_slot = -1;
    aim.held = tick % 20 == 0 ? buttons::held::kFire : 0u;
    REQUIRE(blue_sim_apply_command(c.sim, 1, &aim) == BLUE_SIM_OK);
    BlueUserCommand strafe{};
    strafe.tick = tick;
    strafe.select_slot = -1;
    strafe.held =
        (tick / 60) % 2 == 0 ? buttons::held::kLeft : buttons::held::kRight;
    REQUIRE(blue_sim_apply_command(c.sim, 2, &strafe) == BLUE_SIM_OK);
    REQUIRE(blue_sim_step(c.sim, tick) == BLUE_SIM_OK);

    // Sweep: ask for the count, then fetch; asking again gives the same.
    const int32_t count = blue_sim_projectiles_sweep(c.sim, nullptr, 0);
    REQUIRE(count >= 0);
    segments.resize(static_cast<size_t>(count));
    REQUIRE(blue_sim_projectiles_sweep(c.sim, segments.data(), count) == count);
    if (count > 1) {
      CHECK(blue_sim_projectiles_sweep(c.sim, segments.data(), count - 1) ==
            BLUE_SIM_ERR_BUFFER_TOO_SMALL);
    }
    segments_seen += count;
    const int32_t hit_count = blue_sim_hit_capsules_now(c.sim, nullptr, 0);
    REQUIRE(hit_count >= 0);
    hits.resize(static_cast<size_t>(hit_count));
    REQUIRE(blue_sim_hit_capsules_now(c.sim, hits.data(), hit_count) ==
            hit_count);
    for (const BlueShotHit& hit : hits) {
      CHECK(hit.victim == 2);
      CHECK(segments[static_cast<size_t>(hit.segment)].owner == 1);
    }
    REQUIRE(blue_sim_projectiles_resolve(c.sim, hits.data(), hit_count) ==
            BLUE_SIM_OK);

    // Drain the events three at a time.
    BlueSimEvent events[3];
    int32_t n = 0;
    while ((n = blue_sim_poll_events(c.sim, events, 3)) > 0) {
      for (int32_t i = 0; i < n; ++i) {
        CHECK(events[i].tick == tick);
        switch (events[i].type) {
          case BLUE_SIM_EVENT_SHOT:
            ++shots;
            break;
          case BLUE_SIM_EVENT_DAMAGE:
            // Everything the shooter's hit marker needs.
            ++damage;
            CHECK(events[i].player == 2);
            CHECK(events[i].other == 1);
            CHECK(events[i].weapon_key == rifle_key);
            CHECK(events[i].zone == BLUE_SIM_ZONE_TORSO);
            CHECK(events[i].value > 0);
            CHECK(events[i].health >= 0);
            break;
          case BLUE_SIM_EVENT_DEATH:
            ++deaths;
            CHECK(events[i].player == 2);
            CHECK(events[i].other == 1);
            CHECK(events[i].weapon_key == rifle_key);
            break;
          case BLUE_SIM_EVENT_RESPAWN:
            ++respawns;
            CHECK(events[i].yaw == 0.0f);  // The test scene's spawn yaw.
            break;
          case BLUE_SIM_EVENT_WEAPON_DROPPED: {
            // The dying victim's rifle, thrown with its rounds.
            ++drops;
            CHECK(events[i].player == 2);
            CHECK(events[i].weapon_key == rifle_key);
            BlueDroppedWeapon items[8];
            const int32_t item_count =
                blue_sim_read_dropped_weapons(c.sim, items, 8);
            REQUIRE(item_count > 0);
            const BlueDroppedWeapon* item = nullptr;
            for (int32_t j = 0; j < item_count; ++j) {
              if (items[j].id == events[i].id) {
                item = &items[j];
              }
            }
            REQUIRE(item != nullptr);
            CHECK(item->weapon_key == rifle_key);
            CHECK(item->magazine > 0);
            CHECK(item->magazine == events[i].value);
            CHECK(item->chambered == events[i].chambered);
            CHECK(item->chambered == 1);
            CHECK(item->active == 1);
            CHECK(glm::length(ToGlm(item->linear_velocity)) > 0.1f);
            break;
          }
          default:
            break;
        }
      }
    }
  }
  CHECK(shots == 30);
  CHECK(segments_seen > 0);
  CHECK(damage >= 6);
  CHECK(deaths >= 1);
  CHECK(respawns >= 1);
  CHECK(drops == deaths);
  const BluePlayerState victim = c.Player(2);
  CHECK(victim.deaths == static_cast<uint32_t>(deaths));
  CHECK(victim.slots[0].present == 1);
  CHECK(victim.slots[0].weapon_key ==
        KeyOfWeaponName(PrefabWeaponName("glock17.pbo")));
  CHECK(c.Player(1).shots_fired == 30);
  CHECK(c.Player(1).slots[1].firearm.magazine < 30);
  BlueDroppedWeapon dropped[8];
  CHECK(blue_sim_read_dropped_weapons(c.sim, dropped, 8) ==
        static_cast<int32_t>(deaths));
  CHECK(blue_sim_remove_player(c.sim, 2) == BLUE_SIM_OK);
  BluePlayerState players[4];
  CHECK(blue_sim_read_players(c.sim, players, 4) == 1);
}

}  // namespace
}  // namespace blue::sim
