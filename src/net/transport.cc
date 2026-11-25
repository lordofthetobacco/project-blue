#include "net/transport.h"

#include <netcode.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/result.h"
#include "net/protocol.h"

namespace blue::net {
namespace {

using TransportResult = core::Result<void, TransportError>;

struct ClientDeleter {
  void operator()(netcode_client_t* client) const {
    netcode_client_destroy(client);
  }
};
using ClientPtr = std::unique_ptr<netcode_client_t, ClientDeleter>;

TransportState MapState(int netcode_state) {
  switch (netcode_state) {
    case NETCODE_CLIENT_STATE_CONNECT_TOKEN_EXPIRED:
      return TransportState::kTokenExpired;
    case NETCODE_CLIENT_STATE_INVALID_CONNECT_TOKEN:
      return TransportState::kInvalidToken;
    case NETCODE_CLIENT_STATE_CONNECTION_TIMED_OUT:
      return TransportState::kTimedOut;
    case NETCODE_CLIENT_STATE_CONNECTION_RESPONSE_TIMED_OUT:
    case NETCODE_CLIENT_STATE_CONNECTION_REQUEST_TIMED_OUT:
      return TransportState::kConnectTimedOut;
    case NETCODE_CLIENT_STATE_CONNECTION_DENIED:
      return TransportState::kDenied;
    case NETCODE_CLIENT_STATE_SENDING_CONNECTION_REQUEST:
    case NETCODE_CLIENT_STATE_SENDING_CONNECTION_RESPONSE:
      return TransportState::kConnecting;
    case NETCODE_CLIENT_STATE_CONNECTED:
      return TransportState::kConnected;
    default:
      return TransportState::kDisconnected;
  }
}

// splitmix64: a small deterministic generator for the simulator.
class Random {
 public:
  explicit Random(uint64_t seed) : state_(seed) {}

  uint64_t Next() {
    state_ += 0x9E3779B97F4A7C15ULL;
    uint64_t z = state_;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
  }

  // Returns a uniform double in [0, 1).
  double Unit() {
    constexpr int kMantissaBits = 53;
    return static_cast<double>(Next() >> (64 - kMantissaBits)) /
           static_cast<double>(uint64_t{1} << kMantissaBits);
  }

  // Returns a uniform double in [-range, range].
  double Symmetric(double range) { return (Unit() * 2.0 - 1.0) * range; }

 private:
  uint64_t state_;
};

// A payload held back by the simulator until `deliver_at`.
struct DelayedPayload {
  double deliver_at = 0.0;
  std::vector<uint8_t> bytes;
};

}  // namespace

// --- NetcodeRuntime ----------------------------------------------------------

NetcodeRuntime::NetcodeRuntime() : ok_(netcode_init() == NETCODE_OK) {}

NetcodeRuntime::~NetcodeRuntime() {
  if (ok_) {
    netcode_term();
  }
}

// --- Transport::Impl ---------------------------------------------------------

class Transport::Impl {
 public:
  explicit Impl(const TransportConfig& config)
      : config_(config), random_(config.conditions.seed) {}
  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;

  TransportResult Connect(std::string_view server_address, uint64_t client_id,
                          std::span<const uint8_t, kConnectTokenKeyBytes> key);
  void Update(double time_seconds);
  TransportResult Send(std::span<const uint8_t> payload);
  std::optional<Payload> Receive();
  TransportState state() const;
  void Disconnect();
  void set_conditions(const NetworkConditions& conditions) {
    config_.conditions = conditions;
  }

  // Frees a packet netcode handed out through Receive().
  void FreePacket(uint8_t* packet) {
    if (client_ != nullptr) {
      netcode_client_free_packet(client_.get(), packet);
    }
  }

 private:
  // Queues `bytes` in `queue` with simulated loss, duplication and delay.
  void Enqueue(std::span<const uint8_t> bytes,
               std::vector<DelayedPayload>& queue);
  // Removes and returns the earliest payload of `queue` that is due.
  std::optional<std::vector<uint8_t>> TakeDue(
      std::vector<DelayedPayload>& queue);

