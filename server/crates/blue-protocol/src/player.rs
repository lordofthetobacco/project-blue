//! Player state: the own player at full precision, remote players quantised
//! and delta-encoded against a baseline.
//!
//! Layouts are provisional (the sim core is written in parallel). Each field
//! is one line in a `serialize` function; adding a field means one line here,
//! one line in the C++ codec and a [`crate::PROTOCOL_VERSION`] bump.

use crate::bits::Stream;
use crate::error::Error;
use crate::message::{MAX_PLAYER_ID, MAX_PLAYERS};
use crate::quant::{
    COARSE_ANGLE_BITS, POSITION_MAX, POSITION_MIN, POSITION_SCALE, VELOCITY_MAX, VELOCITY_MIN,
    VELOCITY_SCALE, dequantise_fixed, dequantise_pitch, dequantise_yaw, pitch_limit,
    quantise_pitch, quantise_position, quantise_velocity, quantise_yaw,
};

/// Bits of [`PlayerState::flags`] and [`RemotePlayer::flags`].
pub mod flags {
    /// Crouching.
    pub const CROUCHING: u8 = 1 << 0;
    /// Sprinting.
    pub const SPRINTING: u8 = 1 << 1;
    /// Aiming down sights.
    pub const AIMING: u8 = 1 << 2;
    /// Alive.
    pub const ALIVE: u8 = 1 << 3;
    /// Standing on ground.
    pub const ON_GROUND: u8 = 1 << 4;
    /// A jump is queued.
    pub const JUMP_PENDING: u8 = 1 << 5;
    /// Bits on the wire.
    pub const BITS: u32 = 6;
}

/// Weapon slots of a loadout.
pub const WEAPON_SLOTS: usize = 2;
/// Spare magazines tracked per weapon.
pub const MAX_SPARE_MAGAZINES: usize = 8;
/// Largest slot index of `active_slot` / `selected_slot`.
pub const MAX_SLOT_INDEX: i8 = 7;
/// Largest fire mode value.
pub const MAX_FIRE_MODE: u8 = 15;
/// Largest reload stage value.
pub const MAX_RELOAD_STAGE: u8 = 7;

/// One weapon slot of the own player's loadout.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct WeaponSlotState {
    /// A weapon is in this slot. When false nothing else is sent and every
    /// other field decodes as its default.
    pub present: bool,
    /// Stable weapon key.
    pub weapon_key: u32,
    /// `FireMode` value (0..=15).
    pub fire_mode: u8,
    /// Rounds in the inserted magazine.
    pub magazine_rounds: u8,
    /// A magazine is fitted.
    pub magazine_inserted: bool,
    /// A round is chambered.
    pub chambered: bool,
    /// The bolt is locked back.
    pub bolt_locked: bool,
    /// The running reload is an empty one.
    pub empty_reload: bool,
    /// Reload steps already performed (0..=7).
    pub reload_stage: u8,
    /// Seconds into the running reload.
    pub reload_elapsed: f32,
    /// Total seconds of the running reload, 0 when not reloading.
    pub reload_duration: f32,
    /// Seconds until the bolt can fire again.
    pub cycle_remaining: f32,
    /// Number of valid entries in `spare_magazines` (0..=8).
    pub spare_count: u8,
    /// Round counts of the spare magazines; entries past `spare_count` are 0.
    pub spare_magazines: [u8; MAX_SPARE_MAGAZINES],
    /// Rounds of the magazine being seated by a running reload, -1 for none
    /// (-1..=255).
    pub incoming_magazine: i16,
    /// The current trigger pull already fired or clicked.
    pub pull_used: bool,
    /// Magnification of the fitted scope (0 for none).
    pub scope_magnification: f32,
    /// Zero distance of the fitted scope, metres.
    pub scope_zero: f32,
    /// Spare magazines carried beyond the eight sent (0 for stock weapons).
    pub spares_dropped: u8,
}

/// Smallest `incoming_magazine` (none).
pub const NO_INCOMING_MAGAZINE: i16 = -1;
/// Largest `incoming_magazine`.
pub const MAX_INCOMING_MAGAZINE: i16 = 255;

