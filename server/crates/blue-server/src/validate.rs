//! Semantic validation of client traffic, beyond what the codec enforces:
//! command fields, per-client packet rate and size, and a violation score
//! that disconnects persistent offenders.

use std::f32::consts::{FRAC_PI_2, TAU};

use blue_protocol::UserCommand;
use blue_protocol::command::{MAX_SLOT, interact};

/// Pressed bits the protocol defines (bit 7 is reserved).
pub const KNOWN_PRESSED: u32 = 0x7F;
/// Held bits the protocol defines.
pub const KNOWN_HELD: u32 = 0xFF;
/// Largest `ClientInput` payload a legitimate client produces (16 commands
/// plus a full 300-byte reliable section stay well below this).
pub const MAX_CLIENT_PACKET: usize = 600;

/// Why a command was refused.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CommandError {
    /// Reserved held or pressed bits are set.
    UnknownButtons,
    /// An angle is NaN or infinite.
    NonFinite,
    /// `select_slot` outside -1..=7.
    BadSlot,
    /// Reserved `interact_kind`.
    BadInteract,
}

/// Checks a decoded command and returns it sanitised: yaw wrapped into
/// `[0, 2*pi)`, pitch clamped to `[-pi/2, pi/2]` (quantisation already
/// guarantees both; this is defence in depth for other producers).
///
/// # Errors
/// The first problem found.
pub fn validate_command(command: &UserCommand) -> Result<UserCommand, CommandError> {
    if command.held & !KNOWN_HELD != 0 || command.pressed & !KNOWN_PRESSED != 0 {
        return Err(CommandError::UnknownButtons);
    }
    if !command.yaw.is_finite() || !command.pitch.is_finite() {
        return Err(CommandError::NonFinite);
    }
    if command.select_slot < -1 || command.select_slot > MAX_SLOT {
        return Err(CommandError::BadSlot);
    }
    if command.interact_kind > interact::DROPPED_WEAPON {
        return Err(CommandError::BadInteract);
    }
    let mut out = *command;
    out.yaw = command.yaw.rem_euclid(TAU);
    if out.yaw >= TAU {
        out.yaw = 0.0;
    }
    out.pitch = command.pitch.clamp(-FRAC_PI_2, FRAC_PI_2);
    Ok(out)
}

/// Token bucket limiting packets per client, refilled once per tick.
#[derive(Clone, Copy, Debug)]
pub struct RateLimiter {
    tokens: u32,
    capacity: u32,
    refill_per_tick: u32,
}

impl RateLimiter {
    /// Default limit: 120 packets per second sustained (two per tick:
    /// inputs plus handshake resends), bursts of 30.
    #[must_use]
    pub const fn new() -> Self {
        Self {
            tokens: 30,
            capacity: 30,
            refill_per_tick: 2,
        }
    }

    /// Called once per tick.
    pub fn refill(&mut self) {
        self.tokens = (self.tokens + self.refill_per_tick).min(self.capacity);
    }

    /// Takes a token; false means the packet must be dropped.
    pub fn allow(&mut self) -> bool {
        if self.tokens == 0 {
            false
        } else {
            self.tokens -= 1;
            true
        }
    }
}

impl Default for RateLimiter {
    fn default() -> Self {
        Self::new()
    }
}

/// Leaky violation score: each violation adds one, the score drains by one
/// every [`ViolationScore::DECAY_TICKS`] ticks, and the client is
/// disconnected once it reaches [`ViolationScore::LIMIT`].
#[derive(Clone, Copy, Debug, Default)]
pub struct ViolationScore {
    score: u32,
    ticks: u32,
    total: u64,
}

impl ViolationScore {
    /// Score that disconnects.
    pub const LIMIT: u32 = 60;
    /// Ticks per point of decay (10 points per second at 60 Hz).
    pub const DECAY_TICKS: u32 = 6;

