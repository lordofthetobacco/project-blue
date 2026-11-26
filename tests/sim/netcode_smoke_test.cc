// netcode.io end to end in one process: a server and a client on 127.0.0.1,
// connected with a connect token signed by a test key, exchanging one payload
// each way. Time is advanced by hand (one 60 Hz tick per update); the short
// real sleep per tick only lets the loopback datagrams arrive.

#include <doctest/doctest.h>
#include <netcode.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>

namespace {

constexpr uint64_t kProtocolId = 0x426c7565536d6f6bULL;  // "BlueSmok"
constexpr uint64_t kClientId = 0x1234;
constexpr char kServerAddress[] = "127.0.0.1:47391";
constexpr double kTickSeconds = 1.0 / 60.0;
constexpr double kRealSleepSeconds = 0.001;
constexpr int kMaxTicks = 600;  // 10 simulated seconds
constexpr int kTokenTimeoutSeconds = 5;
// < 0: a token without expiry (netcode's development convenience), so the
// test does not depend on the wall clock relative to the server start.
constexpr int kTokenNoExpiry = -1;

constexpr std::array<uint8_t, NETCODE_KEY_BYTES> kTestPrivateKey = {
    0x60, 0x6a, 0xbe, 0x6e, 0xc9, 0x19, 0x10, 0xea, 0x9a, 0x65, 0x62,
    0xf6, 0x6f, 0x2b, 0x30, 0xe4, 0x43, 0x71, 0xd6, 0x2c, 0xd1, 0x99,
    0x27, 0x26, 0x6b, 0x3c, 0x60, 0xf4, 0xb7, 0x15, 0xab, 0xa1};

constexpr std::array<uint8_t, 5> kClientPayload = {'h', 'e', 'l', 'l', 'o'};
constexpr std::array<uint8_t, 6> kServerPayload = {'w', 'o', 'r',
                                                   'l', 'd', '!'};

// Initialises netcode for one test and shuts it down again.
class NetcodeLibrary {
 public:
  NetcodeLibrary() : ok_(netcode_init() == NETCODE_OK) {
    netcode_log_level(NETCODE_LOG_LEVEL_NONE);
  }
  NetcodeLibrary(const NetcodeLibrary&) = delete;
  NetcodeLibrary& operator=(const NetcodeLibrary&) = delete;
  ~NetcodeLibrary() {
    if (ok_) {
      netcode_term();
    }
  }

  bool ok() const { return ok_; }

 private:
  bool ok_;
};

struct ServerDeleter {
  void operator()(netcode_server_t* server) const {
    netcode_server_destroy(server);
  }
};
struct ClientDeleter {
  void operator()(netcode_client_t* client) const {
    netcode_client_destroy(client);
  }
};
using ServerPtr = std::unique_ptr<netcode_server_t, ServerDeleter>;
using ClientPtr = std::unique_ptr<netcode_client_t, ClientDeleter>;

template <size_t N>
bool SameBytes(const uint8_t* data, int bytes,
               const std::array<uint8_t, N>& expected) {
  return data != nullptr && bytes == static_cast<int>(N) &&
         std::memcmp(data, expected.data(), N) == 0;
}

TEST_CASE("netcode client and server exchange a payload on loopback") {
  NetcodeLibrary library;
  REQUIRE(library.ok());

  double time = 0.0;

  netcode_server_config_t server_config;
  netcode_default_server_config(&server_config);
  server_config.protocol_id = kProtocolId;
  std::memcpy(server_config.private_key, kTestPrivateKey.data(),
              NETCODE_KEY_BYTES);
  const ServerPtr server_owner(
      netcode_server_create(kServerAddress, &server_config, time));
  netcode_server_t* server = server_owner.get();
  if (server == nullptr) {
    MESSAGE("skipped: cannot bind ", kServerAddress,
            " (netcode server create error ", netcode_server_create_error(),
            ")");
    return;
  }
  netcode_server_start(server, 1);

  netcode_client_config_t client_config;
  netcode_default_client_config(&client_config);
  const ClientPtr client_owner(
      netcode_client_create("0.0.0.0", &client_config, time));
  netcode_client_t* client = client_owner.get();
  REQUIRE(client != nullptr);

  std::array<uint8_t, NETCODE_CONNECT_TOKEN_BYTES> connect_token{};
  std::array<uint8_t, NETCODE_USER_DATA_BYTES> user_data{};
  const char* addresses[] = {kServerAddress};
  REQUIRE(netcode_generate_connect_token(
              1, addresses, addresses, kTokenNoExpiry, kTokenTimeoutSeconds,
              kClientId, kProtocolId, kTestPrivateKey.data(), user_data.data(),
              connect_token.data()) == NETCODE_OK);
  netcode_client_connect(client, connect_token.data());

  bool server_got_payload = false;
  bool client_got_payload = false;
  for (int tick = 0; tick < kMaxTicks; ++tick) {
    netcode_client_update(client, time);
    netcode_server_update(server, time);

    if (netcode_client_state(client) == NETCODE_CLIENT_STATE_CONNECTED &&
        !server_got_payload) {
      netcode_client_send_packet(client, kClientPayload.data(),
                                 static_cast<int>(kClientPayload.size()));
    }
    if (netcode_server_client_connected(server, 0) && !client_got_payload) {
      netcode_server_send_packet(server, 0, kServerPayload.data(),
                                 static_cast<int>(kServerPayload.size()));
    }

    int bytes = 0;
    uint64_t sequence = 0;
    while (uint8_t* packet =
               netcode_server_receive_packet(server, 0, &bytes, &sequence)) {
      server_got_payload |= SameBytes(packet, bytes, kClientPayload);
      netcode_server_free_packet(server, packet);
    }
    while (uint8_t* packet =
               netcode_client_receive_packet(client, &bytes, &sequence)) {
      client_got_payload |= SameBytes(packet, bytes, kServerPayload);
      netcode_client_free_packet(client, packet);
    }
    if (server_got_payload && client_got_payload) {
      break;
    }

    netcode_sleep(kRealSleepSeconds);
    time += kTickSeconds;
  }

  CHECK(netcode_server_client_id(server, 0) == kClientId);
  CHECK(server_got_payload);
  CHECK(client_got_payload);

  netcode_client_disconnect(client);
  CHECK(netcode_client_state(client) == NETCODE_CLIENT_STATE_DISCONNECTED);
  // The client's disconnect packets reach the server on its next updates.
  for (int tick = 0;
       tick < kMaxTicks && netcode_server_num_connected_clients(server) > 0;
       ++tick) {
    netcode_sleep(kRealSleepSeconds);
    time += kTickSeconds;
    netcode_server_update(server, time);
  }
  CHECK(netcode_server_num_connected_clients(server) == 0);
}

}  // namespace