impl WeaponSlotState {
    fn serialize<S: Stream>(&mut self, s: &mut S) -> Result<(), Error> {
        s.bool(&mut self.present)?;
        if !self.present {
            if S::WRITING && *self != Self::default() {
                return Err(Error::NonCanonical);
            }
            *self = Self::default();
            return Ok(());
        }
        s.u32(&mut self.weapon_key)?;
        s.bounded_u8(&mut self.fire_mode, MAX_FIRE_MODE)?;
        s.u8(&mut self.magazine_rounds)?;
        s.bool(&mut self.magazine_inserted)?;
        s.bool(&mut self.chambered)?;
        s.bool(&mut self.bolt_locked)?;
        s.bool(&mut self.empty_reload)?;
        s.bounded_u8(&mut self.reload_stage, MAX_RELOAD_STAGE)?;
        s.f32(&mut self.reload_elapsed)?;
        s.f32(&mut self.reload_duration)?;
        s.f32(&mut self.cycle_remaining)?;
        s.bounded_u8(&mut self.spare_count, MAX_SPARE_MAGAZINES as u8)?;
        let used = usize::from(self.spare_count);
        if S::WRITING && self.spare_magazines[used..].iter().any(|&v| v != 0) {
            return Err(Error::NonCanonical);
        }
        for rounds in &mut self.spare_magazines[..used] {
            s.u8(rounds)?;
        }
        if !S::WRITING {
            self.spare_magazines[used..].fill(0);
        }
        let mut incoming = i32::from(self.incoming_magazine);
        s.bounded_i32(
            &mut incoming,
            i32::from(NO_INCOMING_MAGAZINE),
            i32::from(MAX_INCOMING_MAGAZINE),
        )?;
        self.incoming_magazine = i16::try_from(incoming).map_err(|_| Error::OutOfRange)?;
        s.bool(&mut self.pull_used)?;
        s.f32(&mut self.scope_magnification)?;
        s.f32(&mut self.scope_zero)?;
        s.u8(&mut self.spares_dropped)
    }
}

/// The own player's state at full precision, used for reconciliation.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct PlayerState {
    /// Feet position, metres.
    pub position: [f32; 3],
    /// Velocity, m/s.
    pub velocity: [f32; 3],
    /// Yaw, radians.
    pub yaw: f32,
    /// Pitch, radians.
    pub pitch: f32,
    /// Current eye height, metres.
    pub eye_height: f32,
    /// [`flags`] bits.
    pub flags: u8,
    /// Ticks since the last jump.
    pub jump_age: u8,
    /// Ticks until a jump is allowed.
    pub jump_cooldown: u8,
    /// Health.
    pub health: i16,
    /// Ticks until respawn while dead.
    pub respawn_ticks: u16,
    /// Shots fired so far (seeds dispersion).
    pub shots_fired: u32,
    /// Active slot, -1 for empty-handed.
    pub active_slot: i8,
    /// Selected (switching to) slot, -1 for none.
    pub selected_slot: i8,
    /// Weapon switch progress: a linear fraction in `[0, 1]` (not seconds).
    pub raise: f32,
    /// Speed of the pending (or last) jump request, m/s.
    pub jump_speed: f32,
    /// Deaths so far (picks the respawn point).
    pub deaths: u32,
    /// A trigger held through a weapon switch must be released first.
    pub wait_for_release: bool,
    /// Weapon slots.
    pub slots: [WeaponSlotState; WEAPON_SLOTS],
}

