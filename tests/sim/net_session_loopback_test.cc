// net::ClientSession against the in-process TestServer
// (net_session_test_server.h) over 127.0.0.1: handshake, prediction against
// the authoritative sim::SimWorld, a forced server correction, remote player
// interpolation and clock sync, on a perfect network and on one with
// 80 +- 20 ms latency and 5% loss (the Transport's simulator). Skipped (not
// failed) when the port cannot be bound. Simulated time advances one tick
// per frame; a 1 ms real sleep lets loopback datagrams land.

#include <doctest/doctest.h>
#include <netcode.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include "camera/look.h"
#include "net/client_session.h"
#include "net/interpolation.h"
#include "net/prediction.h"
#include "net/replication.h"
#include "net/transport.h"
#include "physics/physics_runtime.h"
#include "physics/physics_world.h"
#include "scene/mscn_serializer.h"
#include "scene/scene_data.h"
#include "sim/player_state.h"
#include "sim/sim_world.h"
#include "sim/user_command.h"
#include "tests/sim/net_session_test_server.h"
#include "tests/sim/sim_test_scene.h"
#include "world/core/collision_world.h"
#include "world/core/placement_plan.h"
#include "world/core/prefab_store.h"

namespace blue::net {
namespace {

constexpr float kFrameSeconds = sim::kTickSeconds;
constexpr double kRealSleepSeconds = 0.001;
constexpr uint8_t kBot = 1;

// The client's side: the scene with kinematic dynamic bodies to predict in,
// a reference SimWorld for the weapon table and content hash (as the game
// does), and the session.
struct ClientRig {
  physics::PhysicsWorld physics;
  world::PrefabStore store;
  world::CollisionWorld collision{
      physics, {.dynamic_bodies = world::DynamicBodies::kKinematic}};
  std::unique_ptr<sim::SimWorld> reference;
  std::unique_ptr<ClientSession> session;

  ClientRig(const physics::PhysicsRuntime& runtime,
            const sim::testing::TestScene& scene,
            const NetworkConditions& conditions)
      : physics(runtime, {.worker_threads = 0}) {
    auto loaded = scene::LoadMscn(scene.dir.Child("test.mscn"));
    REQUIRE(loaded.ok());
    const scene::SceneData data = std::move(loaded).value();
    const world::PlacementPlan plan = world::ResolvePlacements(data, store);
    (void)collision.Build(data, plan.placements, store);
    auto built = sim::SimWorld::Create(runtime, scene.Options());
    REQUIRE(built.ok());
    reference = std::move(built).value();
    SessionConfig config;
    config.transport.conditions = conditions;
    const PredictionContext context{.physics = &physics,
                                    .library = &reference->weapon_library(),
                                    .table = &reference->weapon_table()};
    session = std::make_unique<ClientSession>(config, context);
  }
};

// Commands of the scripted client: walks towards +X for `walk_ticks`, then
// stands.
sim::UserCommand ClientCommand(uint32_t tick, uint32_t walk_from,
                               uint32_t walk_ticks) {
  const camera::LookAngles look =
      camera::LookTowards(glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f));
  sim::UserCommand command;
  command.tick = tick;
  command.yaw = look.yaw;
  command.pitch = 0.0f;
  if (tick - walk_from < walk_ticks) {
    command.held = sim::buttons::held::kForward;
  }
  return command;
}

// One frame: server receives, client updates, predicts its due ticks and
// renders; server runs one tick. Returns the ticks the client ran.
struct Loop {
  testing::TestServer& server;
  ClientRig& client;
  double time = 0.0;
  uint32_t walk_from = 0;
  uint32_t walk_ticks = 0;
  // The client's predicted feet per tick (as first predicted).
  std::vector<std::pair<uint32_t, glm::vec3>> predicted;

