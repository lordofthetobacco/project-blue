#ifndef BLUE_NET_CLIENT_SESSION_H_
#define BLUE_NET_CLIENT_SESSION_H_

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"
#include "net/address_resolver.h"
#include "net/clock_sync.h"
#include "net/interpolation.h"
#include "net/messages.h"
#include "net/net_settings.h"
#include "net/players.h"
#include "net/prediction.h"
#include "net/reliable_channel.h"
#include "net/replication.h"
#include "net/sequence.h"
#include "net/snapshot.h"
#include "net/transport.h"
#include "physics/character.h"
#include "physics/physics_world.h"
#include "player/player_controller.h"
#include "sim/player_sim.h"
#include "sim/tick_clock.h"
#include "sim/user_command.h"
#include "sim/weapon_table.h"
#include "weapon/weapon_def.h"

namespace blue::net {

// Where a ClientSession is in its connection.
enum class SessionState : uint8_t {
  kIdle,          // Never connected.
  kResolving,     // Looking up a host name; the transport is not started.
  kConnecting,    // netcode.io handshake.
  kHandshaking,   // Sending ClientHello, waiting for the welcome.
  kSyncing,       // Welcomed; measuring the round trip, awaiting own state.
  kPlaying,       // Predicting and sending commands every tick.
  kDisconnected,  // Ended; disconnect_reason() says why.
};

// Returns a short English name of `state`.
[[nodiscard]] std::string_view SessionStateText(SessionState state);
// Returns a sentence for ServerReject reason `reason` (reject_reason).
[[nodiscard]] std::string_view RejectReasonText(uint8_t reason);

// Why a connection attempt or session failed (not a deliberate Disconnect).
enum class FailureCause : uint8_t {
  kResolveFailed,       // The host name did not resolve.
  kConnectFailed,       // The transport could not start (bad address, socket).
  kNoReply,             // The server never answered the netcode handshake.
  kDenied,              // The server answered and refused the connection.
  kTokenExpired,        // The connect token expired before connecting.
  kInvalidToken,        // The connect token is malformed.
  kConnectionLost,      // An established connection went silent.
  kNoHandshakeAnswer,   // Netcode connected, no ServerWelcome / ServerReject.
  kTransportClosed,     // The transport left its states without a cause.
  kRejectedProtocol,    // ServerReject: another protocol version.
  kRejectedContent,     // ServerReject: another scene or other weapons.
  kRejectedFull,        // ServerReject: the server is full.
  kRejectedName,        // ServerReject: the player name was refused.
  kRejectedShuttingDown,  // ServerReject: the server is shutting down.
  kRejectedUnknown,     // ServerReject with an unknown reason code.
  kTickRateMismatch,    // The server ticks at another rate.
  kLocalError,          // The client could not use the server's state.
};

// What FailureText needs to name the failure; unused fields stay empty.
struct FailureContext {
  std::string_view typed_address;  // As the user typed it.
  std::string_view address;        // Numeric ip:port; empty before it is known.
  std::string_view detail;         // Cause specific text (lookup error, ...).
  uint64_t content_hash = 0;       // This client's.
  uint16_t server_protocol_version = 0;  // From a ServerReject.
};

// The two texts of a failure: a short sentence for the menu ("Disconnected:
// <reason>.") and the hint lines of what to check, for the log.
struct FailureText {
  std::string reason;
  // One line each: what happened, then one thing to check per line.
  std::vector<std::string> hint;
};

// Returns the texts of `cause`. The only place the hints are written.
[[nodiscard]] FailureText DescribeFailure(FailureCause cause,
                                          const FailureContext& context);
// Returns the cause of a transport that left kConnecting / kConnected.
[[nodiscard]] FailureCause FailureCauseFor(TransportState state);
// Returns the cause of ServerReject reason `reason` (reject_reason).
[[nodiscard]] FailureCause FailureCauseForReject(uint8_t reason);

// What the local player is predicted in: a physics world holding the scene
// (dynamic bodies kinematic), the weapon library and table the server's
// weapon keys map to, and the character and movement tuning the server
// uses. Everything is borrowed and must outlive the session.
struct PredictionContext {
  physics::PhysicsWorld* physics = nullptr;
  const weapon::WeaponLibrary* library = nullptr;
  const sim::WeaponTable* table = nullptr;
  physics::CharacterDesc character;
  player::PlayerTuning tuning;
};

// Tunables of a ClientSession.
struct SessionConfig {
  TransportConfig transport;
  std::array<uint8_t, kConnectTokenKeyBytes> connect_key = kDevConnectKey;
  // ClientHello resend interval and the give-up time of the handshake
  // (netcode connect plus hello).
  double hello_interval_seconds = 0.1;
  double handshake_timeout_seconds = 5.0;
  // Round-trip samples gathered after the welcome before playing, and the
  // longest wait for them (afterwards one sample is enough).
  int sync_round_trips = 4;
  double sync_timeout_seconds = 1.0;
  // Commands per ClientInput: the newest plus the previous ones.
  int command_redundancy = 8;
  ClockSyncConfig clock;
  InterpolationConfig interpolation;
  PredictionConfig prediction;
};

// Connection quality for the HUD.
struct NetStats {
  double round_trip_seconds = 0.0;
  double jitter_seconds = 0.0;
  // Share of our packets not acknowledged, over the last two seconds.
  double loss_percent = 0.0;
  double snapshots_per_second = 0.0;
  double replays_per_second = 0.0;
  double interpolation_delay_seconds = 0.0;
  float time_scale = 1.0f;
  float input_depth = 0.0f;  // smoothed input_buffer_depth
  uint32_t server_tick = 0;  // newest snapshot
  int resyncs = 0;
};

// Totals since Connect(), for diagnostics (blue-net-probe, tests).
struct SessionCounters {
  int64_t payloads = 0;            // received from the transport
  int64_t decode_errors = 0;       // payloads that failed to decode
  int64_t snapshots = 0;           // decoded ServerSnapshot packets
  int64_t stale_or_duplicate = 0;  // snapshots discarded by the ack window
  int64_t applied = 0;             // fresh snapshots applied
  int64_t missing_baselines = 0;   // players sections without our baseline
  int64_t inconsistent = 0;        // players sections contradicting baselines
  int64_t inputs_sent = 0;         // ClientInput packets
  // Sum and count of the reported input_buffer_depth while playing.
  int64_t depth_sum = 0;
  int64_t depth_samples = 0;
};

// The client side of a networked game: the state machine of a connection
// (netcode transport, Control handshake, sequenced ClientInput /
// ServerSnapshot exchange with acknowledgements and the reliable channel,
// baselines), prediction of the local player, clock sync and the
// interpolated replication of everything else. Single-threaded and driven
// by its caller once per frame:
//
//   session.Update(now);                      // receive, handshake, apply
//   const int ticks = session.AdvanceTicks(frame_seconds);
//   for (int i = 0; i < ticks; ++i) {
//     session.Tick(builder.Take(session.next_tick()), now);  // predict, send
//     physics.StepOnce();
//   }
//   session.UpdateView(now, frame_seconds);   // interpolate others
//
// Allocates its buffers at construction and on Connect(); nothing per frame
// in steady state (except when a new dropped weapon or body appears). Not
// thread-safe; neither copyable nor movable.
class ClientSession {
 public:
  ClientSession(const SessionConfig& config, const PredictionContext& context);
  ClientSession(const ClientSession&) = delete;
  ClientSession& operator=(const ClientSession&) = delete;
  ~ClientSession();