impl PlayerState {
    /// Serialises every field in declaration order.
    ///
    /// # Errors
    /// Stream errors; non-finite floats; out-of-range fields; data in an
    /// absent slot or past `spare_count` ([`Error::NonCanonical`]).
    pub fn serialize<S: Stream>(&mut self, s: &mut S) -> Result<(), Error> {
        s.vec3(&mut self.position)?;
        s.vec3(&mut self.velocity)?;
        s.f32(&mut self.yaw)?;
        s.f32(&mut self.pitch)?;
        s.f32(&mut self.eye_height)?;
        s.serialize_bits_u8(&mut self.flags, flags::BITS)?;
        s.u8(&mut self.jump_age)?;
        s.u8(&mut self.jump_cooldown)?;
        s.i16(&mut self.health)?;
        s.u16(&mut self.respawn_ticks)?;
        s.u32(&mut self.shots_fired)?;
        s.bounded_i8(&mut self.active_slot, -1, MAX_SLOT_INDEX)?;
        s.bounded_i8(&mut self.selected_slot, -1, MAX_SLOT_INDEX)?;
        s.f32(&mut self.raise)?;
        s.f32(&mut self.jump_speed)?;
        s.u32(&mut self.deaths)?;
        s.bool(&mut self.wait_for_release)?;
        for slot in &mut self.slots {
            slot.serialize(s)?;
        }
        Ok(())
    }
}

/// Extension used by the player codecs.
trait StreamExt: Stream {
    fn serialize_bits_u8(&mut self, value: &mut u8, bits: u32) -> Result<(), Error> {
        let mut wide = u32::from(*value);
        self.serialize_bits(&mut wide, bits)?;
        *value = u8::try_from(wide).map_err(|_| Error::OutOfRange)?;
        Ok(())
    }
}

impl<S: Stream> StreamExt for S {}

/// A remote player as the simulation sees it.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct RemotePlayer {
    /// Feet position, metres.
    pub position: [f32; 3],
    /// Velocity, m/s.
    pub velocity: [f32; 3],
    /// Yaw, radians.
    pub yaw: f32,
    /// Pitch, radians.
    pub pitch: f32,
    /// [`flags`] bits.
    pub flags: u8,
    /// Health.
    pub health: i16,
    /// Key of the weapon in hand, 0 for none.
    pub weapon_key: u32,
}

/// A remote player in wire units: position 1/512 m, velocity 1/64 m/s,
/// yaw/pitch 1/65536 turn.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub struct RemotePlayerQ {
    /// Position, `POSITION_MIN..=POSITION_MAX`.
    pub position: [i32; 3],
    /// Velocity, `VELOCITY_MIN..=VELOCITY_MAX`.
    pub velocity: [i32; 3],
    /// Yaw, `0..65536`.
    pub yaw: u16,
    /// Pitch, `-16384..=16384`.
    pub pitch: i16,
    /// [`flags`] bits (6 used).
    pub flags: u8,
    /// Health.
    pub health: i16,
    /// Weapon key.
    pub weapon_key: u32,
}

impl RemotePlayer {
    /// Converts to wire units (total: NaN gives 0, values clamp).
    #[must_use]
    pub fn quantise(&self) -> RemotePlayerQ {
        let pitch = quantise_pitch(self.pitch, COARSE_ANGLE_BITS);
        RemotePlayerQ {
            position: self.position.map(quantise_position),
            velocity: self.velocity.map(quantise_velocity),
            yaw: u16::try_from(quantise_yaw(self.yaw, COARSE_ANGLE_BITS)).unwrap_or(0),
            pitch: i16::try_from(pitch).unwrap_or(0),
            flags: self.flags & ((1 << flags::BITS) - 1),
            health: self.health,
            weapon_key: self.weapon_key,
        }
    }
}

impl RemotePlayerQ {
    /// Converts back to simulation units.
    #[must_use]
    pub fn dequantise(&self) -> RemotePlayer {
        RemotePlayer {
            position: self.position.map(|q| dequantise_fixed(q, POSITION_SCALE)),
            velocity: self.velocity.map(|q| dequantise_fixed(q, VELOCITY_SCALE)),
            yaw: dequantise_yaw(u32::from(self.yaw), COARSE_ANGLE_BITS),
            pitch: dequantise_pitch(i32::from(self.pitch), COARSE_ANGLE_BITS),
            flags: self.flags,
            health: self.health,
            weapon_key: self.weapon_key,
        }
    }

    fn serialize_position<S: Stream>(&mut self, s: &mut S) -> Result<(), Error> {
        for c in &mut self.position {
            s.bounded_i32(c, POSITION_MIN, POSITION_MAX)?;
        }
        Ok(())
    }

