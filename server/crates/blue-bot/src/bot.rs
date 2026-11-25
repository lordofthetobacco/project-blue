//! One headless client: netcode connection, handshake, scripted random
//! input at 60 Hz with clock adjustment from `input_buffer_depth`, snapshot
//! decoding with baselines, prediction with the stand-in movement model and
//! invariant checks.

use blue_native::netcode::{
    self, CONNECT_TOKEN_BYTES, ClientState, KEY_BYTES, NetcodeClient, USER_DATA_BYTES,
};
use blue_protocol::command::{held, pressed};
use blue_protocol::message::{MAX_PLAYERS, Name};
use blue_protocol::packet::{self, BASELINE_HISTORY};
use blue_protocol::seq::{AckedList, Receipt, seq_greater, tick_greater};
use blue_protocol::{
    AckTracker, ClientInput, CommandList, Message, PROTOCOL_VERSION, PlayerSet, PlayerState,
    ReliableConfig, ReliableEndpoint, ReliableSection, ServerSnapshot, UserCommand,
};
use blue_server::standin::step_player;
use blue_server::{DEV_TOKEN_EXPIRE_SECONDS, NETCODE_PROTOCOL_ID, TOKEN_TIMEOUT_SECONDS};

use crate::conditioner::{Conditioner, Conditions, Rng};

const RING: usize = 128;
const BASELINES: usize = BASELINE_HISTORY as usize;
/// Commands per packet (the newest plus redundant older ones).
const REDUNDANT_COMMANDS: u32 = 8;
const HELLO_INTERVAL: f64 = 0.1;
const HANDSHAKE_TIMEOUT: f64 = 5.0;
/// A remote player counts as moving once it is this far from where it was
/// first seen.
const MOVE_THRESHOLD: f32 = 0.5;
/// Position error that counts as a misprediction.
const CORRECTION_EPSILON: f32 = 0.01;
/// Frames between clock adjustments.
/// Fewest frames between clock adjustments (also at least one round trip,
/// so an adjustment is seen in the depth before the next one).
const ADJUST_INTERVAL: u32 = 20;
/// Steering band around the target depth, ticks.
const DEPTH_BAND: f64 = 0.5;

/// Static settings of one bot.
#[derive(Clone, Debug)]
pub struct BotSettings {
    /// Server address, `host:port` (also the address in the token).
    pub server: String,
    /// Display name.
    pub name: String,
    /// netcode client id (unique per connection).
    pub client_id: u64,
    /// Key to mint the connect token with.
    pub private_key: [u8; KEY_BYTES],
    /// Content hash to present.
    pub content_hash: u64,
    /// Client -> server conditions.
    pub up: Conditions,
    /// Server -> client conditions.
    pub down: Conditions,
    /// Random seed (input script and conditioners).
    pub seed: u64,
    /// Aim at the nearest other player and fire in bursts.
    pub aim: bool,
}

