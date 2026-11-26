// net::Transport against an in-process netcode.io server (raw netcode API,
// like netcode_smoke_test.cc) on 127.0.0.1: connect with a locally minted
// token, exchange a ClientHello / ServerWelcome built with the wire codecs,
// disconnect. Skipped (not failed) when the port cannot be bound. Time is
// advanced by hand; the short real sleep only lets loopback datagrams land.

#include <doctest/doctest.h>
#include <netcode.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

#include "net/messages.h"
#include "net/packet.h"
#include "net/protocol.h"
#include "net/transport.h"
#include "net/wire_error.h"

namespace blue::net {
namespace {

constexpr uint64_t kClientId = 0x5678;
constexpr double kTickSeconds = 1.0 / 60.0;
constexpr double kRealSleepSeconds = 0.001;
constexpr int kMaxTicks = 600;  // 10 simulated seconds

constexpr std::array<uint8_t, kConnectTokenKeyBytes> kTestKey = {
    0x11, 0x2a, 0x3b, 0x4c, 0x5d, 0x6e, 0x7f, 0x80, 0x91, 0xa2, 0xb3,
    0xc4, 0xd5, 0xe6, 0xf7, 0x08, 0x19, 0x2a, 0x3b, 0x4c, 0x5d, 0x6e,
    0x7f, 0x80, 0x91, 0xa2, 0xb3, 0xc4, 0xd5, 0xe6, 0xf7, 0x09};

struct ServerDeleter {
  void operator()(netcode_server_t* server) const {
    netcode_server_destroy(server);
  }
};
using ServerPtr = std::unique_ptr<netcode_server_t, ServerDeleter>;

// Creates a one-slot server on `address`, or null if it cannot bind.
ServerPtr StartServer(const char* address, double time) {
  netcode_server_config_t config;
  netcode_default_server_config(&config);
  config.protocol_id = kNetcodeProtocolId;
  for (size_t i = 0; i < kTestKey.size(); ++i) {
    config.private_key[i] = kTestKey[i];
  }
  ServerPtr server(netcode_server_create(address, &config, time));
  if (server != nullptr) {
    netcode_server_start(server.get(), 1);
  }
  return server;
}

Message Hello() {
  return ClientHello{.protocol_version = kProtocolVersion,
                     .content_hash = 0xFEEDFACECAFEBEEFULL,
                     .name = Name::Create("tester").value()};
}

Message Welcome() {
  return ServerWelcome{.player_id = 0,
                       .tick_rate = kDefaultTickRate,
                       .server_tick = 4242,
                       .max_players = kDefaultMaxPlayers};
}

// Runs the handshake of section 6 over `transport` and `server`; returns
// true once the client decoded the welcome.
bool Handshake(Transport& transport, netcode_server_t* server, double& time,
               int& hellos_received) {
  std::array<uint8_t, kMaxPayloadBytes> buffer{};
  for (int tick = 0; tick < kMaxTicks; ++tick) {
    transport.Update(time);
    netcode_server_update(server, time);

    if (transport.state() == TransportState::kConnected) {
      WireResult<size_t> size = EncodeControl(Hello(), buffer);
      REQUIRE(size.ok());
      CHECK(
          transport.Send(std::span<const uint8_t>(buffer.data(), size.value()))
              .ok());
    }

    int bytes = 0;
    uint64_t sequence = 0;
    while (uint8_t* packet =
               netcode_server_receive_packet(server, 0, &bytes, &sequence)) {
      WireResult<Message> hello = DecodeControl(
          std::span<const uint8_t>(packet, static_cast<size_t>(bytes)));
      netcode_server_free_packet(server, packet);
      REQUIRE(hello.ok());
      CHECK(hello.value() == Hello());
      ++hellos_received;
      WireResult<size_t> size = EncodeControl(Welcome(), buffer);
      REQUIRE(size.ok());
      netcode_server_send_packet(server, 0, buffer.data(),
                                 static_cast<int>(size.value()));
    }

    while (std::optional<Transport::Payload> payload = transport.Receive()) {
      WireResult<Message> welcome = DecodeControl(payload->bytes());
      REQUIRE(welcome.ok());
      CHECK(welcome.value() == Welcome());
      return true;
    }

    netcode_sleep(kRealSleepSeconds);
    time += kTickSeconds;
  }
  return false;
}

// Disconnects and waits until the server notices.
void DisconnectAndDrain(Transport& transport, netcode_server_t* server,
                        double& time) {
  transport.Disconnect();
  CHECK(transport.state() == TransportState::kDisconnected);
  for (int tick = 0;
       tick < kMaxTicks && netcode_server_num_connected_clients(server) > 0;
       ++tick) {
    netcode_sleep(kRealSleepSeconds);
    time += kTickSeconds;
    netcode_server_update(server, time);
  }
  CHECK(netcode_server_num_connected_clients(server) == 0);
}

TEST_CASE("transport rejects bad input before connecting") {
  Transport transport;
  CHECK(transport.state() == TransportState::kDisconnected);
  const std::array<uint8_t, 4> payload = {1, 2, 3, 4};
  CHECK(transport.Send(payload).error() == TransportError::kNotConnected);
  CHECK(transport.Send(std::span<const uint8_t>()).error() ==
        TransportError::kInvalidPayloadSize);
  CHECK(transport.Connect("not an address", kClientId, kTestKey).error() ==
        TransportError::kInvalidAddress);
  CHECK_FALSE(transport.Receive().has_value());
  transport.Disconnect();
  CHECK(transport.state() == TransportState::kDisconnected);
}

TEST_CASE("transport connects and exchanges control packets on loopback") {
  NetcodeRuntime runtime;
  REQUIRE(runtime.ok());
  netcode_log_level(NETCODE_LOG_LEVEL_NONE);
  double time = 0.0;
  constexpr char kAddress[] = "127.0.0.1:47392";
  const ServerPtr server = StartServer(kAddress, time);
  if (server == nullptr) {
    MESSAGE("skipped: cannot bind ", kAddress);
    return;
  }

  Transport transport;
  transport.Update(time);
  REQUIRE(transport.Connect(kAddress, kClientId, kTestKey).ok());
  CHECK(transport.state() == TransportState::kConnecting);

  int hellos = 0;
  CHECK(Handshake(transport, server.get(), time, hellos));
  CHECK(hellos >= 1);
  CHECK(transport.state() == TransportState::kConnected);
  CHECK(netcode_server_client_id(server.get(), 0) == kClientId);

  // A moved transport keeps its connection.
  Transport moved = std::move(transport);
  CHECK(moved.state() == TransportState::kConnected);
  DisconnectAndDrain(moved, server.get(), time);
}

TEST_CASE("transport delivers through the network simulator") {
  NetcodeRuntime runtime;
  REQUIRE(runtime.ok());
  netcode_log_level(NETCODE_LOG_LEVEL_NONE);
  double time = 0.0;
  constexpr char kAddress[] = "127.0.0.1:47393";
  const ServerPtr server = StartServer(kAddress, time);
  if (server == nullptr) {
    MESSAGE("skipped: cannot bind ", kAddress);
    return;
  }

  TransportConfig config;
  config.conditions = NetworkConditions{.latency_seconds = 0.05,
                                        .jitter_seconds = 0.02,
                                        .loss_percent = 20.0,
                                        .duplicate_percent = 30.0,
                                        .seed = 7};
  Transport transport(config);
  transport.Update(time);
  REQUIRE(transport.Connect(kAddress, kClientId, kTestKey).ok());

  int hellos = 0;
  const double start = time;
  CHECK(Handshake(transport, server.get(), time, hellos));
  CHECK(hellos >= 1);
  // Hellos and the welcome each wait at least latency - jitter one way.
  CHECK(time - start >= 0.06);
  DisconnectAndDrain(transport, server.get(), time);
}

}  // namespace
}  // namespace blue::net
