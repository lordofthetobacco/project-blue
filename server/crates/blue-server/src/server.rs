//! The authoritative server: one call of [`Server::tick`] per simulation
//! tick.
//!
//! Order within a tick: pump netcode (update, connects/disconnects, receive
//! every packet) -> decode and validate `ClientInput`s -> feed the input
//! buffers -> take one command per player (repeat on a miss) -> step the sim
//! -> record capsules, sweep projectiles, lag-compensated capsule tests,
//! resolve -> forward sim events as reliable messages -> handshake timers ->
//! one snapshot per client -> kicks, rate refills, metrics.
//!
//! Everything the tick touches is allocated in [`Server::new`]; the steady
//! state does not allocate (checked by `blue-bot/tests/no_alloc.rs`).

use std::sync::Arc;

use blue_native::netcode::{MAX_PACKET_SIZE, NetcodeServer};
use blue_protocol::message::{DroppedWeapon, MAX_PLAYERS, RejectReason, SYNC_PICKUPS_PER_MESSAGE};
use blue_protocol::packet::{self, MAX_OBJECTS_PER_PACKET};
use blue_protocol::reliable::WINDOW;
use blue_protocol::seq::{AckedList, Receipt, tick_greater};
use blue_protocol::{
    ClientInput, Message, PROTOCOL_VERSION, PlayerSet, ReliableSection, SnapshotInfo,
    SnapshotWriter, WorldObject,
};

use crate::lagcomp::{CapsuleHistory, find_hits, rewind_ticks};
use crate::log;
use crate::metrics::Metrics;
use crate::priority::MAX_OBJECTS;
use crate::session::{ClientSlot, Phase};
use crate::sim::{PlayerHit, Segment, SimEvent, Simulation};
use crate::validate::{MAX_CLIENT_PACKET, name_ok, validate_command};
use crate::{TICK_RATE, input::Insert};

/// Projectile segments per tick at most (bullets in flight x substeps).
const MAX_SEGMENTS: usize = 4096;
/// Sim events per tick at most.
const MAX_EVENTS: usize = 256;
/// Late-join sync entries read at most.
const MAX_SYNC: usize = 256;
/// Bytes of each snapshot kept free of players for world objects.
const OBJECT_RESERVE_BYTES: usize = 160;
/// Ticks between `ServerWelcome` resends until the first input (100 ms).
const WELCOME_RESEND_TICKS: u32 = 6;
/// Ticks between a reject and the disconnect (1 s).
const REJECT_LINGER_TICKS: u32 = TICK_RATE;
/// Reliable window slots a late-join sync leaves free for live events
/// queued before the newcomer's first acknowledgements arrive.
const SYNC_LIVE_MARGIN: usize = 32;
/// Ticks a connection may wait without saying hello (10 s).
const HELLO_TIMEOUT_TICKS: u32 = 10 * TICK_RATE;

/// Tunables of a [`Server`].
#[derive(Clone, Copy, Debug)]
pub struct ServerSettings {
    /// Player cap 1..=64 (also the netcode slot count).
    pub max_players: u8,
    /// Snapshots per second per client, 1..=60.
    pub snapshot_rate: u32,
    /// Lag-compensation rewind cap, ms.
    pub max_rewind_ms: u32,
}

/// The authoritative server.
pub struct Server {
    net: NetcodeServer,
    sim: Box<dyn Simulation>,
    settings: ServerSettings,
    metrics: Arc<Metrics>,
    clients: Vec<ClientSlot>,
    /// The next tick to simulate.
    tick: u32,
    history: Box<CapsuleHistory>,
    packet: Box<[u8; MAX_PACKET_SIZE]>,
    input: Box<ClientInput>,
    section: Box<ReliableSection>,
    acked: AckedList,
    segments: Box<[Segment; MAX_SEGMENTS]>,
    hits: Box<[PlayerHit; MAX_SEGMENTS]>,
    events: Box<[SimEvent; MAX_EVENTS]>,
    objects: Box<[WorldObject; MAX_OBJECTS]>,
    object_order: Box<[u16; MAX_OBJECTS]>,
    players: Box<PlayerSet>,
    view_set: Box<PlayerSet>,
    baseline: Box<PlayerSet>,
    record: Box<PlayerSet>,
    pickups: Box<[u32; MAX_SYNC]>,
    dropped: Box<[DroppedWeapon; MAX_SYNC]>,
}