/// What a bot observed.
#[derive(Clone, Debug, Default)]
pub struct BotStats {
    /// Got a `ServerWelcome`.
    pub welcomed: bool,
    /// Player id from the welcome.
    pub player_id: Option<u8>,
    /// The first fatal problem.
    pub failure: Option<String>,
    /// Snapshots decoded and applied.
    pub snapshots: u64,
    /// Snapshot bytes received.
    pub snapshot_bytes: u64,
    /// Largest snapshot.
    pub max_snapshot: usize,
    /// Snapshot sequence numbers never received (lost, or arrived more
    /// than 32 packets late).
    pub snapshot_gaps: u64,
    /// Snapshots that arrived after a newer one.
    pub reordered: u64,
    /// Sequence numbers expected.
    pub snapshot_expected: u64,
    /// Payloads that failed to decode.
    pub decode_errors: u64,
    /// Delta snapshots against a baseline the bot did not have.
    pub missing_baselines: u64,
    /// In-order snapshots whose tick did not advance.
    pub tick_regressions: u64,
    /// Own-state mispredictions (position error > 1 cm).
    pub corrections: u64,
    /// Round-trip samples, seconds.
    pub rtt: Vec<f64>,
    /// Sum of reported input buffer depths.
    pub depth_sum: i64,
    /// Depth samples.
    pub depth_samples: u64,
    /// Newest server tick seen.
    pub newest_tick: Option<u32>,
    /// Newest `last_command_tick` seen.
    pub last_command_tick: Option<u32>,
    /// Bit i: player i was seen moving.
    pub seen_moving: u64,
    /// Bit i: player i was announced by `PlayerJoined`.
    pub joined: u64,
    /// `WorldSyncDone` arrived.
    pub sync_done: bool,
    /// Reliable messages received.
    pub messages: u64,
    /// `Damage` messages received.
    pub damage: u64,
    /// `Death` messages received.
    pub deaths: u64,
    /// `Shot` messages received.
    pub shots: u64,
    /// `DroppedRemoved` messages received.
    pub dropped_removed: u64,
    /// Deaths of this bot without a killer (world / kill plane).
    pub world_deaths: u64,
    /// Lowest own feet height seen in a snapshot.
    pub lowest_y: Option<f32>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Phase {
    Connecting,
    Hello,
    Welcomed,
    Failed,
}

/// One bot.
pub struct Bot {
    settings: BotSettings,
    net: NetcodeClient,
    phase: Phase,
    rng: Rng,
    up: Conditioner,
    down: Conditioner,
    started: f64,
    last_hello: f64,
    first_hello: Option<f64>,
    hello_count: u32,
    acks: AckTracker,
    reliable: ReliableEndpoint,
    section: Box<ReliableSection>,
    snapshot: Box<ServerSnapshot>,
    baselines: Box<[(Option<u32>, PlayerSet); BASELINES]>,
    applied: Box<PlayerSet>,
    snapshot_ack: Option<u32>,
    newest_seq: Option<u16>,
    newest_seq_tick: u32,
    send_times: Box<[(u16, f64); 256]>,
    next_tick: u32,
    first_tick: u32,
    commands: Box<[UserCommand; RING]>,
    predicted: Box<[(Option<u32>, PlayerState); RING]>,
    synced: bool,
    state: PlayerState,
    buttons: u32,
    next_change: f64,
    yaw: f32,
    depth_avg: f64,
    frames_since_adjust: u32,
    adjust_interval: u32,
    /// Input buffer depth to steer towards: 1 tick plus the jitter.
    depth_target: f64,
    first_seen: [Option<[f32; 3]>; MAX_PLAYERS],
    /// Own state of the newest snapshot (aiming reference).
    snapshot_own: Option<PlayerState>,
    /// Everything observed.
    pub stats: BotStats,
}

impl std::fmt::Debug for Bot {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Bot")
            .field("name", &self.settings.name)
            .field("phase", &self.phase)
            .finish_non_exhaustive()
    }
}

fn fold(seed: u64, salt: u64) -> u64 {
    Rng::new(seed ^ salt.wrapping_mul(0x9E37_79B9_7F4A_7C15)).next_u64()
}

