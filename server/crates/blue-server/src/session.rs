//! Per-client session state. Slots are allocated once at startup (one per
//! netcode client index) and reset in place on connect, so connections do
//! not allocate either.

use blue_protocol::message::RejectReason;
use blue_protocol::packet::BASELINE_HISTORY;
use blue_protocol::seq::tick_greater;
use blue_protocol::{AckTracker, Name, PlayerSet, ReliableConfig, ReliableEndpoint};

use crate::input::InputBuffer;
use crate::priority::PriorityAccumulator;
use crate::validate::{RateLimiter, ViolationScore};

const BASELINES: usize = BASELINE_HISTORY as usize;

/// Where a client is in its lifecycle.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Phase {
    /// No netcode connection in this slot.
    Free,
    /// Connected at the netcode level, waiting for `ClientHello`.
    AwaitingHello,
    /// Welcomed: has a player in the simulation.
    Welcomed,
    /// Refused; the netcode connection is dropped at `disconnect_tick`.
    Rejected {
        /// Tick at which to disconnect.
        disconnect_tick: u32,
    },
}

/// Everything the server keeps for one client.
#[derive(Debug)]
pub struct ClientSlot {
    /// Lifecycle phase.
    pub phase: Phase,
    /// netcode client id (from the connect token).
    pub client_id: u64,
    /// Display name (after `ClientHello`).
    pub name: Name,
    /// Tick the netcode connection was noticed.
    pub connected_tick: u32,
    /// Packet sequence and acknowledgement state.
    pub acks: AckTracker,
    /// Reliable-ordered message channel.
    pub reliable: ReliableEndpoint,
    /// Command jitter buffer.
    pub input: InputBuffer,
    /// Player sets the client will reconstruct, per snapshot tick.
    baselines: Box<[PlayerSet; BASELINES]>,
    baseline_ticks: [Option<u32>; BASELINES],
    /// Newest snapshot tick the client applied.
    pub snapshot_ack: Option<u32>,
    /// Newest snapshot tick sent.
    pub last_snapshot_tick: Option<u32>,
    /// Client's interpolation time (`view_tick + view_fraction / 256`).
    pub view_tick: u32,
    /// See `view_tick`.
    pub view_fraction: u8,
    /// A `ClientInput` arrived since the welcome.
    pub got_input: bool,
    /// Tick the welcome was last sent.
    pub last_welcome_tick: u32,
    /// Newest command tick consumed.
    pub last_command_tick: Option<u32>,
    /// Protocol violations.
    pub violations: ViolationScore,
    /// Packet rate limit.
    pub rate: RateLimiter,
    /// World-object priorities.
    pub objects: PriorityAccumulator,
    /// Set when the client must be dropped at the end of this tick.
    pub kick: bool,
    /// Why the client was rejected (repeated for every further hello).
    pub reject_reason: RejectReason,
}

impl Default for ClientSlot {
    fn default() -> Self {
        Self {
            phase: Phase::Free,
            client_id: 0,
            name: Name::EMPTY,
            connected_tick: 0,
            acks: AckTracker::new(),
            reliable: ReliableEndpoint::new(ReliableConfig::default()),
            input: InputBuffer::default(),
            baselines: Box::new([PlayerSet::default(); BASELINES]),
            baseline_ticks: [None; BASELINES],
            snapshot_ack: None,
            last_snapshot_tick: None,
            view_tick: 0,
            view_fraction: 0,
            got_input: false,
            last_welcome_tick: 0,
            last_command_tick: None,
            violations: ViolationScore::default(),
            rate: RateLimiter::new(),
            objects: PriorityAccumulator::default(),
            kick: false,
            reject_reason: RejectReason::default(),
        }
    }
}

