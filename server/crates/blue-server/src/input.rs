//! Per-client command jitter buffer.
//!
//! Clients send each command several times (every packet repeats the recent
//! ones), stamped with the server tick it is for. The buffer keeps the first
//! copy of each tick, refuses ticks already simulated or more than one second
//! ahead, hands out exactly one command per simulated tick, and reports how
//! far ahead of the simulation the client's commands run
//! ([`InputBuffer::depth`], sent back as `input_buffer_depth`).

use blue_protocol::UserCommand;
use blue_protocol::command::interact;
use blue_protocol::packet::{INPUT_DEPTH_MAX, INPUT_DEPTH_MIN};
use blue_protocol::seq::tick_greater;

/// Ring slots; must exceed [`MAX_AHEAD`].
const SLOTS: usize = 128;

/// Furthest a command may be ahead of the next simulated tick (one second).
pub const MAX_AHEAD: u32 = crate::TICK_RATE;

/// Outcome of [`InputBuffer::insert`].
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Insert {
    /// Stored.
    Accepted,
    /// Same command already stored (normal: commands are resent).
    Duplicate,
    /// For a tick already simulated (normal for resends; dropped).
    Late,
    /// More than [`MAX_AHEAD`] ticks ahead (dropped; a violation).
    TooFarAhead,
    /// A different command for a tick that already has one (dropped; a
    /// violation: at most one command per tick per client).
    Conflict,
}

/// Command jitter buffer of one client. Fixed size, no allocation.
#[derive(Clone, Debug)]
pub struct InputBuffer {
    commands: [UserCommand; SLOTS],
    present: [bool; SLOTS],
    /// Newest tick ever accepted.
    newest: Option<u32>,
    /// The command used for the previous tick (fresh or repeated).
    last: Option<UserCommand>,
    /// Ticks in a row that ran without a fresh command.
    missing_run: u32,
    depth: i8,
}

impl Default for InputBuffer {
    fn default() -> Self {
        Self {
            commands: [UserCommand::default(); SLOTS],
            present: [false; SLOTS],
            newest: None,
            last: None,
            missing_run: 0,
            depth: 0,
        }
    }
}

fn slot(tick: u32) -> usize {
    tick as usize % SLOTS
}

impl InputBuffer {
    /// Empties the buffer.
    pub fn reset(&mut self) {
        self.present = [false; SLOTS];
        self.newest = None;
        self.last = None;
        self.missing_run = 0;
        self.depth = 0;
    }

    /// Offers `command`; `next_tick` is the next tick the simulation will
    /// run (commands for earlier ticks are late).
    pub fn insert(&mut self, command: &UserCommand, next_tick: u32) -> Insert {
        if tick_greater(next_tick, command.tick) {
            return Insert::Late;
        }
        if command.tick.wrapping_sub(next_tick) > MAX_AHEAD {
            return Insert::TooFarAhead;
        }
        let i = slot(command.tick);
        if self.present[i] && self.commands[i].tick == command.tick {
            return if self.commands[i] == *command {
                Insert::Duplicate
            } else {
                Insert::Conflict
            };
        }
        // An occupied slot with another tick holds a stale (never consumed)
        // command more than SLOTS ticks old: overwrite it.
        self.commands[i] = *command;
        self.present[i] = true;
        if self.newest.is_none_or(|n| tick_greater(command.tick, n)) {
            self.newest = Some(command.tick);
        }
        Insert::Accepted
    }

    /// The command for `tick` and whether it was fresh. A missing command
    /// repeats the previous one's held buttons and angles with no pressed
    /// edges and no actions.
    pub fn take(&mut self, tick: u32) -> (UserCommand, bool) {
        let i = slot(tick);
        let fresh = self.present[i] && self.commands[i].tick == tick;
        let command = if fresh {
            self.present[i] = false;
            self.missing_run = 0;
            self.commands[i]
        } else {
            self.missing_run = self.missing_run.saturating_add(1);
            let previous = self.last.unwrap_or_default();
            UserCommand {
                tick,
                held: previous.held,
                pressed: 0,
                yaw: previous.yaw,
                pitch: previous.pitch,
                select_slot: -1,
                interact_kind: interact::NONE,
                interact_id: 0,
            }
        };
        self.last = Some(command);
        self.depth = match self.newest {
            Some(newest) if tick_greater(newest, tick) => {
                let ahead = newest.wrapping_sub(tick);
                ahead.min(INPUT_DEPTH_MAX as u32) as i8
            }
            _ if fresh => 0,
            _ => {
                let late = self.missing_run.min(INPUT_DEPTH_MIN.unsigned_abs().into());
                -(late as i32) as i8
            }
        };
        (command, fresh)
    }

