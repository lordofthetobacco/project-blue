#ifndef BLUE_NET_RELIABLE_CHANNEL_H_
#define BLUE_NET_RELIABLE_CHANNEL_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "net/bit_stream.h"
#include "net/messages.h"
#include "net/protocol.h"
#include "net/wire_error.h"

namespace blue::net {

// The reliable-ordered message channel riding on packet acknowledgements,
// section 11 of docs/netcode/protocol.md.

// One message with its 16-bit id.
struct ReliableEntry {
  uint16_t id = 0;
  Message message;
  friend bool operator==(const ReliableEntry&, const ReliableEntry&) = default;
};

// The reliable section of one packet: up to 32 game messages whose ids
// strictly increase by 1..255 on the 16-bit circle. Fixed capacity.
class ReliableSection {
 public:
  ReliableSection() = default;
  ReliableSection(const ReliableSection&) = default;
  ReliableSection& operator=(const ReliableSection&) = default;

  std::span<const ReliableEntry> entries() const {
    return std::span<const ReliableEntry>(entries_.data(), size_);
  }
  size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }
  void Clear() { size_ = 0; }

  // Appends an entry. kUsage if full or `id` does not follow the previous
  // id by 1..255; kUnexpectedMessage for handshake messages.
  WireResult<void> Push(uint16_t id, const Message& message);

  // Sections are equal when their entries are.
  friend bool operator==(const ReliableSection& a, const ReliableSection& b);

 private:
  friend void ReadReliableSection(BitReader& reader, ReliableSection& out);

  std::array<ReliableEntry, kMaxMessagesPerPacket> entries_ = {};
  size_t size_ = 0;
};

// Writes count as bounded(0, 32), then the first id in 16 bits and each
// later id as varuint(id - previous - 1), each followed by its message.
void WriteReliableSection(const ReliableSection& section, BitWriter& writer);
// Reads a section into `out` (overwriting it). A gap above 254 records
// kOutOfRange, a handshake message kUnexpectedMessage.
void ReadReliableSection(BitReader& reader, ReliableSection& out);

// Tunables of a ReliableEndpoint.
struct ReliableConfig {
  // Seconds before an unacknowledged message is sent again.
  double resend_interval_seconds = kDefaultResendIntervalSeconds;
  // Bits a reliable section may take per packet, count field included.
  size_t budget_bits = kDefaultReliableBudgetBytes * 8;
};

// One side of a reliable-ordered channel (sends and receives).
//
// Send() queues; FillSection() picks the messages due for an outgoing
// packet; OnPacketAcked() (fed from AckTracker::OnReceive) retires them.
// ReceiveSection() takes in the section of a fresh packet, Pop() hands
// messages out strictly in id order, each once. Both peers start with ids 0.
// Allocates its windows (about 70 KB) once, at construction; nothing after.
// Not thread-safe.
class ReliableEndpoint {
 public:
  explicit ReliableEndpoint(const ReliableConfig& config);
  // First sent id `send_id`, first expected id `receive_id` (tests exercise
  // wrap-around with this).
  ReliableEndpoint(const ReliableConfig& config, uint16_t send_id,
                   uint16_t receive_id);
  ReliableEndpoint(const ReliableEndpoint&) = default;
  ReliableEndpoint& operator=(const ReliableEndpoint&) = default;
  ReliableEndpoint(ReliableEndpoint&&) = default;
  ReliableEndpoint& operator=(ReliableEndpoint&&) = default;

  // Messages queued and not yet acknowledged.
  int unacked() const;

  // Queues a message and returns its id. kWindowFull when 256 messages are
  // unacknowledged (drop the connection); kUnexpectedMessage for handshake
  // messages; kMessageTooLarge if it can never fit the budget; field range
  // errors.
  WireResult<uint16_t> Send(const Message& message);

  // Fills `out` with the messages due at `now_seconds` that fit the budget,
  // oldest first, and records them as carried by packet `packet_sequence`.
  void FillSection(uint16_t packet_sequence, double now_seconds,
                   ReliableSection& out);

  // Marks the messages carried by packet `sequence` as delivered.
  void OnPacketAcked(uint16_t sequence);

  // Takes in the messages of a received section (duplicates and delivered
  // ids are ignored). kOutOfRange if an id lies 256 or more ahead of the
  // next expected one, a protocol violation; entries before it are kept.
  WireResult<void> ReceiveSection(const ReliableSection& section);

  // Returns the next message in order, if it has arrived.
  std::optional<Message> Pop();

 private:
  struct SendSlot {
    uint16_t id = 0;
    bool occupied = false;
    bool ever_sent = false;
    double last_sent = 0.0;
    size_t bits = 0;
    Message message;
  };
  struct SentPacket {
    uint16_t sequence = 0;
    bool valid = false;
    int count = 0;
    std::array<uint16_t, kMaxMessagesPerPacket> ids = {};
  };
  struct ReceiveSlot {
    uint16_t id = 0;
    bool occupied = false;
    Message message;
  };

  ReliableConfig config_;
  std::vector<SendSlot> send_;
  uint16_t oldest_unacked_ = 0;
  uint16_t next_send_id_ = 0;
  std::vector<SentPacket> sent_packets_;
  std::vector<ReceiveSlot> receive_;
  uint16_t next_receive_id_ = 0;
};

}  // namespace blue::net

#endif  // BLUE_NET_RELIABLE_CHANNEL_H_