impl std::fmt::Debug for Server {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Server")
            .field("tick", &self.tick)
            .field("settings", &self.settings)
            .finish_non_exhaustive()
    }
}

fn violation(client: &mut ClientSlot, metrics: &Metrics) {
    Metrics::add(&metrics.invalid_packets, 1);
    if client.violations.add() {
        client.kick = true;
    }
}

impl Server {
    /// A server over a started netcode server and a loaded simulation.
    #[must_use]
    pub fn new(
        net: NetcodeServer,
        sim: Box<dyn Simulation>,
        settings: ServerSettings,
        metrics: Arc<Metrics>,
    ) -> Self {
        let slots = usize::from(settings.max_players.clamp(1, MAX_PLAYERS as u8));
        let mut clients = Vec::with_capacity(slots);
        clients.resize_with(slots, ClientSlot::default);
        Self {
            net,
            sim,
            settings,
            metrics,
            clients,
            tick: 1,
            history: Box::default(),
            packet: Box::new([0; MAX_PACKET_SIZE]),
            input: Box::default(),
            section: Box::default(),
            acked: AckedList::default(),
            segments: Box::new([Segment::default(); MAX_SEGMENTS]),
            hits: Box::new([PlayerHit::default(); MAX_SEGMENTS]),
            events: Box::new([SimEvent::default(); MAX_EVENTS]),
            objects: Box::new([WorldObject::default(); MAX_OBJECTS]),
            object_order: Box::new([0; MAX_OBJECTS]),
            players: Box::default(),
            view_set: Box::default(),
            baseline: Box::default(),
            record: Box::default(),
            pickups: Box::new([0; MAX_SYNC]),
            dropped: Box::new([DroppedWeapon::default(); MAX_SYNC]),
        }
    }

    /// The next tick to be simulated.
    #[must_use]
    pub fn next_tick(&self) -> u32 {
        self.tick
    }

    /// Runs one tick at wall time `now` (seconds, non-decreasing).
    pub fn tick(&mut self, now: f64) {
        let tick = self.tick;
        self.net.update(now);
        self.update_connections(tick);
        self.receive_all(tick);
        self.consume_commands(tick);
        self.sim.step(tick);
        let sim = &self.sim;
        self.history.record(tick, |id| sim.capsule(id));
        self.projectiles(tick);
        self.forward_events();
        self.handshake_timers(tick);
        self.send_snapshots(tick, now);
        self.finish_tick();
        self.tick = tick.wrapping_add(1);
    }

    /// Disconnects everyone (graceful shutdown).
    pub fn shutdown(&mut self) {
        self.net.disconnect_all();
        for i in 0..self.clients.len() {
            if self.clients[i].phase == Phase::Welcomed {
                self.sim.remove_player(i as u8);
            }
            self.clients[i].phase = Phase::Free;
        }
        self.metrics
            .players
            .store(0, std::sync::atomic::Ordering::Relaxed);
    }

    fn welcomed_count(&self) -> usize {
        self.clients
            .iter()
            .filter(|c| c.phase == Phase::Welcomed)
            .count()
    }

    fn update_connections(&mut self, tick: u32) {
        for i in 0..self.clients.len() {
            let connected = self.net.client_connected(i);
            let phase = self.clients[i].phase;
            if phase == Phase::Free && connected {
                let id = self.net.client_id(i);
                self.clients[i].reset(Phase::AwaitingHello, id, tick);
                let mut buf = [0u8; 256];
                let address = self.net.client_address(i, &mut buf);
                log::info(
                    "client_connected",
                    &[
                        ("slot", i.into()),
                        ("client_id", id.into()),
                        ("address", address.into()),
                    ],
                );
            } else if phase != Phase::Free && !connected {
                self.drop_client(i, "disconnected");
            }
        }
    }