impl Bot {
    /// Creates the client and starts connecting at time `now`.
    ///
    /// # Errors
    /// Token or socket errors.
    pub fn new(settings: BotSettings, now: f64) -> Result<Self, String> {
        let user = [0u8; USER_DATA_BYTES];
        let token: [u8; CONNECT_TOKEN_BYTES] = netcode::generate_connect_token(
            &[settings.server.as_str()],
            DEV_TOKEN_EXPIRE_SECONDS,
            TOKEN_TIMEOUT_SECONDS,
            settings.client_id,
            NETCODE_PROTOCOL_ID,
            &settings.private_key,
            &user,
        )
        .map_err(|e| format!("token for {}: {e}", settings.server))?;
        let mut net = NetcodeClient::new("0.0.0.0", now).map_err(|e| e.to_string())?;
        net.connect(&token);
        let seed = settings.seed;
        // Jitter needs a deeper buffer to avoid late commands.
        let depth_target = 1.0 + settings.up.jitter * 60.0;
        Ok(Self {
            up: Conditioner::new(settings.up, fold(seed, 1)),
            down: Conditioner::new(settings.down, fold(seed, 2)),
            rng: Rng::new(fold(seed, 3)),
            settings,
            net,
            phase: Phase::Connecting,
            started: now,
            last_hello: f64::NEG_INFINITY,
            first_hello: None,
            hello_count: 0,
            acks: AckTracker::new(),
            reliable: ReliableEndpoint::new(ReliableConfig::default()),
            section: Box::default(),
            snapshot: Box::default(),
            baselines: Box::new([(None, PlayerSet::default()); BASELINES]),
            applied: Box::default(),
            snapshot_ack: None,
            newest_seq: None,
            newest_seq_tick: 0,
            send_times: Box::new([(0, f64::NAN); 256]),
            next_tick: 0,
            first_tick: 0,
            commands: Box::new([UserCommand::default(); RING]),
            predicted: Box::new([(None, PlayerState::default()); RING]),
            synced: false,
            state: PlayerState::default(),
            buttons: 0,
            next_change: now,
            yaw: 0.0,
            depth_avg: depth_target,
            frames_since_adjust: 0,
            adjust_interval: ADJUST_INTERVAL,
            depth_target,
            first_seen: [None; MAX_PLAYERS],
            snapshot_own: None,
            stats: BotStats::default(),
        })
    }

    /// True while connected and welcomed (or still handshaking) without a
    /// failure.
    #[must_use]
    pub fn healthy(&self) -> bool {
        self.phase != Phase::Failed
    }

    fn fail(&mut self, why: String) {
        if self.stats.failure.is_none() {
            self.stats.failure = Some(why);
        }
        self.phase = Phase::Failed;
    }

    /// Runs one 60 Hz frame at time `now`.
    pub fn frame(&mut self, now: f64) {
        if self.phase == Phase::Failed {
            return;
        }
        self.net.update(now);
        let state = self.net.state();
        if state.is_error()
            || (state == ClientState::Disconnected && self.phase != Phase::Connecting)
        {
            self.fail(format!("netcode state {state:?}"));
            return;
        }
        if self.phase == Phase::Connecting {
            if state == ClientState::Connected {
                self.phase = Phase::Hello;
            } else if now - self.started > HANDSHAKE_TIMEOUT {
                self.fail("netcode connect timed out".to_owned());
                return;
            } else {
                return;
            }
        }
        // netcode -> conditioner (down).
        let mut buf = [0u8; 1200];
        while let Some(n) = self.net.receive(&mut buf) {
            if n <= buf.len() {
                self.down.push(now, &buf[..n]);
            } else {
                self.stats.decode_errors += 1;
            }
        }
        while let Some(n) = self.down.pop(now, &mut buf) {
            self.on_payload(&buf[..n], now);
            if self.phase == Phase::Failed {
                return;
            }
        }
        match self.phase {
            Phase::Hello => {
                if now - self.last_hello >= HELLO_INTERVAL {
                    self.send_hello(now);
                }
                if now - self.started > HANDSHAKE_TIMEOUT {
                    self.fail("no welcome".to_owned());
                    return;
                }
            }
            Phase::Welcomed => self.send_input(now),
            Phase::Connecting | Phase::Failed => {}
        }
        // conditioner (up) -> netcode.
        while let Some(n) = self.up.pop(now, &mut buf) {
            let _ = self.net.send(&buf[..n]);
        }
    }

