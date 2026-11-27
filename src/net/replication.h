#ifndef BLUE_NET_REPLICATION_H_
#define BLUE_NET_REPLICATION_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include "net/interpolation.h"
#include "net/messages.h"
#include "net/players.h"
#include "net/protocol.h"
#include "net/world_object.h"

namespace blue::net {

// Reliable messages applied since the last Reset(), by kind.
struct MessageCounts {
  int64_t joined = 0;
  int64_t left = 0;
  int64_t damage = 0;
  int64_t deaths = 0;
  int64_t respawns = 0;
  int64_t shots = 0;  // everyone's, the own player's included
  int64_t impacts = 0;
  int64_t drops = 0;            // WeaponDropped
  int64_t dropped_taken = 0;    // DroppedTaken
  int64_t dropped_removed = 0;  // DroppedRemoved (despawned)
  int64_t pickups_taken = 0;    // PickupTaken
  int64_t world_sync = 0;       // the WorldSync* messages
};

// What the client knows of every player, the own one included.
struct PlayerInfo {
  bool joined = false;  // PlayerJoined seen and no PlayerLeft since
  Name name;
  int16_t health = 0;
  int deaths = 0;
  int kills = 0;
  // Remote players only: snapshot states, and the state at the render time
  // (valid when `has_pose`).
  InterpolationBuffer motion;
  bool has_pose = false;
  InterpSample pose;
};

// A dropped weapon the server announced and nobody has taken.
struct ReplicatedDroppedWeapon {
  uint32_t dropped_id = 0;
  uint32_t weapon_key = 0;
  uint8_t rounds_in_magazine = 0;
  // Snapshot states (ObjectKind::kDroppedWeapon) and the pose at the
  // render time; the announced pose until a snapshot carries it.
  InterpolationBuffer motion;
  glm::vec3 position{0.0f};
  glm::quat rotation = glm::quat::wxyz(1.0f, 0.0f, 0.0f, 0.0f);
};

// A scene body the server moves (ObjectKind::kBody, by scene instance id).
struct ReplicatedBody {
  uint32_t instance_id = 0;
  InterpolationBuffer motion;
  bool has_pose = false;
  glm::vec3 position{0.0f};
  glm::quat rotation = glm::quat::wxyz(1.0f, 0.0f, 0.0f, 0.0f);
};

// A shot another player fired, for a tracer.
struct ShotRecord {
  uint8_t shooter = 0;
  glm::vec3 origin{0.0f};
  glm::vec3 direction{0.0f};
  uint32_t weapon_key = 0;
  double time_seconds = 0.0;  // When it was received.
};

// A bullet hitting the world, for an impact mark.
struct ImpactRecord {
  glm::vec3 point{0.0f};
  glm::vec3 normal{0.0f};
  double time_seconds = 0.0;
};

// One kill feed line.
struct KillRecord {
  std::optional<uint8_t> killer;  // nullopt: the world
  uint8_t victim = 0;
  Name killer_name;
  Name victim_name;
  uint32_t weapon_key = 0;
  double time_seconds = 0.0;
};

// One death, for the victim's ragdoll (the kill feed keeps names, this keeps
// where and how the victim was hit).
struct DeathRecord {
  uint8_t victim = 0;
  std::optional<uint8_t> killer;  // nullopt: the world
  uint32_t weapon_key = 0;
  uint8_t zone = hit_zone::kOther;  // Zone of the killing Damage.
  bool has_hit = false;
  glm::vec3 hit_offset{0.0f};    // Hit point minus the victim's feet, m.
  glm::vec3 hit_velocity{0.0f};  // Bullet velocity at impact, m/s.
  double time_seconds = 0.0;     // When it was received.
};

// A fixed ring of the most recent records of one kind, numbered from 0 in
// arrival order: a reader keeps the number of the next record it wants and
// reads Get(n) for n in [first(), end()).
template <typename T, size_t N>
class RecordRing {
 public:
  void Push(const T& record) {
    records_[static_cast<size_t>(end_ % N)] = record;
    ++end_;
  }
  void Clear() { end_ = 0; }
  // The number of the oldest record still kept.
  [[nodiscard]] uint64_t first() const { return end_ > N ? end_ - N : 0; }
  // One past the number of the newest record.
  [[nodiscard]] uint64_t end() const { return end_; }
  // Record `n`, which must lie in [first(), end()).
  [[nodiscard]] const T& Get(uint64_t n) const {
    return records_[static_cast<size_t>(n % N)];
  }

