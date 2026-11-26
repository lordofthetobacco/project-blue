#include "tools/net-probe/net_probe.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <format>
#include <memory>
#include <numbers>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include "camera/look.h"
#include "core/log.h"
#include "core/path.h"
#include "core/result.h"
#include "core/text.h"
#include "net/client_session.h"
#include "net/interpolation.h"
#include "net/players.h"
#include "net/prediction.h"
#include "net/replication.h"
#include "net/transport.h"
#include "net/user_command_codec.h"
#include "physics/physics_runtime.h"
#include "physics/physics_world.h"
#include "scene/mscn_serializer.h"
#include "scene/scene.h"
#include "scene/scene_data.h"
#include "sim/player_sim.h"
#include "sim/player_state.h"
#include "sim/sim_world.h"
#include "sim/user_command.h"
#include "weapon/loadout.h"
#include "world/core/collision_world.h"
#include "world/core/placement_plan.h"
#include "world/core/prefab_store.h"

namespace blue::netprobe {
namespace {

using Clock = std::chrono::steady_clock;
using OptionsResult = core::Result<ProbeOptions, std::string>;

namespace held = sim::buttons::held;
namespace pressed = sim::buttons::pressed;

// Real time between frames (the game renders at about this rate or more).
constexpr std::chrono::microseconds kFrameSleep{2000};
// Longest wait for kPlaying.
constexpr double kConnectTimeoutSeconds = 15.0;
// Replays described in detail.
constexpr int kDescribedReplays = 12;

std::optional<double> ParseNumber(std::string_view text) {
  const std::optional<float> value = core::ParseFiniteFloat(text);
  return value ? std::optional<double>(*value) : std::nullopt;
}

double Seconds(Clock::duration duration) {
  return std::chrono::duration<double>(duration).count();
}

// The script's fixed angles: a quarter turn per square side.
float SquareYaw(float base_yaw, uint32_t phase_tick) {
  constexpr float kQuarter = std::numbers::pi_v<float> / 2.0f;
  return camera::WrapYaw(base_yaw +
                         kQuarter * static_cast<float>(phase_tick / 120));
}

// The optional drop and pick-up of each cycle, during the quiet part
// (ticks 920..959): drop the weapon in hand, look down at the nearest
// dropped weapon and pick it up.
void AddInteraction(uint32_t script_tick, const sim::PlayerSim& player,
                    const net::Replication& replication,
                    sim::UserCommand& command) {
  const uint32_t t = script_tick % kScriptCycleTicks;
  if (t == 920) {
    command.pressed |= pressed::kDropWeapon;
    return;
  }
  if (t < 935 || t >= 960) {
    return;
  }
  const net::ReplicatedDroppedWeapon* nearest = nullptr;
  float best = 3.0f;  // within reach
  for (const net::ReplicatedDroppedWeapon& item :
       replication.dropped_weapons()) {
    const float distance = glm::length(item.position - player.feet());
    if (distance < best) {
      best = distance;
      nearest = &item;
    }
  }
  if (nearest == nullptr) {
    return;
  }
  const camera::LookAngles look =
      camera::LookTowards(player.eye(), nearest->position);
  command.yaw = look.yaw;
  command.pitch = look.pitch;
  if (t == 950) {
    command.pressed |= pressed::kInteract;
    command.interact_kind = net::interact_kind::kDroppedWeapon;
    command.interact_id = nearest->dropped_id;
  }
}

}  // namespace

OptionsResult ParseOptions(int argc, const char* const argv[]) {
  ProbeOptions options;
  for (int i = 1; i < argc; ++i) {
    const std::string_view flag = argv[i];
    if (flag == "--help" || flag == "-h") {
      options.show_help = true;
      return OptionsResult::Ok(options);
    }
    if (flag == "--aim-at-nearest") {
      options.aim_at_nearest = true;
      continue;
    }
    if (flag == "--trace") {
      options.trace = true;
      continue;
    }
    if (flag == "--interact") {
      options.interact = true;
      continue;
    }
    if (i + 1 >= argc) {
      return OptionsResult::Error(std::format("{} needs a value", flag));
    }
    const std::string_view value = argv[++i];
    const std::optional<double> number = ParseNumber(value);
    const auto need_number = [&](double& target, double min) {
      if (!number || *number < min) {
        return false;
      }
      target = *number;
      return true;
    };
    bool ok = true;
    if (flag == "--server") {
      options.server = std::string(value);
    } else if (flag == "--asset-dir") {
      options.asset_dir = std::string(value);
    } else if (flag == "--scene") {
      options.scene = std::string(value);
    } else if (flag == "--name") {
      options.name = std::string(value);
    } else if (flag == "--duration") {
      ok = need_number(options.duration_seconds, 1.0);
    } else if (flag == "--latency") {
      ok = need_number(options.latency_ms, 0.0);
    } else if (flag == "--jitter") {
      ok = need_number(options.jitter_ms, 0.0);
    } else if (flag == "--loss") {
      ok = need_number(options.loss_percent, 0.0) &&
           options.loss_percent <= 100.0;
    } else if (flag == "--max-replays-per-10s") {
      ok = need_number(options.max_replays_per_10s, 0.0);
    } else if (flag == "--seed") {
      const std::optional<int> seed = core::ParseInt(value);
      ok = seed.has_value() && *seed >= 0;
      if (ok) {
        options.seed = static_cast<uint64_t>(*seed);
      }
    } else {
      return OptionsResult::Error(std::format("unknown flag {}", flag));
    }
    if (!ok) {
      return OptionsResult::Error(
          std::format("bad value '{}' for {}", value, flag));
    }
  }
  if (options.asset_dir.empty()) {
    return OptionsResult::Error("--asset-dir is required");
  }
  return OptionsResult::Ok(options);
}

sim::UserCommand ScriptedCommand(uint32_t script_tick, float base_yaw,
                                 const std::optional<camera::LookAngles>& aim) {
  const uint32_t t = script_tick % kScriptCycleTicks;
  // Odd cycles mirror the sprinted side and the crouched walk, so the
  // player comes back to where it started every two cycles.
  const bool odd = (script_tick / kScriptCycleTicks) % 2 == 1;
  sim::UserCommand command;
  command.tick = script_tick;
  command.yaw = base_yaw;
  if (t < 480) {
    // The square: four sides of 2 s; jumps on the second side, a sprint on
    // the first (third in odd cycles).
    command.held = held::kForward;
    command.yaw = SquareYaw(base_yaw, t);
    if (t >= 120 && t < 240 && t % 60 == 30) {
      command.pressed |= pressed::kJump;
    }
    if (t / 120 == (odd ? 2u : 0u)) {
      command.held |= held::kSprint;
    }
  } else if (t < 600) {
    command.held = held::kForward | held::kCrouch;
    command.yaw =
        camera::WrapYaw(base_yaw + (odd ? std::numbers::pi_v<float> : 0.0f));
  } else if (t < 720) {
    // Turn on the spot, a degree per tick, looking a little up and down.
    const float turned = static_cast<float>(t - 600);
    command.yaw =
        camera::WrapYaw(base_yaw + turned * std::numbers::pi_v<float> / 180.0f);
    command.pitch = 0.3f * std::sin(turned * 0.05f);
  } else if (t < 960) {
    if (t == 720) {
      command.select_slot = 0;  // the pistol
    } else if (t == 840) {
      command.select_slot = 1;  // the rifle
    } else if (t == 900) {
      command.pressed |= pressed::kReload;
    }
  } else {
    // Bursts: 0.3 s of fire every second, aimed.
    command.held = held::kAim;
    if ((t - 960) % 60 < 18) {
      command.held |= held::kFire;
    }
  }
  if (aim.has_value() && (t < 600 || t >= 720)) {
    command.yaw = aim->yaw;
    command.pitch = aim->pitch;
  }
  return command;
}

std::string DescribeMismatch(const sim::PlayerState& p,
                             const sim::PlayerState& a) {
  std::string text;
  const auto add = [&text](std::string item) {
    if (!text.empty()) {
      text += ", ";
    }
    text += item;
  };
  const float position = glm::length(p.position - a.position);
  if (position > sim::kReplayPositionTolerance) {
    add(
        std::format("position {:.4f} m (pred {:.3f},{:.3f},{:.3f} auth "
                    "{:.3f},{:.3f},{:.3f})",
                    position, p.position.x, p.position.y, p.position.z,
                    a.position.x, a.position.y, a.position.z));
  }
  const float velocity = glm::length(p.velocity - a.velocity);
  if (velocity > sim::kReplayVelocityTolerance) {
    add(std::format("velocity {:.3f} m/s", velocity));
  }
  if (std::abs(p.eye_height - a.eye_height) > sim::kReplayScalarTolerance) {
    add(std::format("eye_height {:.4f}/{:.4f}", p.eye_height, a.eye_height));
  }
  if (std::abs(p.jump_speed - a.jump_speed) > sim::kReplayScalarTolerance) {
    add(std::format("jump_speed {}/{}", p.jump_speed, a.jump_speed));
  }
  const auto flag = [&add](const char* name, auto pv, auto av) {
    if (pv != av) {
      add(std::format("{} {}/{}", name, pv, av));
    }
  };
  flag("jump_age", p.jump_age, a.jump_age);
  flag("jump_cooldown", p.jump_cooldown, a.jump_cooldown);
  flag("jump_pending", p.jump_pending, a.jump_pending);
  flag("crouching", p.crouching, a.crouching);
  flag("on_ground", p.on_ground, a.on_ground);
  flag("sprinting", p.sprinting, a.sprinting);
  flag("aiming", p.aiming, a.aiming);
  flag("alive", p.alive, a.alive);
  flag("health", p.health, a.health);
  flag("respawn_ticks", p.respawn_ticks, a.respawn_ticks);
  flag("shots_fired", p.shots_fired, a.shots_fired);
  flag("deaths", p.deaths, a.deaths);
  flag("slot_key0", p.slot_keys[0], a.slot_keys[0]);
  flag("slot_key1", p.slot_keys[1], a.slot_keys[1]);
  if (!weapon::NearlyEqual(p.loadout, a.loadout)) {
    const weapon::LoadoutState& pl = p.loadout;
    const weapon::LoadoutState& al = a.loadout;
    std::string loadout = std::format(
        "loadout(active {}/{} selected {}/{} raise {:.4f}/{:.4f} wait {}/{}",
        pl.active ? static_cast<int>(*pl.active) : -1,
        al.active ? static_cast<int>(*al.active) : -1,
        pl.selected ? static_cast<int>(*pl.selected) : -1,
        al.selected ? static_cast<int>(*al.selected) : -1, pl.raise, al.raise,
        pl.wait_for_release, al.wait_for_release);
    for (size_t i = 0; i < pl.slots.size(); ++i) {
      const weapon::FirearmState& pf = pl.slots[i].firearm;
      const weapon::FirearmState& af = al.slots[i].firearm;
      if (!weapon::NearlyEqual(pf, af) ||
          pl.slots[i].present != al.slots[i].present) {
        loadout += std::format(
            " slot{}: mag {}/{} chamber {}/{} pull {}/{} stage {}/{} "
            "reload {:.3f}/{:.3f} cycle {:.4f}/{:.4f} incoming {}/{}",
            i, pf.magazine, af.magazine, pf.chambered, af.chambered,
            pf.pull_used, af.pull_used, pf.reload_stage, af.reload_stage,
            pf.reload_elapsed, af.reload_elapsed, pf.cycle_remaining,
            af.cycle_remaining, pf.incoming_magazine, af.incoming_magazine);
      }
    }
    add(loadout + ")");
  }
  return text.empty() ? "nothing beyond tolerance" : text;
}

int RunProbe(const ProbeOptions& options) {
  const Clock::time_point start = Clock::now();
  const auto now_seconds = [start] { return Seconds(Clock::now() - start); };

  // The scene, as the game builds it for online play.
  physics::PhysicsRuntime runtime;
  sim::SimWorldOptions world_options;
  world_options.asset_dir = options.asset_dir;
  world_options.scene_path = options.scene;
  world_options.max_players = 1;
  auto reference = sim::SimWorld::Create(runtime, world_options);
  if (!reference.ok()) {
    std::printf("error: %s\n", reference.error().c_str());
    return 2;
  }
  const std::unique_ptr<sim::SimWorld> world = std::move(reference).value();
  physics::PhysicsWorld physics(runtime, {.worker_threads = 0});
  world::PrefabStore store;
  world::CollisionWorld collision(
      physics, {.dynamic_bodies = world::DynamicBodies::kKinematic});
  {
    const scene::SceneData& scene = world->scene();
    const world::PlacementPlan plan = world::ResolvePlacements(scene, store);
    const world::PhysicsBuildSummary built =
        collision.Build(scene, plan.placements, store);
    std::printf("scene: %s, %d bodies, content hash %016llx\n",
                options.scene.c_str(), built.created,
                static_cast<unsigned long long>(world->content_hash()));
  }

  net::SessionConfig config;
  config.transport.conditions =
      net::NetworkConditions{.latency_seconds = options.latency_ms / 1000.0,
                             .jitter_seconds = options.jitter_ms / 1000.0,
                             .loss_percent = options.loss_percent,
                             .seed = options.seed};
  const net::PredictionContext context{.physics = &physics,
                                       .library = &world->weapon_library(),
                                       .table = &world->weapon_table()};
  net::ClientSession session(config, context);
  std::random_device entropy;
  const uint64_t client_id =
      ((static_cast<uint64_t>(entropy()) << 32) ^ entropy()) ^ options.seed;
  const auto connected =
      session.Connect(options.server, options.name, world->content_hash(),
                      client_id, now_seconds());
  if (!connected.ok()) {
    std::printf("error: %s\n", connected.error().c_str());
    return 2;
  }

  std::optional<double> playing_at;
  std::optional<double> lost_at;
  double previous = now_seconds();
  uint32_t script_tick = 0;
  float base_yaw = 0.0f;
  int replays_seen = 0;
  int64_t shots_predicted = 0;
  int replays_described = 0;
  double last_trace = 0.0;
  // The lowest feet of any remote player shown, and the most dropped
  // weapons replicated at once.
  float lowest_remote = 1.0e30f;
  size_t most_dropped = 0;
  for (;;) {
    const double now = now_seconds();
    const auto frame = static_cast<float>(now - previous);
    previous = now;
    session.Update(now);
    const net::SessionState state = session.state();
    if (state == net::SessionState::kDisconnected) {
      lost_at = now;
      break;
    }
    if (state == net::SessionState::kPlaying && !playing_at) {
      playing_at = now;
      base_yaw = session.player()->look().yaw;
      std::printf("playing after %.2f s as player %d\n", now,
                  static_cast<int>(session.player_id().value_or(255)));
    }
    if (!playing_at && now > kConnectTimeoutSeconds) {
      break;
    }
    if (playing_at && now - *playing_at >= options.duration_seconds) {
      break;
    }
    const net::Replication& replication = session.replication();
    const int ticks = session.AdvanceTicks(frame);
    for (int i = 0; i < ticks; ++i) {
      std::optional<camera::LookAngles> aim;
      if (options.aim_at_nearest) {
        const glm::vec3 eye = session.player()->eye();
        float best = 1.0e30f;
        for (uint8_t id = 0; id < net::kMaxPlayers; ++id) {
          const net::PlayerInfo& info = replication.player(id);
          if (id == replication.own_id() || !info.has_pose ||
              (info.pose.flags & net::player_flags::kAlive) == 0) {
            continue;
          }
          const glm::vec3 chest = info.pose.position + glm::vec3(0, 1.3f, 0);
          const float distance = glm::length(chest - eye);
          if (distance < best) {
            best = distance;
            aim = camera::LookTowards(eye, chest);
          }
        }
      }
      sim::UserCommand command = ScriptedCommand(script_tick, base_yaw, aim);
      if (options.interact) {
        AddInteraction(script_tick, *session.player(), replication, command);
      }
      ++script_tick;
      const sim::StepEvents events = session.Tick(command, now);
      shots_predicted += events.shot_count;
      for (const net::ReplicatedBody& body : replication.bodies()) {
        if (const std::optional<physics::BodyId> id =
                collision.BodyFor(body.instance_id);
            id && body.has_pose) {
          physics.MoveKinematic(
              *id, {.position = body.position, .rotation = body.rotation},
              sim::kTickSeconds);
        }
      }
      physics.StepOnce();
    }
    (void)session.UpdateView(now, frame);
    for (uint8_t id = 0; id < net::kMaxPlayers; ++id) {
      const net::PlayerInfo& info = session.replication().player(id);
      if (info.has_pose && id != session.replication().own_id()) {
        lowest_remote = std::min(lowest_remote, info.pose.position.y);
      }
    }
    most_dropped =
        std::max(most_dropped, session.replication().dropped_weapons().size());
    if (options.trace && playing_at && session.player() != nullptr &&
        now - last_trace >= 1.0) {
      last_trace = now;
      const sim::PlayerState player = session.player()->Save();
      std::printf(
          "t=%5.1f tick %u feet %.2f,%.2f,%.2f vel %.2f,%.2f,%.2f "
          "ground %d crouch %d health %d\n",
          now - *playing_at, session.next_tick(), player.position.x,
          player.position.y, player.position.z, player.velocity.x,
          player.velocity.y, player.velocity.z, player.on_ground ? 1 : 0,
          player.crouching ? 1 : 0, player.health);
    }
    if (const net::Prediction* prediction = session.prediction();
        prediction != nullptr && prediction->replays() != replays_seen) {
      replays_seen = prediction->replays();
      if (replays_described < kDescribedReplays) {
        ++replays_described;
        std::printf(
            "replay %d at server tick %u (t=%.2f s, correction %.4f m): %s\n",
            replays_seen, prediction->last_mismatch_tick(), now,
            prediction->last_correction(),
            DescribeMismatch(prediction->last_mismatch_predicted(),
                             prediction->last_mismatch_authoritative())
                .c_str());
        // What the client could have bumped into: the nearest moving
        // scene body, and the nearest replicated dropped weapon.
        const glm::vec3 feet = prediction->last_mismatch_predicted().position;
        float best = 1.0e30f;
        const world::CollisionBody* nearest = nullptr;
        for (const world::CollisionBody& body : collision.bodies()) {
          if (body.removed || body.motion != scene::MotionType::kDynamic) {
            continue;
          }
          const float d =
              glm::length(physics.InterpolatedPose(body.body).position - feet);
          if (d < best) {
            best = d;
            nearest = &body;
          }
        }
        if (nearest != nullptr) {
          const glm::vec3 at = physics.InterpolatedPose(nearest->body).position;
          std::printf(
              "  nearest dynamic body: instance %u at %.2f,%.2f,%.2f "
              "(%.2f m from the feet)\n",
              nearest->instance, at.x, at.y, at.z, best);
        }
        float best_dropped = 1.0e30f;
        for (const net::ReplicatedDroppedWeapon& item :
             session.replication().dropped_weapons()) {
          best_dropped =
              std::min(best_dropped, glm::length(item.position - feet));
        }
        std::printf(
            "  nearest dropped weapon: %.2f m (%zu replicated, the "
            "client has no bodies for them)\n",
            best_dropped, session.replication().dropped_weapons().size());
      }
    }
    std::this_thread::sleep_for(kFrameSleep);
  }

  // The summary.
  const double end = now_seconds();
  const net::SessionCounters& counters = session.counters();
  const net::NetStats stats = session.stats(end);
  const net::MessageCounts& events = session.replication().message_counts();
  const net::Prediction* prediction = session.prediction();
  const int replays = prediction != nullptr ? prediction->replays() : 0;
  const double played =
      playing_at ? (lost_at.value_or(end) - *playing_at) : 0.0;
  const double replays_per_10s = played > 0.0 ? replays * 10.0 / played : 0.0;
  const uint64_t seen = session.replication().players_seen();
  const bool reached = playing_at.has_value();
  const bool stayed = reached && !lost_at.has_value();
  const bool clean = counters.decode_errors == 0;
  const bool few_replays = replays_per_10s < options.max_replays_per_10s;
  std::printf("=== blue-net-probe summary (%s, %s) ===\n", options.name.c_str(),
              options.server.c_str());
  std::printf("network: latency %.0f ms jitter %.0f ms loss %.1f%%\n",
              options.latency_ms, options.jitter_ms, options.loss_percent);
  if (reached) {
    std::printf("time to playing: %.2f s\n", *playing_at);
  } else {
    std::printf("time to playing: never (state %s)\n",
                std::string(net::SessionStateText(session.state())).c_str());
  }
  std::printf("played: %.1f s, final state: %s%s%s\n", played,
              std::string(net::SessionStateText(session.state())).c_str(),
              lost_at ? ", reason: " : "",
              lost_at ? session.disconnect_reason().c_str() : "");
  std::printf(
      "payloads: %lld, snapshots: %lld (applied %lld, stale/duplicate %lld), "
      "decode errors: %lld, missing baselines: %lld, inconsistent: %lld, "
      "inputs sent: %lld\n",
      static_cast<long long>(counters.payloads),
      static_cast<long long>(counters.snapshots),
      static_cast<long long>(counters.applied),
      static_cast<long long>(counters.stale_or_duplicate),
      static_cast<long long>(counters.decode_errors),
      static_cast<long long>(counters.missing_baselines),
      static_cast<long long>(counters.inconsistent),
      static_cast<long long>(counters.inputs_sent));
  std::printf(
      "replays: %d (%.2f per 10 s, %d ticks replayed), largest correction: "
      "%.4f m, clock resyncs: %d\n",
      replays, replays_per_10s,
      prediction != nullptr ? prediction->replayed_ticks() : 0,
      prediction != nullptr ? prediction->max_correction() : 0.0f,
      stats.resyncs);
  std::printf(
      "input_buffer_depth: mean %.2f (smoothed %.2f, target %.2f), rtt %.1f "
      "ms (jitter %.1f ms), loss %.1f%%, snapshots/s %.1f, time scale %.3f\n",
      counters.depth_samples > 0
          ? static_cast<double>(counters.depth_sum) /
                static_cast<double>(counters.depth_samples)
          : 0.0,
      stats.input_depth, session.clock().target_depth(),
      stats.round_trip_seconds * 1000.0, stats.jitter_seconds * 1000.0,
      stats.loss_percent, stats.snapshots_per_second, stats.time_scale);
  std::printf(
      "remote players seen: %d, lowest remote feet y %.2f (kill "
      "plane %.2f), most dropped weapons at once %zu\n",
      std::popcount(seen), seen != 0 ? lowest_remote : 0.0f,
      world->kill_plane_y(), most_dropped);
  std::printf(
      "events: shots %lld (own predicted %lld), impacts %lld, damage %lld, "
      "deaths %lld, respawns %lld, drops %lld, dropped taken %lld, dropped "
      "removed %lld, pickups %lld, joined %lld, left %lld, world sync %lld, "
      "dropped weapons now %zu\n",
      static_cast<long long>(events.shots),
      static_cast<long long>(shots_predicted),
      static_cast<long long>(events.impacts),
      static_cast<long long>(events.damage),
      static_cast<long long>(events.deaths),
      static_cast<long long>(events.respawns),
      static_cast<long long>(events.drops),
      static_cast<long long>(events.dropped_taken),
      static_cast<long long>(events.dropped_removed),
      static_cast<long long>(events.pickups_taken),
      static_cast<long long>(events.joined),
      static_cast<long long>(events.left),
      static_cast<long long>(events.world_sync),
      session.replication().dropped_weapons().size());
  if (const sim::PlayerSim* player = session.player()) {
    const sim::PlayerState state = player->Save();
    std::printf(
        "final player: feet %.2f,%.2f,%.2f health %d alive %d shots %u "
        "deaths %u\n",
        state.position.x, state.position.y, state.position.z, state.health,
        state.alive ? 1 : 0, state.shots_fired, state.deaths);
  }
  const bool pass = reached && stayed && clean && few_replays;
  std::printf(
      "result: %s (played %s, connected %s, decode clean %s, replays "
      "under %.2f/10 s %s)\n",
      pass ? "PASS" : "FAIL", reached ? "yes" : "no", stayed ? "yes" : "no",
      clean ? "yes" : "no", options.max_replays_per_10s,
      few_replays ? "yes" : "no");
  session.Disconnect("probe finished");
  return pass ? 0 : 1;
}

}  // namespace blue::netprobe
