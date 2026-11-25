#ifndef BLUE_NET_WIRE_ERROR_H_
#define BLUE_NET_WIRE_ERROR_H_

#include <cstdint>
#include <string_view>

#include "core/result.h"

namespace blue::net {

// Everything that can go wrong while encoding, decoding or running a channel
// of the Project Blue wire protocol (docs/netcode/protocol.md). Mirrors
// blue_protocol::Error of the Rust server one to one. Decoding never crashes:
// every malformed input maps to one of these values.
enum class WireError : uint8_t {
  // The writer ran out of room (buffer or bit budget).
  kOverflow,
  // The reader needed more bits than the payload holds.
  kTruncated,
  // A value lies outside the range its field allows, or a float is not
  // finite, or the payload is longer than kMaxPayloadBytes.
  kOutOfRange,
  // A bounded string is not valid UTF-8.
  kInvalidUtf8,
  // The bits decode, but not in the one canonical form the protocol allows
  // (overlong varuint, empty delta, duplicate id, ...).
  kNonCanonical,
  // A whole byte, or non-zero padding bits, follow the last field.
  kTrailingData,
  // The packet kind is reserved.
  kUnknownPacketKind,
  // The message type id is reserved.
  kUnknownMessage,
  // A known message appeared where it is not allowed (a handshake message in
  // a reliable section, a game message in a Control packet).
  kUnexpectedMessage,
  // A delta-encoded players section names a baseline the receiver does not
  // have. The rest of the packet is still valid.
  kMissingBaseline,
  // A delta entry contradicts its baseline.
  kInconsistent,
  // The API was used out of order or with contradictory arguments.
  kUsage,
  // The reliable send window is full: the peer is not acknowledging.
  kWindowFull,
  // A message can never fit into a packet's reliable budget.
  kMessageTooLarge,
};

// Returns a short English description of `error` for logs.
std::string_view WireErrorText(WireError error);

// The result type of every fallible wire operation. An exact alias of
// core::Result with WireError as the error type.
template <typename T>
using WireResult = core::Result<T, WireError>;

}  // namespace blue::net

#endif  // BLUE_NET_WIRE_ERROR_H_
