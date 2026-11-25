#include "net/messages.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <variant>

#include <glm/gtc/quaternion.hpp>

#include "net/bit_stream.h"
#include "net/protocol.h"
#include "net/wire_error.h"

namespace blue::net {
namespace {

constexpr uint32_t kMaxTickRate = 0xFFFF;

// --- Shared field encodings --------------------------------------------------

void WritePlayerId(uint8_t id, BitWriter& writer) {
  writer.WriteBoundedU32(id, 0, kMaxPlayerId);
}

uint8_t ReadPlayerId(BitReader& reader) {
  return static_cast<uint8_t>(reader.ReadBoundedU32(0, kMaxPlayerId));
}

void WriteOptionalPlayerId(const std::optional<uint8_t>& id,
                           BitWriter& writer) {
  writer.WriteBool(id.has_value());
  if (id.has_value()) {
    WritePlayerId(*id, writer);
  }
}

std::optional<uint8_t> ReadOptionalPlayerId(BitReader& reader) {
  if (!reader.ReadBool()) {
    return std::nullopt;
  }
  return ReadPlayerId(reader);
}

void WriteName(const Name& name, BitWriter& writer) {
  writer.WriteString(name.view(), kNameMaxBytes);
}

Name ReadName(BitReader& reader) {
  std::array<char, kNameMaxBytes> bytes = {};
  const size_t size = reader.ReadString(bytes);
  if (!reader.ok()) {
    return Name();
  }
  WireResult<Name> name = Name::Create(std::string_view(bytes.data(), size));
  if (!name.ok()) {
    reader.Fail(name.error());
    return Name();
  }
  return name.value();
}

void WriteDroppedWeapon(const DroppedWeapon& weapon, BitWriter& writer) {
  writer.WriteU32(weapon.dropped_id);
  writer.WriteU32(weapon.weapon_key);
  writer.WriteU8(weapon.rounds_in_magazine);
  writer.WriteVec3(weapon.position);
  writer.WriteF32(weapon.rotation.x);
  writer.WriteF32(weapon.rotation.y);
  writer.WriteF32(weapon.rotation.z);
  writer.WriteF32(weapon.rotation.w);
}

DroppedWeapon ReadDroppedWeapon(BitReader& reader) {
  DroppedWeapon weapon;
  weapon.dropped_id = reader.ReadU32();
  weapon.weapon_key = reader.ReadU32();
  weapon.rounds_in_magazine = reader.ReadU8();
  weapon.position = reader.ReadVec3();
  const float x = reader.ReadF32();
  const float y = reader.ReadF32();
  const float z = reader.ReadF32();
  const float w = reader.ReadF32();
  weapon.rotation = glm::quat::wxyz(w, x, y, z);
  return weapon;
}

// --- Bodies: one WriteBody / ReadBody pair per message -----------------------

void WriteBody(const ClientHello& m, BitWriter& writer) {
  writer.WriteU16(m.protocol_version);
  writer.WriteU64(m.content_hash);
  WriteName(m.name, writer);
}

void ReadBody(BitReader& reader, ClientHello& m) {
  m.protocol_version = reader.ReadU16();
  m.content_hash = reader.ReadU64();
  m.name = ReadName(reader);
}

void WriteBody(const ServerWelcome& m, BitWriter& writer) {
  WritePlayerId(m.player_id, writer);
  writer.WriteBoundedU32(m.tick_rate, 1, kMaxTickRate);
  writer.WriteU32(m.server_tick);
  writer.WriteBoundedU32(m.max_players, 1, kMaxPlayers);
}

void ReadBody(BitReader& reader, ServerWelcome& m) {
  m.player_id = ReadPlayerId(reader);
  m.tick_rate = static_cast<uint16_t>(reader.ReadBoundedU32(1, kMaxTickRate));
  m.server_tick = reader.ReadU32();
  m.max_players = static_cast<uint8_t>(reader.ReadBoundedU32(1, kMaxPlayers));
}

void WriteBody(const ServerReject& m, BitWriter& writer) {
  writer.WriteU8(m.reason);
  writer.WriteU16(m.server_protocol_version);
}

void ReadBody(BitReader& reader, ServerReject& m) {
  m.reason = reader.ReadU8();
  m.server_protocol_version = reader.ReadU16();
}

void WriteBody(const PlayerJoined& m, BitWriter& writer) {
  WritePlayerId(m.player_id, writer);
  WriteName(m.name, writer);
}

void ReadBody(BitReader& reader, PlayerJoined& m) {
  m.player_id = ReadPlayerId(reader);
  m.name = ReadName(reader);
}

void WriteBody(const PlayerLeft& m, BitWriter& writer) {
  WritePlayerId(m.player_id, writer);
}

void ReadBody(BitReader& reader, PlayerLeft& m) {
  m.player_id = ReadPlayerId(reader);
}

void WriteBody(const Damage& m, BitWriter& writer) {
  WritePlayerId(m.victim, writer);
  WriteOptionalPlayerId(m.attacker, writer);
  writer.WriteI16(m.amount);
  writer.WriteBoundedU32(m.zone, 0, hit_zone::kMax);
  writer.WriteI16(m.health_after);
}

void ReadBody(BitReader& reader, Damage& m) {
  m.victim = ReadPlayerId(reader);
  m.attacker = ReadOptionalPlayerId(reader);
  m.amount = reader.ReadI16();
  m.zone = static_cast<uint8_t>(reader.ReadBoundedU32(0, hit_zone::kMax));
  m.health_after = reader.ReadI16();
}

void WriteBody(const Death& m, BitWriter& writer) {
  WritePlayerId(m.victim, writer);
  WriteOptionalPlayerId(m.killer, writer);
  writer.WriteU32(m.weapon_key);
}

void ReadBody(BitReader& reader, Death& m) {
  m.victim = ReadPlayerId(reader);
  m.killer = ReadOptionalPlayerId(reader);
  m.weapon_key = reader.ReadU32();
}

void WriteBody(const Respawn& m, BitWriter& writer) {
  WritePlayerId(m.player_id, writer);
  writer.WriteVec3(m.position);
  writer.WriteF32(m.yaw);
}

void ReadBody(BitReader& reader, Respawn& m) {
  m.player_id = ReadPlayerId(reader);
  m.position = reader.ReadVec3();
  m.yaw = reader.ReadF32();
}

void WriteBody(const Shot& m, BitWriter& writer) {
  WritePlayerId(m.shooter, writer);
  writer.WriteVec3(m.origin);
  writer.WriteVec3(m.direction);
  writer.WriteU32(m.weapon_key);
  writer.WriteU32(m.shot_number);
}

void ReadBody(BitReader& reader, Shot& m) {
  m.shooter = ReadPlayerId(reader);
  m.origin = reader.ReadVec3();
  m.direction = reader.ReadVec3();
  m.weapon_key = reader.ReadU32();
  m.shot_number = reader.ReadU32();
}

void WriteBody(const Impact& m, BitWriter& writer) {
  writer.WriteVec3(m.point);
  writer.WriteVec3(m.normal);
}

void ReadBody(BitReader& reader, Impact& m) {
  m.point = reader.ReadVec3();
  m.normal = reader.ReadVec3();
}

void WriteBody(const WeaponDropped& m, BitWriter& writer) {
  WriteDroppedWeapon(m.weapon, writer);
}

void ReadBody(BitReader& reader, WeaponDropped& m) {
  m.weapon = ReadDroppedWeapon(reader);
}

void WriteBody(const DroppedTaken& m, BitWriter& writer) {
  writer.WriteU32(m.dropped_id);
  WritePlayerId(m.player_id, writer);
}

void ReadBody(BitReader& reader, DroppedTaken& m) {
  m.dropped_id = reader.ReadU32();
  m.player_id = ReadPlayerId(reader);
}

void WriteBody(const DroppedRemoved& m, BitWriter& writer) {
  writer.WriteU32(m.dropped_id);
}

void ReadBody(BitReader& reader, DroppedRemoved& m) {
  m.dropped_id = reader.ReadU32();
}

void WriteBody(const PickupTaken& m, BitWriter& writer) {
  writer.WriteU32(m.instance_id);
  WritePlayerId(m.player_id, writer);
}

void ReadBody(BitReader& reader, PickupTaken& m) {
  m.instance_id = reader.ReadU32();
  m.player_id = ReadPlayerId(reader);
}

void WriteBody(const WorldSyncPickupsTaken& m, BitWriter& writer) {
  writer.WriteBoundedU32(m.count, 0, kSyncPickupsPerMessage);
  if (!writer.ok()) {
    return;
  }
  for (size_t i = m.count; i < m.instance_ids.size(); ++i) {
    if (m.instance_ids[i] != 0) {
      writer.Fail(WireError::kNonCanonical);
      return;
    }
  }
  for (size_t i = 0; i < m.count; ++i) {
    writer.WriteU32(m.instance_ids[i]);
  }
}

void ReadBody(BitReader& reader, WorldSyncPickupsTaken& m) {
  m.count =
      static_cast<uint8_t>(reader.ReadBoundedU32(0, kSyncPickupsPerMessage));
  for (size_t i = 0; i < m.count; ++i) {
    m.instance_ids[i] = reader.ReadU32();
  }
}

void WriteBody(const WorldSyncDroppedWeapon& m, BitWriter& writer) {
  WriteDroppedWeapon(m.weapon, writer);
}

void ReadBody(BitReader& reader, WorldSyncDroppedWeapon& m) {
  m.weapon = ReadDroppedWeapon(reader);
}

void WriteBody(const WorldSyncDone& /*m*/, BitWriter& /*writer*/) {}

void ReadBody(BitReader& /*reader*/, WorldSyncDone& /*m*/) {}

// Reads the body of a message of type T.
template <typename T>
Message ReadAs(BitReader& reader) {
  T message;
  ReadBody(reader, message);
  return message;
}

}  // namespace

WireResult<Name> Name::Create(std::string_view text) {
  if (text.size() > kNameMaxBytes) {
    return WireResult<Name>::Error(WireError::kOutOfRange);
  }
  const std::span<const uint8_t> bytes(
      reinterpret_cast<const uint8_t*>(text.data()), text.size());
  if (!IsValidUtf8(bytes)) {
    return WireResult<Name>::Error(WireError::kInvalidUtf8);
  }
  Name name;
  for (size_t i = 0; i < text.size(); ++i) {
    name.bytes_[i] = text[i];
  }
  name.size_ = text.size();
  return WireResult<Name>::Ok(name);
}

MessageType TypeOf(const Message& message) {
  return std::visit(
      [](const auto& m) { return std::decay_t<decltype(m)>::kType; }, message);
}

bool IsHandshake(const Message& message) {
  const MessageType type = TypeOf(message);
  return type == MessageType::kClientHello ||
         type == MessageType::kServerWelcome ||
         type == MessageType::kServerReject;
}

void WriteMessage(const Message& message, BitWriter& writer) {
  writer.WriteU8(static_cast<uint8_t>(TypeOf(message)));
  std::visit([&writer](const auto& m) { WriteBody(m, writer); }, message);
}

Message ReadMessage(BitReader& reader) {
  const uint8_t type = reader.ReadU8();
  if (!reader.ok()) {
    return Message();
  }
  switch (static_cast<MessageType>(type)) {
    case MessageType::kClientHello:
      return ReadAs<ClientHello>(reader);
    case MessageType::kServerWelcome:
      return ReadAs<ServerWelcome>(reader);
    case MessageType::kServerReject:
      return ReadAs<ServerReject>(reader);
    case MessageType::kPlayerJoined:
      return ReadAs<PlayerJoined>(reader);
    case MessageType::kPlayerLeft:
      return ReadAs<PlayerLeft>(reader);
    case MessageType::kDamage:
      return ReadAs<Damage>(reader);
    case MessageType::kDeath:
      return ReadAs<Death>(reader);
    case MessageType::kRespawn:
      return ReadAs<Respawn>(reader);
    case MessageType::kShot:
      return ReadAs<Shot>(reader);
    case MessageType::kImpact:
      return ReadAs<Impact>(reader);
    case MessageType::kWeaponDropped:
      return ReadAs<WeaponDropped>(reader);
    case MessageType::kDroppedTaken:
      return ReadAs<DroppedTaken>(reader);
    case MessageType::kPickupTaken:
      return ReadAs<PickupTaken>(reader);
    case MessageType::kWorldSyncPickupsTaken:
      return ReadAs<WorldSyncPickupsTaken>(reader);
    case MessageType::kWorldSyncDroppedWeapon:
      return ReadAs<WorldSyncDroppedWeapon>(reader);
    case MessageType::kWorldSyncDone:
      return ReadAs<WorldSyncDone>(reader);
    case MessageType::kDroppedRemoved:
      return ReadAs<DroppedRemoved>(reader);
  }
  reader.Fail(WireError::kUnknownMessage);
  return Message();
}

WireResult<size_t> MessageBits(const Message& message) {
  // The largest message (ClientHello) is well under 100 bytes.
  std::array<uint8_t, kMaxPayloadBytes> scratch = {};
  BitWriter writer(scratch);
  WriteMessage(message, writer);
  if (!writer.ok()) {
    return WireResult<size_t>::Error(*writer.error());
  }
  return WireResult<size_t>::Ok(writer.position());
}

WireResult<size_t> EncodeStandaloneMessage(const Message& message,
                                           std::span<uint8_t> buffer) {
  BitWriter writer(buffer);
  WriteMessage(message, writer);
  return writer.Finish();
}

WireResult<Message> DecodeStandaloneMessage(std::span<const uint8_t> payload) {
  BitReader reader(payload);
  Message message = ReadMessage(reader);
  if (WireResult<void> finished = reader.Finish(); !finished.ok()) {
    return WireResult<Message>::Error(finished.error());
  }
  return WireResult<Message>::Ok(message);
}

}  // namespace blue::net