impl ClientSlot {
    /// Resets in place for a new connection (no allocation).
    pub fn reset(&mut self, phase: Phase, client_id: u64, tick: u32) {
        self.phase = phase;
        self.client_id = client_id;
        self.name = Name::EMPTY;
        self.connected_tick = tick;
        self.acks = AckTracker::new();
        self.reliable = ReliableEndpoint::new(ReliableConfig::default());
        self.input.reset();
        self.baseline_ticks = [None; BASELINES];
        self.snapshot_ack = None;
        self.last_snapshot_tick = None;
        self.view_tick = 0;
        self.view_fraction = 0;
        self.got_input = false;
        self.last_welcome_tick = tick;
        self.last_command_tick = None;
        self.violations = ViolationScore::default();
        self.rate = RateLimiter::new();
        self.objects.reset();
        self.kick = false;
        self.reject_reason = RejectReason::default();
    }

    /// Stores the set the client reconstructs from the snapshot of `tick`.
    pub fn store_record(&mut self, tick: u32, record: &PlayerSet) {
        let i = tick as usize % BASELINES;
        self.baselines[i] = *record;
        self.baseline_ticks[i] = Some(tick);
    }

    /// Mutable slot for the record of `tick` (marked as that tick's); the
    /// snapshot writer fills it in place.
    pub fn record_slot(&mut self, tick: u32) -> &mut PlayerSet {
        let i = tick as usize % BASELINES;
        self.baseline_ticks[i] = Some(tick);
        &mut self.baselines[i]
    }

    /// The baseline for a snapshot at `tick`: the record of the client's
    /// acknowledged tick if it is 1..=64 ticks old and still stored. Returns
    /// its tick and a copy (so the caller may write the new record).
    pub fn baseline_for(&self, tick: u32, out: &mut PlayerSet) -> Option<u32> {
        let ack = self.snapshot_ack?;
        let age = tick.wrapping_sub(ack);
        if age == 0 || age > BASELINE_HISTORY {
            return None;
        }
        let i = ack as usize % BASELINES;
        if self.baseline_ticks[i] != Some(ack) {
            return None;
        }
        *out = self.baselines[i];
        Some(ack)
    }

    /// Takes a `snapshot_ack` from the client: it only moves forward and
    /// must name a snapshot that was sent. Returns false for an impossible
    /// value (a violation).
    pub fn accept_snapshot_ack(&mut self, ack: Option<u32>) -> bool {
        let Some(ack) = ack else {
            return true;
        };
        match self.last_snapshot_tick {
            Some(last) if !tick_greater(ack, last) => {
                if self.snapshot_ack.is_none_or(|old| tick_greater(ack, old)) {
                    self.snapshot_ack = Some(ack);
                }
                true
            }
            _ => false,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn baselines() {
        let mut c = ClientSlot::default();
        let mut set = PlayerSet::default();
        let _ = set.set(3, Some(blue_protocol::RemotePlayerQ::default()));
        c.store_record(100, &set);
        c.last_snapshot_tick = Some(110);
        let mut out = PlayerSet::default();
        assert_eq!(c.baseline_for(110, &mut out), None);
        assert!(c.accept_snapshot_ack(Some(100)));
        assert_eq!(c.baseline_for(110, &mut out), Some(100));
        assert!(out.get(3).is_some());
        // Too old.
        assert_eq!(c.baseline_for(165, &mut out), None);
        assert_eq!(c.baseline_for(164, &mut out), Some(100));
        // Overwritten slot (tick 164 shares slot with 100).
        c.store_record(164, &PlayerSet::default());
        assert_eq!(c.baseline_for(164, &mut out), None);
    }

    #[test]
    fn snapshot_ack_rules() {
        let mut c = ClientSlot::default();
        assert!(!c.accept_snapshot_ack(Some(5)), "nothing sent yet");
        assert!(c.accept_snapshot_ack(None));
        c.last_snapshot_tick = Some(10);
        assert!(c.accept_snapshot_ack(Some(8)));
        assert!(c.accept_snapshot_ack(Some(7)), "older is legal but ignored");
        assert_eq!(c.snapshot_ack, Some(8));
        assert!(!c.accept_snapshot_ack(Some(11)), "never sent");
        c.reset(Phase::AwaitingHello, 9, 50);
        assert_eq!(c.snapshot_ack, None);
        assert_eq!(c.phase, Phase::AwaitingHello);
    }
}