    /// Frees slot `i` (removing its player and telling the others).
    fn drop_client(&mut self, i: usize, reason: &str) {
        let was_welcomed = self.clients[i].phase == Phase::Welcomed;
        self.clients[i].phase = Phase::Free;
        if self.net.client_connected(i) {
            self.net.disconnect_client(i);
        }
        let player = i as u8;
        if was_welcomed {
            self.sim.remove_player(player);
            self.broadcast(Message::PlayerLeft { player_id: player }, None);
        }
        log::info(
            "client_left",
            &[
                ("slot", i.into()),
                ("client_id", self.clients[i].client_id.into()),
                ("name", self.clients[i].name.as_str().into()),
                ("reason", reason.into()),
                ("violations", self.clients[i].violations.total().into()),
            ],
        );
    }

    /// Queues `message` for every welcomed client except `exclude`.
    fn broadcast(&mut self, message: Message, exclude: Option<u8>) {
        for (j, client) in self.clients.iter_mut().enumerate() {
            if client.phase != Phase::Welcomed || exclude == Some(j as u8) {
                continue;
            }
            if client.reliable.send(message).is_err() {
                // Window full: the client stopped acknowledging.
                client.kick = true;
            }
        }
    }

    fn receive_all(&mut self, tick: u32) {
        let mut buf = [0u8; MAX_PACKET_SIZE];
        for i in 0..self.clients.len() {
            if self.clients[i].phase == Phase::Free {
                continue;
            }
            while let Some(len) = self.net.receive(i, &mut buf) {
                Metrics::add(&self.metrics.packets_in, 1);
                let client = &mut self.clients[i];
                if len > buf.len() || len > MAX_CLIENT_PACKET {
                    violation(client, &self.metrics);
                    continue;
                }
                Metrics::add(&self.metrics.bytes_in, len as u64);
                if !client.rate.allow() {
                    Metrics::add(&self.metrics.dropped_packets, 1);
                    violation(client, &self.metrics);
                    continue;
                }
                self.handle_packet(i, &buf[..len], tick);
            }
        }
    }

    fn handle_packet(&mut self, i: usize, payload: &[u8], tick: u32) {
        match packet::peek_kind(payload) {
            Ok(packet::kind::CONTROL) => match packet::decode_control(payload) {
                Ok(Message::ClientHello {
                    protocol_version,
                    content_hash,
                    name,
                }) => self.on_hello(i, protocol_version, content_hash, &name, tick),
                _ => violation(&mut self.clients[i], &self.metrics),
            },
            Ok(packet::kind::CLIENT_INPUT) => {
                if self.clients[i].phase != Phase::Welcomed {
                    // In flight across a reject or before the welcome.
                    Metrics::add(&self.metrics.dropped_packets, 1);
                    return;
                }
                if ClientInput::decode(payload, &mut self.input).is_err() {
                    violation(&mut self.clients[i], &self.metrics);
                    return;
                }
                self.on_input(i, tick);
            }
            _ => violation(&mut self.clients[i], &self.metrics),
        }
    }

    fn send_control(&mut self, i: usize, message: &Message) {
        if let Ok(len) = packet::encode_control(message, &mut self.packet[..])
            && self.net.send(i, &self.packet[..len]).is_ok()
        {
            Metrics::add(&self.metrics.packets_out, 1);
            Metrics::add(&self.metrics.bytes_out, len as u64);
        }
    }

    fn welcome_message(&self, i: usize) -> Message {
        Message::ServerWelcome {
            player_id: i as u8,
            tick_rate: TICK_RATE as u16,
            // The last completed tick.
            server_tick: self.tick.wrapping_sub(1),
            max_players: self.settings.max_players,
        }
    }

    fn reject(&mut self, i: usize, reason: RejectReason, tick: u32) {
        let client = &mut self.clients[i];
        client.phase = Phase::Rejected {
            disconnect_tick: tick.wrapping_add(REJECT_LINGER_TICKS),
        };
        client.reject_reason = reason;
        self.send_control(
            i,
            &Message::ServerReject {
                reason,
                server_protocol_version: PROTOCOL_VERSION,
            },
        );
        log::info(
            "client_rejected",
            &[
                ("slot", i.into()),
                ("client_id", self.clients[i].client_id.into()),
                ("reason", reason.0.into()),
            ],
        );
    }