    /// Records one violation; returns true if the client must be
    /// disconnected.
    pub fn add(&mut self) -> bool {
        self.score = self.score.saturating_add(1);
        self.total += 1;
        self.score >= Self::LIMIT
    }

    /// Called once per tick.
    pub fn decay(&mut self) {
        self.ticks += 1;
        if self.ticks >= Self::DECAY_TICKS {
            self.ticks = 0;
            self.score = self.score.saturating_sub(1);
        }
    }

    /// Violations ever recorded.
    #[must_use]
    pub fn total(&self) -> u64 {
        self.total
    }
}

/// Acceptable player name: 1..=32 bytes after trimming, no control
/// characters.
#[must_use]
pub fn name_ok(name: &str) -> bool {
    let trimmed = name.trim();
    !trimmed.is_empty() && trimmed.len() == name.len() && !name.chars().any(char::is_control)
}

#[cfg(test)]
mod tests {
    #![allow(clippy::unwrap_used)]

    use super::*;

    fn good() -> UserCommand {
        UserCommand {
            tick: 1,
            held: 0xFF,
            pressed: 0x7F,
            yaw: 1.0,
            pitch: 0.2,
            select_slot: -1,
            interact_kind: interact::PICKUP,
            interact_id: 9,
        }
    }

    #[test]
    fn accepts_and_sanitises() {
        assert_eq!(validate_command(&good()).unwrap(), good());
        let mut c = good();
        c.yaw = -0.5;
        c.pitch = 3.0;
        let v = validate_command(&c).unwrap();
        assert!((v.yaw - (TAU - 0.5)).abs() < 1e-6);
        assert!((v.pitch - FRAC_PI_2).abs() < f32::EPSILON);
        c.yaw = TAU;
        assert!(validate_command(&c).unwrap().yaw < TAU);
    }

    #[test]
    fn rejects() {
        let mut c = good();
        c.pressed = 0x80;
        assert_eq!(validate_command(&c), Err(CommandError::UnknownButtons));
        c = good();
        c.held = 0x100;
        assert_eq!(validate_command(&c), Err(CommandError::UnknownButtons));
        c = good();
        c.yaw = f32::NAN;
        assert_eq!(validate_command(&c), Err(CommandError::NonFinite));
        c = good();
        c.pitch = f32::INFINITY;
        assert_eq!(validate_command(&c), Err(CommandError::NonFinite));
        c = good();
        c.select_slot = 8;
        assert_eq!(validate_command(&c), Err(CommandError::BadSlot));
        c.select_slot = -2;
        assert_eq!(validate_command(&c), Err(CommandError::BadSlot));
        c = good();
        c.interact_kind = 3;
        assert_eq!(validate_command(&c), Err(CommandError::BadInteract));
    }

    #[test]
    fn rate_limit() {
        let mut r = RateLimiter::new();
        let mut allowed = 0;
        for _ in 0..100 {
            allowed += u32::from(r.allow());
        }
        assert_eq!(allowed, 30);
        // Two per tick sustained.
        let mut allowed = 0;
        for _ in 0..60 {
            r.refill();
            for _ in 0..5 {
                allowed += u32::from(r.allow());
            }
        }
        assert_eq!(allowed, 120);
    }

    #[test]
    fn violation_score() {
        let mut v = ViolationScore::default();
        // A trickle (one per 10 ticks) never disconnects.
        for i in 0..10_000 {
            if i % 10 == 0 {
                assert!(!v.add());
            }
            v.decay();
        }
        // A burst does.
        let mut out = false;
        for _ in 0..ViolationScore::LIMIT {
            out = v.add();
        }
        assert!(out);
        assert!(v.total() > 1000);
    }

    #[test]
    fn names() {
        assert!(name_ok("bot 1"));
        assert!(name_ok("Żółw"));
        assert!(!name_ok(""));
        assert!(!name_ok("   "));
        assert!(!name_ok(" padded"));
        assert!(!name_ok("tab\there"));
    }
}
