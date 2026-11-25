// ServerSnapshot: own state, players delta against baselines, objects, the
// size guarantee and MissingBaseline handling (docs/netcode/protocol.md
// section 9).

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <memory>
#include <numeric>
#include <span>
#include <vector>

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include "net/players.h"
#include "net/protocol.h"
#include "net/quantize.h"
#include "net/reliable_channel.h"
#include "net/sequence.h"
#include "net/snapshot.h"
#include "net/wire_error.h"
#include "net/world_object.h"

namespace blue::net {
namespace {

std::vector<uint8_t> AllIds() {
  std::vector<uint8_t> ids(kMaxPlayers);
  std::iota(ids.begin(), ids.end(), uint8_t{0});
  return ids;
}

RemotePlayerQ Remote(int32_t x, int16_t health) {
  RemotePlayerQ p;
  p.position = {x, 0, 1280};
  p.velocity = {64, 0, -32};
  p.yaw = 10430;
  p.flags = player_flags::kAlive | player_flags::kOnGround;
  p.health = health;
  p.weapon_key = 0xCAFEF00D;
  return p;
}

WirePlayerState OwnState() {
  WirePlayerState own;
  own.position = glm::vec3(1.0f, 2.0f, 3.0f);
  own.flags = player_flags::kJumpPending | player_flags::kAlive;
  own.health = -5;
  own.respawn_ticks = 65535;
  own.active_slot = 7;
  own.selected_slot = -1;
  own.slots[0].present = true;
  own.slots[0].weapon_key = 1;
  own.slots[0].fire_mode = kMaxFireMode;
  own.slots[0].reload_stage = kMaxReloadStage;
  own.slots[0].spare_count = kMaxSpareMagazines;
  own.slots[0].spare_magazines.fill(255);
  own.slots[0].reload_duration = 2.5f;
  return own;
}

// Encodes a snapshot and decodes it again; `record` receives the writer's
// record of the players section.
struct Encoded {
  std::array<uint8_t, kMaxPayloadBytes> bytes = {};
  size_t size = 0;
};

std::unique_ptr<Encoded> Encode(const SnapshotInfo& info,
                                const WirePlayerState* own,
                                const ReliableSection& reliable,
                                const PlayerSet& current,
                                const PlayerSet* baseline,
                                std::span<const WorldObject> objects,
                                PlayerSet& record) {
  auto encoded = std::make_unique<Encoded>();
  WireResult<SnapshotWriter> writer = SnapshotWriter::Begin(
      encoded->bytes, PacketHeader{.sequence = 1, .ack = 2, .ack_bits = 3},
      info, own, reliable);
  REQUIRE(writer.ok());
  REQUIRE(
      writer.value().WritePlayers(current, baseline, AllIds(), 0, record).ok());
  for (const WorldObject& object : objects) {
    WireResult<bool> added = writer.value().TryAddObject(object);
    REQUIRE(added.ok());
    CHECK(added.value());
  }
  WireResult<size_t> size = writer.value().Finish();
  REQUIRE(size.ok());
  encoded->size = size.value();
  return encoded;
}

TEST_CASE("a full snapshot round trips") {
  PlayerSet current;
  REQUIRE(current.Set(0, Remote(kPositionMin, -32768)).ok());
  REQUIRE(current.Set(63, Remote(kPositionMax, 32767)).ok());
  ReliableSection reliable;
  REQUIRE(reliable.Push(65535, PlayerLeft{.player_id = 1}).ok());
  REQUIRE(reliable.Push(254, WorldSyncDone{}).ok());
  WorldObject moving;
  moving.kind = ObjectKind::kDroppedWeapon;
  moving.id = UINT32_MAX;
  moving.position = glm::vec3(20000.0f, -20000.0f, 0.5f);
  moving.rotation = glm::quat::wxyz(0.0f, 1.0f, 0.0f, 0.0f);
  moving.linear_velocity = glm::vec3(500.0f, -1.0f, 0.0f);
  moving.angular_velocity = glm::vec3(0.0f, 3.0f, -1.5f);
  WorldObject resting;
  resting.at_rest = true;
  resting.linear_velocity = glm::vec3(5.0f);  // not sent
  const std::array<WorldObject, 2> objects = {moving, resting};
  const SnapshotInfo info{.server_tick = 7,
                          .last_command_tick = std::nullopt,
                          .input_buffer_depth = kInputDepthMin,
                          .baseline_tick = std::nullopt};
  const WirePlayerState own = OwnState();

  PlayerSet record;
  const std::unique_ptr<Encoded> encoded =
      Encode(info, &own, reliable, current, nullptr, objects, record);
  CHECK(record == current);

  auto decoded = std::make_unique<ServerSnapshot>();
  REQUIRE(DecodeServerSnapshot(
              std::span<const uint8_t>(encoded->bytes.data(), encoded->size),
              *decoded)
              .ok());
  CHECK(decoded->header ==
        PacketHeader{.sequence = 1, .ack = 2, .ack_bits = 3});
  CHECK(decoded->info == info);
  REQUIRE(decoded->own.has_value());
  CHECK(*decoded->own == own);
  CHECK(decoded->reliable == reliable);
  CHECK_FALSE(decoded->players.has_baseline());
  PlayerSet applied;
  REQUIRE(decoded->players.Apply(nullptr, applied).ok());
  CHECK(applied == current);
  REQUIRE(decoded->objects.size() == 2);
  CHECK(decoded->objects.objects()[0] == Quantized(moving));
  CHECK(decoded->objects.objects()[1] == Quantized(resting));
  CHECK(decoded->objects.objects()[1].linear_velocity == glm::vec3(0.0f));
  CHECK(decoded->objects.objects()[0].linear_velocity.x == 128.0f - 1.0f / 64);
}

TEST_CASE("a delta snapshot needs its baseline, and nothing else does") {
  PlayerSet baseline;
  REQUIRE(baseline.Set(1, Remote(0, 100)).ok());
  REQUIRE(baseline.Set(2, Remote(10, 100)).ok());
  REQUIRE(baseline.Set(3, Remote(20, 100)).ok());
  PlayerSet current = baseline;
  RemotePlayerQ moved = Remote(kPositionMin, 100);  // huge negative delta
  moved.weapon_key = 0;
  REQUIRE(current.Set(1, moved).ok());
  current.Remove(3);
  REQUIRE(current.Set(9, Remote(5, 50)).ok());

  ReliableSection reliable;
  REQUIRE(reliable.Push(4, PlayerLeft{.player_id = 3}).ok());
  const SnapshotInfo info{.server_tick = 300,
                          .last_command_tick = 299,
                          .input_buffer_depth = kInputDepthMax,
                          .baseline_tick = 45};  // 255 ticks back
  PlayerSet record;
  const std::unique_ptr<Encoded> encoded =
      Encode(info, nullptr, reliable, current, &baseline, {}, record);
  CHECK(record == current);

  auto decoded = std::make_unique<ServerSnapshot>();
  REQUIRE(DecodeServerSnapshot(
              std::span<const uint8_t>(encoded->bytes.data(), encoded->size),
              *decoded)
              .ok());
  CHECK(decoded->info == info);
  CHECK(decoded->reliable == reliable);
  CHECK(decoded->players.entries().size() == 3);  // removed, delta, full

  PlayerSet applied;
  CHECK(decoded->players.Apply(nullptr, applied).error() ==
        WireError::kMissingBaseline);

  BaselineHistory history;
  CHECK(history.Find(45) == nullptr);
  history.Store(45, baseline);
  REQUIRE(history.Find(45) != nullptr);
  REQUIRE(decoded->players.Apply(history.Find(45), applied).ok());
  CHECK(applied == current);

  // The baseline slot is reused 64 ticks later.
  history.Store(45 + kBaselineHistory, current);
  CHECK(history.Find(45) == nullptr);
  history.Clear();
  CHECK(history.Find(45 + kBaselineHistory) == nullptr);

  // A baseline missing a delta'd player is inconsistent.
  PlayerSet wrong;
  CHECK(decoded->players.Apply(&wrong, applied).error() ==
        WireError::kInconsistent);
}

TEST_CASE("unchanged players cost one bit and the writer validates usage") {
  PlayerSet set;
  REQUIRE(set.Set(5, Remote(1, 1)).ok());
  const SnapshotInfo info{.server_tick = 10, .baseline_tick = 9};
  std::array<uint8_t, kMaxPayloadBytes> buffer{};
  WireResult<SnapshotWriter> writer = SnapshotWriter::Begin(
      buffer, PacketHeader{}, info, nullptr, ReliableSection());
  REQUIRE(writer.ok());
  CHECK(writer.value().TryAddObject(WorldObject{}).error() ==
        WireError::kUsage);
  CHECK(writer.value().Finish().error() == WireError::kUsage);
  PlayerSet record;
  CHECK(
      writer.value().WritePlayers(set, nullptr, AllIds(), 0, record).error() ==
      WireError::kUsage);
  REQUIRE(writer.value().WritePlayers(set, &set, AllIds(), 0, record).ok());
  CHECK(writer.value().WritePlayers(set, &set, AllIds(), 0, record).error() ==
        WireError::kUsage);
  WireResult<size_t> size = writer.value().Finish();
  REQUIRE(size.ok());
  // kind 4 + header 64 + info 32+1+6+1+8 + own 1 + reliable 6 + 2 ends.
  CHECK(size.value() == 16);
}

TEST_CASE("64 full players and 96 objects never exceed the payload") {
  PlayerSet current;
  for (int id = 0; id < kMaxPlayers; ++id) {
    REQUIRE(current.Set(static_cast<uint8_t>(id), Remote(id * 1000, 100)).ok());
  }
  std::array<uint8_t, kMaxPayloadBytes> buffer{};
  const WirePlayerState own = OwnState();
  WireResult<SnapshotWriter> writer = SnapshotWriter::Begin(
      buffer, PacketHeader{}, SnapshotInfo{}, &own, ReliableSection());
  REQUIRE(writer.ok());
  PlayerSet record;
  REQUIRE(writer.value()
              .WritePlayers(current, nullptr, AllIds(), 100, record)
              .ok());
  // 200 bits per player: they cannot all fit; the record holds the ones
  // that did.
  CHECK(record.size() > 0);
  CHECK(record.size() < kMaxPlayers);
  int added = 0;
  WorldObject object;
  for (int i = 0; i < 200; ++i) {
    object.id = static_cast<uint32_t>(i);
    WireResult<bool> fit = writer.value().TryAddObject(object);
    REQUIRE(fit.ok());
    added += fit.value() ? 1 : 0;
  }
  CHECK(added > 0);
  CHECK(added <= kMaxObjectsPerPacket);
  WireResult<size_t> size = writer.value().Finish();
  REQUIRE(size.ok());
  CHECK(size.value() <= kMaxPayloadBytes);

  auto decoded = std::make_unique<ServerSnapshot>();
  REQUIRE(DecodeServerSnapshot(
              std::span<const uint8_t>(buffer.data(), size.value()), *decoded)
              .ok());
  PlayerSet applied;
  REQUIRE(decoded->players.Apply(nullptr, applied).ok());
  CHECK(applied == record);
  CHECK(decoded->objects.size() == static_cast<size_t>(added));
}

TEST_CASE("own state encoders refuse non-canonical slots") {
  std::array<uint8_t, kMaxPayloadBytes> buffer{};
  WirePlayerState own;
  own.slots[1].weapon_key = 3;  // data in an absent slot
  CHECK(SnapshotWriter::Begin(buffer, PacketHeader{}, SnapshotInfo{}, &own,
                              ReliableSection())
            .error() == WireError::kNonCanonical);
  own = WirePlayerState{};
  own.slots[0].present = true;
  own.slots[0].spare_count = 1;
  own.slots[0].spare_magazines[3] = 30;  // past spare_count
  CHECK(SnapshotWriter::Begin(buffer, PacketHeader{}, SnapshotInfo{}, &own,
                              ReliableSection())
            .error() == WireError::kNonCanonical);
  own = WirePlayerState{};
  own.active_slot = 8;
  CHECK(SnapshotWriter::Begin(buffer, PacketHeader{}, SnapshotInfo{}, &own,
                              ReliableSection())
            .error() == WireError::kOutOfRange);
}

TEST_CASE("remote players quantise and dequantise") {
  RemotePlayer player;
  player.position = glm::vec3(-3.0f, 0.0f, 2.5f);
  player.velocity = glm::vec3(1.0f, 0.0f, -0.5f);
  player.yaw = 1.0f;
  player.pitch = -10.0f;
  player.flags = 0xFF;
  player.health = 42;
  const RemotePlayerQ q = Quantize(player);
  CHECK(q.position == std::array<int32_t, 3>{-1536, 0, 1280});
  CHECK(q.yaw == 10430);
  CHECK(q.pitch == -16384);
  CHECK(q.flags == 0x3F);
  const RemotePlayer back = Dequantize(q);
  CHECK(back.position == player.position);
  CHECK(Quantize(back) == q);
}

}  // namespace
}  // namespace blue::net
