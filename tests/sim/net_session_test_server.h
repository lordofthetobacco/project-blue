#ifndef BLUE_TESTS_SIM_NET_SESSION_TEST_SERVER_H_
#define BLUE_TESTS_SIM_NET_SESSION_TEST_SERVER_H_

#include <doctest/doctest.h>
#include <netcode.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include "net/messages.h"
#include "net/net_settings.h"
#include "net/packet.h"
#include "net/players.h"
#include "net/protocol.h"
#include "net/reliable_channel.h"
#include "net/sequence.h"
#include "net/snapshot.h"
#include "net/state_mapping.h"
#include "net/transport.h"
#include "net/world_object.h"
#include "physics/physics_runtime.h"
#include "sim/player_state.h"
#include "sim/sim_world.h"
#include "sim/user_command.h"
#include "tests/sim/sim_test_scene.h"

// A minimal authoritative server for the ClientSession tests, built from
// the C++ pieces of the protocol: a raw netcode.io server on 127.0.0.1, a
// sim::SimWorld on the scratch test scene, the Control handshake, command
// buffering with the input_buffer_depth of protocol.md section 9.1, one
// ServerSnapshot per tick (own state, delta-encoded remote players against
// the client's acknowledged record, dynamic bodies) and the reliable
// channel. One network client (player id 0, netcode slot 0); further
// players are bots driven by the test through SetBotCommand(). Time is
// advanced by the caller.

namespace blue::net::testing {

class TestServer {
 public:
  static constexpr uint8_t kClientPlayer = 0;

  struct ServerDeleter {
    void operator()(netcode_server_t* server) const {
      netcode_server_destroy(server);
    }
  };

  // Records the client player's position at the end of every tick.
  struct TickRecord {
    uint32_t tick = 0;
    bool valid = false;
    glm::vec3 position{0.0f};
  };

  // `runtime` (the process's only PhysicsRuntime) must outlive the server.
  TestServer(const physics::PhysicsRuntime& runtime,
             const sim::testing::TestScene& scene, const char* address,
             double time)
      : address_(address) {
    auto world = sim::SimWorld::Create(runtime, scene.Options());
    REQUIRE(world.ok());
    world_ = std::move(world).value();
    netcode_server_config_t config;
    netcode_default_server_config(&config);
    config.protocol_id = kNetcodeProtocolId;
    for (size_t i = 0; i < kDevConnectKey.size(); ++i) {
      config.private_key[i] = kDevConnectKey[i];
    }
    server_.reset(netcode_server_create(address, &config, time));
    if (server_ != nullptr) {
      netcode_server_start(server_.get(), 1);
    }
  }

  // False if the port could not be bound (the test is skipped).
  [[nodiscard]] bool ok() const { return server_ != nullptr; }
  [[nodiscard]] sim::SimWorld& world() { return *world_; }
  [[nodiscard]] uint32_t tick() const { return tick_; }
  [[nodiscard]] bool welcomed() const { return welcomed_; }
  [[nodiscard]] int rejected_hellos() const { return rejected_hellos_; }

  // Adds bot `id` (a remote player for the client).
  void AddBot(uint8_t id) {
    REQUIRE(world_->AddPlayer(id));
    bots_.push_back(Bot{.id = id});
    QueueJoined(id);
  }
  void SetBotCommand(uint8_t id, const sim::UserCommand& command) {
    for (Bot& bot : bots_) {
      if (bot.id == id) {
        bot.command = command;
      }
    }
  }
  // Puts the client player back at a spawn point (remove + add): a server
  // correction the client did not predict.
  void TeleportClient() {
    REQUIRE(world_->RemovePlayer(kClientPlayer));
    REQUIRE(world_->AddPlayer(kClientPlayer));
  }
  // Pretends to run other content (the next hellos are rejected).
  void set_content_hash(uint64_t hash) { content_hash_override_ = hash; }

