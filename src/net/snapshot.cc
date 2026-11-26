#include "net/snapshot.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "net/bit_stream.h"
#include "net/packet.h"
#include "net/players.h"
#include "net/protocol.h"
#include "net/reliable_channel.h"
#include "net/sequence.h"
#include "net/wire_error.h"
#include "net/world_object.h"

namespace blue::net {
namespace {

// Bits always kept free for the players and objects section terminators.
constexpr size_t kTerminatorBits = 2;

size_t SaturatingSubtract(size_t a, size_t b) { return a > b ? a - b : 0; }

void WriteSnapshotInfo(const SnapshotInfo& info, BitWriter& writer) {
  writer.WriteU32(info.server_tick);
  writer.WriteOptionalU32(info.last_command_tick);
  writer.WriteBoundedI32(info.input_buffer_depth, kInputDepthMin,
                         kInputDepthMax);
  writer.WriteBool(info.baseline_tick.has_value());
  if (info.baseline_tick.has_value()) {
    writer.WriteBoundedU32(info.server_tick - *info.baseline_tick, 1,
                           kMaxBaselineAge);
  }
}

SnapshotInfo ReadSnapshotInfo(BitReader& reader) {
  SnapshotInfo info;
  info.server_tick = reader.ReadU32();
  info.last_command_tick = reader.ReadOptionalU32();
  info.input_buffer_depth = static_cast<int8_t>(
      reader.ReadBoundedI32(kInputDepthMin, kInputDepthMax));
  if (reader.ReadBool()) {
    const uint32_t age = reader.ReadBoundedU32(1, kMaxBaselineAge);
    info.baseline_tick = info.server_tick - age;
  }
  return info;
}

}  // namespace

WireResult<void> DecodeServerSnapshot(std::span<const uint8_t> payload,
                                      ServerSnapshot& out) {
  if (payload.size() > kMaxPayloadBytes) {
    return WireResult<void>::Error(WireError::kOutOfRange);
  }
  BitReader reader(payload);
  ReadExpectedKind(reader, PacketKind::kServerSnapshot);
  out.header = ReadPacketHeader(reader);
  out.info = ReadSnapshotInfo(reader);
  out.own.reset();
  if (reader.ReadBool()) {
    out.own = ReadPlayerState(reader);
  }
  ReadReliableSection(reader, out.reliable);
  ReadPlayerUpdates(reader, out.info.baseline_tick.has_value(), out.players);
  out.objects.size_ = 0;
  while (reader.ReadBool()) {
    const WorldObject object = ReadWorldObject(reader);
    if (!reader.ok()) {
      break;
    }
    if (out.objects.size_ >= out.objects.objects_.size()) {
      reader.Fail(WireError::kNonCanonical);
      break;
    }
    out.objects.objects_[out.objects.size_] = object;
    ++out.objects.size_;
  }
  return reader.Finish();
}

SnapshotWriter::SnapshotWriter(const BitWriter& writer, size_t capacity_bits,
                               bool has_baseline)
    : writer_(writer),
      capacity_bits_(capacity_bits),
      has_baseline_(has_baseline) {}

WireResult<SnapshotWriter> SnapshotWriter::Begin(
    std::span<uint8_t> buffer, const PacketHeader& header,
    const SnapshotInfo& info, const WirePlayerState* own,
    const ReliableSection& reliable) {
  const size_t capacity = PayloadCapacityBits(buffer);
  BitWriter writer(buffer);
  writer.set_limit(SaturatingSubtract(capacity, kTerminatorBits));
  writer.WriteBits(static_cast<uint32_t>(PacketKind::kServerSnapshot),
                   kPacketKindBits);
  WritePacketHeader(header, writer);
  WriteSnapshotInfo(info, writer);
  writer.WriteBool(own != nullptr);
  if (own != nullptr) {
    WritePlayerState(*own, writer);
  }
  WriteReliableSection(reliable, writer);
  if (!writer.ok()) {
    return WireResult<SnapshotWriter>::Error(*writer.error());
  }
  return WireResult<SnapshotWriter>::Ok(
      SnapshotWriter(writer, capacity, info.baseline_tick.has_value()));
}

WireResult<void> SnapshotWriter::WritePlayers(const PlayerSet& current,
                                              const PlayerSet* baseline,
                                              std::span<const uint8_t> priority,
                                              size_t reserve_bytes,
                                              PlayerSet& record) {
  if (stage_ != Stage::kPlayers || (baseline != nullptr) != has_baseline_) {
    return WireResult<void>::Error(WireError::kUsage);
  }
  record = baseline != nullptr ? *baseline : PlayerSet();
  writer_.set_limit(SaturatingSubtract(
      SaturatingSubtract(capacity_bits_, kTerminatorBits), reserve_bytes * 8));
  uint64_t done = 0;
  if (baseline != nullptr) {
    for (int id = 0; id < kMaxPlayers; ++id) {
      const auto player_id = static_cast<uint8_t>(id);
      if (baseline->Find(player_id) != nullptr &&
          current.Find(player_id) == nullptr) {
        if (WireResult<void> written =
                TryPlayer(player_id, current, baseline, record, done);
            !written.ok()) {
          return written;
        }
      }
    }
  }
  for (const uint8_t id : priority) {
    if (WireResult<void> written =
            TryPlayer(id, current, baseline, record, done);
        !written.ok()) {
      return written;
    }
  }
  writer_.set_limit(SaturatingSubtract(capacity_bits_, 1));
  writer_.WriteBool(false);
  if (!writer_.ok()) {
    return WireResult<void>::Error(*writer_.error());
  }
  stage_ = Stage::kObjects;
  return WireResult<void>::Ok();
}

WireResult<void> SnapshotWriter::TryPlayer(uint8_t id, const PlayerSet& current,
                                           const PlayerSet* baseline,
                                           PlayerSet& record, uint64_t& done) {
  if (id >= kMaxPlayers) {
    return WireResult<void>::Error(WireError::kOutOfRange);
  }
  const uint64_t bit = uint64_t{1} << id;
  if ((done & bit) != 0) {
    return WireResult<void>::Ok();
  }
  done |= bit;
  const std::optional<PlayerEntry> entry = DiffPlayer(
      id, current.Find(id), baseline != nullptr ? baseline->Find(id) : nullptr);
  if (!entry.has_value()) {
    return WireResult<void>::Ok();
  }
  const size_t mark = writer_.position();
  writer_.WriteBool(true);
  WritePlayerEntry(*entry, baseline != nullptr, writer_);
  if (writer_.ok()) {
    return ApplyPlayerEntry(*entry, record);
  }
  const WireError error = *writer_.error();
  writer_.Rewind(mark);
  if (error == WireError::kOverflow) {
    return WireResult<void>::Ok();
  }
  return WireResult<void>::Error(error);
}

WireResult<bool> SnapshotWriter::TryAddObject(const WorldObject& object) {
  if (stage_ != Stage::kObjects) {
    return WireResult<bool>::Error(WireError::kUsage);
  }
  if (object_count_ >= kMaxObjectsPerPacket) {
    return WireResult<bool>::Ok(false);
  }
  const size_t mark = writer_.position();
  writer_.WriteBool(true);
  WriteWorldObject(object, writer_);
  if (writer_.ok()) {
    ++object_count_;
    return WireResult<bool>::Ok(true);
  }
  const WireError error = *writer_.error();
  writer_.Rewind(mark);
  if (error == WireError::kOverflow) {
    return WireResult<bool>::Ok(false);
  }
  return WireResult<bool>::Error(error);
}

WireResult<size_t> SnapshotWriter::Finish() {
  if (stage_ != Stage::kObjects) {
    return WireResult<size_t>::Error(WireError::kUsage);
  }
  stage_ = Stage::kFinished;
  writer_.set_limit(capacity_bits_);
  writer_.WriteBool(false);
  return writer_.Finish();
}

}  // namespace blue::net