    fn send_hello(&mut self, now: f64) {
        self.last_hello = now;
        self.first_hello.get_or_insert(now);
        self.hello_count += 1;
        let name = Name::new(&self.settings.name).unwrap_or(Name::EMPTY);
        let hello = Message::ClientHello {
            protocol_version: PROTOCOL_VERSION,
            content_hash: self.settings.content_hash,
            name,
        };
        let mut buf = [0u8; 1200];
        if let Ok(n) = packet::encode_control(&hello, &mut buf) {
            self.up.push(now, &buf[..n]);
        }
    }

    fn on_payload(&mut self, payload: &[u8], now: f64) {
        match packet::peek_kind(payload) {
            Ok(packet::kind::CONTROL) => match packet::decode_control(payload) {
                Ok(Message::ServerWelcome {
                    player_id,
                    server_tick,
                    ..
                }) => self.on_welcome(player_id, server_tick, now),
                Ok(Message::ServerReject { reason, .. }) => {
                    self.fail(format!("rejected (reason {})", reason.0));
                }
                _ => self.stats.decode_errors += 1,
            },
            Ok(packet::kind::SERVER_SNAPSHOT) => {
                if self.phase == Phase::Welcomed {
                    self.on_snapshot(payload, now);
                }
            }
            _ => self.stats.decode_errors += 1,
        }
    }

    fn on_welcome(&mut self, player_id: u8, server_tick: u32, now: f64) {
        if self.phase != Phase::Hello {
            return; // A resent welcome.
        }
        self.phase = Phase::Welcomed;
        self.stats.welcomed = true;
        self.stats.player_id = Some(player_id);
        // Commands must reach the server before it simulates their tick.
        // The welcome left the server at `server_tick` and arrived half a
        // round trip later; a command sent now arrives another half later:
        // lead by the measured hello -> welcome round trip plus the target
        // depth (refined by the depth feedback). If the first hello was
        // lost this overestimates by one hello interval, which the steering
        // removes.
        let rtt = self
            .first_hello
            .map_or(0.1, |sent| (now - sent).clamp(0.0, 1.0));
        let rtt_ticks = (rtt * 60.0).ceil() as u32;
        let lead = rtt_ticks + self.depth_target.ceil() as u32;
        self.adjust_interval = ADJUST_INTERVAL.max(rtt_ticks + 10);
        self.next_tick = server_tick.wrapping_add(lead);
        self.first_tick = self.next_tick;
    }

    fn script_command(&mut self, tick: u32, now: f64) -> UserCommand {
        if now >= self.next_change {
            const PATTERNS: [u32; 6] = [
                held::FORWARD,
                held::FORWARD | held::LEFT,
                held::BACK,
                held::RIGHT,
                held::FORWARD | held::SPRINT,
                held::LEFT | held::CROUCH,
            ];
            let pick = (self.rng.next_u64() % PATTERNS.len() as u64) as usize;
            self.buttons = PATTERNS[pick];
            // Some stretches hold the trigger (ignored by the stand-in sim).
            if self.rng.chance(0.3) {
                self.buttons |= held::FIRE;
            }
            self.next_change = now + self.rng.range(0.4, 1.5);
        }
        // Now and then switch weapons (an edge on one command).
        let select_slot = if self.rng.chance(0.005) {
            (self.rng.next_u64() % 2) as i8
        } else {
            -1
        };
        self.yaw =
            (self.yaw + self.rng.range(-0.05, 0.05) as f32).rem_euclid(std::f32::consts::TAU);
        let mut pitch = 0.1;
        let mut buttons = self.buttons;
        if self.settings.aim
            && let Some((yaw, aim_pitch)) = self.aim_at_nearest()
        {
            self.yaw = yaw;
            pitch = aim_pitch;
            // Semi-automatic weapons fire once per pull: pull for 3 ticks,
            // release for 9.
            buttons &= !held::FIRE;
            if tick % 12 < 3 {
                buttons |= held::FIRE | held::AIM;
            }
        }
        let jump = if self.rng.chance(0.01) {
            pressed::JUMP
        } else {
            0
        };
        UserCommand {
            tick,
            held: buttons,
            pressed: jump,
            yaw: self.yaw,
            pitch,
            select_slot,
            interact_kind: 0,
            interact_id: 0,
        }
        .canonical()
    }

