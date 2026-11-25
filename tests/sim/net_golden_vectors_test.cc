// The golden vectors of the Rust reference codec (docs/netcode/vectors, a copy
// of crates/blue-protocol/tests/vectors in the blue-server repository,
// described in vectors.md): every vector decodes to the logical value
// vectors.md lists, and encoding that value (and re-encoding the decoded
// value) reproduces the bytes exactly. The values below mirror
// crates/blue-protocol/tests/golden.rs in the blue-server repository.

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <memory>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include "net/messages.h"
#include "net/packet.h"
#include "net/players.h"
#include "net/protocol.h"
#include "net/reliable_channel.h"
#include "net/sequence.h"
#include "net/snapshot.h"
#include "net/user_command_codec.h"
#include "net/wire_error.h"
#include "net/world_object.h"
#include "sim/user_command.h"

namespace blue::net {
namespace {

namespace held = sim::buttons::held;
namespace pressed = sim::buttons::pressed;

using Bytes = std::vector<uint8_t>;

// Reads `<name>.bin` from the vectors directory (BLUE_PROTOCOL_VECTORS_DIR,
// set by tests/sim/CMakeLists.txt).
Bytes LoadVector(std::string_view name) {
  const std::string path =
      std::string(BLUE_PROTOCOL_VECTORS_DIR) + "/" + std::string(name) + ".bin";
  std::ifstream file(path, std::ios::binary);
  INFO("vector file: ", path);
  REQUIRE(file.good());
  return Bytes(std::istreambuf_iterator<char>(file),
               std::istreambuf_iterator<char>());
}

Bytes Prefix(std::span<const uint8_t> buffer, size_t size) {
  return Bytes(buffer.begin(), buffer.begin() + static_cast<ptrdiff_t>(size));
}

Name MakeName(std::string_view text) {
  WireResult<Name> name = Name::Create(text);
  REQUIRE(name.ok());
  return name.value();
}

// --- Messages ----------------------------------------------------------------

DroppedWeapon GoldenDroppedWeapon() {
  DroppedWeapon weapon;
  weapon.dropped_id = 70000;
  weapon.weapon_key = 0xDEADBEEF;
  weapon.rounds_in_magazine = 17;
  weapon.position = glm::vec3(12.5f, -3.25f, 100.0f);
  weapon.rotation = glm::quat::wxyz(0.8f, 0.0f, 0.6f, 0.0f);
  return weapon;
}

std::vector<std::pair<std::string_view, Message>> GoldenMessages() {
  WorldSyncPickupsTaken sync;
  sync.count = 3;
  sync.instance_ids[0] = 1;
  sync.instance_ids[1] = 300;
  sync.instance_ids[2] = 0xFFFFFFFF;
  return {
      {"client_hello", ClientHello{.protocol_version = kProtocolVersion,
                                   .content_hash = 0x0123456789ABCDEFULL,
                                   .name = MakeName("Zo\xC3\xAB")}},
      {"server_welcome", ServerWelcome{.player_id = 5,
                                       .tick_rate = 60,
                                       .server_tick = 123456,
                                       .max_players = 16}},
      {"server_reject",
       ServerReject{.reason = reject_reason::kProtocolMismatch,
                    .server_protocol_version = kProtocolVersion}},
      {"player_joined",
       PlayerJoined{.player_id = 63, .name = MakeName("bot_7")}},
      {"player_left", PlayerLeft{.player_id = 2}},
      {"damage", Damage{.victim = 3,
                        .attacker = 4,
                        .amount = 37,
                        .zone = hit_zone::kHead,
                        .health_after = -12}},
      {"death", Death{.victim = 3, .killer = std::nullopt, .weapon_key = 0}},
      {"respawn", Respawn{.player_id = 3,
                          .position = glm::vec3(-8.0f, 0.5f, 16.25f),
                          .yaw = 3.0f}},
      {"shot", Shot{.shooter = 4,
                    .origin = glm::vec3(1.0f, 1.75f, -2.0f),
                    .direction = glm::vec3(0.0f, 0.0f, -1.0f),
                    .weapon_key = 0x12345678,
                    .shot_number = 42}},
      {"impact", Impact{.point = glm::vec3(5.5f, 0.0f, -30.125f),
                        .normal = glm::vec3(0.0f, 1.0f, 0.0f)}},
      {"weapon_dropped", WeaponDropped{.weapon = GoldenDroppedWeapon()}},
      {"dropped_taken", DroppedTaken{.dropped_id = 70000, .player_id = 9}},
      {"pickup_taken", PickupTaken{.instance_id = 12, .player_id = 0}},
      {"world_sync_pickups_taken", sync},
      {"world_sync_dropped_weapon",
       WorldSyncDroppedWeapon{.weapon = GoldenDroppedWeapon()}},
      {"world_sync_done", WorldSyncDone{}},
      {"dropped_removed", DroppedRemoved{.dropped_id = 70001}},
  };
}

TEST_CASE("golden message vectors decode and re-encode exactly") {
  int handshakes = 0;
  for (const auto& [name, message] : GoldenMessages()) {
    CAPTURE(name);
    const Bytes expected = LoadVector("msg_" + std::string(name));
    WireResult<Message> decoded = DecodeStandaloneMessage(expected);
    REQUIRE(decoded.ok());
    CHECK(decoded.value() == message);

    std::array<uint8_t, 256> buffer{};
    WireResult<size_t> size = EncodeStandaloneMessage(message, buffer);
    REQUIRE(size.ok());
    CHECK(Prefix(buffer, size.value()) == expected);
    size = EncodeStandaloneMessage(decoded.value(), buffer);
    REQUIRE(size.ok());
    CHECK(Prefix(buffer, size.value()) == expected);

    if (!IsHandshake(message)) {
      continue;
    }
    ++handshakes;
    const Bytes control = LoadVector("control_" + std::string(name));
    WireResult<PacketKind> kind = PeekPacketKind(control);
    REQUIRE(kind.ok());
    CHECK(kind.value() == PacketKind::kControl);
    WireResult<Message> from_control = DecodeControl(control);
    REQUIRE(from_control.ok());
    CHECK(from_control.value() == message);
    size = EncodeControl(message, buffer);
    REQUIRE(size.ok());
    CHECK(Prefix(buffer, size.value()) == control);
  }
  CHECK(handshakes == 3);
}

// --- ClientInput -------------------------------------------------------------

sim::UserCommand GoldenCommand(uint32_t tick) {
  sim::UserCommand command;
  command.tick = tick;
  command.held = held::kForward | held::kSprint;
  command.pressed = 0;
  command.yaw = 1.5f;
  command.pitch = -0.25f;
  command.select_slot = -1;
  command.interact_kind = interact_kind::kNone;
  command.interact_id = 0;
  return command;
}

ClientInput GoldenInputSingle() {
  ClientInput input;
  input.header = PacketHeader{.sequence = 10, .ack = 20, .ack_bits = 5};
  input.snapshot_ack = std::nullopt;
  input.view_tick = 0;
  input.view_fraction = 0;
  REQUIRE(input.commands.Push(GoldenCommand(1000)).ok());
  return input;
}

ClientInput GoldenInputMulti() {
  sim::UserCommand newest = GoldenCommand(70001);
  newest.pressed = pressed::kJump | pressed::kInteract;
  newest.interact_kind = interact_kind::kPickup;
  newest.interact_id = 77;
  sim::UserCommand turned = GoldenCommand(70000);
  turned.yaw = 1.5f - 0.001f;
  sim::UserCommand aiming = GoldenCommand(69999);
  aiming.pitch = 0.125f;
  aiming.held = held::kFire | held::kAim;
  sim::UserCommand switching = GoldenCommand(69998);
  switching.select_slot = 1;
  const std::array<sim::UserCommand, 4> commands = {newest, turned, aiming,
                                                    switching};

  ClientInput input;
  input.header =
      PacketHeader{.sequence = 65535, .ack = 0, .ack_bits = 0xFFFFFFFF};
  input.snapshot_ack = 69990;
  input.view_tick = 69988;
  input.view_fraction = 192;
  WireResult<CommandList> list = CommandList::FromCommands(commands);
  REQUIRE(list.ok());
  input.commands = list.value();
  REQUIRE(input.reliable.Push(65535, PlayerLeft{.player_id = 1}).ok());
  REQUIRE(input.reliable.Push(2, PickupTaken{.instance_id = 5, .player_id = 1})
              .ok());
  return input;
}

void CheckClientInputVector(std::string_view name, const ClientInput& input,
                            std::span<const std::pair<float, float>> angles) {
  CAPTURE(name);
  const Bytes expected = LoadVector(name);
  ClientInput decoded;
  REQUIRE(DecodeClientInput(expected, decoded).ok());
  ClientInput canonical = input;
  canonical.commands = input.commands.Canonical();
  CHECK(decoded == canonical);
  // The canonical angles vectors.md lists.
  REQUIRE(decoded.commands.size() == angles.size());
  for (size_t i = 0; i < angles.size(); ++i) {
    CHECK(decoded.commands.commands()[i].yaw == angles[i].first);
    CHECK(decoded.commands.commands()[i].pitch == angles[i].second);
  }

  std::array<uint8_t, kMaxPayloadBytes> buffer{};
  WireResult<size_t> size = EncodeClientInput(input, buffer);
  REQUIRE(size.ok());
  CHECK(Prefix(buffer, size.value()) == expected);
  size = EncodeClientInput(decoded, buffer);
  REQUIRE(size.ok());
  CHECK(Prefix(buffer, size.value()) == expected);
}

TEST_CASE("golden ClientInput vectors decode and re-encode exactly") {
  const std::array<std::pair<float, float>, 1> single = {
      std::pair{1.4999995f, -0.25000292f}};
  CheckClientInputVector("client_input_single", GoldenInputSingle(), single);
  const std::array<std::pair<float, float>, 4> multi = {
      std::pair{1.4999995f, -0.25000292f}, std::pair{1.4989989f, -0.25000292f},
      std::pair{1.4999995f, 0.12500146f}, std::pair{1.4999995f, -0.25000292f}};
  CheckClientInputVector("client_input_multi", GoldenInputMulti(), multi);
}

// --- ServerSnapshot ----------------------------------------------------------

WirePlayerState GoldenOwnState() {
  WireWeaponSlot rifle;
  rifle.present = true;
  rifle.weapon_key = 0xCAFEF00D;
  rifle.fire_mode = 1;
  rifle.magazine_rounds = 29;
  rifle.magazine_inserted = true;
  rifle.chambered = true;
  rifle.cycle_remaining = 0.0625f;
  rifle.spare_count = 3;
  rifle.spare_magazines[0] = 30;
  rifle.spare_magazines[1] = 30;
  rifle.spare_magazines[2] = 12;
  rifle.incoming_magazine = -1;
  rifle.pull_used = true;
  rifle.scope_magnification = 4.0f;
  rifle.scope_zero = 300.0f;
  rifle.spares_dropped = 0;
  WirePlayerState own;
  own.position = glm::vec3(10.0f, 0.0f, -4.5f);
  own.velocity = glm::vec3(0.0f, -9.8125f, 6.0f);
  own.yaw = 0.75f;
  own.pitch = -0.125f;
  own.eye_height = 1.625f;
  own.flags = player_flags::kAlive | player_flags::kSprinting;
  own.jump_age = 200;
  own.jump_cooldown = 0;
  own.health = 100;
  own.respawn_ticks = 0;
  own.shots_fired = 1234;
  own.active_slot = 1;
  own.selected_slot = 1;
  own.raise = 0.5f;
  own.jump_speed = 4.5f;
  own.deaths = 3;
  own.wait_for_release = true;
  own.slots = {WireWeaponSlot{}, rifle};
  return own;
}

RemotePlayer GoldenRemote(float x, int16_t health) {
  RemotePlayer player;
  player.position = glm::vec3(x, 0.0f, 2.5f);
  player.velocity = glm::vec3(1.0f, 0.0f, -0.5f);
  player.yaw = 1.0f;
  player.pitch = 0.0f;
  player.flags = player_flags::kAlive | player_flags::kOnGround;
  player.health = health;
  player.weapon_key = 0xCAFEF00D;
  return player;
}

WorldObject GoldenBody() {
  WorldObject object;
  object.kind = ObjectKind::kBody;
  object.id = 3;
  object.at_rest = true;
  object.position = glm::vec3(1.0f, 0.5f, -2.0f);
  return object;
}

WorldObject GoldenDroppedObject() {
  WorldObject object;
  object.kind = ObjectKind::kDroppedWeapon;
  object.id = 70000;
  object.at_rest = false;
  object.position = glm::vec3(12.5f, 3.0f, 100.0f);
  object.rotation = glm::quat::wxyz(0.8f, 0.0f, 0.6f, 0.0f);
  object.linear_velocity = glm::vec3(0.5f, -4.0f, 0.25f);
  object.angular_velocity = glm::vec3(0.0f, 3.0f, -1.5f);
  return object;
}

struct SnapshotCase {
  PacketHeader header;
  SnapshotInfo info;
  std::optional<WirePlayerState> own;
  ReliableSection reliable;
  PlayerSet current;
  std::optional<PlayerSet> baseline;
  std::vector<WorldObject> objects;
};

PlayerSet SetOf(std::span<const std::pair<uint8_t, RemotePlayer>> players) {
  PlayerSet set;
  for (const auto& [id, player] : players) {
    REQUIRE(set.Set(id, Quantize(player)).ok());
  }
  return set;
}

SnapshotCase GoldenFullCase() {
  SnapshotCase c;
  c.header = PacketHeader{.sequence = 300, .ack = 299, .ack_bits = 0x80000001};
  c.info = SnapshotInfo{.server_tick = 5000,
                        .last_command_tick = 5002,
                        .input_buffer_depth = 2,
                        .baseline_tick = std::nullopt};
  c.own = GoldenOwnState();
  REQUIRE(
      c.reliable.Push(0, PlayerJoined{.player_id = 2, .name = MakeName("Ann")})
          .ok());
  REQUIRE(c.reliable
              .Push(1, Damage{.victim = 0,
                              .attacker = 2,
                              .amount = 25,
                              .zone = hit_zone::kTorso,
                              .health_after = 75})
              .ok());
  const std::array<std::pair<uint8_t, RemotePlayer>, 3> current = {
      std::pair{uint8_t{1}, GoldenRemote(-3.0f, 100)},
      std::pair{uint8_t{2}, GoldenRemote(4.0f, 80)},
      std::pair{uint8_t{40}, GoldenRemote(0.001f, 1)}};
  c.current = SetOf(current);
  c.objects = {GoldenBody(), GoldenDroppedObject()};
  return c;
}

SnapshotCase GoldenDeltaCase() {
  RemotePlayer moved = GoldenRemote(-3.0f, 100);
  moved.position = glm::vec3(-2.75f, 0.0f, 2.5f);
  RemotePlayer hurt = GoldenRemote(4.0f, 80);
  hurt.health = 55;
  hurt.yaw = 2.0f;
  SnapshotCase c;
  c.header = PacketHeader{.sequence = 301, .ack = 300, .ack_bits = 3};
  c.info = SnapshotInfo{.server_tick = 5001,
                        .last_command_tick = 5003,
                        .input_buffer_depth = -1,
                        .baseline_tick = 4996};
  const std::array<std::pair<uint8_t, RemotePlayer>, 4> current = {
      std::pair{uint8_t{1}, moved}, std::pair{uint8_t{2}, hurt},
      std::pair{uint8_t{5}, GoldenRemote(7.0f, 100)},
      std::pair{uint8_t{40}, GoldenRemote(0.001f, 1)}};
  const std::array<std::pair<uint8_t, RemotePlayer>, 4> baseline = {
      std::pair{uint8_t{1}, GoldenRemote(-3.0f, 100)},
      std::pair{uint8_t{2}, GoldenRemote(4.0f, 80)},
      std::pair{uint8_t{3}, GoldenRemote(9.0f, 10)},
      std::pair{uint8_t{40}, GoldenRemote(0.001f, 1)}};
  c.current = SetOf(current);
  c.baseline = SetOf(baseline);
  c.objects = {GoldenDroppedObject()};
  return c;
}

// Encodes like the Rust golden test: begin, write_players(priority 0..63,
// reserve 0), every object, finish.
Bytes EncodeSnapshot(const SnapshotCase& c,
                     std::span<const WorldObject> objects,
                     const PlayerSet& current) {
  std::array<uint8_t, kMaxPayloadBytes> buffer{};
  WireResult<SnapshotWriter> writer =
      SnapshotWriter::Begin(buffer, c.header, c.info,
                            c.own.has_value() ? &*c.own : nullptr, c.reliable);
  REQUIRE(writer.ok());
  std::array<uint8_t, kMaxPlayers> priority{};
  std::iota(priority.begin(), priority.end(), uint8_t{0});
  PlayerSet record;
  REQUIRE(writer.value()
              .WritePlayers(current,
                            c.baseline.has_value() ? &*c.baseline : nullptr,
                            priority, 0, record)
              .ok());
  CHECK(record == current);
  for (const WorldObject& object : objects) {
    WireResult<bool> added = writer.value().TryAddObject(object);
    REQUIRE(added.ok());
    CHECK(added.value());
  }
  WireResult<size_t> size = writer.value().Finish();
  REQUIRE(size.ok());
  return Prefix(buffer, size.value());
}

void CheckSnapshotVector(std::string_view name, const SnapshotCase& c) {
  CAPTURE(name);
  const Bytes expected = LoadVector(name);
  CHECK(EncodeSnapshot(c, c.objects, c.current) == expected);

  auto decoded = std::make_unique<ServerSnapshot>();
  REQUIRE(DecodeServerSnapshot(expected, *decoded).ok());
  CHECK(decoded->header == c.header);
  CHECK(decoded->info == c.info);
  CHECK(decoded->own == c.own);
  CHECK(decoded->reliable == c.reliable);
  CHECK(decoded->players.has_baseline() == c.baseline.has_value());
  PlayerSet applied;
  REQUIRE(decoded->players
              .Apply(c.baseline.has_value() ? &*c.baseline : nullptr, applied)
              .ok());
  CHECK(applied == c.current);
  REQUIRE(decoded->objects.size() == c.objects.size());
  for (size_t i = 0; i < c.objects.size(); ++i) {
    CHECK(decoded->objects.objects()[i] == Quantized(c.objects[i]));
  }

  // Re-encoding the decoded value reproduces the bytes.
  SnapshotCase again = c;
  again.own = decoded->own;
  again.reliable = decoded->reliable;
  CHECK(EncodeSnapshot(again, decoded->objects.objects(), applied) == expected);
}

TEST_CASE("golden ServerSnapshot vectors decode and re-encode exactly") {
  CheckSnapshotVector("server_snapshot_full", GoldenFullCase());
  CheckSnapshotVector("server_snapshot_delta", GoldenDeltaCase());
}

TEST_CASE("golden snapshot values match vectors.md") {
  // Spot checks of the logical values vectors.md lists (quantised players
  // and decoded objects), independent of the round trips above.
  const SnapshotCase full = GoldenFullCase();
  const RemotePlayerQ* far = full.current.Find(40);
  REQUIRE(far != nullptr);
  CHECK(far->position == std::array<int32_t, 3>{1, 0, 1280});
  CHECK(far->velocity == std::array<int32_t, 3>{64, 0, -32});
  CHECK(far->yaw == 10430);
  const SnapshotCase delta = GoldenDeltaCase();
  const RemotePlayerQ* moved = delta.current.Find(1);
  REQUIRE(moved != nullptr);
  CHECK(moved->position[0] == -1408);
  CHECK(delta.current.Find(2)->yaw == 20861);
  CHECK(delta.baseline->Find(3)->position[0] == 4608);
  const WorldObject decoded = Quantized(GoldenDroppedObject());
  CHECK(decoded.rotation.x == 0.0f);
  CHECK(decoded.rotation.y == 0.60055643f);
  CHECK(decoded.rotation.z == 0.0f);
  CHECK(decoded.rotation.w == 0.79958236f);
  CHECK(decoded.linear_velocity == glm::vec3(0.5f, -4.0f, 0.25f));
  CHECK(decoded.angular_velocity == glm::vec3(0.0f, 3.0f, -1.5f));
}

TEST_CASE("every golden vector file is covered") {
  // The 24 files of the vectors directory; a new vector must be added to
  // this test (and to the cases above).
  constexpr std::array<std::string_view, 24> kAll = {
      "client_input_multi",
      "client_input_single",
      "control_client_hello",
      "control_server_reject",
      "control_server_welcome",
      "msg_client_hello",
      "msg_damage",
      "msg_death",
      "msg_dropped_taken",
      "msg_impact",
      "msg_pickup_taken",
      "msg_player_joined",
      "msg_player_left",
      "msg_respawn",
      "msg_server_reject",
      "msg_server_welcome",
      "msg_shot",
      "msg_weapon_dropped",
      "msg_world_sync_done",
      "msg_dropped_removed",
      "msg_world_sync_dropped_weapon",
      "msg_world_sync_pickups_taken",
      "server_snapshot_delta",
      "server_snapshot_full",
  };
  for (const std::string_view name : kAll) {
    CAPTURE(name);
    CHECK_FALSE(LoadVector(name).empty());
  }
  CHECK(GoldenMessages().size() == 17);
}

}  // namespace
}  // namespace blue::net
