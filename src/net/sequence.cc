#include "net/sequence.h"

#include <cstdint>

#include "net/bit_stream.h"

namespace blue::net {
namespace {

constexpr uint16_t kNoAck = 0xFFFF;
constexpr uint32_t kAckBits = 32;
constexpr int32_t kHalfSequenceCircle = 32768;
constexpr uint32_t kHalfTickCircle = uint32_t{1} << 31;

}  // namespace

bool SeqGreater(uint16_t a, uint16_t b) {
  // Literally the spec's form: exactly half a circle apart counts as newer
  // only when a > b.
  const int32_t difference = int32_t{a} - int32_t{b};
  return (difference > 0 && difference <= kHalfSequenceCircle) ||
         (difference < 0 && -difference > kHalfSequenceCircle);
}

bool SeqLess(uint16_t a, uint16_t b) { return SeqGreater(b, a); }

bool TickGreater(uint32_t a, uint32_t b) {
  return a != b && a - b < kHalfTickCircle;
}

void WritePacketHeader(const PacketHeader& header, BitWriter& writer) {
  writer.WriteU16(header.sequence);
  writer.WriteU16(header.ack);
  writer.WriteU32(header.ack_bits);
}

PacketHeader ReadPacketHeader(BitReader& reader) {
  PacketHeader header;
  header.sequence = reader.ReadU16();
  header.ack = reader.ReadU16();
  header.ack_bits = reader.ReadU32();
  return header;
}

AckTracker::AckTracker(uint16_t initial_sequence)
    : next_sequence_(initial_sequence) {}

PacketHeader AckTracker::NextHeader() {
  const uint16_t sequence = next_sequence_;
  ++next_sequence_;
  sent_[sequence % kSentPacketHistory] =
      SentRecord{.sequence = sequence, .valid = true, .acked = false};
  PacketHeader header;
  header.sequence = sequence;
  header.ack = has_received_ ? newest_received_ : kNoAck;
  header.ack_bits = has_received_ ? received_bits_ : 0;
  return header;
}

ReceiveOutcome AckTracker::OnReceive(const PacketHeader& header) {
  ReceiveOutcome outcome;
  AckOne(header.ack, outcome);
  for (uint32_t i = 0; i < kAckBits; ++i) {
    if ((header.ack_bits & (uint32_t{1} << i)) != 0) {
      AckOne(static_cast<uint16_t>(header.ack - 1 - i), outcome);
    }
  }

  const uint16_t sequence = header.sequence;
  if (!has_received_) {
    has_received_ = true;
    newest_received_ = sequence;
    received_bits_ = 0;
    outcome.receipt = Receipt::kFresh;
    return outcome;
  }
  if (SeqGreater(sequence, newest_received_)) {
    const uint32_t distance =
        static_cast<uint16_t>(sequence - newest_received_);
    if (distance > kAckBits) {
      received_bits_ = 0;
    } else {
      const uint64_t wide = (uint64_t{received_bits_} << distance) |
                            (uint64_t{1} << (distance - 1));
      received_bits_ = static_cast<uint32_t>(wide & 0xFFFFFFFFu);
    }
    newest_received_ = sequence;
    outcome.receipt = Receipt::kFresh;
    return outcome;
  }
  if (sequence == newest_received_) {
    outcome.receipt = Receipt::kDuplicate;
    return outcome;
  }
  const uint32_t distance = static_cast<uint16_t>(newest_received_ - sequence);
  if (distance > kAckBits) {
    outcome.receipt = Receipt::kStale;
    return outcome;
  }
  const uint32_t bit = uint32_t{1} << (distance - 1);
  if ((received_bits_ & bit) != 0) {
    outcome.receipt = Receipt::kDuplicate;
    return outcome;
  }
  received_bits_ |= bit;
  outcome.receipt = Receipt::kFresh;
  return outcome;
}

void AckTracker::AckOne(uint16_t sequence, ReceiveOutcome& outcome) {
  SentRecord& record = sent_[sequence % kSentPacketHistory];
  if (record.valid && record.sequence == sequence && !record.acked) {
    record.acked = true;
    if (outcome.acked_count < ReceiveOutcome::kMaxAcked) {
      outcome.acked[static_cast<size_t>(outcome.acked_count)] = sequence;
      ++outcome.acked_count;
    }
  }
}

}  // namespace blue::net
