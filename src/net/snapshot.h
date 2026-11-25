#ifndef BLUE_NET_SNAPSHOT_H_
#define BLUE_NET_SNAPSHOT_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "net/bit_stream.h"
#include "net/players.h"
#include "net/protocol.h"
#include "net/reliable_channel.h"
#include "net/sequence.h"
#include "net/wire_error.h"
#include "net/world_object.h"

namespace blue::net {

// ServerSnapshot packets (kind 1), section 9 of docs/netcode/protocol.md.
// The client decodes them with DecodeServerSnapshot(); SnapshotWriter is the
// matching encoder (used by tests and by any C++ server).

// Input buffer depth range of SnapshotInfo.
inline constexpr int8_t kInputDepthMin = -32;
inline constexpr int8_t kInputDepthMax = 31;

// Per-snapshot timing.
struct SnapshotInfo {
  // The tick this snapshot describes (state at the end of it).
  uint32_t server_tick = 0;
  // Newest command tick the server consumed for this client.
  std::optional<uint32_t> last_command_tick;
  // Commands buffered beyond the one consumed this tick; negative when the
  // client's commands arrive late. -32..31.
  int8_t input_buffer_depth = 0;
  // Tick the players section is delta-encoded against (1..255 ticks before
  // server_tick), nullopt for a full section.
  std::optional<uint32_t> baseline_tick;

  friend bool operator==(const SnapshotInfo&, const SnapshotInfo&) = default;
};

struct ServerSnapshot;

// The world objects of one snapshot, in wire order (a later entry for the
// same id wins). Fixed capacity.
class ObjectList {
 public:
  ObjectList() = default;
  ObjectList(const ObjectList&) = default;
  ObjectList& operator=(const ObjectList&) = default;

  std::span<const WorldObject> objects() const {
    return std::span<const WorldObject>(objects_.data(), size_);
  }
  size_t size() const { return size_; }

 private:
  friend WireResult<void> DecodeServerSnapshot(std::span<const uint8_t> payload,
                                               ServerSnapshot& out);

  std::array<WorldObject, kMaxObjectsPerPacket> objects_ = {};
  size_t size_ = 0;
};

// A decoded server -> client packet (about 14 KB: keep one and reuse it).
struct ServerSnapshot {
  PacketHeader header;
  SnapshotInfo info;
  // The receiving client's own player, full precision.
  std::optional<WirePlayerState> own;
  ReliableSection reliable;
  // Other players; apply to the baseline named by info.baseline_tick.
  PlayerUpdates players;
  ObjectList objects;
};

// Decodes `payload` into `out` (no allocation). The whole packet is
// validated; the players section is reconstructed separately with
// out.players.Apply(), which reports kMissingBaseline without invalidating
// the rest. kUsage if the payload is another packet kind. `out` is
// unspecified on error.
WireResult<void> DecodeServerSnapshot(std::span<const uint8_t> payload,
                                      ServerSnapshot& out);

// Builds a ServerSnapshot section by section, never exceeding
// kMaxPayloadBytes (or the buffer, if smaller): Begin() (header, info, own
// player, reliable section), WritePlayers() once, any number of
// TryAddObject(), then Finish(). Two bits are always reserved for the
// section terminators. Borrows the buffer, which must outlive the writer.
class SnapshotWriter {
 public:
  // Writes everything up to the players section. kOverflow if that alone
  // does not fit; field errors.
  static WireResult<SnapshotWriter> Begin(std::span<uint8_t> buffer,
                                          const PacketHeader& header,
                                          const SnapshotInfo& info,
                                          const WirePlayerState* own,
                                          const ReliableSection& reliable);

  SnapshotWriter(SnapshotWriter&&) = default;
  SnapshotWriter& operator=(SnapshotWriter&&) = default;

  // Writes the players section: removals first, then entries for the ids in
  // `priority` order (ids not listed are not sent), stopping short of the
  // last `reserve_bytes` of the packet; entries that do not fit are skipped.
  // `record` receives the set the client will reconstruct (baseline plus
  // the written entries). `baseline` must be non-null exactly when
  // info.baseline_tick was set. kUsage when called twice or with a
  // contradicting baseline; field errors.
  WireResult<void> WritePlayers(const PlayerSet& current,
                                const PlayerSet* baseline,
                                std::span<const uint8_t> priority,
                                size_t reserve_bytes, PlayerSet& record);

  // Whole bytes still free for objects.
  size_t bytes_left() const { return writer_.bits_left() / 8; }
  int object_count() const { return object_count_; }

  // Appends one object if it fits; returns false (writing nothing) when it
  // does not or 96 objects were written. kUsage before WritePlayers().
  WireResult<bool> TryAddObject(const WorldObject& object);

  // Ends the packet and returns its length in bytes. kUsage before
  // WritePlayers().
  WireResult<size_t> Finish();

 private:
  enum class Stage : uint8_t { kPlayers, kObjects, kFinished };

  SnapshotWriter(const BitWriter& writer, size_t capacity_bits,
                 bool has_baseline);

  // Writes the entry for `id` if it changed and fits; marks `id` done.
  WireResult<void> TryPlayer(uint8_t id, const PlayerSet& current,
                             const PlayerSet* baseline, PlayerSet& record,
                             uint64_t& done);

  BitWriter writer_;
  size_t capacity_bits_ = 0;
  bool has_baseline_ = false;
  Stage stage_ = Stage::kPlayers;
  int object_count_ = 0;
};

}  // namespace blue::net

#endif  // BLUE_NET_SNAPSHOT_H_
