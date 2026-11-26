#ifndef BLUE_NET_PLAYERS_H_
#define BLUE_NET_PLAYERS_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <glm/vec3.hpp>

#include "net/bit_stream.h"
#include "net/protocol.h"
#include "net/wire_error.h"

namespace blue::net {

// Player state on the wire, sections 9.2 to 9.4 of docs/netcode/protocol.md:
// the receiving client's own player at full precision (WirePlayerState) and
// remote players quantised (RemotePlayerQ) and delta-encoded against a
// baseline set the client keeps (BaselineHistory).

// Bits of WirePlayerState::flags and RemotePlayer::flags (6 on the wire).
namespace player_flags {
inline constexpr uint8_t kCrouching = 1 << 0;
inline constexpr uint8_t kSprinting = 1 << 1;
inline constexpr uint8_t kAiming = 1 << 2;
inline constexpr uint8_t kAlive = 1 << 3;
inline constexpr uint8_t kOnGround = 1 << 4;
inline constexpr uint8_t kJumpPending = 1 << 5;
inline constexpr int kBits = 6;
}  // namespace player_flags

inline constexpr int kWeaponSlots = 2;
inline constexpr int kMaxSpareMagazines = 8;
inline constexpr int8_t kMaxSlotIndex = 7;
inline constexpr uint8_t kMaxFireMode = 15;
inline constexpr uint8_t kMaxReloadStage = 7;
inline constexpr int32_t kMinIncomingMagazine = -1;
inline constexpr int32_t kMaxIncomingMagazine = 255;

// One weapon slot of the own player's loadout (provisional layout,
// mirroring weapon::LoadoutSlotState and weapon::FirearmState). When
// `present` is false nothing else is sent and every other field must be (and
// decodes as) zero; spare entries past `spare_count` likewise.
struct WireWeaponSlot {
  bool present = false;
  uint32_t weapon_key = 0;
  uint8_t fire_mode = 0;  // 0..15
  uint8_t magazine_rounds = 0;
  bool magazine_inserted = false;
  bool chambered = false;
  bool bolt_locked = false;
  bool empty_reload = false;
  uint8_t reload_stage = 0;      // 0..7
  float reload_elapsed = 0.0f;   // s
  float reload_duration = 0.0f;  // s, 0 when not reloading
  float cycle_remaining = 0.0f;  // s
  uint8_t spare_count = 0;       // 0..8
  std::array<uint8_t, kMaxSpareMagazines> spare_magazines = {};
  // Rounds of the magazine being seated, -1..255 (-1 none). 0 when absent.
  int16_t incoming_magazine = 0;
  bool pull_used = false;  // The current trigger pull fired or clicked.
  float scope_magnification = 0.0f;
  float scope_zero = 0.0f;  // m
  // Spares carried beyond the kMaxSpareMagazines sent (0 for stock weapons).
  uint8_t spares_dropped = 0;

  friend bool operator==(const WireWeaponSlot&,
                         const WireWeaponSlot&) = default;
};

// The own player's state at the end of a server tick, full precision, used
// for reconciliation (provisional layout mirroring sim::PlayerState field by
// field; net/state_mapping.h converts). Floats must be finite.
struct WirePlayerState {
  glm::vec3 position = glm::vec3(0.0f);  // Feet, m.
  glm::vec3 velocity = glm::vec3(0.0f);  // m/s.
  float yaw = 0.0f;
  float pitch = 0.0f;
  float eye_height = 0.0f;
  uint8_t flags = 0;          // player_flags
  uint8_t jump_age = 0;       // Ticks.
  uint8_t jump_cooldown = 0;  // Ticks.
  int16_t health = 0;
  uint16_t respawn_ticks = 0;
  uint32_t shots_fired = 0;
  int8_t active_slot = -1;    // -1..7, -1 empty-handed.
  int8_t selected_slot = -1;  // -1..7, -1 none.
  // The loadout's switch progress, a linear fraction in [0, 1].
  float raise = 0.0f;
  float jump_speed = 0.0f;  // m/s of the pending (or last) jump.
  uint32_t deaths = 0;      // Picks the respawn point.
  // The trigger must be released after a weapon switch.
  bool wait_for_release = false;
  std::array<WireWeaponSlot, kWeaponSlots> slots = {};

