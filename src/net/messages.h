#ifndef BLUE_NET_MESSAGES_H_
#define BLUE_NET_MESSAGES_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <variant>

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include "net/bit_stream.h"
#include "net/protocol.h"
#include "net/wire_error.h"

namespace blue::net {

// The message catalogue, section 10 of docs/netcode/protocol.md: handshake
// messages (Control packets only) and game events (reliable channel only).
// Every message is a fixed-size value; nothing allocates.

// Message type ids (8 bits on the wire). Unlisted values are reserved.
enum class MessageType : uint8_t {
  kClientHello = 1,
  kServerWelcome = 2,
  kServerReject = 3,
  kPlayerJoined = 16,
  kPlayerLeft = 17,
  kDamage = 18,
  kDeath = 19,
  kRespawn = 20,
  kShot = 21,
  kImpact = 22,
  kWeaponDropped = 23,
  kDroppedTaken = 24,
  kPickupTaken = 25,
  kWorldSyncPickupsTaken = 26,
  kWorldSyncDroppedWeapon = 27,
  kWorldSyncDone = 28,
  kDroppedRemoved = 29,
};

// A player name: at most kNameMaxBytes bytes of valid UTF-8, stored inline.
class Name {
 public:
  // The empty name.
  Name() = default;
  Name(const Name&) = default;
  Name& operator=(const Name&) = default;

  // Returns `text` as a name; kOutOfRange if longer than kNameMaxBytes,
  // kInvalidUtf8 if not UTF-8.
  static WireResult<Name> Create(std::string_view text);

  std::string_view view() const {
    return std::string_view(bytes_.data(), size_);
  }

  // Names are equal when their bytes are.
  friend bool operator==(const Name& a, const Name& b) {
    return a.view() == b.view();
  }

