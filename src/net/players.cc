#include "net/players.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "net/bit_stream.h"
#include "net/protocol.h"
#include "net/quantize.h"
#include "net/wire_error.h"

namespace blue::net {
namespace {

constexpr uint8_t kFlagsMask = (1 << player_flags::kBits) - 1;
constexpr int kEntryKindBits = 2;
constexpr uint32_t kHistorySlots = kBaselineHistory;

// --- Own player --------------------------------------------------------------

void WriteWeaponSlot(const WireWeaponSlot& slot, BitWriter& writer) {
  writer.WriteBool(slot.present);
  if (!slot.present) {
    if (!(slot == WireWeaponSlot{})) {
      writer.Fail(WireError::kNonCanonical);
    }
    return;
  }
  writer.WriteU32(slot.weapon_key);
  writer.WriteBoundedU32(slot.fire_mode, 0, kMaxFireMode);
  writer.WriteU8(slot.magazine_rounds);
  writer.WriteBool(slot.magazine_inserted);
  writer.WriteBool(slot.chambered);
  writer.WriteBool(slot.bolt_locked);
  writer.WriteBool(slot.empty_reload);
  writer.WriteBoundedU32(slot.reload_stage, 0, kMaxReloadStage);
  writer.WriteF32(slot.reload_elapsed);
  writer.WriteF32(slot.reload_duration);
  writer.WriteF32(slot.cycle_remaining);
  writer.WriteBoundedU32(slot.spare_count, 0, kMaxSpareMagazines);
  if (!writer.ok()) {
    return;
  }
  for (size_t i = slot.spare_count; i < slot.spare_magazines.size(); ++i) {
    if (slot.spare_magazines[i] != 0) {
      writer.Fail(WireError::kNonCanonical);
      return;
    }
  }
  for (size_t i = 0; i < slot.spare_count; ++i) {
    writer.WriteU8(slot.spare_magazines[i]);
  }
  writer.WriteBoundedI32(slot.incoming_magazine, kMinIncomingMagazine,
                         kMaxIncomingMagazine);
  writer.WriteBool(slot.pull_used);
  writer.WriteF32(slot.scope_magnification);
  writer.WriteF32(slot.scope_zero);
  writer.WriteU8(slot.spares_dropped);
}

WireWeaponSlot ReadWeaponSlot(BitReader& reader) {
  WireWeaponSlot slot;
  slot.present = reader.ReadBool();
  if (!slot.present) {
    return slot;
  }
  slot.weapon_key = reader.ReadU32();
  slot.fire_mode = static_cast<uint8_t>(reader.ReadBoundedU32(0, kMaxFireMode));
  slot.magazine_rounds = reader.ReadU8();
  slot.magazine_inserted = reader.ReadBool();
  slot.chambered = reader.ReadBool();
  slot.bolt_locked = reader.ReadBool();
  slot.empty_reload = reader.ReadBool();
  slot.reload_stage =
      static_cast<uint8_t>(reader.ReadBoundedU32(0, kMaxReloadStage));
  slot.reload_elapsed = reader.ReadF32();
  slot.reload_duration = reader.ReadF32();
  slot.cycle_remaining = reader.ReadF32();
  slot.spare_count =
      static_cast<uint8_t>(reader.ReadBoundedU32(0, kMaxSpareMagazines));
  for (size_t i = 0; i < slot.spare_count; ++i) {
    slot.spare_magazines[i] = reader.ReadU8();
  }
  slot.incoming_magazine = static_cast<int16_t>(
      reader.ReadBoundedI32(kMinIncomingMagazine, kMaxIncomingMagazine));
  slot.pull_used = reader.ReadBool();
  slot.scope_magnification = reader.ReadF32();
  slot.scope_zero = reader.ReadF32();
  slot.spares_dropped = reader.ReadU8();
  return slot;
}

// --- Remote players ----------------------------------------------------------

void WritePositionGroup(const RemotePlayerQ& p, BitWriter& writer) {
  for (const int32_t c : p.position) {
    writer.WriteBoundedI32(c, kPositionMin, kPositionMax);
  }
}

void WriteVelocityGroup(const RemotePlayerQ& p, BitWriter& writer) {
  for (const int32_t c : p.velocity) {
    writer.WriteBoundedI32(c, kVelocityMin, kVelocityMax);
  }
}

void ReadVelocityGroup(BitReader& reader, RemotePlayerQ& p) {
  for (int32_t& c : p.velocity) {
    c = reader.ReadBoundedI32(kVelocityMin, kVelocityMax);
  }
}

void WriteOrientationGroup(const RemotePlayerQ& p, BitWriter& writer) {
  const int32_t limit = PitchLimit(kCoarseAngleBits);
  writer.WriteU16(p.yaw);
  writer.WriteBoundedI32(p.pitch, -limit, limit);
}

void ReadOrientationGroup(BitReader& reader, RemotePlayerQ& p) {
  const int32_t limit = PitchLimit(kCoarseAngleBits);
  p.yaw = reader.ReadU16();
  p.pitch = static_cast<int16_t>(reader.ReadBoundedI32(-limit, limit));
}

void WriteStatusGroup(const RemotePlayerQ& p, BitWriter& writer) {
  writer.WriteBits(p.flags, player_flags::kBits);
  writer.WriteI16(p.health);
}

void ReadStatusGroup(BitReader& reader, RemotePlayerQ& p) {
  p.flags = static_cast<uint8_t>(reader.ReadBits(player_flags::kBits));
  p.health = reader.ReadI16();
}

void WriteFullPlayer(const RemotePlayerQ& p, BitWriter& writer) {
  WritePositionGroup(p, writer);
  WriteVelocityGroup(p, writer);
  WriteOrientationGroup(p, writer);
  WriteStatusGroup(p, writer);
  writer.WriteU32(p.weapon_key);
}

RemotePlayerQ ReadFullPlayer(BitReader& reader) {
  RemotePlayerQ p;
  for (int32_t& c : p.position) {
    c = reader.ReadBoundedI32(kPositionMin, kPositionMax);
  }
  ReadVelocityGroup(reader, p);
  ReadOrientationGroup(reader, p);
  ReadStatusGroup(reader, p);
  p.weapon_key = reader.ReadU32();
  return p;
}

void WriteDeltaPlayer(const PlayerEntry& entry, BitWriter& writer) {
  writer.WriteBits(entry.groups, player_group::kCount);
  if (entry.groups == 0) {
    writer.Fail(WireError::kNonCanonical);
    return;
  }
  const RemotePlayerQ& v = entry.value;
  if ((entry.groups & player_group::kPosition) != 0) {
    for (const int32_t d : v.position) {
      writer.WriteVarInt(d);
    }
  }
  if ((entry.groups & player_group::kVelocity) != 0) {
    WriteVelocityGroup(v, writer);
  }
  if ((entry.groups & player_group::kOrientation) != 0) {
    WriteOrientationGroup(v, writer);
  }
  if ((entry.groups & player_group::kStatus) != 0) {
    WriteStatusGroup(v, writer);
  }
  if ((entry.groups & player_group::kWeapon) != 0) {
    writer.WriteU32(v.weapon_key);
  }
}

void ReadDeltaPlayer(BitReader& reader, PlayerEntry& entry) {
  entry.groups = static_cast<uint8_t>(reader.ReadBits(player_group::kCount));
  if (entry.groups == 0) {
    reader.Fail(WireError::kNonCanonical);
    return;
  }
  RemotePlayerQ& v = entry.value;
  if ((entry.groups & player_group::kPosition) != 0) {
    for (int32_t& d : v.position) {
      d = reader.ReadVarInt();
    }
  }
  if ((entry.groups & player_group::kVelocity) != 0) {
    ReadVelocityGroup(reader, v);
  }
  if ((entry.groups & player_group::kOrientation) != 0) {
    ReadOrientationGroup(reader, v);
  }
  if ((entry.groups & player_group::kStatus) != 0) {
    ReadStatusGroup(reader, v);
  }
  if ((entry.groups & player_group::kWeapon) != 0) {
    v.weapon_key = reader.ReadU32();
  }
}

PlayerEntry ReadPlayerEntry(BitReader& reader, bool has_baseline) {
  PlayerEntry entry;
  entry.id = static_cast<uint8_t>(reader.ReadBoundedU32(0, kMaxPlayerId));
  const uint32_t code = reader.ReadBits(kEntryKindBits);
  if (code > static_cast<uint32_t>(EntryKind::kRemoved)) {
    reader.Fail(WireError::kOutOfRange);
    return entry;
  }
  entry.kind = static_cast<EntryKind>(code);
  if (!has_baseline && entry.kind != EntryKind::kFull) {
    reader.Fail(WireError::kNonCanonical);
    return entry;
  }
  switch (entry.kind) {
    case EntryKind::kRemoved:
      break;
    case EntryKind::kFull:
      entry.value = ReadFullPlayer(reader);
      break;
    case EntryKind::kDelta:
      ReadDeltaPlayer(reader, entry);
      break;
  }
  return entry;
}

}  // namespace

void WritePlayerState(const WirePlayerState& state, BitWriter& writer) {
  writer.WriteVec3(state.position);
  writer.WriteVec3(state.velocity);
  writer.WriteF32(state.yaw);
  writer.WriteF32(state.pitch);
  writer.WriteF32(state.eye_height);
  writer.WriteBits(state.flags, player_flags::kBits);
  writer.WriteU8(state.jump_age);
  writer.WriteU8(state.jump_cooldown);
  writer.WriteI16(state.health);
  writer.WriteU16(state.respawn_ticks);
  writer.WriteU32(state.shots_fired);
  writer.WriteBoundedI32(state.active_slot, -1, kMaxSlotIndex);
  writer.WriteBoundedI32(state.selected_slot, -1, kMaxSlotIndex);
  writer.WriteF32(state.raise);
  writer.WriteF32(state.jump_speed);
  writer.WriteU32(state.deaths);
  writer.WriteBool(state.wait_for_release);
  for (const WireWeaponSlot& slot : state.slots) {
    WriteWeaponSlot(slot, writer);
  }
}

WirePlayerState ReadPlayerState(BitReader& reader) {
  WirePlayerState state;
  state.position = reader.ReadVec3();
  state.velocity = reader.ReadVec3();
  state.yaw = reader.ReadF32();
  state.pitch = reader.ReadF32();
  state.eye_height = reader.ReadF32();
  state.flags = static_cast<uint8_t>(reader.ReadBits(player_flags::kBits));
  state.jump_age = reader.ReadU8();
  state.jump_cooldown = reader.ReadU8();
  state.health = reader.ReadI16();
  state.respawn_ticks = reader.ReadU16();
  state.shots_fired = reader.ReadU32();
  state.active_slot =
      static_cast<int8_t>(reader.ReadBoundedI32(-1, kMaxSlotIndex));
  state.selected_slot =
      static_cast<int8_t>(reader.ReadBoundedI32(-1, kMaxSlotIndex));
  state.raise = reader.ReadF32();
  state.jump_speed = reader.ReadF32();
  state.deaths = reader.ReadU32();
  state.wait_for_release = reader.ReadBool();
  for (WireWeaponSlot& slot : state.slots) {
    slot = ReadWeaponSlot(reader);
  }
  return state;
}

RemotePlayerQ Quantize(const RemotePlayer& player) {
  RemotePlayerQ q;
  for (int axis = 0; axis < 3; ++axis) {
    q.position[axis] = QuantizePosition(player.position[axis]);
    q.velocity[axis] = QuantizeVelocity(player.velocity[axis]);
  }
  // Both fit by construction: yaw < 2^16, |pitch| <= 2^14.
  q.yaw = static_cast<uint16_t>(QuantizeYaw(player.yaw, kCoarseAngleBits));
  q.pitch = static_cast<int16_t>(QuantizePitch(player.pitch, kCoarseAngleBits));
  q.flags = player.flags & kFlagsMask;
  q.health = player.health;
  q.weapon_key = player.weapon_key;
  return q;
}

RemotePlayer Dequantize(const RemotePlayerQ& player) {
  RemotePlayer p;
  for (int axis = 0; axis < 3; ++axis) {
    p.position[axis] = DequantizePosition(player.position[axis]);
    p.velocity[axis] = DequantizeVelocity(player.velocity[axis]);
  }
  p.yaw = DequantizeYaw(player.yaw, kCoarseAngleBits);
  p.pitch = DequantizePitch(player.pitch, kCoarseAngleBits);
  p.flags = player.flags;
  p.health = player.health;
  p.weapon_key = player.weapon_key;
  return p;
}

// --- PlayerSet ---------------------------------------------------------------

const RemotePlayerQ* PlayerSet::Find(uint8_t id) const {
  if (id >= kMaxPlayers || (present_ & (uint64_t{1} << id)) == 0) {
    return nullptr;
  }
  return &players_[id];
}

WireResult<void> PlayerSet::Set(uint8_t id, const RemotePlayerQ& player) {
  if (id >= kMaxPlayers) {
    return WireResult<void>::Error(WireError::kOutOfRange);
  }
  players_[id] = player;
  present_ |= uint64_t{1} << id;
  return WireResult<void>::Ok();
}

void PlayerSet::Remove(uint8_t id) {
  if (id >= kMaxPlayers) {
    return;
  }
  players_[id] = RemotePlayerQ{};
  present_ &= ~(uint64_t{1} << id);
}

int PlayerSet::size() const { return std::popcount(present_); }

bool operator==(const PlayerSet& a, const PlayerSet& b) {
  if (a.present_ != b.present_) {
    return false;
  }
  for (int id = 0; id < kMaxPlayers; ++id) {
    if ((a.present_ & (uint64_t{1} << id)) != 0 &&
        !(a.players_[id] == b.players_[id])) {
      return false;
    }
  }
  return true;
}

// --- Entries -----------------------------------------------------------------

std::optional<PlayerEntry> DiffPlayer(uint8_t id, const RemotePlayerQ* current,
                                      const RemotePlayerQ* base) {
  if (current == nullptr && base == nullptr) {
    return std::nullopt;
  }
  PlayerEntry entry;
  entry.id = id;
  if (current == nullptr) {
    entry.kind = EntryKind::kRemoved;
    return entry;
  }
  if (base == nullptr) {
    entry.kind = EntryKind::kFull;
    entry.value = *current;
    return entry;
  }
  const RemotePlayerQ& c = *current;
  const RemotePlayerQ& b = *base;
  uint8_t groups = 0;
  if (c.position != b.position) {
    groups |= player_group::kPosition;
  }
  if (c.velocity != b.velocity) {
    groups |= player_group::kVelocity;
  }
  if (c.yaw != b.yaw || c.pitch != b.pitch) {
    groups |= player_group::kOrientation;
  }
  if (c.flags != b.flags || c.health != b.health) {
    groups |= player_group::kStatus;
  }
  if (c.weapon_key != b.weapon_key) {
    groups |= player_group::kWeapon;
  }
  if (groups == 0) {
    return std::nullopt;
  }
  entry.kind = EntryKind::kDelta;
  entry.groups = groups;
  entry.value = c;
  for (int axis = 0; axis < 3; ++axis) {
    // Wrapping difference, as the Rust encoder computes it.
    entry.value.position[axis] =
        static_cast<int32_t>(static_cast<uint32_t>(c.position[axis]) -
                             static_cast<uint32_t>(b.position[axis]));
  }
  return entry;
}

void WritePlayerEntry(const PlayerEntry& entry, bool has_baseline,
                      BitWriter& writer) {
  writer.WriteBoundedU32(entry.id, 0, kMaxPlayerId);
  writer.WriteBits(static_cast<uint32_t>(entry.kind), kEntryKindBits);
  if (!has_baseline && entry.kind != EntryKind::kFull) {
    writer.Fail(WireError::kNonCanonical);
    return;
  }
  switch (entry.kind) {
    case EntryKind::kRemoved:
      break;
    case EntryKind::kFull:
      WriteFullPlayer(entry.value, writer);
      break;
    case EntryKind::kDelta:
      WriteDeltaPlayer(entry, writer);
      break;
  }
}

WireResult<void> ApplyPlayerEntry(const PlayerEntry& entry, PlayerSet& set) {
  switch (entry.kind) {
    case EntryKind::kFull:
      return set.Set(entry.id, entry.value);
    case EntryKind::kRemoved:
      if (set.Find(entry.id) == nullptr) {
        return WireResult<void>::Error(WireError::kInconsistent);
      }
      set.Remove(entry.id);
      return WireResult<void>::Ok();
    case EntryKind::kDelta:
      break;
  }
  const RemotePlayerQ* found = set.Find(entry.id);
  if (found == nullptr) {
    return WireResult<void>::Error(WireError::kInconsistent);
  }
  RemotePlayerQ p = *found;
  const RemotePlayerQ& v = entry.value;
  if ((entry.groups & player_group::kPosition) != 0) {
    for (int axis = 0; axis < 3; ++axis) {
      const int64_t sum = int64_t{p.position[axis]} + v.position[axis];
      if (sum < kPositionMin || sum > kPositionMax) {
        return WireResult<void>::Error(WireError::kInconsistent);
      }
      p.position[axis] = static_cast<int32_t>(sum);
    }
  }
  if ((entry.groups & player_group::kVelocity) != 0) {
    p.velocity = v.velocity;
  }
  if ((entry.groups & player_group::kOrientation) != 0) {
    p.yaw = v.yaw;
    p.pitch = v.pitch;
  }
  if ((entry.groups & player_group::kStatus) != 0) {
    p.flags = v.flags;
    p.health = v.health;
  }
  if ((entry.groups & player_group::kWeapon) != 0) {
    p.weapon_key = v.weapon_key;
  }
  return set.Set(entry.id, p);
}

// --- PlayerUpdates -----------------------------------------------------------

WireResult<void> PlayerUpdates::Apply(const PlayerSet* baseline,
                                      PlayerSet& out) const {
  if (has_baseline_) {
    if (baseline == nullptr) {
      return WireResult<void>::Error(WireError::kMissingBaseline);
    }
    out = *baseline;
  } else {
    out = PlayerSet();
  }
  for (const PlayerEntry& entry : entries()) {
    if (WireResult<void> applied = ApplyPlayerEntry(entry, out);
        !applied.ok()) {
      return applied;
    }
  }
  return WireResult<void>::Ok();
}

void ReadPlayerUpdates(BitReader& reader, bool has_baseline,
                       PlayerUpdates& out) {
  out.size_ = 0;
  out.has_baseline_ = has_baseline;
  uint64_t seen = 0;
  while (reader.ReadBool()) {
    if (out.size_ >= out.entries_.size()) {
      reader.Fail(WireError::kNonCanonical);
      return;
    }
    const PlayerEntry entry = ReadPlayerEntry(reader, has_baseline);
    if (!reader.ok()) {
      return;
    }
    const uint64_t bit = uint64_t{1} << entry.id;
    if ((seen & bit) != 0) {
      reader.Fail(WireError::kNonCanonical);
      return;
    }
    seen |= bit;
    out.entries_[out.size_] = entry;
    ++out.size_;
  }
}

// --- BaselineHistory ---------------------------------------------------------

BaselineHistory::BaselineHistory() : slots_(kBaselineHistory) {}

void BaselineHistory::Store(uint32_t tick, const PlayerSet& set) {
  Slot& slot = slots_[tick % kHistorySlots];
  slot.tick = tick;
  slot.valid = true;
  slot.set = set;
}

const PlayerSet* BaselineHistory::Find(uint32_t tick) const {
  const Slot& slot = slots_[tick % kHistorySlots];
  return slot.valid && slot.tick == tick ? &slot.set : nullptr;
}

void BaselineHistory::Clear() {
  for (Slot& slot : slots_) {
    slot.valid = false;
  }
}

}  // namespace blue::net