  // The client player's position at the end of `tick`, if recorded.
  [[nodiscard]] std::optional<glm::vec3> ClientPositionAt(uint32_t tick) const {
    const TickRecord& record = history_[tick % history_.size()];
    if (!record.valid || record.tick != tick) {
      return std::nullopt;
    }
    return record.position;
  }
  [[nodiscard]] std::optional<sim::PlayerState> ClientState() const {
    std::array<sim::PlayerReadout, 8> players{};
    const size_t count = world_->ReadPlayers(players);
    for (size_t i = 0; i < std::min(count, players.size()); ++i) {
      if (players[i].id == kClientPlayer) {
        return players[i].state;
      }
    }
    return std::nullopt;
  }

  // Receives and handles everything the client sent.
  void Receive(double time) {
    netcode_server_update(server_.get(), time);
    int bytes = 0;
    uint64_t sequence = 0;
    while (uint8_t* packet = netcode_server_receive_packet(server_.get(), 0,
                                                           &bytes, &sequence)) {
      const std::vector<uint8_t> payload(packet, packet + bytes);
      netcode_server_free_packet(server_.get(), packet);
      Handle(payload, time);
    }
    if (!netcode_server_client_connected(server_.get(), 0) && welcomed_) {
      welcomed_ = false;
      (void)world_->RemovePlayer(kClientPlayer);
    }
  }

  // Runs one tick and sends the client its snapshot.
  void Tick(double time) {
    const uint32_t tick = tick_ + 1;
    bool fresh = false;
    if (welcomed_) {
      const Buffered& buffered = commands_[tick % commands_.size()];
      if (buffered.valid && buffered.command.tick == tick) {
        (void)world_->ApplyCommand(kClientPlayer, buffered.command);
        fresh = true;
      }
    }
    for (const Bot& bot : bots_) {
      sim::UserCommand command = bot.command;
      command.tick = tick;
      (void)world_->ApplyCommand(bot.id, command);
    }
    sim::testing::RunTick(*world_, tick);
    tick_ = tick;
    if (welcomed_) {
      if (fresh) {
        last_command_tick_ = tick;
        missing_ = 0;
      } else {
        ++missing_;
      }
      if (const std::optional<sim::PlayerState> state = ClientState()) {
        history_[tick % history_.size()] = {
            .tick = tick, .valid = true, .position = state->position};
      }
      SendSnapshot(tick, fresh, time);
    }
    if (welcomed_ && !got_input_ && time - last_welcome_ >= 0.1) {
      SendWelcome(time);
    }
  }

 private:
  struct Buffered {
    bool valid = false;
    sim::UserCommand command;
  };
  struct Bot {
    uint8_t id = 0;
    sim::UserCommand command;
  };
  struct Record {
    uint32_t tick = 0;
    bool valid = false;
    PlayerSet set;
  };

  void QueueJoined(uint8_t id) {
    if (welcomed_) {
      REQUIRE(reliable_
                  .Send(PlayerJoined{.player_id = id,
                                     .name = Name::Create("bot").value()})
                  .ok());
    }
  }

  void Send(std::span<const uint8_t> bytes) {
    netcode_server_send_packet(server_.get(), 0, bytes.data(),
                               static_cast<int>(bytes.size()));
  }

  void SendControl(const Message& message) {
    std::array<uint8_t, kMaxPayloadBytes> buffer{};
    const WireResult<size_t> size = EncodeControl(message, buffer);
    REQUIRE(size.ok());
    Send(std::span<const uint8_t>(buffer.data(), size.value()));
  }

  void SendWelcome(double time) {
    last_welcome_ = time;
    SendControl(ServerWelcome{.player_id = kClientPlayer,
                              .tick_rate = kDefaultTickRate,
                              .server_tick = tick_,
                              .max_players = 8});
  }

