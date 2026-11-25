//! `StandInSim`: a pure-Rust stand-in for the C++ simulation core.
//!
//! Free kinematic movement on the flat plane `y = 0` (Y up, yaw 0 faces
//! -Z like the game's camera), jumping with gravity, no weapons and no
//! projectiles. A few bodies circle around the origin so the world-object
//! path of snapshots is exercised. Deterministic: [`step_player`] is a pure
//! function, so a client (the soak bot) can predict with it exactly.

use blue_protocol::command::{held, pressed};
use blue_protocol::message::{DroppedWeapon, MAX_PLAYERS};
use blue_protocol::player::flags;
use blue_protocol::{PlayerState, RemotePlayer, UserCommand, WorldObject};

use crate::sim::{Capsule, PlayerHit, Segment, SimEvent, Simulation};

/// Content hash clients must present to a stand-in server ("`STANDIN1`").
pub const STANDIN_CONTENT_HASH: u64 = 0x5354_414E_4449_4E31;

const DT: f32 = 1.0 / 60.0;
const WALK_SPEED: f32 = 4.0;
const SPRINT_SPEED: f32 = 6.5;
const CROUCH_SPEED: f32 = 2.0;
const JUMP_SPEED: f32 = 5.0;
const GRAVITY: f32 = 9.81;
const STAND_EYE: f32 = 1.6;
const CROUCH_EYE: f32 = 1.0;
const HEIGHT: f32 = 1.8;
const CROUCH_HEIGHT: f32 = 1.2;
const RADIUS: f32 = 0.35;
/// The plane is bounded so positions stay well inside the wire range.
const WORLD_LIMIT: f32 = 1000.0;
const BODY_COUNT: usize = 8;

/// Spawn state of player `id`: on a 10 m ring around the origin.
#[must_use]
pub fn spawn_state(id: u8) -> PlayerState {
    let angle = f32::from(id) * (std::f32::consts::TAU / 16.0);
    PlayerState {
        position: [10.0 * angle.cos(), 0.0, 10.0 * angle.sin()],
        eye_height: STAND_EYE,
        flags: flags::ALIVE | flags::ON_GROUND,
        health: 100,
        active_slot: -1,
        selected_slot: -1,
        ..PlayerState::default()
    }
}

/// Advances one player one tick with `command` (which should be canonical:
/// the dequantised command both sides see).
pub fn step_player(state: &mut PlayerState, command: &UserCommand) {
    state.yaw = command.yaw;
    state.pitch = command.pitch;
    let on_ground = state.flags & flags::ON_GROUND != 0;
    let crouching = command.held & held::CROUCH != 0;
    let sprinting = command.held & held::SPRINT != 0 && !crouching;
    let mut forward = 0.0f32;
    let mut right = 0.0f32;
    if command.held & held::FORWARD != 0 {
        forward += 1.0;
    }
    if command.held & held::BACK != 0 {
        forward -= 1.0;
    }
    if command.held & held::RIGHT != 0 {
        right += 1.0;
    }
    if command.held & held::LEFT != 0 {
        right -= 1.0;
    }
    let (sin, cos) = command.yaw.sin_cos();
    // FlatForwardFrom(yaw) = (-sin, 0, -cos); FlatRightFrom(yaw) = (cos, 0, -sin).
    let mut dir = [-sin * forward + cos * right, -cos * forward - sin * right];
    let len = (dir[0] * dir[0] + dir[1] * dir[1]).sqrt();
    if len > 1e-6 {
        dir = [dir[0] / len, dir[1] / len];
    }
    let speed = if crouching {
        CROUCH_SPEED
    } else if sprinting {
        SPRINT_SPEED
    } else {
        WALK_SPEED
    };
    state.velocity[0] = dir[0] * speed;
    state.velocity[2] = dir[1] * speed;
    if on_ground && command.pressed & pressed::JUMP != 0 {
        state.velocity[1] = JUMP_SPEED;
    } else if !on_ground {
        state.velocity[1] -= GRAVITY * DT;
    }
    for axis in 0..3 {
        state.position[axis] += state.velocity[axis] * DT;
    }
    state.position[0] = state.position[0].clamp(-WORLD_LIMIT, WORLD_LIMIT);
    state.position[2] = state.position[2].clamp(-WORLD_LIMIT, WORLD_LIMIT);
    let mut ground = false;
    if state.position[1] <= 0.0 {
        state.position[1] = 0.0;
        state.velocity[1] = 0.0;
        ground = true;
    }
    let target_eye = if crouching { CROUCH_EYE } else { STAND_EYE };
    state.eye_height += (target_eye - state.eye_height) * 0.25;
    let mut f = flags::ALIVE;
    if ground {
        f |= flags::ON_GROUND;
    }
    if crouching {
        f |= flags::CROUCHING;
    }
    if sprinting {
        f |= flags::SPRINTING;
    }
    if command.held & held::AIM != 0 {
        f |= flags::AIMING;
    }
    state.flags = f;
    state.jump_age = state.jump_age.saturating_add(1);
}

