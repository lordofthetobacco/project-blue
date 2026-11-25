#include "net/packet.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

#include "net/bit_stream.h"
#include "net/messages.h"
#include "net/protocol.h"
#include "net/reliable_channel.h"
#include "net/sequence.h"
#include "net/user_command_codec.h"
#include "net/wire_error.h"

namespace blue::net {
namespace {

bool IsKnownKind(uint32_t kind) {
  return kind <= static_cast<uint32_t>(PacketKind::kControl);
}

}  // namespace

WireResult<PacketKind> PeekPacketKind(std::span<const uint8_t> payload) {
  BitReader reader(payload);
  const uint32_t kind = reader.ReadBits(kPacketKindBits);
  if (!reader.ok()) {
    return WireResult<PacketKind>::Error(*reader.error());
  }
  if (!IsKnownKind(kind)) {
    return WireResult<PacketKind>::Error(WireError::kUnknownPacketKind);
  }
  return WireResult<PacketKind>::Ok(static_cast<PacketKind>(kind));
}

void ReadExpectedKind(BitReader& reader, PacketKind expected) {
  const uint32_t kind = reader.ReadBits(kPacketKindBits);
  if (!reader.ok() || kind == static_cast<uint32_t>(expected)) {
    return;
  }
  reader.Fail(IsKnownKind(kind) ? WireError::kUsage
                                : WireError::kUnknownPacketKind);
}

size_t PayloadCapacityBits(std::span<const uint8_t> buffer) {
  return std::min(buffer.size(), kMaxPayloadBytes) * 8;
}

WireResult<size_t> EncodeControl(const Message& message,
                                 std::span<uint8_t> buffer) {
  if (!IsHandshake(message)) {
    return WireResult<size_t>::Error(WireError::kUnexpectedMessage);
  }
  BitWriter writer(buffer);
  writer.set_limit(PayloadCapacityBits(buffer));
  writer.WriteBits(static_cast<uint32_t>(PacketKind::kControl),
                   kPacketKindBits);
  WriteMessage(message, writer);
  return writer.Finish();
}

WireResult<Message> DecodeControl(std::span<const uint8_t> payload) {
  if (payload.size() > kMaxPayloadBytes) {
    return WireResult<Message>::Error(WireError::kOutOfRange);
  }
  BitReader reader(payload);
  ReadExpectedKind(reader, PacketKind::kControl);
  const Message message = ReadMessage(reader);
  if (reader.ok() && !IsHandshake(message)) {
    return WireResult<Message>::Error(WireError::kUnexpectedMessage);
  }
  if (WireResult<void> finished = reader.Finish(); !finished.ok()) {
    return WireResult<Message>::Error(finished.error());
  }
  return WireResult<Message>::Ok(message);
}

WireResult<size_t> EncodeClientInput(const ClientInput& input,
                                     std::span<uint8_t> buffer) {
  BitWriter writer(buffer);
  writer.set_limit(PayloadCapacityBits(buffer));
  writer.WriteBits(static_cast<uint32_t>(PacketKind::kClientInput),
                   kPacketKindBits);
  WritePacketHeader(input.header, writer);
  writer.WriteOptionalU32(input.snapshot_ack);
  writer.WriteU32(input.view_tick);
  writer.WriteU8(input.view_fraction);
  WriteCommandList(input.commands, writer);
  WriteReliableSection(input.reliable, writer);
  return writer.Finish();
}

WireResult<void> DecodeClientInput(std::span<const uint8_t> payload,
                                   ClientInput& out) {
  if (payload.size() > kMaxPayloadBytes) {
    return WireResult<void>::Error(WireError::kOutOfRange);
  }
  BitReader reader(payload);
  ReadExpectedKind(reader, PacketKind::kClientInput);
  out.header = ReadPacketHeader(reader);
  out.snapshot_ack = reader.ReadOptionalU32();
  out.view_tick = reader.ReadU32();
  out.view_fraction = reader.ReadU8();
  out.commands = ReadCommandList(reader);
  ReadReliableSection(reader, out.reliable);
  return reader.Finish();
}

}  // namespace blue::net