    fn on_hello(
        &mut self,
        i: usize,
        protocol_version: u16,
        content_hash: u64,
        name: &blue_protocol::Name,
        tick: u32,
    ) {
        match self.clients[i].phase {
            Phase::Welcomed => {
                let welcome = self.welcome_message(i);
                self.send_control(i, &welcome);
            }
            Phase::Rejected { .. } => {
                let reason = self.clients[i].reject_reason;
                self.send_control(
                    i,
                    &Message::ServerReject {
                        reason,
                        server_protocol_version: PROTOCOL_VERSION,
                    },
                );
            }
            Phase::Free => {}
            Phase::AwaitingHello => {
                let reason = if protocol_version != PROTOCOL_VERSION {
                    Some(RejectReason::PROTOCOL_MISMATCH)
                } else if content_hash != self.sim.content_hash() {
                    Some(RejectReason::CONTENT_MISMATCH)
                } else if self.welcomed_count() >= usize::from(self.settings.max_players) {
                    Some(RejectReason::SERVER_FULL)
                } else if !name_ok(name.as_str()) {
                    Some(RejectReason::BAD_NAME)
                } else if !self.sim.add_player(i as u8) {
                    Some(RejectReason::SERVER_FULL)
                } else {
                    None
                };
                if let Some(reason) = reason {
                    self.reject(i, reason, tick);
                    return;
                }
                self.welcome(i, *name, tick);
            }
        }
    }

    fn welcome(&mut self, i: usize, name: blue_protocol::Name, tick: u32) {
        let player = i as u8;
        {
            let client = &mut self.clients[i];
            client.phase = Phase::Welcomed;
            client.name = name;
            client.last_welcome_tick = tick;
        }
        let welcome = self.welcome_message(i);
        self.send_control(i, &welcome);
        // Late-join sync: every present player (including the newcomer),
        // taken pickups, dropped weapons, then done.
        for j in 0..self.clients.len() {
            if self.clients[j].phase == Phase::Welcomed {
                let joined = Message::PlayerJoined {
                    player_id: j as u8,
                    name: self.clients[j].name,
                };
                self.queue(i, joined);
            }
        }
        // Everything below must fit the reliable window next to the joins,
        // WorldSyncDone and a margin for live events; whatever does not fit
        // is left out (dropped weapons still appear as snapshot objects).
        let taken = self.sim.taken_pickups(&mut self.pickups[..]);
        let pickup_chunks = taken.div_ceil(SYNC_PICKUPS_PER_MESSAGE);
        let room = self.sync_room(i);
        let sent_chunks = pickup_chunks.min(room);
        let sent_pickups = taken.min(sent_chunks * SYNC_PICKUPS_PER_MESSAGE);
        for chunk in self.pickups[..sent_pickups].chunks(SYNC_PICKUPS_PER_MESSAGE) {
            let mut instance_ids = [0u32; SYNC_PICKUPS_PER_MESSAGE];
            instance_ids[..chunk.len()].copy_from_slice(chunk);
            let message = Message::WorldSyncPickupsTaken {
                count: chunk.len() as u8,
                instance_ids,
            };
            if self.clients[i].reliable.send(message).is_err() {
                self.clients[i].kick = true;
            }
        }
        let dropped = self.sim.dropped_weapons(&mut self.dropped[..]);
        let sent_dropped = dropped.min(self.sync_room(i));
        if sent_pickups < taken || sent_dropped < dropped {
            log::warn(
                "late_join_sync_capped",
                &[
                    ("slot", i.into()),
                    ("pickups_taken", taken.into()),
                    ("pickups_sent", sent_pickups.into()),
                    ("dropped_weapons", dropped.into()),
                    ("dropped_weapons_sent", sent_dropped.into()),
                    ("window", WINDOW.into()),
                ],
            );
        }
        for k in 0..sent_dropped {
            let message = Message::WorldSyncDroppedWeapon(self.dropped[k]);
            self.queue(i, message);
        }
        self.queue(i, Message::WorldSyncDone);
        self.broadcast(
            Message::PlayerJoined {
                player_id: player,
                name,
            },
            Some(player),
        );
        log::info(
            "player_joined",
            &[
                ("slot", i.into()),
                ("client_id", self.clients[i].client_id.into()),
                ("name", name.as_str().into()),
                ("tick", tick.into()),
                ("players", self.welcomed_count().into()),
            ],
        );
    }

