#include "net/user_command_codec.h"

#include <cstddef>
#include <cstdint>
#include <span>

#include "net/bit_stream.h"
#include "net/protocol.h"
#include "net/quantize.h"
#include "net/wire_error.h"
#include "sim/user_command.h"

namespace blue::net {
namespace {

constexpr int kButtonBits = 8;
constexpr int32_t kMaxSlot = 7;
constexpr int64_t kYawTurn = int64_t{1} << kFineAngleBits;
constexpr int64_t kYawHalfTurn = kYawTurn / 2;

// A command in wire units: angles as integers.
struct CommandQ {
  uint32_t tick = 0;
  uint32_t held = 0;
  uint32_t pressed = 0;
  uint32_t yaw = 0;
  int32_t pitch = 0;
  int8_t select_slot = -1;
  uint8_t interact_kind = interact_kind::kNone;
  uint32_t interact_id = 0;
};

// Returns true if the action fields (slot and interaction) of `a` and `b`
// are equal.
bool SameActions(const CommandQ& a, const CommandQ& b) {
  return a.select_slot == b.select_slot && a.interact_kind == b.interact_kind &&
         a.interact_id == b.interact_id;
}

// Returns `value` modulo the yaw turn, in 0..turn.
int64_t WrapYaw(int64_t value) {
  const int64_t wrapped = value % kYawTurn;
  return wrapped < 0 ? wrapped + kYawTurn : wrapped;
}

CommandQ ToWire(const sim::UserCommand& command) {
  CommandQ q;
  q.tick = command.tick;
  q.held = command.held;
  q.pressed = command.pressed;
  q.yaw = QuantizeYaw(command.yaw, kFineAngleBits);
  q.pitch = QuantizePitch(command.pitch, kFineAngleBits);
  q.select_slot = command.select_slot;
  q.interact_kind = command.interact_kind;
  q.interact_id =
      command.interact_kind == interact_kind::kNone ? 0 : command.interact_id;
  return q;
}

sim::UserCommand FromWire(const CommandQ& q) {
  sim::UserCommand command;
  command.tick = q.tick;
  command.held = q.held;
  command.pressed = q.pressed;
  command.yaw = DequantizeYaw(q.yaw, kFineAngleBits);
  command.pitch = DequantizePitch(q.pitch, kFineAngleBits);
  command.select_slot = q.select_slot;
  command.interact_kind = q.interact_kind;
  command.interact_id = q.interact_id;
  return command;
}

void WriteActions(const CommandQ& q, BitWriter& writer) {
  const bool has_slot = q.select_slot >= 0;
  writer.WriteBool(has_slot);
  if (has_slot) {
    writer.WriteBoundedI32(q.select_slot, 0, kMaxSlot);
  } else if (q.select_slot != -1) {
    writer.Fail(WireError::kOutOfRange);
  }
  writer.WriteBoundedU32(q.interact_kind, 0, interact_kind::kMax);
  if (q.interact_kind != interact_kind::kNone) {
    writer.WriteU32(q.interact_id);
  }
}

void ReadActions(BitReader& reader, CommandQ& q) {
  q.select_slot = reader.ReadBool()
                      ? static_cast<int8_t>(reader.ReadBoundedI32(0, kMaxSlot))
                      : int8_t{-1};
  q.interact_kind =
      static_cast<uint8_t>(reader.ReadBoundedU32(0, interact_kind::kMax));
  q.interact_id =
      q.interact_kind != interact_kind::kNone ? reader.ReadU32() : 0;
}

void WriteFull(const CommandQ& q, BitWriter& writer) {
  const int32_t limit = PitchLimit(kFineAngleBits);
  writer.WriteU32(q.tick);
  writer.WriteBits(q.held, kButtonBits);
  writer.WriteBits(q.pressed, kButtonBits);
  writer.WriteBits(q.yaw, kFineAngleBits);
  writer.WriteBoundedI32(q.pitch, -limit, limit);
  WriteActions(q, writer);
}

CommandQ ReadFull(BitReader& reader) {
  const int32_t limit = PitchLimit(kFineAngleBits);
  CommandQ q;
  q.tick = reader.ReadU32();
  q.held = reader.ReadBits(kButtonBits);
  q.pressed = reader.ReadBits(kButtonBits);
  q.yaw = reader.ReadBits(kFineAngleBits);
  q.pitch = reader.ReadBoundedI32(-limit, limit);
  ReadActions(reader, q);
  return q;
}

// Writes `q` as a delta against `newer`, the command one tick later. A
// changed_* bit is set exactly when the field differs.
void WriteDelta(const CommandQ& q, const CommandQ& newer, BitWriter& writer) {
  const bool held_changed = q.held != newer.held;
  writer.WriteBool(held_changed);
  if (held_changed) {
    writer.WriteBits(q.held, kButtonBits);
  }

  const bool pressed_changed = q.pressed != newer.pressed;
  writer.WriteBool(pressed_changed);
  if (pressed_changed) {
    writer.WriteBits(q.pressed, kButtonBits);
  }

  const bool yaw_changed = q.yaw != newer.yaw;
  writer.WriteBool(yaw_changed);
  if (yaw_changed) {
    int64_t diff = WrapYaw(int64_t{q.yaw} - int64_t{newer.yaw});
    if (diff >= kYawHalfTurn) {
      diff -= kYawTurn;
    }
    writer.WriteVarInt(static_cast<int32_t>(diff));
  }

  const bool pitch_changed = q.pitch != newer.pitch;
  writer.WriteBool(pitch_changed);
  if (pitch_changed) {
    // Both pitches lie within +-2^18 (quantised), so this cannot overflow.
    writer.WriteVarInt(q.pitch - newer.pitch);
  }

  const bool actions_changed = !SameActions(q, newer);
  writer.WriteBool(actions_changed);
  if (actions_changed) {
    WriteActions(q, writer);
  }
}

CommandQ ReadDelta(BitReader& reader, const CommandQ& newer) {
  CommandQ q = newer;
  q.tick = newer.tick - 1;

  if (reader.ReadBool()) {
    q.held = reader.ReadBits(kButtonBits);
    if (q.held == newer.held) {
      reader.Fail(WireError::kNonCanonical);
    }
  }

  if (reader.ReadBool()) {
    q.pressed = reader.ReadBits(kButtonBits);
    if (q.pressed == newer.pressed) {
      reader.Fail(WireError::kNonCanonical);
    }
  }

  if (reader.ReadBool()) {
    const int64_t diff = reader.ReadVarInt();
    if (diff == 0) {
      reader.Fail(WireError::kNonCanonical);
    } else if (diff < -kYawHalfTurn || diff >= kYawHalfTurn) {
      reader.Fail(WireError::kOutOfRange);
    } else {
      q.yaw = static_cast<uint32_t>(WrapYaw(int64_t{newer.yaw} + diff));
    }
  }

  if (reader.ReadBool()) {
    const int64_t diff = reader.ReadVarInt();
    const int64_t pitch = int64_t{newer.pitch} + diff;
    const int64_t limit = PitchLimit(kFineAngleBits);
    if (diff == 0) {
      reader.Fail(WireError::kNonCanonical);
    } else if (pitch < -limit || pitch > limit) {
      reader.Fail(WireError::kOutOfRange);
    } else {
      q.pitch = static_cast<int32_t>(pitch);
    }
  }

  if (reader.ReadBool()) {
    ReadActions(reader, q);
    if (SameActions(q, newer)) {
      reader.Fail(WireError::kNonCanonical);
    }
  }
  return q;
}

}  // namespace

sim::UserCommand Canonical(const sim::UserCommand& command) {
  return FromWire(ToWire(command));
}

WireResult<CommandList> CommandList::FromCommands(
    std::span<const sim::UserCommand> commands) {
  CommandList list;
  for (const sim::UserCommand& command : commands) {
    if (WireResult<void> pushed = list.Push(command); !pushed.ok()) {
      return WireResult<CommandList>::Error(pushed.error());
    }
  }
  if (list.empty()) {
    return WireResult<CommandList>::Error(WireError::kUsage);
  }
  return WireResult<CommandList>::Ok(list);
}

WireResult<void> CommandList::Push(const sim::UserCommand& command) {
  if (size_ >= commands_.size()) {
    return WireResult<void>::Error(WireError::kUsage);
  }
  if (size_ > 0 && command.tick != commands_[size_ - 1].tick - 1) {
    return WireResult<void>::Error(WireError::kUsage);
  }
  commands_[size_] = command;
  ++size_;
  return WireResult<void>::Ok();
}

CommandList CommandList::Canonical() const {
  CommandList out = *this;
  for (size_t i = 0; i < out.size_; ++i) {
    out.commands_[i] = net::Canonical(out.commands_[i]);
  }
  return out;
}

bool operator==(const CommandList& a, const CommandList& b) {
  const std::span<const sim::UserCommand> left = a.commands();
  const std::span<const sim::UserCommand> right = b.commands();
  if (left.size() != right.size()) {
    return false;
  }
  for (size_t i = 0; i < left.size(); ++i) {
    if (!(left[i] == right[i])) {
      return false;
    }
  }
  return true;
}

void WriteCommandList(const CommandList& list, BitWriter& writer) {
  if (list.empty()) {
    writer.Fail(WireError::kUsage);
    return;
  }
  const std::span<const sim::UserCommand> commands = list.commands();
  writer.WriteBoundedU32(static_cast<uint32_t>(commands.size()), 1,
                         kMaxCommandsPerPacket);
  CommandQ newer;
  for (size_t i = 0; i < commands.size(); ++i) {
    const CommandQ q = ToWire(commands[i]);
    if (i == 0) {
      WriteFull(q, writer);
    } else {
      WriteDelta(q, newer, writer);
    }
    newer = q;
  }
}

CommandList ReadCommandList(BitReader& reader) {
  CommandList list;
  const uint32_t count = reader.ReadBoundedU32(1, kMaxCommandsPerPacket);
  if (!reader.ok()) {
    return list;
  }
  CommandQ newer;
  for (uint32_t i = 0; i < count; ++i) {
    const CommandQ q = i == 0 ? ReadFull(reader) : ReadDelta(reader, newer);
    list.commands_[i] = FromWire(q);
    newer = q;
  }
  list.size_ = count;
  return list;
}

}  // namespace blue::net