    /// Yaw and pitch from the own eye to the chest of the nearest other
    /// player (yaw 0 faces -Z, positive pitch looks up).
    fn aim_at_nearest(&self) -> Option<(f32, f32)> {
        let own = self.snapshot_own?;
        let eye = [
            own.position[0],
            own.position[1] + own.eye_height,
            own.position[2],
        ];
        let me = self.stats.player_id;
        let mut best: Option<([f32; 3], f32)> = None;
        for (id, player) in self.applied.iter() {
            if Some(id) == me {
                continue;
            }
            let p = player.dequantise().position;
            let d = [p[0] - eye[0], p[1] + 1.2 - eye[1], p[2] - eye[2]];
            let dist = (d[0] * d[0] + d[1] * d[1] + d[2] * d[2]).sqrt();
            if best.is_none_or(|(_, b)| dist < b) {
                best = Some((d, dist));
            }
        }
        let (d, _) = best?;
        let flat = (d[0] * d[0] + d[2] * d[2]).sqrt();
        let yaw = (-d[0]).atan2(-d[2]).rem_euclid(std::f32::consts::TAU);
        let pitch = d[1].atan2(flat).clamp(-1.5, 1.5);
        Some((yaw, pitch))
    }

    fn push_command(&mut self, now: f64) {
        let tick = self.next_tick;
        let command = self.script_command(tick, now);
        self.commands[tick as usize % RING] = command;
        if self.synced {
            step_player(&mut self.state, &command);
            self.predicted[tick as usize % RING] = (Some(tick), self.state);
        }
        self.next_tick = tick.wrapping_add(1);
    }

    fn send_input(&mut self, now: f64) {
        // Clock dilation: one command per frame normally, two when the
        // server says commands arrive late, none when they are far ahead.
        self.frames_since_adjust += 1;
        let mut count = 1;
        if self.frames_since_adjust >= self.adjust_interval && self.stats.depth_samples > 0 {
            // One-tick nudges; the average is shifted by the expected effect
            // right away so the feedback delay does not cause a second nudge.
            if self.depth_avg < self.depth_target - DEPTH_BAND {
                count = 2;
                self.depth_avg += 1.0;
                self.frames_since_adjust = 0;
            } else if self.depth_avg > self.depth_target + DEPTH_BAND {
                count = 0;
                self.depth_avg -= 1.0;
                self.frames_since_adjust = 0;
            }
        }
        for _ in 0..count {
            self.push_command(now);
        }
        let newest = self.next_tick.wrapping_sub(1);
        let available = self.next_tick.wrapping_sub(self.first_tick);
        let send = available.min(REDUNDANT_COMMANDS);
        if send == 0 {
            return;
        }
        let mut list = CommandList::default();
        for k in 0..send {
            let tick = newest.wrapping_sub(k);
            let _ = list.push(self.commands[tick as usize % RING]);
        }
        let header = self.acks.next_header();
        self.send_times[usize::from(header.sequence) % 256] = (header.sequence, now);
        self.reliable
            .fill_section(header.sequence, now, &mut self.section);
        let view_tick = self.stats.newest_tick.map_or(0, |t| t.wrapping_sub(2));
        let input = ClientInput {
            header,
            snapshot_ack: self.snapshot_ack,
            view_tick,
            view_fraction: 0,
            commands: list,
            reliable: *self.section,
        };
        let mut buf = [0u8; 1200];
        match input.encode(&mut buf) {
            Ok(n) => self.up.push(now, &buf[..n]),
            Err(e) => self.fail(format!("encode ClientInput: {e}")),
        }
    }