  // Starts connecting to `address` ("host:port") as `player_name` with the
  // content hash of the local scene and weapons (sim::SimWorld's). Replaces
  // any previous connection. A numeric address (IsNumericAddress) starts
  // the transport at once (kConnecting) and fails (state unchanged) when
  // it is bad; a host name is looked up on another thread first
  // (kResolving) and Update() then starts the transport or, when the
  // lookup fails, ends the session with a reason. Fails (state unchanged)
  // on a bad name; the error is a sentence for the menu.
  core::Result<void, std::string> Connect(std::string_view address,
                                          std::string_view player_name,
                                          uint64_t content_hash,
                                          uint64_t client_id,
                                          double now_seconds);
  // Ends the connection with `reason` (kDisconnected). Does nothing while
  // idle or already disconnected.
  void Disconnect(std::string_view reason);

  // Pumps the transport at `now_seconds` (a monotonic clock) and applies
  // everything received: handshake, snapshots (reconciliation, clock sync,
  // replication), reliable messages. Detects timeouts.
  void Update(double now_seconds);

  // Adds `frame_seconds` to the tick clock (dilated by clock sync) and
  // returns the ticks to run now; 0 unless playing.
  int AdvanceTicks(float frame_seconds);
  // Fraction of the next tick elapsed after the last AdvanceTicks().
  [[nodiscard]] float tick_alpha() const { return tick_alpha_; }