    fn serialize_velocity<S: Stream>(&mut self, s: &mut S) -> Result<(), Error> {
        for c in &mut self.velocity {
            s.bounded_i32(c, VELOCITY_MIN, VELOCITY_MAX)?;
        }
        Ok(())
    }

    fn serialize_orientation<S: Stream>(&mut self, s: &mut S) -> Result<(), Error> {
        s.u16(&mut self.yaw)?;
        let limit = pitch_limit(COARSE_ANGLE_BITS);
        let mut pitch = i32::from(self.pitch);
        s.bounded_i32(&mut pitch, -limit, limit)?;
        self.pitch = i16::try_from(pitch).map_err(|_| Error::OutOfRange)?;
        Ok(())
    }

    fn serialize_status<S: Stream>(&mut self, s: &mut S) -> Result<(), Error> {
        s.serialize_bits_u8(&mut self.flags, flags::BITS)?;
        s.i16(&mut self.health)
    }

    fn serialize_weapon<S: Stream>(&mut self, s: &mut S) -> Result<(), Error> {
        s.u32(&mut self.weapon_key)
    }

    /// Full encoding: every group, absolute.
    fn serialize_full<S: Stream>(&mut self, s: &mut S) -> Result<(), Error> {
        self.serialize_position(s)?;
        self.serialize_velocity(s)?;
        self.serialize_orientation(s)?;
        self.serialize_status(s)?;
        self.serialize_weapon(s)
    }
}

/// Field groups of a delta entry, in wire order.
mod group {
    pub const POSITION: u8 = 1 << 0;
    pub const VELOCITY: u8 = 1 << 1;
    pub const ORIENTATION: u8 = 1 << 2;
    pub const STATUS: u8 = 1 << 3;
    pub const WEAPON: u8 = 1 << 4;
    pub const COUNT: u32 = 5;
}

/// The remote players of one snapshot, indexed by player id.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PlayerSet {
    players: [Option<RemotePlayerQ>; MAX_PLAYERS],
}

impl Default for PlayerSet {
    fn default() -> Self {
        Self {
            players: [None; MAX_PLAYERS],
        }
    }
}

impl PlayerSet {
    /// The player with `id`, if present.
    #[must_use]
    pub fn get(&self, id: u8) -> Option<&RemotePlayerQ> {
        self.players.get(usize::from(id)).and_then(Option::as_ref)
    }

    /// Sets or clears player `id`.
    ///
    /// # Errors
    /// [`Error::OutOfRange`] for `id > 63`.
    pub fn set(&mut self, id: u8, player: Option<RemotePlayerQ>) -> Result<(), Error> {
        let slot = self
            .players
            .get_mut(usize::from(id))
            .ok_or(Error::OutOfRange)?;
        *slot = player;
        Ok(())
    }

    /// Present players as `(id, state)` in id order.
    pub fn iter(&self) -> impl Iterator<Item = (u8, &RemotePlayerQ)> {
        self.players
            .iter()
            .enumerate()
            .filter_map(|(i, p)| p.as_ref().map(|p| (i as u8, p)))
    }

    /// Number of present players.
    #[must_use]
    pub fn len(&self) -> usize {
        self.players.iter().filter(|p| p.is_some()).count()
    }

    /// True when no player is present.
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }
}

/// How one players-section entry changes the baseline.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum EntryKind {
    /// Changed field groups of a player in the baseline.
    #[default]
    Delta,
    /// Complete state of a player (new, or no baseline).
    Full,
    /// The player in the baseline is gone.
    Removed,
}

impl EntryKind {
    fn code(self) -> u32 {
        match self {
            Self::Delta => 0,
            Self::Full => 1,
            Self::Removed => 2,
        }
    }

    fn from_code(code: u32) -> Result<Self, Error> {
        match code {
            0 => Ok(Self::Delta),
            1 => Ok(Self::Full),
            2 => Ok(Self::Removed),
            _ => Err(Error::OutOfRange),
        }
    }
}