  // Destroyed last: the client must go before netcode_term.
  NetcodeRuntime runtime_;
  TransportConfig config_;
  ClientPtr client_;
  double time_ = 0.0;
  Random random_;
  std::vector<DelayedPayload> outgoing_;
  std::vector<DelayedPayload> incoming_;
};

TransportResult Transport::Impl::Connect(
    std::string_view server_address, uint64_t client_id,
    std::span<const uint8_t, kConnectTokenKeyBytes> key) {
  if (!runtime_.ok()) {
    return TransportResult::Error(TransportError::kNetcodeInitFailed);
  }
  Disconnect();
  client_.reset();

  std::string address(server_address);
  netcode_address_t parsed;
  if (netcode_parse_address(address.c_str(), &parsed) != NETCODE_OK) {
    return TransportResult::Error(TransportError::kInvalidAddress);
  }
  const char* bind_address =
      parsed.type == NETCODE_ADDRESS_IPV6 ? "[::]" : "0.0.0.0";

  netcode_client_config_t client_config;
  netcode_default_client_config(&client_config);
  client_.reset(netcode_client_create(bind_address, &client_config, time_));
  if (client_ == nullptr) {
    return TransportResult::Error(TransportError::kClientCreateFailed);
  }

  std::array<uint8_t, NETCODE_KEY_BYTES> private_key = {};
  std::copy(key.begin(), key.end(), private_key.begin());
  std::array<uint8_t, NETCODE_USER_DATA_BYTES> user_data = {};
  std::array<uint8_t, NETCODE_CONNECT_TOKEN_BYTES> token = {};
  const char* addresses[] = {address.c_str()};
  if (netcode_generate_connect_token(
          1, addresses, addresses, config_.token_expire_seconds,
          config_.token_timeout_seconds, client_id, config_.protocol_id,
          private_key.data(), user_data.data(), token.data()) != NETCODE_OK) {
    client_.reset();
    return TransportResult::Error(TransportError::kTokenFailed);
  }
  netcode_client_connect(client_.get(), token.data());
  return TransportResult::Ok();
}

void Transport::Impl::Update(double time_seconds) {
  time_ = time_seconds;
  if (client_ == nullptr) {
    return;
  }
  netcode_client_update(client_.get(), time_);
  if (!config_.conditions.active() && outgoing_.empty() && incoming_.empty()) {
    return;
  }
  while (std::optional<std::vector<uint8_t>> due = TakeDue(outgoing_)) {
    netcode_client_send_packet(client_.get(), due->data(),
                               static_cast<int>(due->size()));
  }
  // With the simulator on, every arriving payload goes through its queue.
  if (config_.conditions.active()) {
    int bytes = 0;
    uint64_t sequence = 0;
    while (uint8_t* packet = netcode_client_receive_packet(client_.get(),
                                                           &bytes, &sequence)) {
      Enqueue(std::span<const uint8_t>(packet, static_cast<size_t>(bytes)),
              incoming_);
      netcode_client_free_packet(client_.get(), packet);
    }
  }
}

TransportResult Transport::Impl::Send(std::span<const uint8_t> payload) {
  if (payload.empty() || payload.size() > kMaxPayloadBytes) {
    return TransportResult::Error(TransportError::kInvalidPayloadSize);
  }
  if (state() != TransportState::kConnected) {
    return TransportResult::Error(TransportError::kNotConnected);
  }
  if (config_.conditions.active()) {
    Enqueue(payload, outgoing_);
    return TransportResult::Ok();
  }
  netcode_client_send_packet(client_.get(), payload.data(),
                             static_cast<int>(payload.size()));
  return TransportResult::Ok();
}

std::optional<Transport::Payload> Transport::Impl::Receive() {
  if (client_ == nullptr) {
    return std::nullopt;
  }
  if (std::optional<std::vector<uint8_t>> due = TakeDue(incoming_)) {
    return Payload(std::move(*due));
  }
  if (config_.conditions.active()) {
    return std::nullopt;
  }
  int bytes = 0;
  uint64_t sequence = 0;
  uint8_t* packet =
      netcode_client_receive_packet(client_.get(), &bytes, &sequence);
  if (packet == nullptr) {
    return std::nullopt;
  }
  return Payload(this, packet, static_cast<size_t>(bytes));
}

TransportState Transport::Impl::state() const {
  if (client_ == nullptr) {
    return TransportState::kDisconnected;
  }
  return MapState(netcode_client_state(client_.get()));
}

void Transport::Impl::Disconnect() {
  if (client_ != nullptr) {
    netcode_client_disconnect(client_.get());
  }
  outgoing_.clear();
  incoming_.clear();
}

void Transport::Impl::Enqueue(std::span<const uint8_t> bytes,
                              std::vector<DelayedPayload>& queue) {
  const NetworkConditions& conditions = config_.conditions;
  if (random_.Unit() * 100.0 < conditions.loss_percent) {
    return;
  }
  const int copies =
      random_.Unit() * 100.0 < conditions.duplicate_percent ? 2 : 1;
  for (int i = 0; i < copies; ++i) {
    const double delay =
        std::max(0.0, conditions.latency_seconds +
                          random_.Symmetric(conditions.jitter_seconds));
    queue.push_back(DelayedPayload{
        .deliver_at = time_ + delay,
        .bytes = std::vector<uint8_t>(bytes.begin(), bytes.end())});
  }
}

std::optional<std::vector<uint8_t>> Transport::Impl::TakeDue(
    std::vector<DelayedPayload>& queue) {
  auto earliest = queue.end();
  for (auto it = queue.begin(); it != queue.end(); ++it) {
    if (it->deliver_at <= time_ &&
        (earliest == queue.end() || it->deliver_at < earliest->deliver_at)) {
      earliest = it;
    }
  }
  if (earliest == queue.end()) {
    return std::nullopt;
  }
  std::vector<uint8_t> bytes = std::move(earliest->bytes);
  queue.erase(earliest);
  return bytes;
}

// --- Transport::Payload ------------------------------------------------------

Transport::Payload::Payload(Impl* owner, uint8_t* packet, size_t size)
    : owner_(owner), packet_(packet), bytes_(packet, size) {}

Transport::Payload::Payload(std::vector<uint8_t> copy)
    : copy_(std::move(copy)), bytes_(copy_) {}

// Moving the vector keeps its heap buffer, so `bytes_` stays valid for an
// owned copy as well as for a netcode packet.
Transport::Payload::Payload(Payload&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)),
      packet_(std::exchange(other.packet_, nullptr)),
      copy_(std::move(other.copy_)),
      bytes_(std::exchange(other.bytes_, {})) {}