  void Handle(const std::vector<uint8_t>& payload, double time) {
    const WireResult<PacketKind> kind = PeekPacketKind(payload);
    REQUIRE(kind.ok());
    if (kind.value() == PacketKind::kControl) {
      const WireResult<Message> message = DecodeControl(payload);
      REQUIRE(message.ok());
      const auto* hello = std::get_if<ClientHello>(&message.value());
      REQUIRE(hello != nullptr);
      CHECK(hello->protocol_version == kProtocolVersion);
      const uint64_t hash =
          content_hash_override_.value_or(world_->content_hash());
      if (hello->content_hash != hash) {
        ++rejected_hellos_;
        SendControl(ServerReject{.reason = reject_reason::kContentMismatch,
                                 .server_protocol_version = kProtocolVersion});
        return;
      }
      if (!welcomed_) {
        Welcome(hello->name);
      }
      SendWelcome(time);
      return;
    }
    REQUIRE(kind.value() == PacketKind::kClientInput);
    ClientInput input;
    REQUIRE(DecodeClientInput(payload, input).ok());
    const ReceiveOutcome outcome = acks_.OnReceive(input.header);
    for (const uint16_t acked : outcome.newly_acked()) {
      reliable_.OnPacketAcked(acked);
    }
    if (outcome.receipt != Receipt::kFresh || !welcomed_) {
      return;
    }
    got_input_ = true;
    if (input.snapshot_ack.has_value() &&
        (!snapshot_ack_ || TickGreater(*input.snapshot_ack, *snapshot_ack_))) {
      snapshot_ack_ = input.snapshot_ack;
    }
    CHECK(reliable_.ReceiveSection(input.reliable).ok());
    const uint32_t next = tick_ + 1;
    for (const sim::UserCommand& command : input.commands.commands()) {
      // Window of protocol.md section 9.1: next ..= next + 60.
      if (TickGreater(next, command.tick)) {
        continue;
      }
      CHECK_FALSE(TickGreater(command.tick, next + 60));
      Buffered& slot = commands_[command.tick % commands_.size()];
      if (slot.valid && slot.command.tick == command.tick) {
        CHECK(slot.command == command);  // A second copy must match.
        continue;
      }
      slot = Buffered{.valid = true, .command = command};
      if (!newest_accepted_ || TickGreater(command.tick, *newest_accepted_)) {
        newest_accepted_ = command.tick;
      }
    }
  }

  void Welcome(const Name& name) {
    REQUIRE(world_->AddPlayer(kClientPlayer));
    welcomed_ = true;
    got_input_ = false;
    acks_ = AckTracker();
    reliable_ = ReliableEndpoint(ReliableConfig{});
    REQUIRE(
        reliable_.Send(PlayerJoined{.player_id = kClientPlayer, .name = name})
            .ok());
    for (const Bot& bot : bots_) {
      QueueJoined(bot.id);
    }
    REQUIRE(reliable_.Send(WorldSyncDone{}).ok());
  }

  // The remote players the client sees: everyone but itself.
  PlayerSet CurrentRemotes() const {
    std::array<sim::PlayerReadout, 8> players{};
    const size_t count = world_->ReadPlayers(players);
    PlayerSet set;
    for (size_t i = 0; i < std::min(count, players.size()); ++i) {
      const sim::PlayerReadout& p = players[i];
      if (p.id == kClientPlayer) {
        continue;
      }
      const WirePlayerState wire = ToWire(p.state);
      RemotePlayer remote{.position = p.state.position,
                          .velocity = p.state.velocity,
                          .yaw = p.state.yaw,
                          .pitch = p.state.pitch,
                          .flags = wire.flags,
                          .health = p.state.health};
      REQUIRE(set.Set(static_cast<uint8_t>(p.id), Quantize(remote)).ok());
    }
    return set;
  }