  friend bool operator==(const WirePlayerState&,
                         const WirePlayerState&) = default;
};

// Writes every field in spec order. Non-finite floats and out-of-range
// fields record kOutOfRange, data in an absent slot or past spare_count
// kNonCanonical.
void WritePlayerState(const WirePlayerState& state, BitWriter& writer);
WirePlayerState ReadPlayerState(BitReader& reader);

// A remote player in simulation units.
struct RemotePlayer {
  glm::vec3 position = glm::vec3(0.0f);  // Feet, m.
  glm::vec3 velocity = glm::vec3(0.0f);  // m/s.
  float yaw = 0.0f;
  float pitch = 0.0f;
  uint8_t flags = 0;  // player_flags
  int16_t health = 0;
  uint32_t weapon_key = 0;  // In hand; 0 for none.

  friend bool operator==(const RemotePlayer&, const RemotePlayer&) = default;
};

// A remote player in wire units: position 1/512 m, velocity 1/64 m/s,
// yaw 0..65535 and pitch -16384..16384 (1/65536 turn).
struct RemotePlayerQ {
  std::array<int32_t, 3> position = {};
  std::array<int32_t, 3> velocity = {};
  uint16_t yaw = 0;
  int16_t pitch = 0;
  uint8_t flags = 0;
  int16_t health = 0;
  uint32_t weapon_key = 0;

  friend bool operator==(const RemotePlayerQ&, const RemotePlayerQ&) = default;
};

// Converts to wire units (total: NaN gives 0, values clamp).
RemotePlayerQ Quantize(const RemotePlayer& player);
// Converts back to simulation units.
RemotePlayer Dequantize(const RemotePlayerQ& player);

// The remote players of one snapshot, indexed by player id (0..63). Fixed
// size (about 2.5 KB), no allocation.
class PlayerSet {
 public:
  PlayerSet() = default;
  PlayerSet(const PlayerSet&) = default;
  PlayerSet& operator=(const PlayerSet&) = default;

  // Returns the player with `id`, or null if absent (or id > 63).
  const RemotePlayerQ* Find(uint8_t id) const;
  // Sets player `id`. kOutOfRange for id > 63.
  WireResult<void> Set(uint8_t id, const RemotePlayerQ& player);
  // Removes player `id` (no effect if absent or out of range).
  void Remove(uint8_t id);
  // Number of present players.
  int size() const;
  bool empty() const { return present_ == 0; }
  // Bit i set: player i is present.
  uint64_t present_mask() const { return present_; }

  // Sets are equal when the same ids are present with equal states.
  friend bool operator==(const PlayerSet& a, const PlayerSet& b);

 private:
  std::array<RemotePlayerQ, kMaxPlayers> players_ = {};
  uint64_t present_ = 0;
};

// How a players-section entry changes the baseline (2 bits on the wire;
// 3 is rejected).
enum class EntryKind : uint8_t {
  kDelta = 0,    // Changed field groups of a baseline player.
  kFull = 1,     // Complete state (new player, or no baseline).
  kRemoved = 2,  // The baseline player is gone.
};

// Field groups of a delta entry (bit g = group g changed).
namespace player_group {
inline constexpr uint8_t kPosition = 1 << 0;
inline constexpr uint8_t kVelocity = 1 << 1;
inline constexpr uint8_t kOrientation = 1 << 2;
inline constexpr uint8_t kStatus = 1 << 3;
inline constexpr uint8_t kWeapon = 1 << 4;
inline constexpr int kCount = 5;
}  // namespace player_group

// One decoded players-section entry.
struct PlayerEntry {
  uint8_t id = 0;
  EntryKind kind = EntryKind::kDelta;
  uint8_t groups = 0;  // Delta only: player_group bits, non-zero.
  // Full: the state. Delta: `position` holds per-axis differences, the
  // other groups hold new values where their bit is set.
  RemotePlayerQ value;

