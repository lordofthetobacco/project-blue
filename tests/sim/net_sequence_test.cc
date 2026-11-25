// Sequence wrap-around comparisons, the packet header and the ack tracker
// (docs/netcode/protocol.md section 7).

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <span>

#include "net/bit_stream.h"
#include "net/sequence.h"

namespace blue::net {
namespace {

TEST_CASE("sequence and tick comparisons wrap") {
  CHECK(SeqGreater(1, 0));
  CHECK(SeqGreater(0, 65535));
  CHECK(SeqGreater(100, 65500));
  CHECK_FALSE(SeqGreater(65500, 100));
  CHECK(SeqLess(65535, 0));
  CHECK_FALSE(SeqGreater(5, 5));
  CHECK(SeqGreater(32768, 0));
  CHECK_FALSE(SeqGreater(0, 32768));  // half a circle: only a > b wins
  CHECK_FALSE(SeqGreater(32769, 0));
  CHECK(TickGreater(0, UINT32_MAX));
  CHECK_FALSE(TickGreater(UINT32_MAX, 0));
  CHECK_FALSE(TickGreater(7, 7));
}

TEST_CASE("packet header round trips") {
  const PacketHeader header{
      .sequence = 65535, .ack = 0, .ack_bits = 0x80000001};
  std::array<uint8_t, 8> buffer{};
  BitWriter writer(buffer);
  WritePacketHeader(header, writer);
  WireResult<size_t> size = writer.Finish();
  REQUIRE(size.ok());
  CHECK(size.value() == 8);
  BitReader reader(buffer);
  CHECK(ReadPacketHeader(reader) == header);
  CHECK(reader.Finish().ok());
}

TEST_CASE("ack tracker classifies fresh, duplicate and stale packets") {
  AckTracker tracker;
  auto receive = [&tracker](uint16_t sequence) {
    return tracker.OnReceive(PacketHeader{.sequence = sequence, .ack = 0xFFFF})
        .receipt;
  };
  CHECK(receive(10) == Receipt::kFresh);
  CHECK(receive(10) == Receipt::kDuplicate);
  CHECK(receive(12) == Receipt::kFresh);
  CHECK(receive(11) == Receipt::kFresh);
  CHECK(receive(11) == Receipt::kDuplicate);
  CHECK(receive(50) == Receipt::kFresh);
  CHECK(receive(18) == Receipt::kFresh);  // 32 back: still in the window
  CHECK(receive(17) == Receipt::kStale);  // 33 back
  CHECK(receive(12) == Receipt::kStale);

  const PacketHeader header = tracker.NextHeader();
  CHECK(header.sequence == 0);
  CHECK(header.ack == 50);
  CHECK(header.ack_bits == 0x80000000u);
}

TEST_CASE("ack tracker reports each acknowledged sequence once") {
  AckTracker tracker(65534);
  CHECK(tracker.NextHeader().ack == 0xFFFF);  // nothing received yet
  tracker.NextHeader();                       // 65535
  tracker.NextHeader();                       // 0
  CHECK(tracker.next_sequence() == 1);

  ReceiveOutcome outcome = tracker.OnReceive(
      PacketHeader{.sequence = 7, .ack = 0, .ack_bits = 0b10});
  REQUIRE(outcome.acked_count == 2);
  CHECK(outcome.newly_acked()[0] == 0);
  CHECK(outcome.newly_acked()[1] == 65534);

  outcome = tracker.OnReceive(
      PacketHeader{.sequence = 8, .ack = 0, .ack_bits = 0b11});
  REQUIRE(outcome.acked_count == 1);
  CHECK(outcome.newly_acked()[0] == 65535);

  // Never sent: not acknowledged.
  outcome = tracker.OnReceive(PacketHeader{.sequence = 9, .ack = 400});
  CHECK(outcome.acked_count == 0);
}

}  // namespace
}  // namespace blue::net