    /// Sync messages client `i` can still take: the reliable window minus
    /// what is queued, `WorldSyncDone` and [`SYNC_LIVE_MARGIN`].
    fn sync_room(&self, i: usize) -> usize {
        WINDOW.saturating_sub(self.clients[i].reliable.unacked() + 1 + SYNC_LIVE_MARGIN)
    }

    fn queue(&mut self, i: usize, message: Message) {
        if self.clients[i].reliable.send(message).is_err() {
            self.clients[i].kick = true;
        }
    }

    fn on_input(&mut self, i: usize, tick: u32) {
        let Self {
            clients,
            input,
            metrics,
            acked,
            ..
        } = self;
        let client = &mut clients[i];
        *acked = AckedList::default();
        let receipt = client.acks.on_receive(&input.header, acked);
        for &seq in acked.as_slice() {
            client.reliable.on_packet_acked(seq);
        }
        if receipt != Receipt::Fresh {
            Metrics::add(&metrics.dropped_packets, 1);
            return;
        }
        client.got_input = true;
        if !client.accept_snapshot_ack(input.snapshot_ack) {
            violation(client, metrics);
        }
        client.view_tick = input.view_tick;
        client.view_fraction = input.view_fraction;
        for command in input.commands.as_slice() {
            let Ok(command) = validate_command(command) else {
                violation(client, metrics);
                continue;
            };
            match client.input.insert(&command, tick) {
                Insert::Accepted | Insert::Duplicate => {}
                Insert::Late => Metrics::add(&metrics.late_commands, 1),
                Insert::TooFarAhead | Insert::Conflict => violation(client, metrics),
            }
        }
        if client.reliable.receive_section(&input.reliable).is_err() {
            // An id beyond the window: the peer's channel is broken.
            violation(client, metrics);
            client.kick = true;
        }
        // No client -> server game messages exist yet: drain and count.
        while client.reliable.pop().is_some() {
            violation(client, metrics);
        }
    }

    fn consume_commands(&mut self, tick: u32) {
        for (i, client) in self.clients.iter_mut().enumerate() {
            if client.phase != Phase::Welcomed {
                continue;
            }
            let (command, fresh) = client.input.take(tick);
            if fresh {
                client.last_command_tick = Some(tick);
            } else if client.got_input {
                Metrics::add(&self.metrics.missing_commands, 1);
            }
            self.sim.apply_command(i as u8, &command);
        }
    }

    fn projectiles(&mut self, tick: u32) {
        let n = self.sim.sweep_projectiles(&mut self.segments[..]);
        if n == 0 {
            return;
        }
        Metrics::add(&self.metrics.segments, n as u64);
        let max_ticks = f64::from(self.settings.max_rewind_ms) * f64::from(TICK_RATE) / 1000.0;
        let clients = &self.clients;
        let rewind_of = |shooter: u8| match clients.get(usize::from(shooter)) {
            Some(c) if c.phase == Phase::Welcomed && c.last_snapshot_tick.is_some() => {
                rewind_ticks(tick, c.view_tick, c.view_fraction, max_ticks)
            }
            _ => 0.0,
        };
        let hits = find_hits(
            &self.history,
            tick,
            &self.segments[..n.min(MAX_SEGMENTS)],
            rewind_of,
            &mut self.hits[..],
        );
        Metrics::add(&self.metrics.hits, hits as u64);
        self.sim.resolve_projectiles(&self.hits[..hits]);
    }

