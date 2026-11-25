// Decoders never crash on garbage: random payloads, truncations and bit
// flips of valid packets all come back as values (docs/netcode/protocol.md
// section 12). Runs under the sanitizer-free Debug build too, so every
// out-of-bounds access would trip the STL's checked iterators.

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <memory>
#include <random>
#include <span>
#include <vector>

#include "net/bit_stream.h"
#include "net/messages.h"
#include "net/packet.h"
#include "net/players.h"
#include "net/protocol.h"
#include "net/snapshot.h"
#include "net/wire_error.h"

namespace blue::net {
namespace {

// Runs every decoder over `payload`; returns how many accepted it.
int DecodeAll(std::span<const uint8_t> payload, ServerSnapshot& snapshot,
              const PlayerSet& baseline) {
  int accepted = 0;
  if (PeekPacketKind(payload).ok()) {
    ++accepted;
  }
  if (DecodeStandaloneMessage(payload).ok()) {
    ++accepted;
  }
  if (DecodeControl(payload).ok()) {
    ++accepted;
  }
  ClientInput input;
  if (DecodeClientInput(payload, input).ok()) {
    ++accepted;
  }
  if (DecodeServerSnapshot(payload, snapshot).ok()) {
    ++accepted;
    PlayerSet out;
    // Either outcome is fine; it must just not crash.
    static_cast<void>(snapshot.players.Apply(&baseline, out).ok());
    static_cast<void>(snapshot.players.Apply(nullptr, out).ok());
  }
  return accepted;
}

// A valid snapshot with every section populated, to mutate.
std::vector<uint8_t> ValidSnapshot(const PlayerSet& baseline) {
  std::array<uint8_t, kMaxPayloadBytes> buffer{};
  ReliableSection reliable;
  REQUIRE(
      reliable.Push(7, Damage{.victim = 1, .attacker = 2, .amount = 3}).ok());
  WirePlayerState own;
  own.slots[0].present = true;
  own.slots[0].spare_count = 2;
  own.slots[0].spare_magazines = {30, 29};
  WireResult<SnapshotWriter> writer = SnapshotWriter::Begin(
      buffer, PacketHeader{.sequence = 9},
      SnapshotInfo{.server_tick = 100, .baseline_tick = 90}, &own, reliable);
  REQUIRE(writer.ok());
  PlayerSet current = baseline;
  RemotePlayerQ changed = *baseline.Find(1);
  changed.position[0] += 77;
  changed.health = 3;
  REQUIRE(current.Set(1, changed).ok());
  current.Remove(2);
  REQUIRE(current.Set(9, RemotePlayerQ{}).ok());
  std::array<uint8_t, kMaxPlayers> priority{};
  for (size_t i = 0; i < priority.size(); ++i) {
    priority[i] = static_cast<uint8_t>(i);
  }
  PlayerSet record;
  REQUIRE(writer.value()
              .WritePlayers(current, &baseline, priority, 0, record)
              .ok());
  WorldObject object;
  object.id = 123456;
  REQUIRE(writer.value().TryAddObject(object).ok());
  WireResult<size_t> size = writer.value().Finish();
  REQUIRE(size.ok());
  return std::vector<uint8_t>(
      buffer.begin(), buffer.begin() + static_cast<ptrdiff_t>(size.value()));
}

PlayerSet Baseline() {
  PlayerSet baseline;
  for (uint8_t id = 0; id < 4; ++id) {
    RemotePlayerQ player;
    player.position = {id * 100, 0, -id * 100};
    player.health = 100;
    REQUIRE(baseline.Set(id, player).ok());
  }
  return baseline;
}

TEST_CASE("random payloads never crash any decoder") {
  auto snapshot = std::make_unique<ServerSnapshot>();
  const PlayerSet baseline = Baseline();
  std::mt19937 random(0xB1E5EEDu);
  std::uniform_int_distribution<int> byte(0, 255);
  std::uniform_int_distribution<int> length(0, 160);
  std::vector<uint8_t> payload;
  for (int iteration = 0; iteration < 20000; ++iteration) {
    payload.resize(static_cast<size_t>(length(random)));
    for (uint8_t& b : payload) {
      b = static_cast<uint8_t>(byte(random));
    }
    // Bias half of the payloads towards a valid kind nibble so the
    // decoders get past the first field.
    if (!payload.empty() && iteration % 2 == 0) {
      payload[0] = static_cast<uint8_t>((payload[0] & 0xF0) | (iteration % 3));
    }
    DecodeAll(payload, *snapshot, baseline);
  }
  CHECK(PeekPacketKind(std::span<const uint8_t>()).error() ==
        WireError::kTruncated);
}

TEST_CASE("every truncation of a valid snapshot is rejected") {
  auto snapshot = std::make_unique<ServerSnapshot>();
  const PlayerSet baseline = Baseline();
  const std::vector<uint8_t> valid = ValidSnapshot(baseline);
  REQUIRE(DecodeServerSnapshot(valid, *snapshot).ok());
  PlayerSet applied;
  REQUIRE(snapshot->players.Apply(&baseline, applied).ok());
  for (size_t size = 0; size < valid.size(); ++size) {
    CAPTURE(size);
    CHECK_FALSE(DecodeServerSnapshot(
                    std::span<const uint8_t>(valid.data(), size), *snapshot)
                    .ok());
  }
  // One extra zero byte is trailing data.
  std::vector<uint8_t> longer = valid;
  longer.push_back(0);
  CHECK(DecodeServerSnapshot(longer, *snapshot).error() ==
        WireError::kTrailingData);
}

TEST_CASE("bit flips of a valid snapshot never crash") {
  auto snapshot = std::make_unique<ServerSnapshot>();
  const PlayerSet baseline = Baseline();
  const std::vector<uint8_t> valid = ValidSnapshot(baseline);
  for (size_t bit = 0; bit < valid.size() * 8; ++bit) {
    std::vector<uint8_t> flipped = valid;
    flipped[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
    DecodeAll(flipped, *snapshot, baseline);
  }
}

TEST_CASE("oversized and wrong-kind payloads are rejected") {
  auto snapshot = std::make_unique<ServerSnapshot>();
  const std::vector<uint8_t> huge(kMaxPayloadBytes + 1, 0);
  CHECK(DecodeServerSnapshot(huge, *snapshot).error() ==
        WireError::kOutOfRange);
  CHECK(DecodeControl(huge).error() == WireError::kOutOfRange);
  ClientInput input;
  CHECK(DecodeClientInput(huge, input).error() == WireError::kOutOfRange);

  const std::array<uint8_t, 1> reserved_kind = {0x0F};
  CHECK(PeekPacketKind(reserved_kind).error() == WireError::kUnknownPacketKind);
  CHECK(DecodeControl(reserved_kind).error() == WireError::kUnknownPacketKind);
  const std::array<uint8_t, 1> input_kind = {0x00};
  CHECK(DecodeControl(input_kind).error() == WireError::kUsage);

  // A Control packet carrying a game message.
  std::array<uint8_t, 8> buffer{};
  BitWriter writer(buffer);
  writer.WriteBits(static_cast<uint32_t>(PacketKind::kControl),
                   kPacketKindBits);
  WriteMessage(WorldSyncDone{}, writer);
  WireResult<size_t> size = writer.Finish();
  REQUIRE(size.ok());
  CHECK(DecodeControl(std::span<const uint8_t>(buffer.data(), size.value()))
            .error() == WireError::kUnexpectedMessage);
  CHECK(EncodeControl(WorldSyncDone{}, buffer).error() ==
        WireError::kUnexpectedMessage);
}

}  // namespace
}  // namespace blue::net
