//! `UserCommand`: one tick of player input, and its packet encoding.

use crate::bits::Stream;
use crate::error::Error;
use crate::quant::{
    FINE_ANGLE_BITS, dequantise_pitch, dequantise_yaw, pitch_limit, quantise_pitch, quantise_yaw,
};

/// Held-button bits on the wire (bits above are reserved, must be 0).
pub const HELD_BITS: u32 = 8;
/// Pressed-button bits on the wire (bits above are reserved, must be 0).
pub const PRESSED_BITS: u32 = 8;

/// Bits of [`UserCommand::held`].
pub mod held {
    /// Move forward.
    pub const FORWARD: u32 = 1 << 0;
    /// Move back.
    pub const BACK: u32 = 1 << 1;
    /// Strafe left.
    pub const LEFT: u32 = 1 << 2;
    /// Strafe right.
    pub const RIGHT: u32 = 1 << 3;
    /// Crouch (toggle already resolved by the client).
    pub const CROUCH: u32 = 1 << 4;
    /// Sprint.
    pub const SPRINT: u32 = 1 << 5;
    /// Trigger.
    pub const FIRE: u32 = 1 << 6;
    /// Aim down sights.
    pub const AIM: u32 = 1 << 7;
}

/// Bits of [`UserCommand::pressed`] (edges, set on exactly one command).
pub mod pressed {
    /// Jump.
    pub const JUMP: u32 = 1 << 0;
    /// Reload.
    pub const RELOAD: u32 = 1 << 1;
    /// Toggle fire mode.
    pub const FIRE_MODE: u32 = 1 << 2;
    /// Drop the weapon in hand.
    pub const DROP_WEAPON: u32 = 1 << 3;
    /// Interact with `interact_kind` / `interact_id`.
    pub const INTERACT: u32 = 1 << 4;
    /// Scope zero one click further.
    pub const ZERO_UP: u32 = 1 << 5;
    /// Scope zero one click nearer.
    pub const ZERO_DOWN: u32 = 1 << 6;
}

/// Values of [`UserCommand::interact_kind`] (4 bits on the wire).
pub mod interact {
    /// Nothing; `interact_id` is not sent and decodes as 0.
    pub const NONE: u8 = 0;
    /// A scene pickup; `interact_id` is its `instance_id`.
    pub const PICKUP: u8 = 1;
    /// A dropped weapon; `interact_id` is its `dropped_id`.
    pub const DROPPED_WEAPON: u8 = 2;
    /// Largest encodable kind.
    pub const MAX: u8 = 15;
}

/// Largest slot index [`UserCommand::select_slot`] can carry.
pub const MAX_SLOT: i8 = 7;

/// One tick of input.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct UserCommand {
    /// Server tick this command is for.
    pub tick: u32,
    /// Buttons held during the tick ([`held`]).
    pub held: u32,
    /// Buttons pressed during the tick ([`pressed`]).
    pub pressed: u32,
    /// Absolute yaw, radians (sent with 20 bits per turn).
    pub yaw: f32,
    /// Absolute pitch, radians, within a quarter turn either way.
    pub pitch: f32,
    /// Slot to switch to, -1 for none.
    pub select_slot: i8,
    /// What to interact with ([`interact`]).
    pub interact_kind: u8,
    /// Id of the interaction target.
    pub interact_id: u32,
}

/// Wire form of a command: angles as integers.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
struct CommandQ {
    tick: u32,
    held: u32,
    pressed: u32,
    yaw: u32,
    pitch: i32,
    select_slot: i8,
    interact_kind: u8,
    interact_id: u32,
}

impl UserCommand {
    /// The command as the receiver will see it: angles quantised, and
    /// `interact_id` zeroed when `interact_kind` is none. Clients must
    /// predict with this value so prediction and server agree.
    #[must_use]
    pub fn canonical(&self) -> Self {
        CommandQ::from_command(self).to_command()
    }
}

impl CommandQ {
    fn from_command(c: &UserCommand) -> Self {
        Self {
            tick: c.tick,
            held: c.held,
            pressed: c.pressed,
            yaw: quantise_yaw(c.yaw, FINE_ANGLE_BITS),
            pitch: quantise_pitch(c.pitch, FINE_ANGLE_BITS),
            select_slot: c.select_slot,
            interact_kind: c.interact_kind,
            interact_id: if c.interact_kind == interact::NONE {
                0
            } else {
                c.interact_id
            },
        }
    }