    fn forward_events(&mut self) {
        let n = self.sim.poll_events(&mut self.events[..]).min(MAX_EVENTS);
        Metrics::add(&self.metrics.sim_events, n as u64);
        for k in 0..n {
            let event = self.events[k];
            self.broadcast(event.message, event.exclude);
        }
    }

    fn handshake_timers(&mut self, tick: u32) {
        for i in 0..self.clients.len() {
            match self.clients[i].phase {
                Phase::Welcomed
                    if !self.clients[i].got_input
                        && tick.wrapping_sub(self.clients[i].last_welcome_tick)
                            >= WELCOME_RESEND_TICKS =>
                {
                    self.clients[i].last_welcome_tick = tick;
                    let welcome = self.welcome_message(i);
                    self.send_control(i, &welcome);
                }
                Phase::Rejected { disconnect_tick } if !tick_greater(disconnect_tick, tick) => {
                    self.drop_client(i, "rejected");
                }
                Phase::AwaitingHello
                    if tick.wrapping_sub(self.clients[i].connected_tick) > HELLO_TIMEOUT_TICKS =>
                {
                    self.drop_client(i, "no_hello");
                }
                _ => {}
            }
        }
    }

    fn snapshot_due(&self, tick: u32) -> bool {
        // `rate` of every `TICK_RATE` consecutive ticks are due, evenly spread.
        let rate = u64::from(self.settings.snapshot_rate.clamp(1, TICK_RATE));
        (u64::from(tick) * rate) % u64::from(TICK_RATE) < rate
    }

    fn send_snapshots(&mut self, tick: u32, now: f64) {
        if !self.snapshot_due(tick) {
            return;
        }
        // Everyone as remote players see them.
        *self.players = PlayerSet::default();
        for i in 0..self.clients.len() {
            if self.clients[i].phase == Phase::Welcomed
                && let Some(state) = self.sim.remote_state(i as u8)
            {
                let _ = self.players.set(i as u8, Some(state.quantise()));
            }
        }
        let object_count = self
            .sim
            .read_objects(&mut self.objects[..])
            .min(MAX_OBJECTS);
        let dropped = self.objects[..object_count]
            .iter()
            .filter(|o| o.kind == blue_protocol::ObjectKind::DroppedWeapon)
            .count();
        self.metrics
            .dropped_weapons
            .store(dropped as u64, std::sync::atomic::Ordering::Relaxed);
        self.metrics
            .world_objects
            .store(object_count as u64, std::sync::atomic::Ordering::Relaxed);
        for i in 0..self.clients.len() {
            if self.clients[i].phase == Phase::Welcomed {
                self.send_snapshot(i, tick, now, object_count);
            }
        }
    }

    fn send_snapshot(&mut self, i: usize, tick: u32, now: f64, object_count: usize) {
        let player = i as u8;
        let own = self.sim.own_state(player);
        let viewer = own.map(|s| s.position);
        let Self {
            clients,
            players,
            view_set,
            baseline,
            record,
            packet,
            section,
            objects,
            object_order,
            metrics,
            net,
            ..
        } = self;
        let client = &mut clients[i];
        **view_set = **players;
        let _ = view_set.set(player, None);
        // Remote players nearest first.
        let mut priority = [0u8; MAX_PLAYERS];
        let mut count = 0;
        for (id, _) in view_set.iter() {
            priority[count] = id;
            count += 1;
        }
        let distance = |id: u8| -> f32 {
            match (viewer, view_set.get(id)) {
                (Some(v), Some(p)) => {
                    let q = p.dequantise().position;
                    let d = [q[0] - v[0], q[1] - v[1], q[2] - v[2]];
                    d[0] * d[0] + d[1] * d[1] + d[2] * d[2]
                }
                _ => 0.0,
            }
        };
        priority[..count].sort_unstable_by(|&a, &b| distance(a).total_cmp(&distance(b)));

        let baseline_tick = client.baseline_for(tick, baseline);
        let header = client.acks.next_header();
        client.reliable.fill_section(header.sequence, now, section);
        let info = SnapshotInfo {
            server_tick: tick,
            last_command_tick: client.last_command_tick,
            input_buffer_depth: client.input.depth(),
            baseline_tick,
        };
        let result = encode_snapshot(
            SnapshotParts {
                header: &header,
                info: &info,
                own: own.as_ref(),
                section,
                view_set,
                baseline: baseline_tick.map(|_| &**baseline),
                priority: &priority[..count],
                objects: &objects[..object_count],
                viewer,
            },
            &mut packet[..],
            record,
            &mut client.objects,
            &mut object_order[..],
        );
        match result {
            Ok(len) => {
                client.store_record(tick, record);
                client.last_snapshot_tick = Some(tick);
                metrics.snapshot_size.observe(len as u64);
                if net.send(i, &packet[..len]).is_ok() {
                    Metrics::add(&metrics.packets_out, 1);
                    Metrics::add(&metrics.bytes_out, len as u64);
                }
            }
            Err(error) => {
                log::error(
                    "snapshot_encode_failed",
                    &[("slot", i.into()), ("error", error_name(error).into())],
                );
            }
        }
    }