#[derive(Clone, Copy, Debug, Default)]
struct Player {
    state: PlayerState,
    command: UserCommand,
}

/// The stand-in simulation.
#[derive(Clone, Debug)]
pub struct StandInSim {
    players: [Option<Player>; MAX_PLAYERS],
    tick: u32,
}

impl Default for StandInSim {
    fn default() -> Self {
        Self::new()
    }
}

impl StandInSim {
    /// An empty world.
    #[must_use]
    pub fn new() -> Self {
        Self {
            players: [None; MAX_PLAYERS],
            tick: 0,
        }
    }

    fn player(&self, id: u8) -> Option<&Player> {
        self.players.get(usize::from(id)).and_then(Option::as_ref)
    }

    fn body(&self, index: usize) -> WorldObject {
        // Even bodies rest on a 20 m ring; odd ones circle it slowly.
        let phase = index as f32 * (std::f32::consts::TAU / BODY_COUNT as f32);
        let moving = index % 2 == 1;
        let angular = 0.5f32;
        let angle = if moving {
            phase + angular * self.tick as f32 * DT
        } else {
            phase
        };
        let radius = 20.0;
        let (sin, cos) = angle.sin_cos();
        let half = angle * 0.5;
        WorldObject {
            id: 1000 + index as u32,
            at_rest: !moving,
            position: [radius * cos, 0.5, radius * sin],
            rotation: [0.0, half.sin(), 0.0, half.cos()],
            linear_velocity: if moving {
                [-radius * angular * sin, 0.0, radius * angular * cos]
            } else {
                [0.0; 3]
            },
            angular_velocity: if moving {
                [0.0, angular, 0.0]
            } else {
                [0.0; 3]
            },
            ..WorldObject::default()
        }
    }
}

impl Simulation for StandInSim {
    fn content_hash(&self) -> u64 {
        STANDIN_CONTENT_HASH
    }

    fn add_player(&mut self, id: u8) -> bool {
        match self.players.get_mut(usize::from(id)) {
            Some(slot @ None) => {
                *slot = Some(Player {
                    state: spawn_state(id),
                    command: UserCommand::default(),
                });
                true
            }
            _ => false,
        }
    }

    fn remove_player(&mut self, id: u8) {
        if let Some(slot) = self.players.get_mut(usize::from(id)) {
            *slot = None;
        }
    }

    fn apply_command(&mut self, id: u8, command: &UserCommand) {
        if let Some(Some(p)) = self.players.get_mut(usize::from(id)) {
            p.command = *command;
        }
    }

    fn step(&mut self, tick: u32) {
        self.tick = tick;
        for p in self.players.iter_mut().flatten() {
            step_player(&mut p.state, &p.command);
        }
    }

    fn own_state(&self, id: u8) -> Option<PlayerState> {
        self.player(id).map(|p| p.state)
    }

    fn remote_state(&self, id: u8) -> Option<RemotePlayer> {
        self.player(id).map(|p| RemotePlayer {
            position: p.state.position,
            velocity: p.state.velocity,
            yaw: p.state.yaw,
            pitch: p.state.pitch,
            flags: p.state.flags,
            health: p.state.health,
            weapon_key: 0,
        })
    }