    fn on_snapshot(&mut self, payload: &[u8], now: f64) {
        if ServerSnapshot::decode(payload, &mut self.snapshot).is_err() {
            self.stats.decode_errors += 1;
            return;
        }
        let mut acked = AckedList::default();
        let receipt = self.acks.on_receive(&self.snapshot.header, &mut acked);
        for &seq in acked.as_slice() {
            let (s, sent) = self.send_times[usize::from(seq) % 256];
            if s == seq && sent.is_finite() {
                self.stats.rtt.push(now - sent);
            }
            self.reliable.on_packet_acked(seq);
        }
        if receipt != Receipt::Fresh {
            return;
        }
        let seq = self.snapshot.header.sequence;
        let info = self.snapshot.info;
        let in_order = self.newest_seq.is_none_or(|n| seq_greater(seq, n));
        if in_order {
            if let Some(n) = self.newest_seq {
                let step = u64::from(seq.wrapping_sub(n));
                self.stats.snapshot_expected += step;
                self.stats.snapshot_gaps += step - 1;
                if !tick_greater(info.server_tick, self.newest_seq_tick) {
                    self.stats.tick_regressions += 1;
                }
            } else {
                self.stats.snapshot_expected += 1;
            }
            self.newest_seq = Some(seq);
            self.newest_seq_tick = info.server_tick;
        } else {
            // A fresh packet older than the newest: it was counted as a gap
            // when the newer one arrived.
            self.stats.reordered += 1;
            self.stats.snapshot_gaps = self.stats.snapshot_gaps.saturating_sub(1);
        }
        self.stats.snapshots += 1;
        self.stats.snapshot_bytes += payload.len() as u64;
        self.stats.max_snapshot = self.stats.max_snapshot.max(payload.len());
        self.stats.depth_sum += i64::from(info.input_buffer_depth);
        self.stats.depth_samples += 1;
        self.depth_avg = self.depth_avg * 0.9 + f64::from(info.input_buffer_depth) * 0.1;
        if let Some(t) = info.last_command_tick
            && self
                .stats
                .last_command_tick
                .is_none_or(|old| tick_greater(t, old))
        {
            self.stats.last_command_tick = Some(t);
        }
        if self
            .stats
            .newest_tick
            .is_none_or(|old| tick_greater(info.server_tick, old))
        {
            self.stats.newest_tick = Some(info.server_tick);
        }
        self.on_messages();
        self.on_players(info.server_tick, info.baseline_tick);
        if let Some(own) = self.snapshot.own {
            if in_order {
                self.snapshot_own = Some(own);
            }
            let y = own.position[1];
            if self.stats.lowest_y.is_none_or(|low| y < low) {
                self.stats.lowest_y = Some(y);
            }
            self.reconcile(info.server_tick, &own);
        }
    }

    fn on_messages(&mut self) {
        if self
            .reliable
            .receive_section(&self.snapshot.reliable)
            .is_err()
        {
            self.fail("reliable id beyond the window".to_owned());
            return;
        }
        while let Some(message) = self.reliable.pop() {
            self.stats.messages += 1;
            match message {
                Message::PlayerJoined { player_id, .. } => {
                    self.stats.joined |= 1u64 << (player_id & 63);
                }
                Message::PlayerLeft { player_id } => {
                    self.stats.joined &= !(1u64 << (player_id & 63));
                }
                Message::WorldSyncDone => self.stats.sync_done = true,
                Message::Damage { .. } => self.stats.damage += 1,
                Message::Death { victim, killer, .. } => {
                    self.stats.deaths += 1;
                    if killer.is_none() && Some(victim) == self.stats.player_id {
                        self.stats.world_deaths += 1;
                    }
                }
                Message::DroppedRemoved { .. } => self.stats.dropped_removed += 1,
                Message::Shot { .. } => self.stats.shots += 1,
                _ => {}
            }
        }
    }