 private:
  std::array<T, N> records_{};
  uint64_t end_ = 0;
};

// The replicated game state a networked client reads every frame: players
// (names, health, kills and deaths, remote poses), dropped weapons, taken
// scene pick-ups, server-moved scene bodies, recent shots and impacts, and
// the kill feed. Fed by ClientSession from reliable messages and snapshots;
// Sample() moves every pose to the render time.
//
// Reserves its storage at construction; dropped weapons and bodies grow
// their vectors only when a new one appears. Not thread-safe.
class Replication {
 public:
  static constexpr size_t kShotHistory = 64;
  static constexpr size_t kImpactHistory = 64;
  static constexpr size_t kKillFeed = 8;
  static constexpr size_t kDeathHistory = 16;

  Replication();
  Replication(const Replication&) = delete;
  Replication& operator=(const Replication&) = delete;

  // Forgets everything (a new connection). `own_id` is the local player,
  // whose motion is predicted rather than interpolated.
  void Reset(std::optional<uint8_t> own_id);

  // Applies one reliable message received at `now_seconds`.
  void Apply(const Message& message, double now_seconds);
  // Applies the reconstructed remote player set of snapshot `server_tick`:
  // listed players get a sample, unlisted ones lose their buffered motion.
  void ApplyPlayers(uint32_t server_tick, const PlayerSet& players);
  // Applies the world objects of snapshot `server_tick`.
  void ApplyObjects(uint32_t server_tick, std::span<const WorldObject> objects);
  // Moves every remote pose to `time` (InterpolationBuffer::Sample).
  void Sample(ViewTime time, double max_extrapolation_ticks);

  [[nodiscard]] std::optional<uint8_t> own_id() const { return own_id_; }
  [[nodiscard]] const MessageCounts& message_counts() const {
    return message_counts_;
  }
  // Bit i set: player i appeared in a snapshot since the last Reset().
  [[nodiscard]] uint64_t players_seen() const { return players_seen_; }
  // Player `id` (0..63).
  [[nodiscard]] const PlayerInfo& player(uint8_t id) const {
    return players_[id];
  }
  [[nodiscard]] std::span<const ReplicatedDroppedWeapon> dropped_weapons()
      const {
    return dropped_;
  }
  [[nodiscard]] std::span<const ReplicatedBody> bodies() const {
    return bodies_;
  }
  // Scene pick-up instances taken (in any order).
  [[nodiscard]] std::span<const uint32_t> taken_pickups() const {
    return taken_pickups_;
  }
  // The late-join sync is complete (WorldSyncDone).
  [[nodiscard]] bool world_synced() const { return world_synced_; }
  [[nodiscard]] const RecordRing<ShotRecord, kShotHistory>& shots() const {
    return shots_;
  }
  [[nodiscard]] const RecordRing<ImpactRecord, kImpactHistory>& impacts()
      const {
    return impacts_;
  }
  [[nodiscard]] const RecordRing<KillRecord, kKillFeed>& kills() const {
    return kills_;
  }
  // Recent deaths with their hit data, oldest first by number.
  [[nodiscard]] const RecordRing<DeathRecord, kDeathHistory>& deaths() const {
    return deaths_;
  }

 private:
  // Counts `message` in message_counts_.
  void Count(const Message& message);
  void AddDropped(const DroppedWeapon& weapon);
  void RemoveDropped(uint32_t dropped_id);
  void AddTakenPickup(uint32_t instance_id);
  [[nodiscard]] PlayerInfo* Find(uint8_t id);

  std::optional<uint8_t> own_id_;
  MessageCounts message_counts_;
  uint64_t players_seen_ = 0;
  std::array<PlayerInfo, kMaxPlayers> players_{};
  std::vector<ReplicatedDroppedWeapon> dropped_;
  std::vector<ReplicatedBody> bodies_;
  std::vector<uint32_t> taken_pickups_;
  bool world_synced_ = false;
  RecordRing<ShotRecord, kShotHistory> shots_;
  RecordRing<ImpactRecord, kImpactHistory> impacts_;
  RecordRing<KillRecord, kKillFeed> kills_;
  RecordRing<DeathRecord, kDeathHistory> deaths_;
};

}  // namespace blue::net

#endif  // BLUE_NET_REPLICATION_H_
