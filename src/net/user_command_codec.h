#ifndef BLUE_NET_USER_COMMAND_CODEC_H_
#define BLUE_NET_USER_COMMAND_CODEC_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "net/bit_stream.h"
#include "net/protocol.h"
#include "net/wire_error.h"
#include "sim/user_command.h"

namespace blue::net {

// sim::UserCommand on the wire, section 8 of docs/netcode/protocol.md.
// Angles travel quantised to 20 bits per turn; held and pressed use 8 bits
// each (higher bits must be 0); select_slot is -1 or 0..7; interact_kind is
// 0..15, and interact_id is only sent when the kind is not 0.

// interact_kind values.
namespace interact_kind {
inline constexpr uint8_t kNone = 0;
inline constexpr uint8_t kPickup = 1;         // interact_id = instance_id
inline constexpr uint8_t kDroppedWeapon = 2;  // interact_id = dropped_id
inline constexpr uint8_t kMax = 15;
}  // namespace interact_kind

// Returns the command as the server will decode it: yaw and pitch
// quantised (yaw lands in [0, 2*pi), pitch is clamped to a quarter turn) and
// interact_id zeroed when interact_kind is none. The client must predict
// with this value so prediction and authority see the same angles.
sim::UserCommand Canonical(const sim::UserCommand& command);

// The commands of one ClientInput: 1..16 commands for consecutive ticks,
// newest first. Fixed capacity, no allocation.
class CommandList {
 public:
  // An empty list (it cannot be encoded until a command is pushed).
  CommandList() = default;
  CommandList(const CommandList&) = default;
  CommandList& operator=(const CommandList&) = default;

  // Returns a list of `commands`, ordered newest first. kUsage if empty,
  // longer than kMaxCommandsPerPacket, or the ticks are not consecutive and
  // descending.
  static WireResult<CommandList> FromCommands(
      std::span<const sim::UserCommand> commands);

  // Appends the next older command. kUsage if the list is full or the tick
  // is not one before the previous command's.
  WireResult<void> Push(const sim::UserCommand& command);

  // Commands, newest first.
  std::span<const sim::UserCommand> commands() const {
    return std::span<const sim::UserCommand>(commands_.data(), size_);
  }
  size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }

  // Returns the list with every command canonicalised.
  CommandList Canonical() const;

  // Lists are equal when their commands are.
  friend bool operator==(const CommandList& a, const CommandList& b);

 private:
  // Fills the list without the Push() checks (the decoder derives the
  // ticks, so they are consecutive by construction).
  friend CommandList ReadCommandList(BitReader& reader);

  std::array<sim::UserCommand, kMaxCommandsPerPacket> commands_ = {};
  size_t size_ = 0;
};

// Writes a command list: count as bounded(1, 16), the newest command in
// full, then each older one delta-encoded against the next newer one.
// Records kUsage for an empty list and kOutOfRange for fields outside their
// wire range.
void WriteCommandList(const CommandList& list, BitWriter& writer);
// Reads a command list. The decoded commands are canonical. Non-canonical
// deltas record kNonCanonical.
CommandList ReadCommandList(BitReader& reader);

}  // namespace blue::net

#endif  // BLUE_NET_USER_COMMAND_CODEC_H_
