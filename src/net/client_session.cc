#include "net/client_session.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "camera/look.h"
#include "core/log.h"
#include "core/result.h"
#include "net/address_resolver.h"
#include "net/clock_sync.h"
#include "net/interpolation.h"
#include "net/messages.h"
#include "net/packet.h"
#include "net/players.h"
#include "net/prediction.h"
#include "net/protocol.h"
#include "net/reliable_channel.h"
#include "net/sequence.h"
#include "net/snapshot.h"
#include "net/state_mapping.h"
#include "net/transport.h"
#include "net/user_command_codec.h"
#include "net/wire_error.h"
#include "sim/player_sim.h"
#include "sim/player_state.h"
#include "sim/user_command.h"
#include "weapon/loadout.h"

namespace blue::net {
namespace {

using ConnectResult = core::Result<void, std::string>;

// Length of the windows the HUD rates are counted over.
constexpr double kRateWindowSeconds = 1.0;
// Loss is measured over packets sent this long ago at most...
constexpr double kLossWindowSeconds = 2.0;
// ... and at least this long plus one round trip ago (their acks are due).
constexpr double kLossGraceSeconds = 0.1;

}  // namespace

std::string_view SessionStateText(SessionState state) {
  switch (state) {
    case SessionState::kIdle:
      return "idle";
    case SessionState::kResolving:
      return "resolving";
    case SessionState::kConnecting:
      return "connecting";
    case SessionState::kHandshaking:
      return "handshaking";
    case SessionState::kSyncing:
      return "syncing";
    case SessionState::kPlaying:
      return "playing";
    case SessionState::kDisconnected:
      return "disconnected";
  }
  return "unknown";
}

std::string_view RejectReasonText(uint8_t reason) {
  switch (reason) {
    case reject_reason::kProtocolMismatch:
      return "the server runs another protocol version";
    case reject_reason::kContentMismatch:
      return "the server runs another scene or other weapons";
    case reject_reason::kServerFull:
      return "the server is full";
    case reject_reason::kBadName:
      return "the server refused the player name";
    case reject_reason::kShuttingDown:
      return "the server is shutting down";
    default:
      return "the server refused the connection (unknown reason)";
  }
}

FailureCause FailureCauseFor(TransportState state) {
  switch (state) {
    case TransportState::kConnectTimedOut:
      return FailureCause::kNoReply;
    case TransportState::kTimedOut:
      return FailureCause::kConnectionLost;
    case TransportState::kDenied:
      return FailureCause::kDenied;
    case TransportState::kTokenExpired:
      return FailureCause::kTokenExpired;
    case TransportState::kInvalidToken:
      return FailureCause::kInvalidToken;
    case TransportState::kDisconnected:
    case TransportState::kConnecting:
    case TransportState::kConnected:
      break;
  }
  return FailureCause::kTransportClosed;
}

FailureCause FailureCauseForReject(uint8_t reason) {
  switch (reason) {
    case reject_reason::kProtocolMismatch:
      return FailureCause::kRejectedProtocol;
    case reject_reason::kContentMismatch:
      return FailureCause::kRejectedContent;
    case reject_reason::kServerFull:
      return FailureCause::kRejectedFull;
    case reject_reason::kBadName:
      return FailureCause::kRejectedName;
    case reject_reason::kShuttingDown:
      return FailureCause::kRejectedShuttingDown;
    default:
      return FailureCause::kRejectedUnknown;
  }
}

FailureText DescribeFailure(FailureCause cause,
                            const FailureContext& context) {
  const std::string_view address =
      context.address.empty() ? context.typed_address : context.address;
  switch (cause) {
    case FailureCause::kResolveFailed:
      return {std::format("cannot resolve '{}': {}", context.typed_address,
                          context.detail),
              {"The host name did not resolve.",
               "Check the host name for typos.",
               "Check that this machine has a working DNS, or connect by "
               "numeric ip:port."}};
    case FailureCause::kConnectFailed:
      return {std::format("cannot connect to '{}': {}", context.typed_address,
                          context.detail),
              {"The client could not start connecting.",
               "Check that the address is a valid ip:port.",
               "Check that a local UDP socket can be opened."}};
    case FailureCause::kNoReply:
      return {
          std::format("no reply from {}", address),
          {std::format("No reply over UDP from {}. A netcode server drops a "
                       "connection request silently when:",
                       address),
           "(a) the packet never arrives: the server is not running, the IP "
           "or port is wrong, the port is published as TCP instead of UDP, "
           "or a firewall blocks it;",
           std::format("(b) the connect token does not name the server's own "
                       "public address: the server's BLUE_PUBLIC_ADDR must be "
                       "exactly {};",
                       address),
           "(c) the token was sealed with another private key: the server's "
           "BLUE_PRIVATE_KEY differs from this client's development key;",
           "(d) the protocol id differs;",
           "(e) a client with this id or from this address is still "
           "connected on the server (a previous session that has not timed "
           "out yet), or the token was already used: wait a few seconds and "
           "retry.",
           "To find out which one: run the server with BLUE_NETCODE_LOG=3 "
           "(flag --netcode-log 3); it prints \"server ignored connection "
           "request. <cause>\" for each dropped request."}};
    case FailureCause::kDenied:
      return {std::format("{} refused the connection", address),
              {"The server answered and refused: all its client slots are "
               "taken (BLUE_MAX_PLAYERS)."}};
    case FailureCause::kTokenExpired:
      return {"the connect token expired",
              {"The token's lifetime ran out before the connection was made.",
               "Check for a slow network or a clock problem on this "
               "machine."}};
    case FailureCause::kInvalidToken:
      return {"the connect token was rejected",
              {"The connect token is malformed: a client bug; report it."}};
    case FailureCause::kConnectionLost:
      return {std::format("{} stopped answering", address),
              {"An established connection went silent.",
               "Check that the server is still running (see its log).",
               "Check that the network did not drop."}};
    case FailureCause::kNoHandshakeAnswer:
      return {std::format("{} did not answer the handshake", address),
              {"The server accepted the netcode connection but never "
               "answered ClientHello with ServerWelcome or ServerReject.",
               "Check the server log.",
               "Check that the server runs the same protocol version; an "
               "older one may drop the hello."}};
    case FailureCause::kTransportClosed:
      return {std::format("the connection to {} closed", address),
              {"The transport closed without a reason; check the server."}};
    case FailureCause::kRejectedProtocol:
      return {std::format("rejected by {}: {} (server {}, client {})", address,
                          RejectReasonText(reject_reason::kProtocolMismatch),
                          context.server_protocol_version, kProtocolVersion),
              {"Client and server are different builds.",
               "Update the one with the older protocol version."}};
    case FailureCause::kRejectedContent:
      return {std::format("rejected by {}: {}", address,
                          RejectReasonText(reject_reason::kContentMismatch)),
              {std::format("This client's content hash is {:016x}.",
                           context.content_hash),
               "The server must run the same cooked scene and weapons."}};
    case FailureCause::kRejectedFull:
      return {std::format("rejected by {}: {}", address,
                          RejectReasonText(reject_reason::kServerFull)),
              {"All player slots are taken; wait for a free slot."}};
    case FailureCause::kRejectedName:
      return {std::format("rejected by {}: {}", address,
                          RejectReasonText(reject_reason::kBadName)),
              {"Pick another player name (the server found it invalid or "
               "already taken)."}};
    case FailureCause::kRejectedShuttingDown:
      return {std::format("rejected by {}: {}", address,
                          RejectReasonText(reject_reason::kShuttingDown)),
              {"The server is stopping or restarting; try again shortly."}};
    case FailureCause::kRejectedUnknown:
      return {std::format("rejected by {}: {}", address, RejectReasonText(0)),
              {"The server sent a reject reason this client does not know.",
               "The server is probably newer."}};
    case FailureCause::kTickRateMismatch:
      return {std::string(context.detail),
              {"Client and server are different builds; run matching "
               "versions."}};
    case FailureCause::kLocalError:
      return {std::string(context.detail),
              {"The client could not use the server's state.",
               "Check that client and server run the same content and "
               "version."}};
  }
  return {"unknown failure", {}};
}

ClientSession::ClientSession(const SessionConfig& config,
                             const PredictionContext& context)
    : config_(config),
      context_(context),
      transport_(config.transport),
      reliable_(ReliableConfig{}),
      clock_(config.clock),
      interpolation_(config.interpolation) {}

ClientSession::~ClientSession() {
  // The prediction borrows the player, whose character lives in the
  // borrowed physics world.
  prediction_.reset();
  player_.reset();
  transport_.Disconnect();
}

ConnectResult ClientSession::Connect(std::string_view address,
                                     std::string_view player_name,
                                     uint64_t content_hash, uint64_t client_id,
                                     double now_seconds) {
  WireResult<Name> name = Name::Create(player_name);
  if (!name.ok() || name.value().view().empty()) {
    return ConnectResult::Error(std::format(
        "The player name must be 1 to {} bytes of text.", kNameMaxBytes));
  }
  if (!runtime_.ok()) {
    return ConnectResult::Error("Networking is not available.");
  }
  const ClientHello hello{.protocol_version = kProtocolVersion,
                          .content_hash = content_hash,
                          .name = name.value()};
  core::log::Info(
      "Connecting to '{}' as '{}' (protocol {}, content {:016x}, client "
      "{:016x})",
      address, player_name, kProtocolVersion, content_hash, client_id);
  if (IsNumericAddress(address)) {
    transport_.Update(now_seconds);
    const core::Result<void, TransportError> connected =
        transport_.Connect(address, client_id, config_.connect_key);
    if (!connected.ok()) {
      core::log::Warn("Cannot connect to '{}': {}", address,
                      TransportErrorText(connected.error()));
      return ConnectResult::Error(
          std::format("Cannot connect to '{}': {}.", address,
                      TransportErrorText(connected.error())));
    }
    BeginConnection(hello, SessionState::kConnecting, now_seconds);
    typed_address_ = std::string(address);
    numeric_address_ = typed_address_;
    return ConnectResult::Ok();
  }

  // A host name: look it up off the frame loop. The old connection ends now.
  transport_.Disconnect();
  BeginConnection(hello, SessionState::kResolving, now_seconds);
  typed_address_ = std::string(address);
  pending_resolve_ = std::make_unique<PendingResolve>(typed_address_);
  resolve_client_id_ = client_id;
  return ConnectResult::Ok();
}

void ClientSession::BeginConnection(const ClientHello& hello,
                                    SessionState state, double now_seconds) {
  prediction_.reset();
  player_.reset();
  pending_resolve_.reset();
  hello_ = hello;
  state_ = state;
  disconnect_reason_.clear();
  connect_time_ = now_seconds;
  attempt_start_ = now_seconds;
  last_now_ = now_seconds;
  typed_address_.clear();
  numeric_address_.clear();
  decode_error_logged_ = false;
  last_hello_time_ = -1.0;
  player_id_.reset();
  welcome_.reset();
  acks_ = AckTracker();
  reliable_ = ReliableEndpoint(ReliableConfig{});
  baselines_.Clear();
  sent_ = {};
  snapshot_ack_.reset();
  newest_server_tick_.reset();
  sync_own_.reset();
  round_trip_samples_ = 0;
  tick_clock_.Reset();
  tick_clock_.SetTimeScale(1.0f);
  tick_alpha_ = 0.0f;
  hold_ticks_ = 0;
  clock_.Reset();
  interpolation_.Reset();
  replication_.Reset(std::nullopt);
  counters_ = SessionCounters{};
  rate_window_start_ = now_seconds;
  window_snapshots_ = 0;
  window_replays_start_ = 0;
  snapshots_per_second_ = 0.0;
  replays_per_second_ = 0.0;
}

void ClientSession::End(std::string_view reason) {
  if (state_ == SessionState::kIdle || state_ == SessionState::kDisconnected) {
    return;
  }
  core::log::Info("Disconnected: {}", reason);
  Finish(reason);
}

void ClientSession::Fail(FailureCause cause, FailureContext context) {
  if (state_ == SessionState::kIdle || state_ == SessionState::kDisconnected) {
    return;
  }
  context.typed_address = typed_address_;
  context.address = numeric_address_;
  context.content_hash = hello_.content_hash;
  FailureText text = DescribeFailure(cause, context);
  core::log::Warn(
      "Connection failed: {} (state {}, server {}, {:.1f} s after connecting "
      "began, transport {})",
      text.reason, SessionStateText(state_),
      numeric_address_.empty() ? "unresolved" : numeric_address_,
      last_now_ - attempt_start_, TransportStateText(transport_.state()));
  for (size_t i = 0; i < text.hint.size(); ++i) {
    core::log::Warn("{}{}", i == 0 ? "Hint: " : "  ", text.hint[i]);
  }
  Finish(text.reason);
}

void ClientSession::Finish(std::string_view reason) {
  state_ = SessionState::kDisconnected;
  disconnect_reason_ = std::string(reason);
  transport_.Disconnect();
  pending_resolve_.reset();
  prediction_.reset();
  player_.reset();
}

void ClientSession::Disconnect(std::string_view reason) { End(reason); }

uint32_t ClientSession::next_tick() const {
  return prediction_ ? prediction_->next_tick() : 0;
}

bool ClientSession::FinishResolve(double now_seconds) {
  const std::optional<core::Result<std::string, ResolveError>> resolved =
      pending_resolve_->Poll();
  if (!resolved) {
    return false;
  }
  if (!resolved->ok()) {
    Fail(FailureCause::kResolveFailed,
         {.detail = ResolveErrorText(resolved->error())});
    return false;
  }
  numeric_address_ = resolved->value();
  core::log::Info("Resolved '{}' to {}", typed_address_, numeric_address_);
  transport_.Update(now_seconds);
  const core::Result<void, TransportError> connected = transport_.Connect(
      resolved->value(), resolve_client_id_, config_.connect_key);
  if (!connected.ok()) {
    Fail(FailureCause::kConnectFailed,
         {.detail = TransportErrorText(connected.error())});
    return false;
  }
  pending_resolve_.reset();
  state_ = SessionState::kConnecting;
  connect_time_ = now_seconds;  // The handshake clock starts here.
  return true;
}

void ClientSession::Update(double now_seconds) {
  if (state_ == SessionState::kIdle || state_ == SessionState::kDisconnected) {
    return;
  }
  last_now_ = now_seconds;
  if (state_ == SessionState::kResolving && !FinishResolve(now_seconds)) {
    return;
  }
  transport_.Update(now_seconds);
  const TransportState transport = transport_.state();
  if (transport != TransportState::kConnected &&
      transport != TransportState::kConnecting) {
    Fail(FailureCauseFor(transport));
    return;
  }
  if (transport == TransportState::kConnected &&
      state_ == SessionState::kConnecting) {
    state_ = SessionState::kHandshaking;
    core::log::Info(
        "Netcode connected to {} after {:.1f} s; sending ClientHello",
        numeric_address_, now_seconds - attempt_start_);
  }

  while (std::optional<Transport::Payload> payload = transport_.Receive()) {
    Receive(payload->bytes(), now_seconds);
    if (state_ == SessionState::kDisconnected) {
      return;
    }
  }

  if (state_ == SessionState::kConnecting ||
      state_ == SessionState::kHandshaking) {
    if (now_seconds - connect_time_ > config_.handshake_timeout_seconds) {
      Fail(state_ == SessionState::kConnecting
               ? FailureCause::kNoReply
               : FailureCause::kNoHandshakeAnswer);
      return;
    }
  }
  if (state_ == SessionState::kHandshaking &&
      (last_hello_time_ < 0.0 ||
       now_seconds - last_hello_time_ >= config_.hello_interval_seconds)) {
    std::array<uint8_t, kMaxPayloadBytes> buffer{};
    const WireResult<size_t> size = EncodeControl(hello_, buffer);
    if (size.ok()) {
      (void)transport_.Send(
          std::span<const uint8_t>(buffer.data(), size.value()));
    }
    last_hello_time_ = now_seconds;
  }
  if (state_ == SessionState::kSyncing) {
    SendSyncInput(now_seconds);
    TryStartPlaying(now_seconds);
  }

  if (now_seconds - rate_window_start_ >= kRateWindowSeconds) {
    const double window = now_seconds - rate_window_start_;
    const int replays = prediction_ ? prediction_->replays() : 0;
    snapshots_per_second_ = window_snapshots_ / window;
    replays_per_second_ = (replays - window_replays_start_) / window;
    window_snapshots_ = 0;
    window_replays_start_ = replays;
    rate_window_start_ = now_seconds;
  }
}

void ClientSession::Receive(std::span<const uint8_t> payload,
                            double now_seconds) {
  ++counters_.payloads;
  const WireResult<PacketKind> kind = PeekPacketKind(payload);
  if (!kind.ok()) {
    ++counters_.decode_errors;
    return;  // Garbage: netcode authenticated it, so ignore quietly.
  }
  switch (kind.value()) {
    case PacketKind::kControl:
      ReceiveControl(payload, now_seconds);
      return;
    case PacketKind::kServerSnapshot:
      ReceiveSnapshot(payload, now_seconds);
      return;
    case PacketKind::kClientInput:
      return;
  }
}

void ClientSession::ReceiveControl(std::span<const uint8_t> payload,
                                   double now_seconds) {
  const WireResult<Message> message = DecodeControl(payload);
  if (!message.ok()) {
    ++counters_.decode_errors;
    if (state_ == SessionState::kHandshaking && !decode_error_logged_) {
      decode_error_logged_ = true;  // Once per connection.
      core::log::Warn(
          "Control message from {} not decoded during the handshake: {} "
          "(further ones are only counted)",
          numeric_address_, WireErrorText(message.error()));
    }
    return;
  }
  if (const auto* reject = std::get_if<ServerReject>(&message.value())) {
    if (state_ == SessionState::kHandshaking) {
      core::log::Warn("ServerReject from {}: reason {} ({}), server protocol {}",
                      numeric_address_, reject->reason,
                      RejectReasonText(reject->reason),
                      reject->server_protocol_version);
      Fail(FailureCauseForReject(reject->reason),
           {.server_protocol_version = reject->server_protocol_version});
    } else {
      core::log::Warn(
          "Ignored ServerReject (reason {}, {}) in state {}: the session is "
          "not waiting for a welcome",
          reject->reason, RejectReasonText(reject->reason),
          SessionStateText(state_));
    }
    return;
  }
  const auto* welcome = std::get_if<ServerWelcome>(&message.value());
  if (welcome == nullptr || state_ != SessionState::kHandshaking) {
    return;  // Repeated welcomes after the first are expected.
  }
  if (welcome->tick_rate != kDefaultTickRate) {
    const std::string detail =
        std::format("the server ticks at {} Hz; this client needs {} Hz",
                    welcome->tick_rate, kDefaultTickRate);
    Fail(FailureCause::kTickRateMismatch, {.detail = detail});
    return;
  }
  welcome_ = *welcome;
  welcome_time_ = now_seconds;
  player_id_ = welcome->player_id;
  replication_.Reset(welcome->player_id);
  state_ = SessionState::kSyncing;
  last_sync_input_time_ = now_seconds - sim::kTickSeconds;
  core::log::Info("Welcomed as player {} at server tick {}", welcome->player_id,
                  welcome->server_tick);
}

void ClientSession::ReceiveSnapshot(std::span<const uint8_t> payload,
                                    double now_seconds) {
  if (state_ != SessionState::kSyncing && state_ != SessionState::kPlaying) {
    return;  // Before the welcome: never acknowledged, nothing is lost.
  }
  const WireResult<void> decoded = DecodeServerSnapshot(payload, snapshot_);
  if (!decoded.ok()) {
    ++counters_.decode_errors;
    core::log::Warn("ServerSnapshot not decoded: {}",
                    WireErrorText(decoded.error()));
    return;
  }
  ++counters_.snapshots;
  const ReceiveOutcome outcome = acks_.OnReceive(snapshot_.header);
  for (const uint16_t sequence : outcome.newly_acked()) {
    reliable_.OnPacketAcked(sequence);
    SentInput& sent = sent_[sequence % sent_.size()];
    if (sent.valid && sent.sequence == sequence && !sent.acked) {
      sent.acked = true;
      clock_.AddRoundTrip(now_seconds - sent.time);
      ++round_trip_samples_;
    }
  }
  if (outcome.receipt != Receipt::kFresh) {
    ++counters_.stale_or_duplicate;
    return;
  }
  ++counters_.applied;
  if (!reliable_.ReceiveSection(snapshot_.reliable).ok()) {
    Fail(FailureCause::kLocalError,
         {.detail = "protocol violation (reliable window)"});
    return;
  }
  while (std::optional<Message> message = reliable_.Pop()) {
    replication_.Apply(*message, now_seconds);
  }
  ApplySnapshot(now_seconds);
}

void ClientSession::ApplySnapshot(double now_seconds) {
  const uint32_t server_tick = snapshot_.info.server_tick;
  const bool newest =
      !newest_server_tick_ || TickGreater(server_tick, *newest_server_tick_);
  if (newest) {
    newest_server_tick_ = server_tick;
  }
  ++window_snapshots_;
  interpolation_.OnSnapshot(server_tick, now_seconds);

  const PlayerSet* baseline = nullptr;
  if (snapshot_.info.baseline_tick.has_value()) {
    baseline = baselines_.Find(*snapshot_.info.baseline_tick);
  }
  const WireResult<void> players =
      snapshot_.players.Apply(baseline, players_scratch_);
  if (!players.ok()) {
    if (players.error() == WireError::kMissingBaseline) {
      ++counters_.missing_baselines;
    } else {
      ++counters_.inconsistent;
    }
  } else {
    baselines_.Store(server_tick, players_scratch_);
    if (!snapshot_ack_ || TickGreater(server_tick, *snapshot_ack_)) {
      snapshot_ack_ = server_tick;
    }
    replication_.ApplyPlayers(server_tick, players_scratch_);
  }
  replication_.ApplyObjects(server_tick, snapshot_.objects.objects());

  if (snapshot_.own.has_value()) {
    ApplyOwnState(server_tick, *snapshot_.own);
  }
  if (state_ == SessionState::kPlaying && newest) {
    counters_.depth_sum += snapshot_.info.input_buffer_depth;
    ++counters_.depth_samples;
    const ClockAdjust adjust =
        clock_.OnSnapshot(server_tick, snapshot_.info.input_buffer_depth,
                          next_tick() - hold_ticks_);
    tick_clock_.SetTimeScale(adjust.time_scale);
    if (adjust.resync_tick.has_value()) {
      ResyncTo(*adjust.resync_tick);
    }
  }
}

void ClientSession::ApplyOwnState(uint32_t server_tick,
                                  const WirePlayerState& own) {
  if (state_ == SessionState::kSyncing) {
    if (!sync_own_ || TickGreater(server_tick, sync_own_tick_)) {
      sync_own_ = own;
      sync_own_tick_ = server_tick;
    }
    return;
  }
  if (state_ != SessionState::kPlaying || !prediction_) {
    return;
  }
  (void)prediction_->Reconcile(server_tick, FromWire(own));
}

bool ClientSession::CreatePlayer(const WirePlayerState& own) {
  if (context_.physics == nullptr || context_.library == nullptr ||
      context_.table == nullptr || !player_id_) {
    Fail(FailureCause::kLocalError,
         {.detail = "no world to predict the player in"});
    return false;
  }
  auto created = sim::PlayerSim::Create(
      *context_.physics, *context_.library, *context_.table, *player_id_,
      own.position, camera::LookAngles{.yaw = own.yaw, .pitch = own.pitch},
      weapon::Loadout(), context_.character, context_.tuning);
  if (!created.ok()) {
    Fail(FailureCause::kLocalError,
         {.detail = "the player character could not be created"});
    return false;
  }
  player_ = std::move(created).value();
  prediction_.emplace(*player_, config_.prediction);
  return true;
}

void ClientSession::TryStartPlaying(double now_seconds) {
  if (!sync_own_ || round_trip_samples_ < 1) {
    return;
  }
  const bool enough =
      round_trip_samples_ >= config_.sync_round_trips ||
      now_seconds - welcome_time_ >= config_.sync_timeout_seconds;
  if (!enough) {
    return;
  }
  if (!player_ && !CreatePlayer(*sync_own_)) {
    return;
  }
  const uint32_t start = clock_.StartTick(*newest_server_tick_);
  // The state is the server's at sync_own_tick_. The server runs the ticks
  // up to the start with the idle sync command (repeated); so does the
  // prediction.
  const bool restored =
      prediction_->Reset(FromWire(*sync_own_), sync_own_tick_);
  sim::UserCommand idle;
  idle.yaw = sync_own_->yaw;
  idle.pitch = sync_own_->pitch;
  PredictRepeatsUntil(start, idle);
  if (!restored) {
    Fail(FailureCause::kLocalError,
         {.detail = "the server's weapons are unknown to this client"});
    return;
  }
  sync_own_.reset();
  tick_clock_.Reset();
  clock_.Start(start);
  state_ = SessionState::kPlaying;
  core::log::Info("Playing from tick {} (server {}, round trip {:.0f} ms)",
                  start, *newest_server_tick_, clock_.round_trip() * 1000.0);
}

void ClientSession::SendSyncInput(double now_seconds) {
  if (now_seconds - last_sync_input_time_ < sim::kTickSeconds || !welcome_) {
    return;
  }
  last_sync_input_time_ = now_seconds;
  // The server's tick now, estimated from the newest snapshot (or the
  // welcome), plus a little: an idle command it can consume and ack.
  const uint32_t base =
      newest_server_tick_ ? *newest_server_tick_ : welcome_->server_tick;
  sim::UserCommand idle;
  idle.tick = base + 2;
  if (sync_own_) {
    idle.yaw = sync_own_->yaw;
    idle.pitch = sync_own_->pitch;
  }
  ClientInput input;
  input.header = acks_.NextHeader();
  input.snapshot_ack = snapshot_ack_;
  (void)input.commands.Push(Canonical(idle));
  reliable_.FillSection(input.header.sequence, now_seconds, input.reliable);
  std::array<uint8_t, kMaxPayloadBytes> buffer{};
  const WireResult<size_t> size = EncodeClientInput(input, buffer);
  if (!size.ok()) {
    return;
  }
  SentInput& sent = sent_[input.header.sequence % sent_.size()];
  sent = SentInput{
      .sequence = input.header.sequence, .valid = true, .time = now_seconds};
  (void)transport_.Send(std::span<const uint8_t>(buffer.data(), size.value()));
}

void ClientSession::ResyncTo(uint32_t tick) {
  const uint32_t next = next_tick();
  core::log::Info(
      "Clock resync: next command tick {} -> {} (server tick {}, depth "
      "{:.1f}, round trip {:.0f} ms)",
      next, tick, newest_server_tick_.value_or(0), clock_.smoothed_depth(),
      clock_.round_trip() * 1000.0);
  if (TickGreater(tick, next)) {
    // Jump ahead; the server repeats the last command for skipped ticks.
    hold_ticks_ = 0;
    const sim::UserCommand* last = prediction_->CommandAt(next - 1);
    PredictRepeatsUntil(tick, last != nullptr ? *last : sim::UserCommand{});
  } else {
    // The server already has commands up to next - 1: wait for it.
    hold_ticks_ = next - tick;
  }
  tick_clock_.SetTimeScale(clock_.time_scale());
}

void ClientSession::PredictRepeatsUntil(uint32_t tick,
                                        sim::UserCommand repeated) {
  // What the server does on a tick without a fresh command (protocol.md
  // section 9.1): the previous held buttons and angles, no edges, no
  // selection, no interaction.
  repeated.pressed = 0;
  repeated.select_slot = -1;
  repeated.interact_kind = 0;
  repeated.interact_id = 0;
  for (int i = 0;
       i < Prediction::kHistory && TickGreater(tick, prediction_->next_tick());
       ++i) {
    (void)prediction_->Step(repeated);
  }
  prediction_->SkipTo(tick);  // Only beyond the history.
}

int ClientSession::AdvanceTicks(float frame_seconds) {
  if (state_ != SessionState::kPlaying) {
    tick_alpha_ = 0.0f;
    return 0;
  }
  const sim::TickAdvance advance = tick_clock_.Advance(frame_seconds);
  tick_alpha_ = advance.alpha;
  int ticks = advance.ticks;
  const auto held = static_cast<uint32_t>(ticks);
  const uint32_t skipped = std::min(hold_ticks_, held);
  hold_ticks_ -= skipped;
  ticks -= static_cast<int>(skipped);
  return ticks;
}

sim::StepEvents ClientSession::Tick(const sim::UserCommand& command,
                                    double now_seconds) {
  if (state_ != SessionState::kPlaying || !prediction_) {
    return {};
  }
  const sim::StepEvents events = prediction_->Step(command);
  SendInput(now_seconds);
  return events;
}

void ClientSession::SendInput(double now_seconds) {
  ClientInput input;
  input.header = acks_.NextHeader();
  input.snapshot_ack = snapshot_ack_;
  if (interpolation_.started()) {
    const ViewTime view = interpolation_.render_time();
    input.view_tick = view.tick;
    input.view_fraction = view.fraction_256();
  }
  const int count =
      std::clamp(config_.command_redundancy, 1, kMaxCommandsPerPacket);
  const uint32_t newest = prediction_->newest_tick();
  for (int i = 0; i < count; ++i) {
    const sim::UserCommand* command =
        prediction_->CommandAt(newest - static_cast<uint32_t>(i));
    if (command == nullptr ||
        command->tick != newest - static_cast<uint32_t>(i)) {
      break;
    }
    if (!input.commands.Push(*command).ok()) {
      break;
    }
  }
  if (input.commands.empty()) {
    return;
  }
  reliable_.FillSection(input.header.sequence, now_seconds, input.reliable);
  std::array<uint8_t, kMaxPayloadBytes> buffer{};
  const WireResult<size_t> size = EncodeClientInput(input, buffer);
  if (!size.ok()) {
    core::log::Warn("ClientInput not encoded: {}", WireErrorText(size.error()));
    return;
  }
  sent_[input.header.sequence % sent_.size()] = SentInput{
      .sequence = input.header.sequence, .valid = true, .time = now_seconds};
  ++counters_.inputs_sent;
  (void)transport_.Send(std::span<const uint8_t>(buffer.data(), size.value()));
}

ViewTime ClientSession::UpdateView(double now_seconds, float frame_seconds) {
  const ViewTime time = interpolation_.Advance(now_seconds);
  replication_.Sample(time, config_.interpolation.max_extrapolation_ticks);
  if (prediction_) {
    prediction_->DecayOffset(frame_seconds);
  }
  return time;
}

NetStats ClientSession::stats(double now_seconds) const {
  NetStats stats;
  stats.round_trip_seconds = clock_.round_trip();
  stats.jitter_seconds = clock_.jitter();
  stats.snapshots_per_second = snapshots_per_second_;
  stats.replays_per_second = replays_per_second_;
  stats.interpolation_delay_seconds =
      interpolation_.delay_ticks() * config_.interpolation.tick_seconds;
  stats.time_scale = tick_clock_.time_scale();
  stats.input_depth = clock_.smoothed_depth();
  stats.server_tick = newest_server_tick_.value_or(0);
  stats.resyncs = clock_.resyncs();
  const double newest_due =
      now_seconds - clock_.round_trip() - kLossGraceSeconds;
  int sent = 0;
  int acked = 0;
  for (const SentInput& input : sent_) {
    if (input.valid && input.time <= newest_due &&
        input.time >= now_seconds - kLossWindowSeconds) {
      ++sent;
      acked += input.acked ? 1 : 0;
    }
  }
  if (sent > 0) {
    stats.loss_percent = 100.0 * (sent - acked) / sent;
  }
  return stats;
}

}  // namespace blue::net
