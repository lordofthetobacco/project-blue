//! The simulation interface the server drives, independent of the
//! implementation ([`crate::standin::StandInSim`] or
//! [`crate::native_sim::NativeSim`]).
//!
//! Every read-out fills a caller-provided slice and returns the count, so
//! the tick loop never allocates.

use blue_protocol::message::DroppedWeapon;
use blue_protocol::{Message, PlayerState, RemotePlayer, UserCommand, WorldObject};

/// A player's collision capsule: vertical, from `feet` up `height` metres.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Capsule {
    /// Bottom of the capsule (feet), metres.
    pub feet: [f32; 3],
    /// Total height including both caps, metres.
    pub height: f32,
    /// Radius, metres.
    pub radius: f32,
    /// Only live players can be hit.
    pub alive: bool,
}

/// One projectile's motion during the last step.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Segment {
    /// Projectile id (opaque to the server).
    pub projectile: u32,
    /// Player who fired it.
    pub shooter: u8,
    /// Start of the swept segment.
    pub from: [f32; 3],
    /// End of the swept segment.
    pub to: [f32; 3],
    /// Fraction along the segment where it hits the world (1 = no hit).
    pub world_fraction: f32,
}

/// A projectile segment that hit a player's (lag-compensated) capsule.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct PlayerHit {
    /// Index of the segment in the sweep output.
    pub segment: u32,
    /// Projectile id.
    pub projectile: u32,
    /// Player hit.
    pub victim: u8,
    /// Fraction along the segment of the hit.
    pub fraction: f32,
    /// Hit point in the rewound frame, metres.
    pub point: [f32; 3],
    /// Height of the hit above the rewound feet over the capsule height,
    /// 0..=1 (the sim derives the hit zone from it).
    pub height_ratio: f32,
}

/// A simulation event to forward as a reliable message.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct SimEvent {
    /// The message, already in wire form.
    pub message: Message,
    /// A player who should not receive it (e.g. the shooter of a `Shot`,
    /// who predicted it).
    pub exclude: Option<u8>,
}

/// What the server needs from a simulation.
///
/// Players are identified by their wire `player_id` (0..=63). The server
/// calls [`Simulation::apply_command`] exactly once per player per tick
/// (repeating the previous held buttons when the client's command is
/// missing), then [`Simulation::step`]. Implementations need not be `Send`:
/// the server creates, uses and drops its simulation on the tick thread.
pub trait Simulation {
    /// Hash of the content clients must match (scene, weapon table).
    fn content_hash(&self) -> u64;
    /// Adds a player at a spawn point; false if it cannot (id in use).
    fn add_player(&mut self, id: u8) -> bool;
    /// Removes a player (no-op if absent).
    fn remove_player(&mut self, id: u8);
    /// Sets the command player `id` runs in the next step.
    fn apply_command(&mut self, id: u8, command: &UserCommand);
    /// Advances one tick.
    fn step(&mut self, tick: u32);
    /// Full state of player `id` (for its own client).
    fn own_state(&self, id: u8) -> Option<PlayerState>;
    /// State of player `id` as other clients see it.
    fn remote_state(&self, id: u8) -> Option<RemotePlayer>;
    /// Current capsule of player `id`.
    fn capsule(&self, id: u8) -> Option<Capsule>;
    /// Projectile segments swept during the last step.
    fn sweep_projectiles(&mut self, out: &mut [Segment]) -> usize;
    /// Applies the player hits found for the last sweep (damage, deaths,
    /// impacts) and retires the projectiles that hit something.
    fn resolve_projectiles(&mut self, hits: &[PlayerHit]);
    /// World objects (dynamic bodies, dropped weapons).
    fn read_objects(&self, out: &mut [WorldObject]) -> usize;
    /// Events since the last call.
    fn poll_events(&mut self, out: &mut [SimEvent]) -> usize;
    /// Instance ids of scene pickups already taken (late-join sync).
    fn taken_pickups(&self, out: &mut [u32]) -> usize;
    /// Dropped weapons lying in the world (late-join sync).
    fn dropped_weapons(&self, out: &mut [DroppedWeapon]) -> usize;
}