  int Frame() {
    server.Receive(time);
    ClientSession& session = *client.session;
    session.Update(time);
    const int ticks = session.AdvanceTicks(kFrameSeconds);
    for (int i = 0; i < ticks; ++i) {
      const uint32_t tick = session.next_tick();
      (void)session.Tick(ClientCommand(tick, walk_from, walk_ticks), time);
      predicted.emplace_back(tick, session.player()->feet());
      client.physics.StepOnce();
    }
    (void)session.UpdateView(time, kFrameSeconds);
    server.Tick(time);
    netcode_sleep(kRealSleepSeconds);
    time += kFrameSeconds;
    return ticks;
  }

  // Runs frames until the session plays (or `max_frames`).
  bool RunUntilPlaying(int max_frames) {
    for (int i = 0; i < max_frames; ++i) {
      if (client.session->state() == SessionState::kPlaying) {
        return true;
      }
      if (client.session->state() == SessionState::kDisconnected) {
        return false;
      }
      Frame();
    }
    return client.session->state() == SessionState::kPlaying;
  }
};

// Connects a rig to `server` and returns the loop once playing.
bool ConnectAndPlay(Loop& loop, const char* address) {
  ClientSession& session = *loop.client.session;
  const auto connected =
      session.Connect(address, "tester", loop.client.reference->content_hash(),
                      0x1234, loop.time);
  REQUIRE(connected.ok());
  return loop.RunUntilPlaying(600);
}

// Returns the largest distance between the client's predictions and the
// server's positions over ticks after `from`, and how many were compared.
std::pair<float, int> PredictionError(const Loop& loop, uint32_t from) {
  float worst = 0.0f;
  int compared = 0;
  for (const auto& [tick, feet] : loop.predicted) {
    if (static_cast<int32_t>(tick - from) < 0) {
      continue;
    }
    if (const std::optional<glm::vec3> server =
            loop.server.ClientPositionAt(tick)) {
      worst = std::max(worst, glm::length(*server - feet));
      ++compared;
    }
  }
  return {worst, compared};
}

TEST_CASE("session handshake, prediction and interpolation on loopback") {
  NetcodeRuntime netcode;
  REQUIRE(netcode.ok());
  netcode_log_level(NETCODE_LOG_LEVEL_NONE);
  constexpr char kAddress[] = "127.0.0.1:47410";
  physics::PhysicsRuntime runtime;
  sim::testing::TestScene scene;
  testing::TestServer server(runtime, scene, kAddress, 0.0);
  if (!server.ok()) {
    MESSAGE("skipped: cannot bind ", kAddress);
    return;
  }
  server.AddBot(kBot);
  ClientRig client(runtime, scene, NetworkConditions{});
  Loop loop{.server = server, .client = client};
  REQUIRE(ConnectAndPlay(loop, kAddress));
  ClientSession& session = *client.session;
  CHECK(session.player_id() == testing::TestServer::kClientPlayer);
  CHECK(session.replication().player(kBot).joined);
  CHECK(session.replication().world_synced());

  // Settle for a second, then walk for 3 s and stand for 2 s; the bot walks
  // towards +Z meanwhile.
  for (int i = 0; i < 60; ++i) {
    loop.Frame();
  }
  const int replays_before = session.prediction()->replays();
  loop.walk_from = session.next_tick();
  loop.walk_ticks = 180;
  const sim::PlayerSim* bot = server.world().player(kBot);
  REQUIRE(bot != nullptr);
  sim::UserCommand bot_walk;
  bot_walk.held = sim::buttons::held::kForward;
  bot_walk.yaw =
      camera::LookTowards(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 1.0f)).yaw;
  server.SetBotCommand(kBot, bot_walk);

  float last_bot_z = -1.0e9f;
  bool monotonic = true;
  double lag_sum = 0.0;
  int lag_samples = 0;
  for (int i = 0; i < 300; ++i) {
    if (i == 90) {
      server.SetBotCommand(kBot, sim::UserCommand{.yaw = bot_walk.yaw});
    }
    loop.Frame();
    const PlayerInfo& remote = session.replication().player(kBot);
    if (remote.has_pose) {
      monotonic = monotonic && remote.pose.position.z >= last_bot_z - 1.0e-4f;
      last_bot_z = remote.pose.position.z;
      if (session.newest_server_tick().has_value()) {
        lag_sum += TicksBetween(ViewTime{.tick = *session.newest_server_tick()},
                                session.interpolation().render_time());
        ++lag_samples;
      }
    }
  }
  const auto [worst, compared] = PredictionError(loop, loop.walk_from);
  CHECK(compared > 200);
  CHECK(worst < 0.01f);
  CHECK(session.prediction()->replays() == replays_before);
  // The player really walked.
  const std::optional<sim::PlayerState> state = server.ClientState();
  REQUIRE(state.has_value());
  CHECK(std::abs(state->position.x) > 5.0f);

