#ifndef BLUE_NET_SEQUENCE_H_
#define BLUE_NET_SEQUENCE_H_

#include <array>
#include <cstdint>
#include <span>

#include "net/bit_stream.h"
#include "net/protocol.h"

namespace blue::net {

// Sequence numbers, the packet header and packet-level acknowledgement,
// section 7 of docs/netcode/protocol.md.

// Returns true if `a` is newer than `b` on the 16-bit circle.
bool SeqGreater(uint16_t a, uint16_t b);
// Returns true if `a` is older than `b` on the 16-bit circle.
bool SeqLess(uint16_t a, uint16_t b);
// Returns true if tick `a` is newer than tick `b` on the 32-bit circle.
bool TickGreater(uint32_t a, uint32_t b);

// Starts every ClientInput and ServerSnapshot packet.
struct PacketHeader {
  uint16_t sequence = 0;  // Of this packet.
  // Newest sequence received from the peer; 0xFFFF before any.
  uint16_t ack = 0;
  // Bit i (LSB = 0) set: packet (ack - 1 - i) mod 2^16 was received too.
  uint32_t ack_bits = 0;

  friend bool operator==(const PacketHeader&, const PacketHeader&) = default;
};

void WritePacketHeader(const PacketHeader& header, BitWriter& writer);
PacketHeader ReadPacketHeader(BitReader& reader);

// Classification of a received packet sequence.
enum class Receipt : uint8_t {
  kFresh,      // First time seen: process the whole packet.
  kDuplicate,  // Already received: discard the packet.
  // Older than the 32-packet ack window: discard (it is never acknowledged,
  // so its reliable messages will be resent).
  kStale,
};

// Outcome of AckTracker::OnReceive.
struct ReceiveOutcome {
  // A header acknowledges its `ack` plus up to 32 older sequences.
  static constexpr int kMaxAcked = 33;

  Receipt receipt = Receipt::kFresh;
  // Sequences newly acknowledged by the header, newest first; the first
  // `acked_count` entries are valid.
  std::array<uint16_t, kMaxAcked> acked = {};
  int acked_count = 0;

  std::span<const uint16_t> newly_acked() const {
    return std::span<const uint16_t>(acked.data(),
                                     static_cast<size_t>(acked_count));
  }
};

// Per-connection sequence and acknowledgement state (one per peer).
//
// Call NextHeader() for every outgoing packet, and OnReceive() only after an
// incoming packet decoded completely; process the packet's content only when
// the receipt is kFresh. Pass every newly acknowledged sequence to the
// reliable channel. Fixed size, no allocation. Not thread-safe.
class AckTracker {
 public:
  // Starts at sequence 0.
  AckTracker() = default;
  // Starts at `initial_sequence` (tests exercise wrap-around with this).
  explicit AckTracker(uint16_t initial_sequence);
  AckTracker(const AckTracker&) = default;
  AckTracker& operator=(const AckTracker&) = default;

  // Sequence the next outgoing packet will use.
  uint16_t next_sequence() const { return next_sequence_; }

  // Returns the header for the next outgoing packet and records the packet
  // as sent.
  PacketHeader NextHeader();

  // Registers a received header: applies its acks (always; idempotent) and
  // classifies its sequence against the receive history.
  ReceiveOutcome OnReceive(const PacketHeader& header);

 private:
  struct SentRecord {
    uint16_t sequence = 0;
    bool valid = false;
    bool acked = false;
  };

  // Acknowledges `sequence` if it is in the sent history and not yet acked.
  void AckOne(uint16_t sequence, ReceiveOutcome& outcome);

  uint16_t next_sequence_ = 0;
  bool has_received_ = false;
  uint16_t newest_received_ = 0;
  uint32_t received_bits_ = 0;
  std::array<SentRecord, kSentPacketHistory> sent_ = {};
};

}  // namespace blue::net

#endif  // BLUE_NET_SEQUENCE_H_