    fn to_command(self) -> UserCommand {
        UserCommand {
            tick: self.tick,
            held: self.held,
            pressed: self.pressed,
            yaw: dequantise_yaw(self.yaw, FINE_ANGLE_BITS),
            pitch: dequantise_pitch(self.pitch, FINE_ANGLE_BITS),
            select_slot: self.select_slot,
            interact_kind: self.interact_kind,
            interact_id: self.interact_id,
        }
    }

    fn serialize_yaw<S: Stream>(s: &mut S, yaw: &mut u32) -> Result<(), Error> {
        s.serialize_bits(yaw, FINE_ANGLE_BITS)
    }

    fn serialize_pitch<S: Stream>(s: &mut S, pitch: &mut i32) -> Result<(), Error> {
        let limit = pitch_limit(FINE_ANGLE_BITS);
        s.bounded_i32(pitch, -limit, limit)
    }

    fn serialize_actions<S: Stream>(&mut self, s: &mut S) -> Result<(), Error> {
        let mut has_slot = self.select_slot >= 0;
        s.bool(&mut has_slot)?;
        if has_slot {
            let mut slot = u8::try_from(self.select_slot).map_err(|_| Error::OutOfRange)?;
            s.bounded_u8(&mut slot, MAX_SLOT as u8)?;
            self.select_slot = slot as i8;
        } else {
            if S::WRITING && self.select_slot != -1 {
                return Err(Error::OutOfRange);
            }
            self.select_slot = -1;
        }
        s.bounded_u8(&mut self.interact_kind, interact::MAX)?;
        if self.interact_kind == interact::NONE {
            self.interact_id = 0;
        } else {
            s.u32(&mut self.interact_id)?;
        }
        Ok(())
    }

    /// Full encoding.
    fn serialize_full<S: Stream>(&mut self, s: &mut S) -> Result<(), Error> {
        s.u32(&mut self.tick)?;
        s.serialize_bits(&mut self.held, HELD_BITS)?;
        s.serialize_bits(&mut self.pressed, PRESSED_BITS)?;
        Self::serialize_yaw(s, &mut self.yaw)?;
        Self::serialize_pitch(s, &mut self.pitch)?;
        self.serialize_actions(s)
    }

    /// Delta encoding against `newer` (the command one tick later).
    fn serialize_delta<S: Stream>(&mut self, newer: &Self, s: &mut S) -> Result<(), Error> {
        self.tick = newer.tick.wrapping_sub(1);

        let mut changed = self.held != newer.held;
        s.bool(&mut changed)?;
        if changed {
            s.serialize_bits(&mut self.held, HELD_BITS)?;
            if !S::WRITING && self.held == newer.held {
                return Err(Error::NonCanonical);
            }
        } else {
            self.held = newer.held;
        }

        let mut changed = self.pressed != newer.pressed;
        s.bool(&mut changed)?;
        if changed {
            s.serialize_bits(&mut self.pressed, PRESSED_BITS)?;
            if !S::WRITING && self.pressed == newer.pressed {
                return Err(Error::NonCanonical);
            }
        } else {
            self.pressed = newer.pressed;
        }

        let turn = 1i64 << FINE_ANGLE_BITS;
        let half = turn / 2;
        let mut changed = self.yaw != newer.yaw;
        s.bool(&mut changed)?;
        if changed {
            let mut diff = (i64::from(self.yaw) - i64::from(newer.yaw)).rem_euclid(turn);
            if diff >= half {
                diff -= turn;
            }
            let mut d = i32::try_from(diff).map_err(|_| Error::OutOfRange)?;
            s.varint(&mut d)?;
            if !S::WRITING {
                let d = i64::from(d);
                if d == 0 {
                    return Err(Error::NonCanonical);
                }
                if d < -half || d >= half {
                    return Err(Error::OutOfRange);
                }
                let yaw = (i64::from(newer.yaw) + d).rem_euclid(turn);
                self.yaw = u32::try_from(yaw).map_err(|_| Error::OutOfRange)?;
            }
        } else {
            self.yaw = newer.yaw;
        }

        let mut changed = self.pitch != newer.pitch;
        s.bool(&mut changed)?;
        if changed {
            let mut d = self.pitch.wrapping_sub(newer.pitch);
            s.varint(&mut d)?;
            if !S::WRITING {
                if d == 0 {
                    return Err(Error::NonCanonical);
                }
                let pitch = i64::from(newer.pitch) + i64::from(d);
                let limit = i64::from(pitch_limit(FINE_ANGLE_BITS));
                if pitch < -limit || pitch > limit {
                    return Err(Error::OutOfRange);
                }
                self.pitch = i32::try_from(pitch).map_err(|_| Error::OutOfRange)?;
            }
        } else {
            self.pitch = newer.pitch;
        }

        let mut changed = self.select_slot != newer.select_slot
            || self.interact_kind != newer.interact_kind
            || self.interact_id != newer.interact_id;
        s.bool(&mut changed)?;
        if changed {
            self.serialize_actions(s)?;
            if !S::WRITING
                && self.select_slot == newer.select_slot
                && self.interact_kind == newer.interact_kind
                && self.interact_id == newer.interact_id
            {
                return Err(Error::NonCanonical);
            }
        } else {
            self.select_slot = newer.select_slot;
            self.interact_kind = newer.interact_kind;
            self.interact_id = newer.interact_id;
        }
        Ok(())
    }
}