  // Predicts tick next_tick() under `command` (its tick is replaced) and
  // sends the ClientInput for it. Only while playing; returns no events
  // otherwise.
  sim::StepEvents Tick(const sim::UserCommand& command, double now_seconds);

  // Advances the render time of remote entities to `now_seconds`, moves
  // them there and fades the prediction's visual offset by
  // `frame_seconds`. Returns the render time.
  ViewTime UpdateView(double now_seconds, float frame_seconds);

  [[nodiscard]] SessionState state() const { return state_; }
  // Why the session ended (kDisconnected), or empty.
  [[nodiscard]] const std::string& disconnect_reason() const {
    return disconnect_reason_;
  }
  // The local player's id, once welcomed.
  [[nodiscard]] std::optional<uint8_t> player_id() const { return player_id_; }
  // The tick the next Tick() predicts.
  [[nodiscard]] uint32_t next_tick() const;
  // The predicted local player, once syncing found its state; null before.
  [[nodiscard]] const sim::PlayerSim* player() const { return player_.get(); }
  [[nodiscard]] const Prediction* prediction() const {
    return prediction_ ? &*prediction_ : nullptr;
  }
  [[nodiscard]] const Replication& replication() const { return replication_; }
  [[nodiscard]] const ClockSync& clock() const { return clock_; }
  [[nodiscard]] const InterpolationClock& interpolation() const {
    return interpolation_;
  }
  // The newest server tick seen in a snapshot.
  [[nodiscard]] std::optional<uint32_t> newest_server_tick() const {
    return newest_server_tick_;
  }
  [[nodiscard]] NetStats stats(double now_seconds) const;
  [[nodiscard]] const SessionCounters& counters() const { return counters_; }

 private:
  // A ClientInput we sent, for round trips and loss.
  struct SentInput {
    uint16_t sequence = 0;
    bool valid = false;
    bool acked = false;
    double time = 0.0;
  };

