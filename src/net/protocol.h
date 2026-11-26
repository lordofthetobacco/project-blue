#ifndef BLUE_NET_PROTOCOL_H_
#define BLUE_NET_PROTOCOL_H_

#include <cstddef>
#include <cstdint>

namespace blue::net {

// Constants of the Project Blue wire protocol, section 1 of
// docs/netcode/protocol.md. The Rust server (crates/blue-protocol in the
// blue-server repository) uses the same values; changing one is a protocol
// version bump.

// Sent in ClientHello; a mismatch is answered with ServerReject.
inline constexpr uint16_t kProtocolVersion = 3;
// No packet is larger (the netcode.io payload limit).
inline constexpr size_t kMaxPayloadBytes = 1200;
// Ticks per second, the physics fixed step.
inline constexpr uint16_t kDefaultTickRate = 60;
// Player ids are 0..kMaxPlayers-1 (6 bits).
inline constexpr int kMaxPlayers = 64;
inline constexpr uint8_t kMaxPlayerId = kMaxPlayers - 1;
inline constexpr uint8_t kDefaultMaxPlayers = 16;
// Player names, UTF-8 bytes.
inline constexpr size_t kNameMaxBytes = 32;
inline constexpr int kMaxCommandsPerPacket = 16;
inline constexpr int kMaxMessagesPerPacket = 32;
// Reliable send and receive window, in message ids.
inline constexpr int kReliableWindow = 256;
// Sent packets remembered for acknowledgement.
inline constexpr int kSentPacketHistory = 256;
// Oldest baseline a snapshot can name, ticks before its server_tick.
inline constexpr uint32_t kMaxBaselineAge = 255;
// Snapshot player sets a client keeps as baselines; servers use no older.
inline constexpr int kBaselineHistory = 64;
inline constexpr int kMaxObjectsPerPacket = 96;
inline constexpr int kSyncPickupsPerMessage = 16;
inline constexpr size_t kDefaultReliableBudgetBytes = 300;
inline constexpr double kDefaultResendIntervalSeconds = 0.1;

}  // namespace blue::net

#endif  // BLUE_NET_PROTOCOL_H_