    /// `input_buffer_depth` after the last [`InputBuffer::take`]: see
    /// `docs/netcode/protocol.md` section 9.1.
    #[must_use]
    pub fn depth(&self) -> i8 {
        self.depth
    }

    /// Ticks in a row that ran without a fresh command.
    #[must_use]
    pub fn missing_run(&self) -> u32 {
        self.missing_run
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use blue_protocol::command::held;

    fn cmd(tick: u32, held: u32) -> UserCommand {
        UserCommand {
            tick,
            held,
            pressed: 1,
            yaw: 1.0,
            pitch: 0.5,
            select_slot: 1,
            interact_kind: 0,
            interact_id: 0,
        }
    }

    #[test]
    fn window_rules() {
        let mut b = InputBuffer::default();
        assert_eq!(b.insert(&cmd(99, 0), 100), Insert::Late);
        assert_eq!(b.insert(&cmd(100, 0), 100), Insert::Accepted);
        assert_eq!(b.insert(&cmd(100, 0), 100), Insert::Duplicate);
        assert_eq!(b.insert(&cmd(100, 1), 100), Insert::Conflict);
        assert_eq!(b.insert(&cmd(160, 0), 100), Insert::Accepted);
        assert_eq!(b.insert(&cmd(161, 0), 100), Insert::TooFarAhead);
        // Far in the past on the circle is late, not ahead.
        assert_eq!(
            b.insert(&cmd(100u32.wrapping_sub(1 << 30), 0), 100),
            Insert::Late
        );
    }

    #[test]
    fn takes_in_order_and_reports_depth() {
        let mut b = InputBuffer::default();
        for t in 10..=13 {
            assert_eq!(b.insert(&cmd(t, held::FORWARD), 10), Insert::Accepted);
        }
        let (c, fresh) = b.take(10);
        assert!(fresh);
        assert_eq!(c.tick, 10);
        assert_eq!(b.depth(), 3);
        b.take(11);
        b.take(12);
        assert_eq!(b.depth(), 1);
        b.take(13);
        assert_eq!(b.depth(), 0);
    }

    #[test]
    fn missing_repeats_held_without_edges() {
        let mut b = InputBuffer::default();
        b.insert(&cmd(5, held::FORWARD | held::FIRE), 5);
        b.take(5);
        let (c, fresh) = b.take(6);
        assert!(!fresh);
        assert_eq!(c.tick, 6);
        assert_eq!(c.held, held::FORWARD | held::FIRE);
        assert_eq!(c.pressed, 0);
        assert_eq!(c.select_slot, -1);
        assert!((c.yaw - 1.0).abs() < f32::EPSILON);
        assert_eq!(b.depth(), -1);
        b.take(7);
        b.take(8);
        assert_eq!(b.depth(), -3);
        assert_eq!(b.missing_run(), 3);
        // Arrival of a newer command: ahead again, not late.
        b.insert(&cmd(11, 0), 9);
        let (_, fresh) = b.take(9);
        assert!(!fresh);
        assert_eq!(b.depth(), 2);
    }

    #[test]
    fn late_depth_saturates() {
        let mut b = InputBuffer::default();
        for t in 0..100 {
            b.take(t);
        }
        assert_eq!(b.depth(), INPUT_DEPTH_MIN);
        b.reset();
        assert_eq!(b.depth(), 0);
        for t in 0..60 {
            b.insert(&cmd(t, 0), 0);
        }
        b.take(0);
        assert_eq!(b.depth(), INPUT_DEPTH_MAX);
    }

    #[test]
    fn wraps_around_the_tick_circle() {
        let mut b = InputBuffer::default();
        let start = u32::MAX - 2;
        for i in 0..6 {
            assert_eq!(
                b.insert(&cmd(start.wrapping_add(i), 0), start),
                Insert::Accepted
            );
        }
        for i in 0..6 {
            let (c, fresh) = b.take(start.wrapping_add(i));
            assert!(fresh);
            assert_eq!(c.tick, start.wrapping_add(i));
        }
    }
}