  // Resets all per-connection state for a new connection in `state`.
  void BeginConnection(const ClientHello& hello, SessionState state,
                       double now_seconds);
  // While resolving: polls the lookup. Returns true once the transport
  // connect has started (state kConnecting); false while the lookup runs or
  // when it failed (the session has ended).
  bool FinishResolve(double now_seconds);
  // Ends the connection on purpose with `reason` (logged at Info).
  void End(std::string_view reason);
  // Ends the connection because of `cause`: logs a warning with the state
  // and timing and a hint, and sets the reason for the menu. Fills in the
  // addresses and the content hash of `context`.
  void Fail(FailureCause cause, FailureContext context = {});
  // Moves to kDisconnected with `reason` and releases the connection.
  void Finish(std::string_view reason);
  // Handles one received payload.
  void Receive(std::span<const uint8_t> payload, double now_seconds);
  void ReceiveControl(std::span<const uint8_t> payload, double now_seconds);
  void ReceiveSnapshot(std::span<const uint8_t> payload, double now_seconds);
  // Applies the decoded snapshot_ (fresh) to the game state.
  void ApplySnapshot(double now_seconds);
  // Applies the own state of snapshot `server_tick`.
  void ApplyOwnState(uint32_t server_tick, const WirePlayerState& own);
  // Creates the predicted player from its first authoritative state.
  bool CreatePlayer(const WirePlayerState& own);
  // Starts playing once the clock knows enough.
  void TryStartPlaying(double now_seconds);
  // Sends one ClientInput carrying the newest commands ending at `tick`.
  void SendInput(double now_seconds);
  // While syncing: sends an idle command for the estimated server tick
  // every tick interval (the server needs commands to measure from).
  void SendSyncInput(double now_seconds);
  // Predicts the ticks before `tick` as the server runs ticks without a
  // fresh command: `repeated` without edges (its held buttons and angles).
  void PredictRepeatsUntil(uint32_t tick, sim::UserCommand repeated);
  // Applies a clock resync to tick `tick` (the next command's).
  void ResyncTo(uint32_t tick);

  SessionConfig config_;
  PredictionContext context_;
  NetcodeRuntime runtime_;
  Transport transport_;
  SessionState state_ = SessionState::kIdle;
  std::string disconnect_reason_;

  // Host name lookup (kResolving): the lookup and the client id to connect
  // with once it succeeds.
  std::unique_ptr<PendingResolve> pending_resolve_;
  uint64_t resolve_client_id_ = 0;

  // The server as typed and as dialled (numeric; empty until resolved), for
  // logs. attempt_start_ is when Connect() was called, last_now_ the newest
  // time the session saw.
  std::string typed_address_;
  std::string numeric_address_;
  double attempt_start_ = 0.0;
  double last_now_ = 0.0;
  bool decode_error_logged_ = false;  // Once per connection.

  // Handshake.
  ClientHello hello_;
  // When the transport connect started (not the host name lookup).
  double connect_time_ = 0.0;
  double last_hello_time_ = -1.0;
  std::optional<uint8_t> player_id_;
  std::optional<ServerWelcome> welcome_;
  double welcome_time_ = 0.0;

  // Sequenced exchange.
  AckTracker acks_;
  ReliableEndpoint reliable_;
  BaselineHistory baselines_;
  std::array<SentInput, kSentPacketHistory> sent_{};
  ServerSnapshot snapshot_;  // decode scratch, reused
  PlayerSet players_scratch_;
  std::optional<uint32_t> snapshot_ack_;
  std::optional<uint32_t> newest_server_tick_;
  // Newest own state while syncing.
  std::optional<WirePlayerState> sync_own_;
  uint32_t sync_own_tick_ = 0;
  double last_sync_input_time_ = 0.0;
  int round_trip_samples_ = 0;

  // Prediction and clocks.
  std::unique_ptr<sim::PlayerSim> player_;
  std::optional<Prediction> prediction_;
  sim::TickClock tick_clock_;
  float tick_alpha_ = 0.0f;
  // Due ticks still to be skipped after a backward resync (the server
  // already has commands for them).
  uint32_t hold_ticks_ = 0;
  ClockSync clock_;
  InterpolationClock interpolation_;
  Replication replication_;

  SessionCounters counters_;

  // Rates for the HUD, counted over windows of about a second.
  double rate_window_start_ = 0.0;
  int window_snapshots_ = 0;
  int window_replays_start_ = 0;
  double snapshots_per_second_ = 0.0;
  double replays_per_second_ = 0.0;
};

}  // namespace blue::net

#endif  // BLUE_NET_CLIENT_SESSION_H_