 private:
  std::array<char, kNameMaxBytes> bytes_ = {};
  size_t size_ = 0;
};

// ServerReject reasons. Other values are carried through (display them as
// "unknown").
namespace reject_reason {
inline constexpr uint8_t kProtocolMismatch = 1;
inline constexpr uint8_t kContentMismatch = 2;
inline constexpr uint8_t kServerFull = 3;
inline constexpr uint8_t kBadName = 4;
inline constexpr uint8_t kShuttingDown = 5;
}  // namespace reject_reason

// Damage hit zones (3 bits on the wire; 4..7 reserved).
namespace hit_zone {
inline constexpr uint8_t kHead = 0;
inline constexpr uint8_t kTorso = 1;
inline constexpr uint8_t kLegs = 2;
inline constexpr uint8_t kOther = 3;  // World damage.
inline constexpr uint8_t kMax = 7;
}  // namespace hit_zone

// Client -> server, first thing sent (frozen layout).
struct ClientHello {
  static constexpr MessageType kType = MessageType::kClientHello;
  uint16_t protocol_version = 0;
  uint64_t content_hash = 0;  // Scene, weapon table, catalogue.
  Name name;
  friend bool operator==(const ClientHello&, const ClientHello&) = default;
};

// Server -> client: accepted.
struct ServerWelcome {
  static constexpr MessageType kType = MessageType::kServerWelcome;
  uint8_t player_id = 0;   // 0..63
  uint16_t tick_rate = 1;  // 1..65535
  uint32_t server_tick = 0;
  uint8_t max_players = 1;  // 1..64
  friend bool operator==(const ServerWelcome&, const ServerWelcome&) = default;
};

// Server -> client: refused (frozen layout).
struct ServerReject {
  static constexpr MessageType kType = MessageType::kServerReject;
  uint8_t reason = 0;  // reject_reason
  uint16_t server_protocol_version = 0;
  friend bool operator==(const ServerReject&, const ServerReject&) = default;
};

// A player joined (also sent for every present player to a newcomer).
struct PlayerJoined {
  static constexpr MessageType kType = MessageType::kPlayerJoined;
  uint8_t player_id = 0;
  Name name;
  friend bool operator==(const PlayerJoined&, const PlayerJoined&) = default;
};

struct PlayerLeft {
  static constexpr MessageType kType = MessageType::kPlayerLeft;
  uint8_t player_id = 0;
  friend bool operator==(const PlayerLeft&, const PlayerLeft&) = default;
};

struct Damage {
  static constexpr MessageType kType = MessageType::kDamage;
  uint8_t victim = 0;
  std::optional<uint8_t> attacker;  // nullopt: world damage.
  int16_t amount = 0;
  uint8_t zone = hit_zone::kHead;  // hit_zone, 0..7
  int16_t health_after = 0;
  friend bool operator==(const Damage&, const Damage&) = default;
};

// The bullet that landed the killing hit, for the victim's ragdoll.
struct DeathHit {
  // Hit point minus the victim's feet at death, metres.
  glm::vec3 offset = glm::vec3(0.0f);
  // Bullet velocity at impact, m/s.
  glm::vec3 velocity = glm::vec3(0.0f);
  friend bool operator==(const DeathHit&, const DeathHit&) = default;
};

struct Death {
  static constexpr MessageType kType = MessageType::kDeath;
  uint8_t victim = 0;
  std::optional<uint8_t> killer;  // nullopt: world.
  uint32_t weapon_key = 0;        // 0: none.
  // nullopt: no bullet (kill plane, world damage).
  std::optional<DeathHit> hit;
  friend bool operator==(const Death&, const Death&) = default;
};

struct Respawn {
  static constexpr MessageType kType = MessageType::kRespawn;
  uint8_t player_id = 0;
  glm::vec3 position = glm::vec3(0.0f);  // Feet, metres.
  float yaw = 0.0f;
  friend bool operator==(const Respawn&, const Respawn&) = default;
};

// A shot was fired (for effects on other clients).
struct Shot {
  static constexpr MessageType kType = MessageType::kShot;
  uint8_t shooter = 0;
  glm::vec3 origin = glm::vec3(0.0f);     // Muzzle origin (the sim eye).
  glm::vec3 direction = glm::vec3(0.0f);  // Unit direction.
  uint32_t weapon_key = 0;
  uint32_t shot_number = 0;  // The shooter's shots_fired for this shot.
  friend bool operator==(const Shot&, const Shot&) = default;
};

// A projectile hit the world.
struct Impact {
  static constexpr MessageType kType = MessageType::kImpact;
  glm::vec3 point = glm::vec3(0.0f);
  glm::vec3 normal = glm::vec3(0.0f);
  friend bool operator==(const Impact&, const Impact&) = default;
};

// A dropped weapon, as announced by WeaponDropped and WorldSyncDroppedWeapon.
struct DroppedWeapon {
  uint32_t dropped_id = 0;
  uint32_t weapon_key = 0;
  uint8_t rounds_in_magazine = 0;
  glm::vec3 position = glm::vec3(0.0f);
  // Full precision on the wire as x, y, z, w.
  glm::quat rotation = glm::quat::wxyz(1.0f, 0.0f, 0.0f, 0.0f);
  friend bool operator==(const DroppedWeapon&, const DroppedWeapon&) = default;
};

struct WeaponDropped {
  static constexpr MessageType kType = MessageType::kWeaponDropped;
  DroppedWeapon weapon;
  friend bool operator==(const WeaponDropped&, const WeaponDropped&) = default;
};

struct DroppedTaken {
  static constexpr MessageType kType = MessageType::kDroppedTaken;
  uint32_t dropped_id = 0;
  uint8_t player_id = 0;
  friend bool operator==(const DroppedTaken&, const DroppedTaken&) = default;
};

struct PickupTaken {
  static constexpr MessageType kType = MessageType::kPickupTaken;
  uint32_t instance_id = 0;
  uint8_t player_id = 0;
  friend bool operator==(const PickupTaken&, const PickupTaken&) = default;
};

// Late-join sync: pickups already taken. Entries past `count` must be 0
// (encoding refuses others with kNonCanonical; decoding zeroes them).
struct WorldSyncPickupsTaken {
  static constexpr MessageType kType = MessageType::kWorldSyncPickupsTaken;
  uint8_t count = 0;  // 0..16
  std::array<uint32_t, kSyncPickupsPerMessage> instance_ids = {};
  friend bool operator==(const WorldSyncPickupsTaken&,
                         const WorldSyncPickupsTaken&) = default;
};

struct WorldSyncDroppedWeapon {
  static constexpr MessageType kType = MessageType::kWorldSyncDroppedWeapon;
  DroppedWeapon weapon;
  friend bool operator==(const WorldSyncDroppedWeapon&,
                         const WorldSyncDroppedWeapon&) = default;
};

// A dropped weapon left the world without a taker (its lifetime ran out,
// the cap removed it, or it fell below the kill plane).
struct DroppedRemoved {
  static constexpr MessageType kType = MessageType::kDroppedRemoved;
  uint32_t dropped_id = 0;
  friend bool operator==(const DroppedRemoved&,
                         const DroppedRemoved&) = default;
};

// Late-join sync complete.
struct WorldSyncDone {
  static constexpr MessageType kType = MessageType::kWorldSyncDone;
  friend bool operator==(const WorldSyncDone&, const WorldSyncDone&) = default;
};

// Any message of the protocol. Default-constructs to WorldSyncDone, as in
// the Rust implementation.
using Message =
    std::variant<WorldSyncDone, ClientHello, ServerWelcome, ServerReject,
                 PlayerJoined, PlayerLeft, Damage, Death, Respawn, Shot, Impact,
                 WeaponDropped, DroppedTaken, PickupTaken,
                 WorldSyncPickupsTaken, WorldSyncDroppedWeapon, DroppedRemoved>;

// Returns the message's type id.
MessageType TypeOf(const Message& message);
// Returns true for the messages that travel in Control packets only.
bool IsHandshake(const Message& message);

// Writes the type id and body. Range violations record kOutOfRange (or
// kNonCanonical for data past WorldSyncPickupsTaken::count).
void WriteMessage(const Message& message, BitWriter& writer);
// Reads a type id and body. A reserved id records kUnknownMessage.
Message ReadMessage(BitReader& reader);

// Returns the bits of type id + body.
WireResult<size_t> MessageBits(const Message& message);

// Encodes type id + body alone, zero padded to a byte (the golden vector
// form). Returns the bytes written.
WireResult<size_t> EncodeStandaloneMessage(const Message& message,
                                           std::span<uint8_t> buffer);
// Decodes what EncodeStandaloneMessage wrote, including the padding check.
WireResult<Message> DecodeStandaloneMessage(std::span<const uint8_t> payload);

}  // namespace blue::net

#endif  // BLUE_NET_MESSAGES_H_