  // Remote interpolation is monotonic and lags by about the configured
  // delay (2 ticks plus jitter, measured from the newest snapshot).
  CHECK(monotonic);
  CHECK(last_bot_z > 2.0f);
  REQUIRE(lag_samples > 0);
  const double lag = lag_sum / lag_samples;
  CHECK(lag >= 1.5);
  CHECK(lag <= session.interpolation().delay_ticks() + 2.0);

  // Clock sync holds the buffer target and never resynced after starting.
  CHECK(std::abs(session.clock().smoothed_depth() -
                 session.clock().target_depth()) < 1.5f);
  CHECK(session.clock().resyncs() == 0);

  // A server-side teleport is corrected by the next snapshot.
  server.TeleportClient();
  const std::optional<sim::PlayerState> moved = server.ClientState();
  REQUIRE(moved.has_value());
  bool corrected = false;
  for (int i = 0; i < 30 && !corrected; ++i) {
    loop.Frame();
    corrected = glm::length(session.player()->feet() - moved->position) < 0.5f;
  }
  CHECK(corrected);
  CHECK(session.prediction()->replays() > replays_before);

  session.Disconnect("test over");
  CHECK(session.state() == SessionState::kDisconnected);
  CHECK(session.disconnect_reason() == "test over");
}

TEST_CASE("session stays in sync on a lossy, delayed network") {
  NetcodeRuntime netcode;
  REQUIRE(netcode.ok());
  netcode_log_level(NETCODE_LOG_LEVEL_NONE);
  constexpr char kAddress[] = "127.0.0.1:47411";
  physics::PhysicsRuntime runtime;
  sim::testing::TestScene scene;
  testing::TestServer server(runtime, scene, kAddress, 0.0);
  if (!server.ok()) {
    MESSAGE("skipped: cannot bind ", kAddress);
    return;
  }
  ClientRig client(runtime, scene,
                   NetworkConditions{.latency_seconds = 0.08,
                                     .jitter_seconds = 0.02,
                                     .loss_percent = 5.0,
                                     .seed = 11});
  Loop loop{.server = server, .client = client};
  REQUIRE(ConnectAndPlay(loop, kAddress));
  ClientSession& session = *client.session;
  for (int i = 0; i < 120; ++i) {
    loop.Frame();
  }
  const int replays_before = session.prediction()->replays();
  loop.walk_from = session.next_tick();
  loop.walk_ticks = 180;
  for (int i = 0; i < 300; ++i) {
    loop.Frame();
  }
  // Loss makes commands late now and then: bounded replays, and the final
  // prediction agrees with the server once everything settled.
  const int replays = session.prediction()->replays() - replays_before;
  MESSAGE("replays under loss: ", replays,
          ", resyncs: ", session.clock().resyncs());
  CHECK(replays <= 30);
  const NetStats stats = session.stats(loop.time);
  CHECK(stats.round_trip_seconds > 0.12);
  CHECK(stats.round_trip_seconds < 0.3);
  CHECK(stats.snapshots_per_second > 45.0);
  const std::optional<sim::PlayerState> state = server.ClientState();
  REQUIRE(state.has_value());
  CHECK(glm::length(session.player()->feet() - state->position) < 0.01f);
  CHECK(std::abs(state->position.x) > 5.0f);
  // The clock holds a positive buffer.
  CHECK(session.clock().smoothed_depth() > 0.0f);
  CHECK(session.clock().resyncs() <= 1);
}

