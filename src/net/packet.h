#ifndef BLUE_NET_PACKET_H_
#define BLUE_NET_PACKET_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "net/bit_stream.h"
#include "net/messages.h"
#include "net/reliable_channel.h"
#include "net/sequence.h"
#include "net/user_command_codec.h"
#include "net/wire_error.h"

namespace blue::net {

// Top-level packets, sections 5, 6 and 8 of docs/netcode/protocol.md. One
// netcode.io payload carries exactly one packet; every packet starts with a
// 4-bit kind and ends with zero padding to a byte. ServerSnapshot lives in
// net/snapshot.h.

// Packet kinds (4 bits, frozen). 3..15 are reserved.
enum class PacketKind : uint8_t {
  kClientInput = 0,     // Client -> server, one per tick.
  kServerSnapshot = 1,  // Server -> client, one per tick.
  kControl = 2,         // Either direction: one handshake message.
};

inline constexpr int kPacketKindBits = 4;

// Returns the kind of `payload` without decoding it. kTruncated for an
// empty payload, kUnknownPacketKind for reserved kinds.
WireResult<PacketKind> PeekPacketKind(std::span<const uint8_t> payload);

// Reads the kind field and records kUsage if it is another known kind,
// kUnknownPacketKind if it is reserved.
void ReadExpectedKind(BitReader& reader, PacketKind expected);

// Returns the bits a packet may occupy in `buffer`: its size, at most
// kMaxPayloadBytes.
size_t PayloadCapacityBits(std::span<const uint8_t> buffer);

// Encodes a Control packet carrying one handshake message (ClientHello,
// ServerWelcome or ServerReject; kUnexpectedMessage for others). Returns the
// bytes written.
WireResult<size_t> EncodeControl(const Message& message,
                                 std::span<uint8_t> buffer);
// Decodes a Control packet. kUnexpectedMessage for a game message, kUsage
// if the payload is another packet kind, kOutOfRange if longer than
// kMaxPayloadBytes.
WireResult<Message> DecodeControl(std::span<const uint8_t> payload);

// Client -> server packet, one per tick.
struct ClientInput {
  PacketHeader header;
  // Server tick of the newest snapshot fully applied (the delta baseline).
  std::optional<uint32_t> snapshot_ack;
  // Server time, in ticks plus 1/256 fractions, at which the client renders
  // remote players (its interpolation time). 0 before the first snapshot.
  uint32_t view_tick = 0;
  uint8_t view_fraction = 0;
  // 1..16 commands, newest first, consecutive ticks.
  CommandList commands;
  ReliableSection reliable;

  friend bool operator==(const ClientInput&, const ClientInput&) = default;
};

// Encodes `input` (at most kMaxPayloadBytes of `buffer` are used). Commands
// are quantised on the way. kOverflow if it does not fit; field errors.
WireResult<size_t> EncodeClientInput(const ClientInput& input,
                                     std::span<uint8_t> buffer);
// Decodes `payload` into `out` (no allocation); the decoded commands are
// canonical. `out` is unspecified on error.
WireResult<void> DecodeClientInput(std::span<const uint8_t> payload,
                                   ClientInput& out);

}  // namespace blue::net

#endif  // BLUE_NET_PACKET_H_