  int8_t InputDepth(uint32_t tick, bool fresh) const {
    if (newest_accepted_ && TickGreater(*newest_accepted_, tick)) {
      return static_cast<int8_t>(
          std::min<uint32_t>(*newest_accepted_ - tick, kInputDepthMax));
    }
    if (fresh) {
      return 0;
    }
    return static_cast<int8_t>(-std::min(missing_, 32));
  }

  void SendSnapshot(uint32_t tick, bool fresh, double time) {
    SnapshotInfo info;
    info.server_tick = tick;
    info.last_command_tick = last_command_tick_;
    info.input_buffer_depth = InputDepth(tick, fresh);
    const PlayerSet* baseline = nullptr;
    if (snapshot_ack_.has_value() && tick - *snapshot_ack_ <= 64 &&
        tick != *snapshot_ack_) {
      const Record& record = records_[*snapshot_ack_ % records_.size()];
      if (record.valid && record.tick == *snapshot_ack_) {
        baseline = &record.set;
        info.baseline_tick = record.tick;
      }
    }
    const std::optional<sim::PlayerState> own = ClientState();
    std::optional<WirePlayerState> own_wire;
    if (own.has_value()) {
      own_wire = ToWire(*own);
    }
    const PacketHeader header = acks_.NextHeader();
    ReliableSection reliable;
    reliable_.FillSection(header.sequence, time, reliable);
    std::array<uint8_t, kMaxPayloadBytes> buffer{};
    WireResult<SnapshotWriter> writer = SnapshotWriter::Begin(
        buffer, header, info, own_wire ? &*own_wire : nullptr, reliable);
    REQUIRE(writer.ok());
    const PlayerSet current = CurrentRemotes();
    std::array<uint8_t, kMaxPlayers> priority{};
    for (size_t i = 0; i < priority.size(); ++i) {
      priority[i] = static_cast<uint8_t>(i);
    }
    Record& record = records_[tick % records_.size()];
    record.tick = tick;
    record.valid = true;
    REQUIRE(writer.value()
                .WritePlayers(current, baseline, priority, 200, record.set)
                .ok());
    std::array<sim::BodyReadout, 16> bodies{};
    const size_t body_count = world_->ReadBodies(bodies);
    for (size_t i = 0; i < std::min(body_count, bodies.size()); ++i) {
      const sim::BodyReadout& body = bodies[i];
      WorldObject object;
      object.kind = ObjectKind::kBody;
      object.id = body.instance;
      object.position = body.state.pose.position;
      object.rotation = body.state.pose.rotation;
      object.linear_velocity = body.state.linear_velocity;
      object.angular_velocity = body.state.angular_velocity;
      object.at_rest = glm::length(object.linear_velocity) < 1.0e-3f &&
                       glm::length(object.angular_velocity) < 1.0e-3f;
      REQUIRE(writer.value().TryAddObject(object).ok());
    }
    const WireResult<size_t> size = writer.value().Finish();
    REQUIRE(size.ok());
    Send(std::span<const uint8_t>(buffer.data(), size.value()));
  }

  std::string address_;
  NetcodeRuntime netcode_;
  std::unique_ptr<sim::SimWorld> world_;
  std::unique_ptr<netcode_server_t, ServerDeleter> server_;
  uint32_t tick_ = 1000;
  bool welcomed_ = false;
  bool got_input_ = false;
  double last_welcome_ = 0.0;
  int rejected_hellos_ = 0;
  std::optional<uint64_t> content_hash_override_;
  AckTracker acks_;
  ReliableEndpoint reliable_{ReliableConfig{}};
  std::optional<uint32_t> snapshot_ack_;
  std::optional<uint32_t> newest_accepted_;
  std::optional<uint32_t> last_command_tick_;
  int missing_ = 0;
  std::array<Buffered, 128> commands_{};
  std::array<Record, 64> records_{};
  std::array<TickRecord, 256> history_{};
  std::vector<Bot> bots_;
};

}  // namespace blue::net::testing

#endif  // BLUE_TESTS_SIM_NET_SESSION_TEST_SERVER_H_