/// One decoded players-section entry.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct PlayerEntry {
    /// Player id.
    pub id: u8,
    /// Entry kind.
    pub kind: EntryKind,
    /// Delta only: changed groups (bit 0 position .. bit 4 weapon).
    pub groups: u8,
    /// Full: the state. Delta: `position` holds per-axis differences, other
    /// groups hold new values where their bit is set.
    pub value: RemotePlayerQ,
}

impl PlayerEntry {
    fn serialize<S: Stream>(&mut self, has_baseline: bool, s: &mut S) -> Result<(), Error> {
        s.bounded_u8(&mut self.id, MAX_PLAYER_ID)?;
        let mut code = self.kind.code();
        s.serialize_bits(&mut code, 2)?;
        self.kind = EntryKind::from_code(code)?;
        if !has_baseline && self.kind != EntryKind::Full {
            return Err(Error::NonCanonical);
        }
        match self.kind {
            EntryKind::Removed => Ok(()),
            EntryKind::Full => self.value.serialize_full(s),
            EntryKind::Delta => self.serialize_delta(s),
        }
    }

    fn serialize_delta<S: Stream>(&mut self, s: &mut S) -> Result<(), Error> {
        let mut groups = u32::from(self.groups);
        s.serialize_bits(&mut groups, group::COUNT)?;
        self.groups = u8::try_from(groups).map_err(|_| Error::OutOfRange)?;
        if self.groups == 0 {
            return Err(Error::NonCanonical);
        }
        let v = &mut self.value;
        if self.groups & group::POSITION != 0 {
            for d in &mut v.position {
                s.varint(d)?;
            }
        }
        if self.groups & group::VELOCITY != 0 {
            v.serialize_velocity(s)?;
        }
        if self.groups & group::ORIENTATION != 0 {
            v.serialize_orientation(s)?;
        }
        if self.groups & group::STATUS != 0 {
            v.serialize_status(s)?;
        }
        if self.groups & group::WEAPON != 0 {
            v.serialize_weapon(s)?;
        }
        Ok(())
    }

    /// The entry that turns `base` into `current` for player `id`, or
    /// `None` when nothing changed.
    fn diff(id: u8, current: Option<&RemotePlayerQ>, base: Option<&RemotePlayerQ>) -> Option<Self> {
        match (current, base) {
            (None, None) => None,
            (None, Some(_)) => Some(Self {
                id,
                kind: EntryKind::Removed,
                ..Self::default()
            }),
            (Some(c), None) => Some(Self {
                id,
                kind: EntryKind::Full,
                groups: 0,
                value: *c,
            }),
            (Some(c), Some(b)) => {
                let mut groups = 0;
                if c.position != b.position {
                    groups |= group::POSITION;
                }
                if c.velocity != b.velocity {
                    groups |= group::VELOCITY;
                }
                if c.yaw != b.yaw || c.pitch != b.pitch {
                    groups |= group::ORIENTATION;
                }
                if c.flags != b.flags || c.health != b.health {
                    groups |= group::STATUS;
                }
                if c.weapon_key != b.weapon_key {
                    groups |= group::WEAPON;
                }
                if groups == 0 {
                    return None;
                }
                let mut value = *c;
                for axis in 0..3 {
                    value.position[axis] = c.position[axis].wrapping_sub(b.position[axis]);
                }
                Some(Self {
                    id,
                    kind: EntryKind::Delta,
                    groups,
                    value,
                })
            }
        }
    }