/// Most commands one `ClientInput` packet carries.
pub const MAX_COMMANDS_PER_PACKET: usize = 16;

/// The commands of one packet: consecutive ticks, newest first.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct CommandList {
    commands: [UserCommand; MAX_COMMANDS_PER_PACKET],
    len: usize,
}

impl Default for CommandList {
    fn default() -> Self {
        Self {
            commands: [UserCommand::default(); MAX_COMMANDS_PER_PACKET],
            len: 0,
        }
    }
}

impl CommandList {
    /// Builds a list from commands ordered newest first.
    ///
    /// # Errors
    /// [`Error::Usage`] if empty, longer than the maximum, or ticks are not
    /// consecutive and descending.
    pub fn from_slice(commands: &[UserCommand]) -> Result<Self, Error> {
        let mut list = Self::default();
        for c in commands {
            list.push(*c)?;
        }
        if list.len == 0 {
            return Err(Error::Usage);
        }
        Ok(list)
    }

    /// Appends the next older command.
    ///
    /// # Errors
    /// [`Error::Usage`] if full or its tick is not one before the last one.
    pub fn push(&mut self, command: UserCommand) -> Result<(), Error> {
        if self.len >= MAX_COMMANDS_PER_PACKET {
            return Err(Error::Usage);
        }
        if let Some(last) = self.as_slice().last()
            && command.tick != last.tick.wrapping_sub(1)
        {
            return Err(Error::Usage);
        }
        self.commands[self.len] = command;
        self.len += 1;
        Ok(())
    }

    /// Commands, newest first.
    #[must_use]
    pub fn as_slice(&self) -> &[UserCommand] {
        &self.commands[..self.len]
    }

    /// Number of commands.
    #[must_use]
    pub fn len(&self) -> usize {
        self.len
    }

    /// True when empty.
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.len == 0
    }

    /// The list with every command canonicalised.
    #[must_use]
    pub fn canonical(&self) -> Self {
        let mut out = *self;
        for c in &mut out.commands[..out.len] {
            *c = c.canonical();
        }
        out
    }

    /// Serialises: count as `bounded(1, 16)`, the newest command in full,
    /// then each older one delta-encoded against the one before it.
    ///
    /// # Errors
    /// Stream errors; [`Error::Usage`] when writing an empty list.
    pub fn serialize<S: Stream>(&mut self, s: &mut S) -> Result<(), Error> {
        let mut count = u32::try_from(self.len).map_err(|_| Error::Usage)?;
        if S::WRITING && count == 0 {
            return Err(Error::Usage);
        }
        s.bounded_u32(&mut count, 1, MAX_COMMANDS_PER_PACKET as u32)?;
        let count = count as usize;
        let mut newer = CommandQ::default();
        for i in 0..count {
            let mut q = if S::WRITING {
                CommandQ::from_command(&self.commands[i])
            } else {
                CommandQ::default()
            };
            if i == 0 {
                q.serialize_full(s)?;
            } else {
                q.serialize_delta(&newer, s)?;
            }
            if !S::WRITING {
                self.commands[i] = q.to_command();
            }
            newer = q;
        }
        if !S::WRITING {
            self.len = count;
            self.commands[count..].fill(UserCommand::default());
        }
        Ok(())
    }
}
