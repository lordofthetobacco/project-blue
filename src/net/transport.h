#ifndef BLUE_NET_TRANSPORT_H_
#define BLUE_NET_TRANSPORT_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "core/result.h"

namespace blue::net {

// The client side of the netcode.io transport (mas-bandwidth/netcode
// v1.5.0): connection, encryption, keep-alives and timeouts. One payload
// carries one packet of the wire protocol (net/packet.h, net/snapshot.h).
// No netcode type appears in this header.

// netcode.io protocol id of Project Blue. Connect tokens carry it and the
// server only accepts tokens with its own id, so the dedicated server must
// use exactly this value ("BLUE_MP1" in ASCII, docs/netcode/protocol.md
// section 1).
inline constexpr uint64_t kNetcodeProtocolId = 0x424C55455F4D5031ULL;
// Size of the key that signs connect tokens.
inline constexpr size_t kConnectTokenKeyBytes = 32;

// Holds netcode.io's global state (sockets, libsodium) for as long as it
// lives. netcode reference-counts its init/term pairs, so any number of
// these may coexist (one per Transport, plus one per in-process test
// server). Construct and destroy on one thread. Neither copyable nor
// movable.
class NetcodeRuntime {
 public:
  NetcodeRuntime();
  NetcodeRuntime(const NetcodeRuntime&) = delete;
  NetcodeRuntime& operator=(const NetcodeRuntime&) = delete;
  ~NetcodeRuntime();

  // False if netcode_init failed (no networking available).
  bool ok() const { return ok_; }

 private:
  bool ok_ = false;
};

// Connection state of a Transport, mapped from netcode's client states.
enum class TransportState : uint8_t {
  kDisconnected,     // Never connected, or Disconnect() was called.
  kConnecting,       // Sending connection request / response.
  kConnected,        // Payloads flow.
  kConnectTimedOut,  // The server never answered the handshake.
  kTimedOut,         // An established connection went silent.
  kDenied,           // The server answered and refused: it is full.
  kTokenExpired,     // The connect token expired before connecting.
  kInvalidToken,     // The connect token is malformed.
};

// Returns a short English name of `state` for logs and status lines.
std::string_view TransportStateText(TransportState state);

// Failures of Transport operations.
enum class TransportError : uint8_t {
  kNetcodeInitFailed,   // netcode_init failed.
  kInvalidAddress,      // The server address does not parse.
  kTokenFailed,         // netcode_generate_connect_token failed.
  kClientCreateFailed,  // No local socket could be bound.
  kInvalidPayloadSize,  // Empty, or larger than kMaxPayloadBytes.
  kNotConnected,        // Send() outside the kConnected state.
};

std::string_view TransportErrorText(TransportError error);

// A development network simulator applied by the Transport to payloads in
// both directions (each direction gets `latency_seconds` +- `jitter_seconds`
// of delay, `loss_percent` of drops and `duplicate_percent` of duplicates).
// It wraps the real UDP connection, so it also works against a remote
// server; netcode's own simulator replaces the sockets and only serves
// in-process client/server pairs. Deterministic for a given `seed`.
struct NetworkConditions {
  double latency_seconds = 0.0;
  double jitter_seconds = 0.0;
  double loss_percent = 0.0;
  double duplicate_percent = 0.0;
  uint64_t seed = 1;

  // True if any condition differs from a perfect network.
  bool active() const {
    return latency_seconds > 0.0 || jitter_seconds > 0.0 ||
           loss_percent > 0.0 || duplicate_percent > 0.0;
  }
};

// Connect-token parameters and the optional simulator.
struct TransportConfig {
  uint64_t protocol_id = kNetcodeProtocolId;
  // Seconds a minted token stays valid (client and server clocks must agree
  // to within this); negative: never expires.
  int token_expire_seconds = 300;
  // Seconds without packets before the connection times out.
  int token_timeout_seconds = 10;
  NetworkConditions conditions;
};

// A netcode.io client. Drive it from one thread: Update() once per frame or
// tick, then drain Receive(), then Send(). Move-only; owns its socket.
//
//   net::Transport transport;
//   transport.Connect("127.0.0.1:27015", client_id, key);
//   while (running) {
//     transport.Update(now_seconds);
//     while (std::optional<net::Transport::Payload> p = transport.Receive()) {
//       Handle(p->bytes());
//     }
//     transport.Send(packet);
//   }
class Transport {
  // Declared first so Payload can name it; defined in transport.cc.
  class Impl;

 public:
  // A received payload. Borrows netcode's packet buffer (or owns a copy
  // when the simulator is active) and frees it when destroyed. Must be
  // destroyed before its Transport, and before the next Connect().
  // Move-only.
  class Payload {
   public:
    Payload(Payload&& other) noexcept;
    Payload& operator=(Payload&& other) noexcept;
    Payload(const Payload&) = delete;
    Payload& operator=(const Payload&) = delete;
    ~Payload();

    std::span<const uint8_t> bytes() const { return bytes_; }

   private:
    friend class Impl;

    // A payload in a netcode packet buffer, freed through `owner`.
    Payload(Impl* owner, uint8_t* packet, size_t size);
    // A payload owned by this object.
    explicit Payload(std::vector<uint8_t> copy);

    void Release();

    Impl* owner_ = nullptr;
    uint8_t* packet_ = nullptr;
    std::vector<uint8_t> copy_;
    std::span<const uint8_t> bytes_;
  };

  Transport();
  explicit Transport(const TransportConfig& config);
  // A moved-from Transport may only be destroyed or assigned to.
  Transport(Transport&&) noexcept;
  Transport& operator=(Transport&&) noexcept;
  Transport(const Transport&) = delete;
  Transport& operator=(const Transport&) = delete;
  ~Transport();

  // Starts connecting to `server_address`, a numeric "a.b.c.d:port" or
  // "[v6]:port" (netcode_parse_address does no name lookup; resolve host
  // names first, see net/address_resolver.h; kInvalidAddress otherwise)
  // as `client_id`. Mints the connect token locally from
  // `dev_private_key` with netcode_generate_connect_token: this is the
  // development path until a token service exists, and only works with a
  // server that shares the key. Replaces any previous connection.
  core::Result<void, TransportError> Connect(
      std::string_view server_address, uint64_t client_id,
      std::span<const uint8_t, kConnectTokenKeyBytes> dev_private_key);

  // Advances netcode (handshake, keep-alives, timeouts, receiving) and the
  // simulator to `time_seconds`, a monotonic clock.
  void Update(double time_seconds);

  // Sends one payload (1..kMaxPayloadBytes bytes) unreliably.
  core::Result<void, TransportError> Send(std::span<const uint8_t> payload);

  // Returns the next received payload, or nullopt when none is due.
  std::optional<Payload> Receive();

  TransportState state() const;

  // Disconnects (telling the server) and drops queued payloads.
  void Disconnect();

  // Replaces the simulated network conditions; queued payloads keep their
  // delivery times.
  void set_network_conditions(const NetworkConditions& conditions);

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace blue::net

#endif  // BLUE_NET_TRANSPORT_H_