Transport::Payload& Transport::Payload::operator=(Payload&& other) noexcept {
  if (this != &other) {
    Release();
    owner_ = std::exchange(other.owner_, nullptr);
    packet_ = std::exchange(other.packet_, nullptr);
    copy_ = std::move(other.copy_);
    bytes_ = std::exchange(other.bytes_, {});
  }
  return *this;
}

Transport::Payload::~Payload() { Release(); }

void Transport::Payload::Release() {
  if (owner_ != nullptr && packet_ != nullptr) {
    owner_->FreePacket(packet_);
  }
  owner_ = nullptr;
  packet_ = nullptr;
  copy_.clear();
  bytes_ = {};
}

// --- Transport ---------------------------------------------------------------

Transport::Transport() : Transport(TransportConfig{}) {}

Transport::Transport(const TransportConfig& config)
    : impl_(std::make_unique<Impl>(config)) {}

Transport::Transport(Transport&&) noexcept = default;
Transport& Transport::operator=(Transport&&) noexcept = default;
Transport::~Transport() = default;

core::Result<void, TransportError> Transport::Connect(
    std::string_view server_address, uint64_t client_id,
    std::span<const uint8_t, kConnectTokenKeyBytes> dev_private_key) {
  return impl_->Connect(server_address, client_id, dev_private_key);
}

void Transport::Update(double time_seconds) { impl_->Update(time_seconds); }

core::Result<void, TransportError> Transport::Send(
    std::span<const uint8_t> payload) {
  return impl_->Send(payload);
}

std::optional<Transport::Payload> Transport::Receive() {
  return impl_->Receive();
}

TransportState Transport::state() const { return impl_->state(); }

void Transport::Disconnect() { impl_->Disconnect(); }

void Transport::set_network_conditions(const NetworkConditions& conditions) {
  impl_->set_conditions(conditions);
}

// --- Text --------------------------------------------------------------------

std::string_view TransportStateText(TransportState state) {
  switch (state) {
    case TransportState::kDisconnected:
      return "disconnected";
    case TransportState::kConnecting:
      return "connecting";
    case TransportState::kConnected:
      return "connected";
    case TransportState::kConnectTimedOut:
      return "server did not answer";
    case TransportState::kTimedOut:
      return "connection timed out";
    case TransportState::kDenied:
      return "connection denied";
    case TransportState::kTokenExpired:
      return "connect token expired";
    case TransportState::kInvalidToken:
      return "connect token rejected";
  }
  return "unknown";
}

std::string_view TransportErrorText(TransportError error) {
  switch (error) {
    case TransportError::kNetcodeInitFailed:
      return "netcode initialisation failed";
    case TransportError::kInvalidAddress:
      return "invalid server address";
    case TransportError::kTokenFailed:
      return "could not create a connect token";
    case TransportError::kClientCreateFailed:
      return "could not open a local socket";
    case TransportError::kInvalidPayloadSize:
      return "payload size out of range";
    case TransportError::kNotConnected:
      return "not connected";
  }
  return "unknown";
}

}  // namespace blue::net
