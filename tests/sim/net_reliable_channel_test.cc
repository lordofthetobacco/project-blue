// The reliable-ordered channel over a scripted lossy network: every message
// arrives exactly once and in order despite loss, duplication and
// reordering (docs/netcode/protocol.md sections 7.3 and 11).

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <optional>
#include <random>
#include <span>
#include <variant>
#include <vector>

#include "net/messages.h"
#include "net/packet.h"
#include "net/reliable_channel.h"
#include "net/sequence.h"
#include "net/user_command_codec.h"
#include "net/wire_error.h"
#include "sim/user_command.h"

namespace blue::net {
namespace {

constexpr double kTickSeconds = 1.0 / 60.0;

// One side of a connection: sequencing plus the reliable endpoint.
struct Peer {
  explicit Peer(uint16_t first_id)
      : endpoint(ReliableConfig{}, first_id, first_id) {}

  AckTracker acks;
  ReliableEndpoint endpoint;
  std::vector<uint32_t> delivered;
};

// A packet in flight, encoded as a real ClientInput so the reliable
// section also crosses the bit stream.
struct InFlight {
  int deliver_tick = 0;
  std::vector<uint8_t> bytes;
};

// Scripted network: deterministic loss, duplication and reordering.
class Network {
 public:
  Network(double loss, double duplicate, int max_delay_ticks)
      : loss_(loss), duplicate_(duplicate), delay_(0, max_delay_ticks) {}

  void Send(int now_tick, std::span<const uint8_t> bytes,
            std::vector<InFlight>& queue) {
    if (unit_(random_) < loss_) {
      return;
    }
    const int copies = unit_(random_) < duplicate_ ? 2 : 1;
    for (int i = 0; i < copies; ++i) {
      queue.push_back(InFlight{.deliver_tick = now_tick + delay_(random_),
                               .bytes = {bytes.begin(), bytes.end()}});
    }
  }