TEST_CASE("session reports a content mismatch reject") {
  NetcodeRuntime netcode;
  REQUIRE(netcode.ok());
  netcode_log_level(NETCODE_LOG_LEVEL_NONE);
  constexpr char kAddress[] = "127.0.0.1:47412";
  physics::PhysicsRuntime runtime;
  sim::testing::TestScene scene;
  testing::TestServer server(runtime, scene, kAddress, 0.0);
  if (!server.ok()) {
    MESSAGE("skipped: cannot bind ", kAddress);
    return;
  }
  server.set_content_hash(0xBAD);
  ClientRig client(runtime, scene, NetworkConditions{});
  Loop loop{.server = server, .client = client};
  CHECK_FALSE(ConnectAndPlay(loop, kAddress));
  CHECK(client.session->state() == SessionState::kDisconnected);
  CHECK(client.session->disconnect_reason() ==
        std::string("rejected by ") + kAddress + ": " +
            std::string(RejectReasonText(reject_reason::kContentMismatch)));
  CHECK(server.rejected_hellos() >= 1);
}

TEST_CASE("session names the dialled address when nobody answers") {
  NetcodeRuntime netcode;
  REQUIRE(netcode.ok());
  netcode_log_level(NETCODE_LOG_LEVEL_NONE);
  physics::PhysicsRuntime runtime;
  sim::testing::TestScene scene;
  ClientRig client(runtime, scene, NetworkConditions{});
  ClientSession& session = *client.session;
  // Nothing listens on this port.
  constexpr char kAddress[] = "127.0.0.1:47419";
  REQUIRE(session.Connect(kAddress, "tester", 1, 1, 0.0).ok());
  for (int i = 0; i < 2000 && session.state() != SessionState::kDisconnected;
       ++i) {
    session.Update(i * kFrameSeconds);
    netcode_sleep(kRealSleepSeconds);
  }
  CHECK(session.state() == SessionState::kDisconnected);
  CHECK(session.disconnect_reason() ==
        std::string("no reply from ") + kAddress);
}

TEST_CASE("failure texts name the cause and the address to check") {
  const FailureContext context{.typed_address = "game.example:27015",
                               .address = "203.0.113.7:27015",
                               .content_hash = 0xABCDEF};
  const FailureText none = DescribeFailure(FailureCause::kNoReply, context);
  CHECK(none.reason == "no reply from 203.0.113.7:27015");
  const auto has = [](const FailureText& text, std::string_view needle) {
    return std::any_of(text.hint.begin(), text.hint.end(),
                       [&](const std::string& line) {
                         return line.find(needle) != std::string::npos;
                       });
  };
  CHECK(none.hint.size() >= 6);
  CHECK(has(none, "BLUE_PUBLIC_ADDR must be exactly 203.0.113.7:27015"));
  CHECK(has(none, "BLUE_PRIVATE_KEY"));
  CHECK(has(none, "TCP"));
  CHECK(has(none, "still connected"));
  CHECK(has(none, "BLUE_NETCODE_LOG=3"));
  for (const std::string& line : none.hint) {
    CHECK(line.size() < 200);
  }
  CHECK(has(DescribeFailure(FailureCause::kDenied, context),
            "BLUE_MAX_PLAYERS"));

  const FailureText silent =
      DescribeFailure(FailureCause::kNoHandshakeAnswer, context);
  CHECK(silent.reason != none.reason);
  CHECK(has(silent, "ClientHello"));

  const FailureText content =
      DescribeFailure(FailureCause::kRejectedContent, context);
  CHECK(has(content, "0000000000abcdef"));

  CHECK(FailureCauseFor(TransportState::kConnectTimedOut) ==
        FailureCause::kNoReply);
  CHECK(FailureCauseFor(TransportState::kTimedOut) ==
        FailureCause::kConnectionLost);
  CHECK(FailureCauseFor(TransportState::kDenied) == FailureCause::kDenied);
  CHECK(FailureCauseForReject(reject_reason::kServerFull) ==
        FailureCause::kRejectedFull);
  CHECK(FailureCauseForReject(99) == FailureCause::kRejectedUnknown);
}