    /// Applies this entry to `set` (which starts as a copy of the baseline).
    fn apply(&self, set: &mut PlayerSet) -> Result<(), Error> {
        let id = self.id;
        match self.kind {
            EntryKind::Full => set.set(id, Some(self.value)),
            EntryKind::Removed => {
                if set.get(id).is_none() {
                    return Err(Error::Inconsistent);
                }
                set.set(id, None)
            }
            EntryKind::Delta => {
                let mut p = *set.get(id).ok_or(Error::Inconsistent)?;
                let v = &self.value;
                if self.groups & group::POSITION != 0 {
                    for axis in 0..3 {
                        let sum = i64::from(p.position[axis]) + i64::from(v.position[axis]);
                        if sum < i64::from(POSITION_MIN) || sum > i64::from(POSITION_MAX) {
                            return Err(Error::Inconsistent);
                        }
                        p.position[axis] = i32::try_from(sum).map_err(|_| Error::Inconsistent)?;
                    }
                }
                if self.groups & group::VELOCITY != 0 {
                    p.velocity = v.velocity;
                }
                if self.groups & group::ORIENTATION != 0 {
                    p.yaw = v.yaw;
                    p.pitch = v.pitch;
                }
                if self.groups & group::STATUS != 0 {
                    p.flags = v.flags;
                    p.health = v.health;
                }
                if self.groups & group::WEAPON != 0 {
                    p.weapon_key = v.weapon_key;
                }
                set.set(id, Some(p))
            }
        }
    }
}

/// The decoded players section of a snapshot. Decoding never needs the
/// baseline; [`PlayerUpdates::apply`] does.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PlayerUpdates {
    entries: [PlayerEntry; MAX_PLAYERS],
    len: usize,
    has_baseline: bool,
}

impl Default for PlayerUpdates {
    fn default() -> Self {
        Self {
            entries: [PlayerEntry::default(); MAX_PLAYERS],
            len: 0,
            has_baseline: false,
        }
    }
}

impl PlayerUpdates {
    /// Entries in wire order.
    #[must_use]
    pub fn entries(&self) -> &[PlayerEntry] {
        &self.entries[..self.len]
    }

    /// True if the section was encoded against a baseline.
    #[must_use]
    pub fn has_baseline(&self) -> bool {
        self.has_baseline
    }

    /// Reconstructs the snapshot's player set into `out`.
    ///
    /// # Errors
    /// [`Error::MissingBaseline`] if the section was encoded against a
    /// baseline and `baseline` is `None`; [`Error::Inconsistent`] if an entry
    /// contradicts `baseline`. `out` is unspecified on error.
    pub fn apply(&self, baseline: Option<&PlayerSet>, out: &mut PlayerSet) -> Result<(), Error> {
        *out = match (self.has_baseline, baseline) {
            (true, None) => return Err(Error::MissingBaseline),
            (true, Some(b)) => *b,
            (false, _) => PlayerSet::default(),
        };
        for entry in self.entries() {
            entry.apply(out)?;
        }
        Ok(())
    }

    /// Reads a players section: entries each preceded by a 1 bit, ended by
    /// a 0 bit. Ids must be distinct.
    pub(crate) fn read<S: Stream>(&mut self, has_baseline: bool, s: &mut S) -> Result<(), Error> {
        self.len = 0;
        self.has_baseline = has_baseline;
        let mut seen: u64 = 0;
        loop {
            let mut more = false;
            s.bool(&mut more)?;
            if !more {
                return Ok(());
            }
            if self.len >= MAX_PLAYERS {
                return Err(Error::NonCanonical);
            }
            let mut entry = PlayerEntry::default();
            entry.serialize(has_baseline, s)?;
            let bit = 1u64 << entry.id;
            if seen & bit != 0 {
                return Err(Error::NonCanonical);
            }
            seen |= bit;
            self.entries[self.len] = entry;
            self.len += 1;
        }
    }
}

/// Writes one entry (preceded by its 1 bit) for player `id` if it changed.
/// Returns the entry written, `None` if unchanged.
pub(crate) fn write_player_entry<S: Stream>(
    id: u8,
    current: &PlayerSet,
    baseline: Option<&PlayerSet>,
    s: &mut S,
) -> Result<Option<PlayerEntry>, Error> {
    let Some(mut entry) = PlayerEntry::diff(id, current.get(id), baseline.and_then(|b| b.get(id)))
    else {
        return Ok(None);
    };
    let mut more = true;
    s.bool(&mut more)?;
    entry.serialize(baseline.is_some(), s)?;
    Ok(Some(entry))
}

/// Applies an entry the writer produced to the server-side record.
pub(crate) fn apply_entry(entry: &PlayerEntry, set: &mut PlayerSet) -> Result<(), Error> {
    entry.apply(set)
}