 private:
  std::mt19937 random_{12345};
  std::uniform_real_distribution<double> unit_{0.0, 1.0};
  double loss_;
  double duplicate_;
  std::uniform_int_distribution<int> delay_;
};

// Builds and encodes the next packet of `peer`.
std::vector<uint8_t> BuildPacket(Peer& peer, uint32_t tick, double now) {
  ClientInput input;
  input.header = peer.acks.NextHeader();
  sim::UserCommand command;
  command.tick = tick;
  REQUIRE(input.commands.Push(command).ok());
  peer.endpoint.FillSection(input.header.sequence, now, input.reliable);
  std::array<uint8_t, kMaxPayloadBytes> buffer{};
  WireResult<size_t> size = EncodeClientInput(input, buffer);
  REQUIRE(size.ok());
  return std::vector<uint8_t>(buffer.begin(), buffer.begin() + size.value());
}

// Registers a received packet the way the protocol prescribes.
void ReceivePacket(Peer& peer, std::span<const uint8_t> bytes) {
  ClientInput input;
  REQUIRE(DecodeClientInput(bytes, input).ok());
  const ReceiveOutcome outcome = peer.acks.OnReceive(input.header);
  for (const uint16_t sequence : outcome.newly_acked()) {
    peer.endpoint.OnPacketAcked(sequence);
  }
  if (outcome.receipt != Receipt::kFresh) {
    return;
  }
  REQUIRE(peer.endpoint.ReceiveSection(input.reliable).ok());
  while (std::optional<Message> message = peer.endpoint.Pop()) {
    const auto* pickup = std::get_if<PickupTaken>(&*message);
    REQUIRE(pickup != nullptr);
    peer.delivered.push_back(pickup->instance_id);
  }
}

// Delivers every queued packet due at `tick`, in queue order.
void DeliverDue(int tick, std::vector<InFlight>& queue, Peer& to) {
  std::vector<InFlight> later;
  for (InFlight& packet : queue) {
    if (packet.deliver_tick <= tick) {
      ReceivePacket(to, packet.bytes);
    } else {
      later.push_back(std::move(packet));
    }
  }
  queue = std::move(later);
}

// Runs both directions until each side delivered `count` messages.
void RunExchange(uint16_t first_id, Network network, uint32_t count) {
  Peer a(first_id);
  Peer b(first_id);
  uint32_t next_a = 0;
  uint32_t next_b = 0;
  std::vector<InFlight> to_a;
  std::vector<InFlight> to_b;
  constexpr int kMaxTicks = 60 * 600;
  int tick = 0;
  for (; tick < kMaxTicks; ++tick) {
    const double now = tick * kTickSeconds;
    // A few messages per tick each way, until the window is full.
    for (int i = 0; i < 3; ++i) {
      if (next_a < count &&
          a.endpoint.Send(PickupTaken{.instance_id = next_a}).ok()) {
        ++next_a;
      }
      if (next_b < count &&
          b.endpoint.Send(PickupTaken{.instance_id = next_b}).ok()) {
        ++next_b;
      }
    }
    network.Send(tick, BuildPacket(a, static_cast<uint32_t>(tick), now), to_b);
    network.Send(tick, BuildPacket(b, static_cast<uint32_t>(tick), now), to_a);
    DeliverDue(tick, to_b, b);
    DeliverDue(tick, to_a, a);
    if (a.delivered.size() == count && b.delivered.size() == count &&
        a.endpoint.unacked() == 0 && b.endpoint.unacked() == 0) {
      break;
    }
  }
  CHECK(tick < kMaxTicks);
  for (const Peer* peer : {&a, &b}) {
    REQUIRE(peer->delivered.size() == count);
    for (uint32_t i = 0; i < count; ++i) {
      CHECK(peer->delivered[i] == i);
    }
  }
}

TEST_CASE("reliable messages survive a perfect network") {
  RunExchange(0, Network(0.0, 0.0, 0), 500);
}

TEST_CASE("reliable messages survive loss, duplication and reordering") {
  RunExchange(0, Network(0.25, 0.15, 8), 2000);
}

TEST_CASE("reliable message ids wrap around") {
  RunExchange(65000, Network(0.1, 0.1, 4), 1500);
}

TEST_CASE("the send window and the budget are enforced") {
  ReliableEndpoint endpoint(ReliableConfig{});
  for (int i = 0; i < kReliableWindow; ++i) {
    REQUIRE(endpoint.Send(WorldSyncDone{}).ok());
  }
  CHECK(endpoint.unacked() == kReliableWindow);
  CHECK(endpoint.Send(WorldSyncDone{}).error() == WireError::kWindowFull);
  CHECK(endpoint.Send(ClientHello{}).error() == WireError::kUnexpectedMessage);
  ReliableEndpoint fresh(ReliableConfig{});
  CHECK(fresh.Send(PlayerLeft{.player_id = 64}).error() ==
        WireError::kOutOfRange);

  // count (6) + id (16) + PlayerLeft (14) = 36 bits.
  ReliableEndpoint tight(
      ReliableConfig{.resend_interval_seconds = 0.1, .budget_bits = 35});
  CHECK(tight.Send(PlayerLeft{}).error() == WireError::kMessageTooLarge);
  ReliableEndpoint exact(
      ReliableConfig{.resend_interval_seconds = 0.1, .budget_bits = 36});
  REQUIRE(exact.Send(PlayerLeft{}).ok());
  REQUIRE(exact.Send(PlayerLeft{}).ok());
  ReliableSection section;
  exact.FillSection(0, 0.0, section);
  CHECK(section.size() == 1);  // the second does not fit this packet
  exact.FillSection(1, 0.05, section);
  CHECK(section.size() == 1);  // the first is not due yet
  CHECK(section.entries()[0].id == 1);
  exact.FillSection(2, 0.05, section);
  CHECK(section.empty());
  exact.OnPacketAcked(1);
  CHECK(exact.unacked() == 2);  // id 0 still outstanding
  exact.OnPacketAcked(0);
  CHECK(exact.unacked() == 0);
}

TEST_CASE("at most 32 messages travel in one packet") {
  ReliableEndpoint endpoint(ReliableConfig{});
  for (int i = 0; i < 100; ++i) {
    REQUIRE(endpoint.Send(WorldSyncDone{}).ok());
  }
  ReliableSection section;
  endpoint.FillSection(0, 0.0, section);
  CHECK(section.size() == kMaxMessagesPerPacket);
}

TEST_CASE("the receiver rejects ids beyond its window") {
  ReliableEndpoint endpoint(ReliableConfig{}, 0, 10);
  ReliableSection old;
  REQUIRE(old.Push(9, PlayerLeft{.player_id = 1}).ok());  // already past
  REQUIRE(endpoint.ReceiveSection(old).ok());
  CHECK_FALSE(endpoint.Pop().has_value());

  ReliableSection section;
  REQUIRE(section.Push(10, PlayerLeft{.player_id = 2}).ok());   // next
  REQUIRE(section.Push(200, PlayerLeft{.player_id = 3}).ok());  // buffered
  REQUIRE(section.Push(455, PlayerLeft{.player_id = 4}).ok());  // too far
  CHECK(endpoint.ReceiveSection(section).error() == WireError::kOutOfRange);
  const std::optional<Message> first = endpoint.Pop();
  REQUIRE(first.has_value());
  CHECK(*first == Message(PlayerLeft{.player_id = 2}));
  CHECK_FALSE(endpoint.Pop().has_value());  // 11 has not arrived
}

TEST_CASE("sections validate ids and message kinds") {
  ReliableSection section;
  REQUIRE(section.Push(10, WorldSyncDone{}).ok());
  CHECK(section.Push(10, WorldSyncDone{}).error() == WireError::kUsage);
  CHECK(section.Push(10 + 256, WorldSyncDone{}).error() == WireError::kUsage);
  CHECK(section.Push(11, ServerReject{}).error() ==
        WireError::kUnexpectedMessage);
  REQUIRE(section.Push(10 + 255, WorldSyncDone{}).ok());
}

}  // namespace
}  // namespace blue::net
