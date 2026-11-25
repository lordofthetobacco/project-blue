// Every message of the catalogue round trips with boundary values, and the
// encoders refuse what the decoders would reject (docs/netcode/protocol.md
// section 10).

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include "net/messages.h"
#include "net/wire_error.h"

namespace blue::net {
namespace {

Name MakeName(std::string_view text) {
  WireResult<Name> name = Name::Create(text);
  REQUIRE(name.ok());
  return name.value();
}

void CheckRoundTrip(const Message& message) {
  std::array<uint8_t, 128> buffer{};
  WireResult<size_t> size = EncodeStandaloneMessage(message, buffer);
  REQUIRE(size.ok());
  WireResult<size_t> bits = MessageBits(message);
  REQUIRE(bits.ok());
  CHECK(size.value() == (bits.value() + 7) / 8);
  WireResult<Message> decoded = DecodeStandaloneMessage(
      std::span<const uint8_t>(buffer.data(), size.value()));
  REQUIRE(decoded.ok());
  CHECK(decoded.value() == message);
  CHECK(TypeOf(decoded.value()) == TypeOf(message));
}

TEST_CASE("names are bounded UTF-8") {
  CHECK(MakeName("").view().empty());
  CHECK(MakeName(std::string_view("0123456789abcdef0123456789abcdef"))
            .view()
            .size() == kNameMaxBytes);
  CHECK(Name::Create("0123456789abcdef0123456789abcdefX").error() ==
        WireError::kOutOfRange);
  CHECK(Name::Create("\xFF").error() == WireError::kInvalidUtf8);
}

TEST_CASE("every message round trips with boundary values") {
  DroppedWeapon weapon;
  weapon.dropped_id = UINT32_MAX;
  weapon.weapon_key = 0;
  weapon.rounds_in_magazine = 255;
  weapon.position = glm::vec3(-16384.0f, 0.0f, 1.0e30f);
  weapon.rotation = glm::quat::wxyz(-1.0f, 0.0f, -0.0f, 0.5f);

  WorldSyncPickupsTaken full_sync;
  full_sync.count = kSyncPickupsPerMessage;
  for (size_t i = 0; i < full_sync.instance_ids.size(); ++i) {
    full_sync.instance_ids[i] = static_cast<uint32_t>(UINT32_MAX - i);
  }

  const std::vector<Message> messages = {
      ClientHello{.protocol_version = 0xFFFF,
                  .content_hash = UINT64_MAX,
                  .name = MakeName("0123456789abcdef0123456789abcdef")},
      ClientHello{.protocol_version = 0, .content_hash = 0, .name = Name()},
      ServerWelcome{.player_id = 63,
                    .tick_rate = 65535,
                    .server_tick = UINT32_MAX,
                    .max_players = 64},
      ServerWelcome{
          .player_id = 0, .tick_rate = 1, .server_tick = 0, .max_players = 1},
      ServerReject{.reason = 255, .server_protocol_version = 0xFFFF},
      PlayerJoined{.player_id = 63, .name = MakeName("\xF0\x9F\x98\x80")},
      PlayerLeft{.player_id = 0},
      Damage{.victim = 63,
             .attacker = std::nullopt,
             .amount = -32768,
             .zone = hit_zone::kMax,
             .health_after = 32767},
      Damage{.victim = 0,
             .attacker = 63,
             .amount = 1,
             .zone = 0,
             .health_after = 0},
      Death{.victim = 1, .killer = 0, .weapon_key = UINT32_MAX},
      Respawn{.player_id = 2,
              .position = glm::vec3(std::numeric_limits<float>::max()),
              .yaw = std::numeric_limits<float>::lowest()},
      Shot{.shooter = 63,
           .origin = glm::vec3(1.0f, 2.0f, 3.0f),
           .direction = glm::vec3(0.0f, -1.0f, 0.0f),
           .weapon_key = 1,
           .shot_number = UINT32_MAX},
      Impact{.point = glm::vec3(std::numeric_limits<float>::denorm_min()),
             .normal = glm::vec3(0.0f, 0.0f, 1.0f)},
      WeaponDropped{.weapon = weapon},
      DroppedTaken{.dropped_id = 0, .player_id = 63},
      DroppedRemoved{.dropped_id = UINT32_MAX},
      PickupTaken{.instance_id = UINT32_MAX, .player_id = 1},
      WorldSyncPickupsTaken{},
      full_sync,
      WorldSyncDroppedWeapon{.weapon = DroppedWeapon{}},
      WorldSyncDone{},
  };
  for (const Message& message : messages) {
    CAPTURE(static_cast<int>(TypeOf(message)));
    CheckRoundTrip(message);
  }
}

TEST_CASE("handshake messages are classified") {
  CHECK(IsHandshake(ClientHello{}));
  CHECK(IsHandshake(ServerWelcome{}));
  CHECK(IsHandshake(ServerReject{}));
  CHECK_FALSE(IsHandshake(PlayerLeft{}));
  CHECK_FALSE(IsHandshake(WorldSyncDone{}));
  CHECK(TypeOf(Message()) == MessageType::kWorldSyncDone);
}

TEST_CASE("encoders refuse out-of-range fields") {
  std::array<uint8_t, 128> buffer{};
  CHECK(EncodeStandaloneMessage(PlayerLeft{.player_id = 64}, buffer).error() ==
        WireError::kOutOfRange);
  CHECK(
      EncodeStandaloneMessage(ServerWelcome{.tick_rate = 0}, buffer).error() ==
      WireError::kOutOfRange);
  CHECK(EncodeStandaloneMessage(ServerWelcome{.max_players = 65}, buffer)
            .error() == WireError::kOutOfRange);
  CHECK(EncodeStandaloneMessage(Damage{.zone = 8}, buffer).error() ==
        WireError::kOutOfRange);
  CHECK(EncodeStandaloneMessage(
            Respawn{.yaw = std::numeric_limits<float>::quiet_NaN()}, buffer)
            .error() == WireError::kOutOfRange);
  WorldSyncPickupsTaken stray;
  stray.count = 1;
  stray.instance_ids[5] = 9;
  CHECK(EncodeStandaloneMessage(stray, buffer).error() ==
        WireError::kNonCanonical);
  WorldSyncPickupsTaken too_many;
  too_many.count = 17;
  CHECK(EncodeStandaloneMessage(too_many, buffer).error() ==
        WireError::kOutOfRange);
}

TEST_CASE("decoders reject reserved ids and bad fields") {
  for (const int reserved : {0, 4, 15, 30, 127, 128, 255}) {
    const std::array<uint8_t, 1> payload = {static_cast<uint8_t>(reserved)};
    CHECK(DecodeStandaloneMessage(payload).error() ==
          WireError::kUnknownMessage);
  }
  // PlayerLeft (17) with a truncated id.
  const std::array<uint8_t, 1> truncated = {17};
  CHECK(DecodeStandaloneMessage(truncated).error() == WireError::kTruncated);
  // ServerWelcome with a raw tick_rate of 65535 (= 65536, out of range).
  std::array<uint8_t, 16> buffer{};
  BitWriter writer(buffer);
  writer.WriteU8(2);
  writer.WriteBits(0, 6);
  writer.WriteBits(0xFFFF, 16);
  writer.WriteU32(0);
  writer.WriteBits(0, 6);
  WireResult<size_t> size = writer.Finish();
  REQUIRE(size.ok());
  CHECK(DecodeStandaloneMessage(
            std::span<const uint8_t>(buffer.data(), size.value()))
            .error() == WireError::kOutOfRange);
}

}  // namespace
}  // namespace blue::net