TEST_CASE("session connects to a host name through the resolver") {
  NetcodeRuntime netcode;
  REQUIRE(netcode.ok());
  netcode_log_level(NETCODE_LOG_LEVEL_NONE);
  constexpr char kServerAddress[] = "127.0.0.1:47414";
  physics::PhysicsRuntime runtime;
  sim::testing::TestScene scene;
  testing::TestServer server(runtime, scene, kServerAddress, 0.0);
  if (!server.ok()) {
    MESSAGE("skipped: cannot bind ", kServerAddress);
    return;
  }
  ClientRig client(runtime, scene, NetworkConditions{});
  Loop loop{.server = server, .client = client};
  ClientSession& session = *client.session;
  REQUIRE(session
              .Connect("localhost:47414", "tester",
                       client.reference->content_hash(), 0x1234, loop.time)
              .ok());
  CHECK((session.state() == SessionState::kResolving ||
         session.state() == SessionState::kConnecting));
  CHECK(loop.RunUntilPlaying(600));
  CHECK(session.state() == SessionState::kPlaying);
  CHECK(session.player_id() == testing::TestServer::kClientPlayer);
}

TEST_CASE("session ends with a reason when a host name does not resolve") {
  NetcodeRuntime netcode;
  REQUIRE(netcode.ok());
  netcode_log_level(NETCODE_LOG_LEVEL_NONE);
  physics::PhysicsRuntime runtime;
  sim::testing::TestScene scene;
  ClientRig client(runtime, scene, NetworkConditions{});
  ClientSession& session = *client.session;
  REQUIRE(session.Connect("name.invalid:27015", "tester", 1, 1, 0.0).ok());
  CHECK(session.state() == SessionState::kResolving);
  // ".invalid" never resolves (RFC 6761); give the lookup a bounded time.
  for (int i = 0; i < 5000 && session.state() == SessionState::kResolving;
       ++i) {
    session.Update(i * kFrameSeconds);
    netcode_sleep(kRealSleepSeconds);
  }
  CHECK(session.state() == SessionState::kDisconnected);
  CHECK(session.disconnect_reason().find("name.invalid:27015") !=
        std::string::npos);
  CHECK(session.disconnect_reason().find("cannot resolve") !=
        std::string::npos);
}

TEST_CASE("session drops a pending lookup on disconnect and reconnect") {
  NetcodeRuntime netcode;
  REQUIRE(netcode.ok());
  netcode_log_level(NETCODE_LOG_LEVEL_NONE);
  physics::PhysicsRuntime runtime;
  sim::testing::TestScene scene;
  ClientRig client(runtime, scene, NetworkConditions{});
  ClientSession& session = *client.session;
  REQUIRE(session.Connect("name.invalid:27015", "tester", 1, 1, 0.0).ok());
  session.Disconnect("cancelled");
  CHECK(session.state() == SessionState::kDisconnected);
  CHECK(session.disconnect_reason() == "cancelled");
  session.Update(1.0);  // Does nothing, and must not touch the dropped lookup.
  CHECK(session.state() == SessionState::kDisconnected);
  REQUIRE(session.Connect("name.invalid:27015", "tester", 1, 1, 2.0).ok());
  CHECK(session.state() == SessionState::kResolving);
  CHECK(session.disconnect_reason().empty());
  REQUIRE(session.Connect("127.0.0.1:47415", "tester", 1, 1, 3.0).ok());
  CHECK(session.state() == SessionState::kConnecting);
}

TEST_CASE("session rejects a bad player name before connecting") {
  physics::PhysicsRuntime runtime;
  sim::testing::TestScene scene;
  ClientRig client(runtime, scene, NetworkConditions{});
  CHECK_FALSE(client.session->Connect("127.0.0.1:47413", "", 1, 1, 0.0).ok());
  CHECK_FALSE(client.session
                  ->Connect("127.0.0.1:47413", std::string(40, 'x'), 1, 1, 0.0)
                  .ok());
  CHECK(client.session->state() == SessionState::kIdle);
}

}  // namespace
}  // namespace blue::net