    fn capsule(&self, id: u8) -> Option<Capsule> {
        self.player(id).map(|p| Capsule {
            feet: p.state.position,
            height: if p.state.flags & flags::CROUCHING != 0 {
                CROUCH_HEIGHT
            } else {
                HEIGHT
            },
            radius: RADIUS,
            alive: p.state.flags & flags::ALIVE != 0,
        })
    }

    fn sweep_projectiles(&mut self, _out: &mut [Segment]) -> usize {
        0
    }

    fn resolve_projectiles(&mut self, _hits: &[PlayerHit]) {}

    fn read_objects(&self, out: &mut [WorldObject]) -> usize {
        let n = out.len().min(BODY_COUNT);
        for (i, slot) in out.iter_mut().take(n).enumerate() {
            *slot = self.body(i);
        }
        n
    }

    fn poll_events(&mut self, _out: &mut [SimEvent]) -> usize {
        0
    }

    fn taken_pickups(&self, _out: &mut [u32]) -> usize {
        0
    }

    fn dropped_weapons(&self, _out: &mut [DroppedWeapon]) -> usize {
        0
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn cmd(buttons: u32, edges: u32, yaw: f32) -> UserCommand {
        UserCommand {
            held: buttons,
            pressed: edges,
            yaw,
            select_slot: -1,
            ..UserCommand::default()
        }
        .canonical()
    }

    #[test]
    fn walks_forward_along_minus_z() {
        let mut s = spawn_state(0);
        let start = s.position;
        for _ in 0..60 {
            step_player(&mut s, &cmd(held::FORWARD, 0, 0.0));
        }
        assert!((s.position[2] - (start[2] - WALK_SPEED)).abs() < 1e-3);
        assert!((s.position[0] - start[0]).abs() < 1e-4);
        assert!(s.flags & flags::ON_GROUND != 0);
    }

    #[test]
    fn jumps_and_lands() {
        let mut s = spawn_state(1);
        step_player(&mut s, &cmd(0, pressed::JUMP, 0.0));
        assert!(s.position[1] > 0.0);
        let mut airborne = 0;
        for _ in 0..120 {
            step_player(&mut s, &cmd(0, 0, 0.0));
            if s.flags & flags::ON_GROUND == 0 {
                airborne += 1;
            }
        }
        assert!(airborne > 30);
        assert!(s.position[1].abs() < f32::EPSILON);
    }

    #[test]
    fn deterministic_replay() {
        let commands: Vec<UserCommand> = (0..300u32)
            .map(|i| {
                cmd(
                    [
                        held::FORWARD,
                        held::LEFT | held::SPRINT,
                        held::BACK | held::CROUCH,
                    ][i as usize % 3],
                    if i % 50 == 0 { pressed::JUMP } else { 0 },
                    i as f32 * 0.01,
                )
            })
            .collect();
        let mut a = spawn_state(3);
        let mut states = Vec::new();
        for c in &commands {
            step_player(&mut a, c);
            states.push(a);
        }
        // Restore tick 150 and replay: identical bits.
        let mut b = states[150];
        for c in &commands[151..] {
            step_player(&mut b, c);
        }
        assert_eq!(a, b);
    }

    #[test]
    fn sim_interface() {
        let mut sim = StandInSim::new();
        assert!(sim.add_player(2));
        assert!(!sim.add_player(2));
        assert!(!sim.add_player(64));
        sim.apply_command(2, &cmd(held::RIGHT, 0, 0.0));
        sim.step(1);
        let own = sim.own_state(2);
        assert!(own.is_some_and(|s| s.velocity[0] > 0.0));
        assert!(sim.remote_state(2).is_some());
        assert!(sim.capsule(2).is_some_and(|c| c.alive));
        let mut objects = [WorldObject::default(); 16];
        assert_eq!(sim.read_objects(&mut objects), BODY_COUNT);
        assert!(objects[1].linear_velocity[0].abs() + objects[1].linear_velocity[2].abs() > 0.0);
        sim.remove_player(2);
        assert!(sim.own_state(2).is_none());
        assert_eq!(sim.content_hash(), STANDIN_CONTENT_HASH);
    }
}
