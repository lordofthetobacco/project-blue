#include "net/reliable_channel.h"

#include <cstddef>
#include <cstdint>
#include <optional>

#include "net/bit_stream.h"
#include "net/messages.h"
#include "net/protocol.h"
#include "net/sequence.h"
#include "net/wire_error.h"

namespace blue::net {
namespace {

constexpr int kFirstIdBits = 16;
// Largest encodable gap between consecutive ids (id step 255).
constexpr uint32_t kMaxIdGap = kReliableWindow - 2;
constexpr uint32_t kWindowSlots = kReliableWindow;
constexpr uint32_t kSentPacketSlots = kSentPacketHistory;

size_t CountBits() {
  return static_cast<size_t>(BitsRequired(kMaxMessagesPerPacket));
}

}  // namespace

// --- ReliableSection ---------------------------------------------------------

WireResult<void> ReliableSection::Push(uint16_t id, const Message& message) {
  if (size_ >= entries_.size()) {
    return WireResult<void>::Error(WireError::kUsage);
  }
  if (IsHandshake(message)) {
    return WireResult<void>::Error(WireError::kUnexpectedMessage);
  }
  if (size_ > 0) {
    const uint16_t gap = static_cast<uint16_t>(id - entries_[size_ - 1].id);
    if (gap == 0 || gap >= kReliableWindow) {
      return WireResult<void>::Error(WireError::kUsage);
    }
  }
  entries_[size_] = ReliableEntry{.id = id, .message = message};
  ++size_;
  return WireResult<void>::Ok();
}

bool operator==(const ReliableSection& a, const ReliableSection& b) {
  if (a.size_ != b.size_) {
    return false;
  }
  for (size_t i = 0; i < a.size_; ++i) {
    if (!(a.entries_[i] == b.entries_[i])) {
      return false;
    }
  }
  return true;
}

void WriteReliableSection(const ReliableSection& section, BitWriter& writer) {
  const std::span<const ReliableEntry> entries = section.entries();
  writer.WriteBoundedU32(static_cast<uint32_t>(entries.size()), 0,
                         kMaxMessagesPerPacket);
  uint16_t previous = 0;
  for (size_t i = 0; i < entries.size(); ++i) {
    const ReliableEntry& entry = entries[i];
    if (i == 0) {
      writer.WriteU16(entry.id);
    } else {
      const uint32_t gap = static_cast<uint16_t>(entry.id - previous - 1);
      if (gap > kMaxIdGap) {
        writer.Fail(WireError::kOutOfRange);
        return;
      }
      writer.WriteVarUint(gap);
    }
    previous = entry.id;
    if (IsHandshake(entry.message)) {
      writer.Fail(WireError::kUnexpectedMessage);
      return;
    }
    WriteMessage(entry.message, writer);
  }
}

void ReadReliableSection(BitReader& reader, ReliableSection& out) {
  out.size_ = 0;
  const uint32_t count = reader.ReadBoundedU32(0, kMaxMessagesPerPacket);
  uint16_t previous = 0;
  for (uint32_t i = 0; i < count && reader.ok(); ++i) {
    ReliableEntry& entry = out.entries_[i];
    if (i == 0) {
      entry.id = reader.ReadU16();
    } else {
      const uint32_t gap = reader.ReadVarUint();
      if (gap > kMaxIdGap) {
        reader.Fail(WireError::kOutOfRange);
        return;
      }
      entry.id = static_cast<uint16_t>(previous + gap + 1);
    }
    previous = entry.id;
    entry.message = ReadMessage(reader);
    if (reader.ok() && IsHandshake(entry.message)) {
      reader.Fail(WireError::kUnexpectedMessage);
      return;
    }
  }
  if (reader.ok()) {
    out.size_ = count;
  }
}

// --- ReliableEndpoint --------------------------------------------------------

ReliableEndpoint::ReliableEndpoint(const ReliableConfig& config)
    : ReliableEndpoint(config, 0, 0) {}

ReliableEndpoint::ReliableEndpoint(const ReliableConfig& config,
                                   uint16_t send_id, uint16_t receive_id)
    : config_(config),
      send_(kWindowSlots),
      oldest_unacked_(send_id),
      next_send_id_(send_id),
      sent_packets_(kSentPacketSlots),
      receive_(kWindowSlots),
      next_receive_id_(receive_id) {}

int ReliableEndpoint::unacked() const {
  return static_cast<uint16_t>(next_send_id_ - oldest_unacked_);
}

WireResult<uint16_t> ReliableEndpoint::Send(const Message& message) {
  if (IsHandshake(message)) {
    return WireResult<uint16_t>::Error(WireError::kUnexpectedMessage);
  }
  if (unacked() >= kReliableWindow) {
    return WireResult<uint16_t>::Error(WireError::kWindowFull);
  }
  const WireResult<size_t> bits = MessageBits(message);
  if (!bits.ok()) {
    return WireResult<uint16_t>::Error(bits.error());
  }
  if (CountBits() + kFirstIdBits + bits.value() > config_.budget_bits) {
    return WireResult<uint16_t>::Error(WireError::kMessageTooLarge);
  }
  const uint16_t id = next_send_id_;
  send_[id % kWindowSlots] = SendSlot{.id = id,
                                      .occupied = true,
                                      .ever_sent = false,
                                      .last_sent = 0.0,
                                      .bits = bits.value(),
                                      .message = message};
  ++next_send_id_;
  return WireResult<uint16_t>::Ok(id);
}

void ReliableEndpoint::FillSection(uint16_t packet_sequence, double now_seconds,
                                   ReliableSection& out) {
  out.Clear();
  SentPacket record;
  record.sequence = packet_sequence;
  record.valid = true;
  size_t used = CountBits();
  std::optional<uint16_t> previous;
  for (uint16_t id = oldest_unacked_;
       id != next_send_id_ && out.size() < kMaxMessagesPerPacket; ++id) {
    SendSlot& slot = send_[id % kWindowSlots];
    const bool due = !slot.ever_sent || now_seconds - slot.last_sent >=
                                            config_.resend_interval_seconds;
    if (!slot.occupied || slot.id != id || !due) {
      continue;
    }
    const size_t id_bits =
        previous.has_value()
            ? VarUintBits(static_cast<uint16_t>(id - *previous - 1))
            : kFirstIdBits;
    const size_t cost = id_bits + slot.bits;
    if (used + cost > config_.budget_bits || !out.Push(id, slot.message).ok()) {
      continue;
    }
    used += cost;
    slot.ever_sent = true;
    slot.last_sent = now_seconds;
    record.ids[static_cast<size_t>(record.count)] = id;
    ++record.count;
    previous = id;
  }
  sent_packets_[packet_sequence % kSentPacketSlots] = record;
}

void ReliableEndpoint::OnPacketAcked(uint16_t sequence) {
  SentPacket& record = sent_packets_[sequence % kSentPacketSlots];
  if (!record.valid || record.sequence != sequence) {
    return;
  }
  record.valid = false;
  for (int i = 0; i < record.count; ++i) {
    const uint16_t id = record.ids[static_cast<size_t>(i)];
    SendSlot& slot = send_[id % kWindowSlots];
    if (slot.occupied && slot.id == id) {
      slot.occupied = false;
    }
  }
  while (oldest_unacked_ != next_send_id_) {
    const SendSlot& slot = send_[oldest_unacked_ % kWindowSlots];
    if (slot.occupied && slot.id == oldest_unacked_) {
      break;
    }
    ++oldest_unacked_;
  }
}

WireResult<void> ReliableEndpoint::ReceiveSection(
    const ReliableSection& section) {
  for (const ReliableEntry& entry : section.entries()) {
    if (SeqLess(entry.id, next_receive_id_)) {
      continue;
    }
    if (static_cast<uint16_t>(entry.id - next_receive_id_) >= kReliableWindow) {
      return WireResult<void>::Error(WireError::kOutOfRange);
    }
    ReceiveSlot& slot = receive_[entry.id % kWindowSlots];
    if (!slot.occupied) {
      slot = ReceiveSlot{
          .id = entry.id, .occupied = true, .message = entry.message};
    }
  }
  return WireResult<void>::Ok();
}

std::optional<Message> ReliableEndpoint::Pop() {
  ReceiveSlot& slot = receive_[next_receive_id_ % kWindowSlots];
  if (!slot.occupied || slot.id != next_receive_id_) {
    return std::nullopt;
  }
  slot.occupied = false;
  ++next_receive_id_;
  return slot.message;
}

}  // namespace blue::net