  friend bool operator==(const PlayerEntry&, const PlayerEntry&) = default;
};

// Returns the entry that turns `base` into `current` for player `id`, or
// nullopt when nothing changed (or both are absent).
std::optional<PlayerEntry> DiffPlayer(uint8_t id, const RemotePlayerQ* current,
                                      const RemotePlayerQ* base);
// Writes one entry (without its leading "more" bit). Delta and removed
// entries without a baseline record kNonCanonical.
void WritePlayerEntry(const PlayerEntry& entry, bool has_baseline,
                      BitWriter& writer);
// Applies an entry to `set` (which starts as a copy of the baseline).
// kInconsistent for a delta or removal of an absent player or a position
// leaving the wire range.
WireResult<void> ApplyPlayerEntry(const PlayerEntry& entry, PlayerSet& set);

// The decoded players section of a snapshot. Decoding never needs the
// baseline; Apply() does. Fixed capacity, no allocation.
class PlayerUpdates {
 public:
  PlayerUpdates() = default;
  PlayerUpdates(const PlayerUpdates&) = default;
  PlayerUpdates& operator=(const PlayerUpdates&) = default;

  // Entries in wire order.
  std::span<const PlayerEntry> entries() const {
    return std::span<const PlayerEntry>(entries_.data(), size_);
  }
  // True if the section was encoded against a baseline.
  bool has_baseline() const { return has_baseline_; }

  // Reconstructs the snapshot's player set into `out`: a copy of
  // `baseline` (empty without one) with the entries applied in order.
  // kMissingBaseline if the section has a baseline and `baseline` is null;
  // kInconsistent if an entry contradicts the baseline. `out` is
  // unspecified on error.
  WireResult<void> Apply(const PlayerSet* baseline, PlayerSet& out) const;

 private:
  friend void ReadPlayerUpdates(BitReader& reader, bool has_baseline,
                                PlayerUpdates& out);

  std::array<PlayerEntry, kMaxPlayers> entries_ = {};
  size_t size_ = 0;
  bool has_baseline_ = false;
};

// Reads a players section into `out` (overwriting it): entries each
// preceded by a 1 bit, ended by a 0 bit. More than 64 entries, duplicate
// ids, empty deltas and (without a baseline) non-full entries record
// kNonCanonical.
void ReadPlayerUpdates(BitReader& reader, bool has_baseline,
                       PlayerUpdates& out);

// The reconstructed player sets of the last kBaselineHistory snapshot ticks
// a client applied, for decoding later delta sections. Allocates its
// storage (about 160 KB) once, at construction. Not thread-safe.
class BaselineHistory {
 public:
  BaselineHistory();
  BaselineHistory(const BaselineHistory&) = default;
  BaselineHistory& operator=(const BaselineHistory&) = default;
  BaselineHistory(BaselineHistory&&) = default;
  BaselineHistory& operator=(BaselineHistory&&) = default;

  // Stores the set reconstructed for `tick`, replacing the entry of the
  // tick kBaselineHistory ticks older.
  void Store(uint32_t tick, const PlayerSet& set);
  // Returns the set stored for `tick`, or null if it is not (or no longer)
  // stored. The pointer is valid until the next Store() or Clear().
  const PlayerSet* Find(uint32_t tick) const;
  // Forgets every stored set (for a new connection).
  void Clear();

 private:
  struct Slot {
    uint32_t tick = 0;
    bool valid = false;
    PlayerSet set;
  };

  std::vector<Slot> slots_;
};

}  // namespace blue::net

#endif  // BLUE_NET_PLAYERS_H_