    fn finish_tick(&mut self) {
        for i in 0..self.clients.len() {
            if self.clients[i].kick {
                self.clients[i].kick = false;
                if self.clients[i].phase != Phase::Free {
                    Metrics::add(&self.metrics.kicked, 1);
                    self.drop_client(i, "violations");
                }
            }
            self.clients[i].violations.decay();
            self.clients[i].rate.refill();
        }
        let players = self.welcomed_count() as u64;
        let ordering = std::sync::atomic::Ordering::Relaxed;
        self.metrics.players.store(players, ordering);
        self.metrics
            .connections
            .store(self.net.num_connected() as u64, ordering);
        Metrics::add(&self.metrics.ticks, 1);
    }
}

/// Read-only inputs of one snapshot.
#[derive(Clone, Copy)]
struct SnapshotParts<'a> {
    header: &'a blue_protocol::PacketHeader,
    info: &'a SnapshotInfo,
    own: Option<&'a blue_protocol::PlayerState>,
    section: &'a ReliableSection,
    view_set: &'a PlayerSet,
    baseline: Option<&'a PlayerSet>,
    priority: &'a [u8],
    objects: &'a [WorldObject],
    viewer: Option<[f32; 3]>,
}

/// Encodes one snapshot into `packet`; fills `record` with the player set
/// the client will reconstruct and updates the object priorities.
fn encode_snapshot(
    parts: SnapshotParts<'_>,
    packet: &mut [u8],
    record: &mut PlayerSet,
    accumulator: &mut crate::priority::PriorityAccumulator,
    order: &mut [u16],
) -> Result<usize, blue_protocol::Error> {
    let mut writer =
        SnapshotWriter::begin(packet, parts.header, parts.info, parts.own, parts.section)?;
    writer.write_players(
        parts.view_set,
        parts.baseline,
        parts.priority,
        OBJECT_RESERVE_BYTES,
        record,
    )?;
    let ranked = accumulator.rank(parts.objects, parts.viewer, order);
    for &index in &order[..ranked] {
        if writer.object_count() >= MAX_OBJECTS_PER_PACKET || writer.bytes_left() < 15 {
            break;
        }
        if writer.try_add_object(&parts.objects[usize::from(index)])? {
            accumulator.sent(index);
        }
    }
    writer.finish()
}

fn error_name(error: blue_protocol::Error) -> &'static str {
    use blue_protocol::Error as E;
    match error {
        E::Overflow => "overflow",
        E::Truncated => "truncated",
        E::OutOfRange => "out_of_range",
        E::InvalidUtf8 => "invalid_utf8",
        E::NonCanonical => "non_canonical",
        E::TrailingData => "trailing_data",
        E::UnknownPacketKind => "unknown_packet_kind",
        E::UnknownMessage => "unknown_message",
        E::UnexpectedMessage => "unexpected_message",
        E::MissingBaseline => "missing_baseline",
        E::Inconsistent => "inconsistent",
        E::Usage => "usage",
        E::WindowFull => "window_full",
        E::MessageTooLarge => "message_too_large",
    }
}