    fn on_players(&mut self, tick: u32, baseline_tick: Option<u32>) {
        let baseline = match baseline_tick {
            None => None,
            Some(b) => {
                let entry = &self.baselines[b as usize % BASELINES];
                if entry.0 == Some(b) {
                    Some(entry.1)
                } else {
                    self.stats.missing_baselines += 1;
                    return;
                }
            }
        };
        if self
            .snapshot
            .players
            .apply(baseline.as_ref(), &mut self.applied)
            .is_err()
        {
            self.stats.decode_errors += 1;
            return;
        }
        self.baselines[tick as usize % BASELINES] = (Some(tick), *self.applied);
        if self.snapshot_ack.is_none_or(|a| tick_greater(tick, a)) {
            self.snapshot_ack = Some(tick);
        }
        for (id, player) in self.applied.iter() {
            let p = player.dequantise().position;
            let slot = &mut self.first_seen[usize::from(id)];
            match slot {
                None => *slot = Some(p),
                Some(first) => {
                    let d = [p[0] - first[0], p[1] - first[1], p[2] - first[2]];
                    if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2]).sqrt() > MOVE_THRESHOLD {
                        self.stats.seen_moving |= 1u64 << id;
                    }
                }
            }
        }
    }

    fn reconcile(&mut self, tick: u32, own: &PlayerState) {
        // Compare with the prediction for that tick; replay on a mismatch
        // (or on the first own state).
        let slot = self.predicted[tick as usize % RING];
        let mismatch = match slot {
            (Some(t), predicted) if t == tick => {
                let d = [
                    predicted.position[0] - own.position[0],
                    predicted.position[1] - own.position[1],
                    predicted.position[2] - own.position[2],
                ];
                d.iter().any(|v| v.abs() > CORRECTION_EPSILON)
            }
            _ => !self.synced || !tick_greater(self.next_tick, tick),
        };
        if !mismatch {
            return;
        }
        if self.synced {
            self.stats.corrections += 1;
        }
        self.synced = true;
        self.state = *own;
        self.predicted[tick as usize % RING] = (Some(tick), *own);
        let mut t = tick.wrapping_add(1);
        while tick_greater(self.next_tick, t) {
            let command = self.commands[t as usize % RING];
            if command.tick == t {
                step_player(&mut self.state, &command);
            }
            self.predicted[t as usize % RING] = (Some(t), self.state);
            t = t.wrapping_add(1);
        }
    }

    /// Disconnects.
    pub fn disconnect(&mut self) {
        self.net.disconnect();
    }

    /// Checks the end-of-run invariants; returns the first violation.
    #[must_use]
    pub fn verdict(&self) -> Option<String> {
        let s = &self.stats;
        if let Some(f) = &s.failure {
            return Some(f.clone());
        }
        if !s.welcomed {
            return Some("never welcomed".to_owned());
        }
        if self.net.state() != ClientState::Connected {
            return Some(format!("ended {:?}", self.net.state()));
        }
        if s.snapshots < 10 {
            return Some(format!("only {} snapshots", s.snapshots));
        }
        if s.decode_errors > 0 {
            return Some(format!("{} decode errors", s.decode_errors));
        }
        if s.missing_baselines > 0 {
            return Some(format!("{} missing baselines", s.missing_baselines));
        }
        if s.tick_regressions > 0 {
            return Some(format!("{} tick regressions", s.tick_regressions));
        }
        if !s.sync_done {
            return Some("no WorldSyncDone".to_owned());
        }
        match (s.newest_tick, s.last_command_tick) {
            (Some(newest), Some(consumed)) if newest.wrapping_sub(consumed) <= 60 => None,
            (newest, consumed) => Some(format!(
                "commands not consumed (newest tick {newest:?}, last command {consumed:?})"
            )),
        }
    }

    /// Conditioner counters (up, down).
    #[must_use]
    pub fn conditioner_stats(
        &self,
    ) -> (
        crate::conditioner::ConditionerStats,
        crate::conditioner::ConditionerStats,
    ) {
        (self.up.stats, self.down.stats)
    }
}
