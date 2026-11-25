//! `NativeSim`: the C++ simulation core through `blue-native`.
//!
//! Translates between wire types and the C ABI: player ids are offset by one
//! (wire id `n` is sim id `n + 1`, because the C events use 0 for "no
//! player"), own state maps field for field onto the v2 wire `PlayerState`
//! (saturating `jump_age`, `jump_cooldown` and round counts to `u8`), and
//! sim events become protocol messages. Players, bodies and dropped weapons
//! are read into caches after each step and resolve, so per-player queries
//! and snapshots do not cross the FFI boundary.
//!
//! Not `Send`: the library must be created, used and destroyed on one
//! thread, so the server builds it on its tick thread.

use blue_native::sim::{
    self, BlueBodyState, BlueDroppedWeapon, BluePickup, BluePlayerState, BlueSegment, BlueShotHit,
    BlueSimEvent, BlueUserCommand, BlueVec3, Sim, SimConfig,
};
use blue_protocol::message::{DroppedWeapon, HitZone, MAX_PLAYERS, Pose};
use blue_protocol::object::ObjectKind;
use blue_protocol::player::{WeaponSlotState, flags};
use blue_protocol::{Message, PlayerState, RemotePlayer, UserCommand, WorldObject};

use crate::log;
use crate::sim::{Capsule, PlayerHit, Segment, SimEvent, Simulation};

const MAX_BODIES: usize = 512;
const MAX_DROPPED: usize = 256;
const MAX_PICKUPS: usize = 256;
const MAX_SEGMENTS: usize = 4096;
const MAX_EVENTS: usize = 256;

fn sim_id(wire: u8) -> u32 {
    u32::from(wire) + 1
}

/// Wire id of sim id `id` (`None` for 0 or out of range).
fn wire_id(id: u32) -> Option<u8> {
    let wire = id.checked_sub(1)?;
    u8::try_from(wire)
        .ok()
        .filter(|&w| usize::from(w) < MAX_PLAYERS)
}

fn sat_u8(v: i32) -> u8 {
    v.clamp(0, 255) as u8
}

fn finite(v: f32) -> f32 {
    if v.is_finite() { v } else { 0.0 }
}

fn finite3(v: BlueVec3) -> [f32; 3] {
    [finite(v.x), finite(v.y), finite(v.z)]
}

/// The v2 wire own state of a sim player.
#[must_use]
pub fn own_state(p: &BluePlayerState) -> PlayerState {
    let mut f = 0u8;
    for (on, bit) in [
        (p.crouching, flags::CROUCHING),
        (p.sprinting, flags::SPRINTING),
        (p.aiming, flags::AIMING),
        (p.alive, flags::ALIVE),
        (p.on_ground, flags::ON_GROUND),
        (p.jump_pending, flags::JUMP_PENDING),
    ] {
        if on != 0 {
            f |= bit;
        }
    }
    let mut slots = [WeaponSlotState::default(); 2];
    for (out, slot) in slots.iter_mut().zip(&p.slots) {
        if slot.present == 0 {
            continue;
        }
        let fa = &slot.firearm;
        let spare_count = fa.spare_count.min(8);
        let mut spare_magazines = [0u8; 8];
        for (dst, src) in spare_magazines
            .iter_mut()
            .zip(&fa.spares)
            .take(usize::from(spare_count))
        {
            *dst = sat_u8(i32::from(*src));
        }
        *out = WeaponSlotState {
            present: true,
            weapon_key: slot.weapon_key,
            fire_mode: fa.fire_mode.min(15),
            magazine_rounds: sat_u8(i32::from(fa.magazine)),
            magazine_inserted: fa.magazine_inserted != 0,
            chambered: fa.chambered != 0,
            bolt_locked: fa.bolt_locked != 0,
            empty_reload: fa.empty_reload != 0,
            reload_stage: fa.reload_stage.clamp(0, 7) as u8,
            reload_elapsed: finite(fa.reload_elapsed),
            reload_duration: finite(fa.reload_duration),
            cycle_remaining: finite(fa.cycle_remaining),
            spare_count,
            spare_magazines,
            incoming_magazine: fa.incoming_magazine.clamp(-1, 255),
            pull_used: fa.pull_used != 0,
            scope_magnification: finite(slot.scope_magnification),
            scope_zero: finite(slot.scope_zero),
            spares_dropped: fa.spares_dropped,
        };
    }
    PlayerState {
        position: finite3(p.position),
        velocity: finite3(p.velocity),
        yaw: finite(p.yaw),
        pitch: finite(p.pitch),
        eye_height: finite(p.eye_height),
        flags: f,
        jump_age: sat_u8(p.jump_age),
        jump_cooldown: sat_u8(p.jump_cooldown),
        health: p.health,
        respawn_ticks: p.respawn_ticks,
        shots_fired: p.shots_fired,
        active_slot: p.active_slot.clamp(-1, 7),
        selected_slot: p.selected_slot.clamp(-1, 7),
        raise: finite(p.raise),
        jump_speed: finite(p.jump_speed),
        deaths: p.deaths,
        wait_for_release: p.wait_for_release != 0,
        slots,
    }
}

fn active_weapon(p: &BluePlayerState) -> u32 {
    usize::try_from(p.active_slot)
        .ok()
        .and_then(|i| p.slots.get(i))
        .filter(|s| s.present != 0)
        .map_or(0, |s| s.weapon_key)
}

fn unit(v: BlueVec3) -> [f32; 3] {
    let len = (v.x * v.x + v.y * v.y + v.z * v.z).sqrt();
    if len > 1e-6 && len.is_finite() {
        [v.x / len, v.y / len, v.z / len]
    } else {
        [0.0, 0.0, -1.0]
    }
}

/// The native simulation.
pub struct NativeSim {
    sim: Sim,
    content_hash: u64,
    players: Box<[Option<BluePlayerState>; MAX_PLAYERS]>,
    read: Box<[BluePlayerState; MAX_PLAYERS]>,
    bodies: Box<[BlueBodyState; MAX_BODIES]>,
    body_count: usize,
    dropped: Box<[BlueDroppedWeapon; MAX_DROPPED]>,
    dropped_count: usize,
    segments: Box<[BlueSegment; MAX_SEGMENTS]>,
    hits: Box<[BlueShotHit; MAX_SEGMENTS]>,
    events: Box<[BlueSimEvent; MAX_EVENTS]>,
    errors: u64,
}

impl std::fmt::Debug for NativeSim {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("NativeSim")
            .field("content_hash", &self.content_hash)
            .finish_non_exhaustive()
    }
}

/// Engine log lines become structured log lines (may run on physics
/// worker threads; the logger only locks stdout).
fn sim_log(level: i32, message: &str) {
    let fields = [("message", message.into())];
    match level {
        0 => log::info("blue_sim", &fields),
        1 => log::warn("blue_sim", &fields),
        2 => log::error("blue_sim", &fields),
        _ => log::error("blue_sim_fatal", &fields),
    }
}

impl NativeSim {
    /// Loads `scene` from `asset_dir` (checks the ABI first).
    ///
    /// # Errors
    /// ABI mismatch or the library's load error, as a message.
    pub fn create(asset_dir: &str, scene: &str, max_players: u8) -> Result<Self, String> {
        if let Some(feature) = crate::cpu::missing_native_feature() {
            return Err(format!(
                "this CPU lacks {feature}: the native simulation needs an x86-64-v3 CPU \
                 (AVX2, BMI1, F16C, LZCNT, POPCNT; Intel Haswell / AMD Excavator or newer)"
            ));
        }
        sim::set_log_sink(sim_log);
        let sim = Sim::create(&SimConfig {
            asset_dir,
            scene_path: scene,
            worker_threads: 0,
            max_players: i32::from(max_players),
        })
        .map_err(|e| e.to_string())?;
        let content_hash = sim.content_hash().map_err(|e| e.to_string())?;
        let mut native = Self {
            sim,
            content_hash,
            players: Box::new([None; MAX_PLAYERS]),
            read: Box::new([BluePlayerState::default(); MAX_PLAYERS]),
            bodies: Box::new([BlueBodyState::default(); MAX_BODIES]),
            body_count: 0,
            dropped: Box::new([BlueDroppedWeapon::default(); MAX_DROPPED]),
            dropped_count: 0,
            segments: Box::new([BlueSegment::default(); MAX_SEGMENTS]),
            hits: Box::new([BlueShotHit::default(); MAX_SEGMENTS]),
            events: Box::new([BlueSimEvent::default(); MAX_EVENTS]),
            errors: 0,
        };
        native.refresh();
        Ok(native)
    }

    fn report(&mut self, what: &str, error: &sim::SimError) {
        self.errors += 1;
        // Log the first few and then every 1000th, never every tick.
        if self.errors <= 5 || self.errors.is_multiple_of(1000) {
            let text = error.to_string();
            log::error(
                "native_sim_error",
                &[
                    ("call", what.into()),
                    ("error", text.as_str().into()),
                    ("count", self.errors.into()),
                ],
            );
        }
    }

    /// Re-reads players, bodies and dropped weapons into the caches.
    fn refresh(&mut self) {
        match self.sim.read_bodies(&mut self.bodies[..]) {
            Ok(n) => self.body_count = n,
            Err(e) => {
                self.body_count = 0;
                self.report("read_bodies", &e);
            }
        }
        match self.sim.read_players(&mut self.read[..]) {
            Ok(n) => {
                *self.players = [None; MAX_PLAYERS];
                for p in &self.read[..n] {
                    if let Some(id) = wire_id(p.id) {
                        self.players[usize::from(id)] = Some(*p);
                    }
                }
            }
            Err(e) => self.report("read_players", &e),
        }
        match self.sim.read_dropped_weapons(&mut self.dropped[..]) {
            Ok(n) => self.dropped_count = n,
            Err(e) => {
                self.dropped_count = 0;
                self.report("read_dropped_weapons", &e);
            }
        }
    }

    fn player(&self, id: u8) -> Option<&BluePlayerState> {
        self.players.get(usize::from(id)).and_then(Option::as_ref)
    }

    fn dropped_weapon(&self, id: u32, fallback_key: u32, rounds: Option<i32>) -> DroppedWeapon {
        let found = self.dropped[..self.dropped_count]
            .iter()
            .find(|d| d.id == id);
        DroppedWeapon {
            dropped_id: id,
            weapon_key: found.map_or(fallback_key, |d| d.weapon_key),
            rounds_in_magazine: sat_u8(
                rounds.unwrap_or_else(|| found.map_or(0, |d| i32::from(d.magazine))),
            ),
            pose: Pose {
                position: found.map_or([0.0; 3], |d| finite3(d.position)),
                rotation: found.map_or([0.0, 0.0, 0.0, 1.0], |d| d.rotation.to_array()),
            },
        }
    }

    fn convert_event(&mut self, e: &BlueSimEvent) -> Option<SimEvent> {
        let player = wire_id(e.player);
        let other = wire_id(e.other);
        let message = match e.kind {
            sim::event::SHOT => {
                let shooter = player?;
                return Some(SimEvent {
                    message: Message::Shot {
                        shooter,
                        origin: finite3(e.position),
                        direction: unit(e.vector),
                        weapon_key: e.weapon_key,
                        shot_number: e.id,
                    },
                    exclude: Some(shooter),
                });
            }
            sim::event::IMPACT => Message::Impact {
                point: finite3(e.position),
                normal: finite3(e.vector),
            },
            sim::event::DAMAGE => Message::Damage {
                victim: player?,
                attacker: other,
                amount: e.value.clamp(i32::from(i16::MIN), i32::from(i16::MAX)) as i16,
                zone: if other.is_some() {
                    HitZone(e.zone.min(HitZone::MAX))
                } else {
                    HitZone::OTHER
                },
                health_after: e.health.clamp(i32::from(i16::MIN), i32::from(i16::MAX)) as i16,
            },
            sim::event::DEATH => Message::Death {
                victim: player?,
                killer: other,
                weapon_key: e.weapon_key,
            },
            sim::event::RESPAWN => {
                let id = player?;
                Message::Respawn {
                    player_id: id,
                    position: finite3(e.position),
                    yaw: finite(e.yaw),
                }
            }
            sim::event::WEAPON_DROPPED => {
                Message::WeaponDropped(self.dropped_weapon(e.id, e.weapon_key, Some(e.value)))
            }
            sim::event::DROPPED_TAKEN => Message::DroppedTaken {
                dropped_id: e.id,
                player_id: player?,
            },
            sim::event::DROPPED_REMOVED => Message::DroppedRemoved { dropped_id: e.id },
            sim::event::PICKUP_TAKEN => Message::PickupTaken {
                instance_id: e.id,
                player_id: player?,
            },
            // Unknown kinds are skipped.
            _ => return None,
        };
        Some(SimEvent {
            message,
            exclude: None,
        })
    }
}

impl Simulation for NativeSim {
    fn content_hash(&self) -> u64 {
        self.content_hash
    }

    fn add_player(&mut self, id: u8) -> bool {
        let ok = match self.sim.add_player(sim_id(id)) {
            Ok(()) => true,
            Err(e) => {
                self.report("add_player", &e);
                false
            }
        };
        self.refresh();
        ok
    }

    fn remove_player(&mut self, id: u8) {
        if let Err(e) = self.sim.remove_player(sim_id(id)) {
            self.report("remove_player", &e);
        }
        self.refresh();
    }

    fn apply_command(&mut self, id: u8, command: &UserCommand) {
        let c = BlueUserCommand {
            tick: command.tick,
            held: command.held,
            pressed: command.pressed,
            yaw: command.yaw,
            pitch: command.pitch,
            select_slot: command.select_slot,
            interact_kind: command.interact_kind,
            _pad: [0; 2],
            interact_id: command.interact_id,
        };
        if let Err(e) = self.sim.apply_command(sim_id(id), &c) {
            self.report("apply_command", &e);
        }
    }

    fn step(&mut self, tick: u32) {
        if let Err(e) = self.sim.step(tick) {
            self.report("step", &e);
        }
        self.refresh();
    }

    fn own_state(&self, id: u8) -> Option<PlayerState> {
        self.player(id).map(own_state)
    }

    fn remote_state(&self, id: u8) -> Option<RemotePlayer> {
        let p = self.player(id)?;
        let own = own_state(p);
        Some(RemotePlayer {
            position: own.position,
            velocity: own.velocity,
            yaw: own.yaw,
            pitch: own.pitch,
            flags: own.flags,
            health: own.health,
            weapon_key: active_weapon(p),
        })
    }

    fn capsule(&self, id: u8) -> Option<Capsule> {
        let p = self.player(id)?;
        let c = self.sim.player_capsule(sim_id(id)).ok()?;
        Some(Capsule {
            feet: finite3(c.feet),
            height: finite(c.height),
            radius: finite(c.radius),
            alive: p.alive != 0,
        })
    }

    fn sweep_projectiles(&mut self, out: &mut [Segment]) -> usize {
        let cap = out.len().min(MAX_SEGMENTS);
        let n = match self.sim.sweep(&mut self.segments[..cap]) {
            Ok(n) => n,
            Err(e) => {
                self.report("projectiles_sweep", &e);
                return 0;
            }
        };
        for (dst, s) in out.iter_mut().zip(&self.segments[..n]) {
            let from = finite3(s.origin);
            let delta = finite3(s.delta);
            *dst = Segment {
                projectile: s.projectile as u32,
                // Unknown owners (0, or not a player) exclude nobody.
                shooter: wire_id(s.owner).unwrap_or(u8::MAX),
                from,
                to: [from[0] + delta[0], from[1] + delta[1], from[2] + delta[2]],
                world_fraction: finite(s.world_fraction).clamp(0.0, 1.0),
            };
        }
        n
    }

    fn resolve_projectiles(&mut self, hits: &[PlayerHit]) {
        let n = hits.len().min(MAX_SEGMENTS);
        for (dst, h) in self.hits.iter_mut().zip(&hits[..n]) {
            *dst = BlueShotHit {
                segment: h.segment as i32,
                victim: sim_id(h.victim),
                fraction: h.fraction,
                height_fraction: h.height_ratio,
            };
        }
        if let Err(e) = self.sim.resolve(&self.hits[..n]) {
            self.report("projectiles_resolve", &e);
        }
        self.refresh();
    }

    fn read_objects(&self, out: &mut [WorldObject]) -> usize {
        let mut count = 0;
        for b in &self.bodies[..self.body_count] {
            let Some(slot) = out.get_mut(count) else {
                return count;
            };
            *slot = WorldObject {
                kind: ObjectKind::Body,
                id: b.instance,
                at_rest: b.active == 0,
                position: finite3(b.position),
                rotation: b.rotation.to_array(),
                linear_velocity: finite3(b.linear_velocity),
                angular_velocity: finite3(b.angular_velocity),
            };
            count += 1;
        }
        for d in &self.dropped[..self.dropped_count] {
            let Some(slot) = out.get_mut(count) else {
                return count;
            };
            *slot = WorldObject {
                kind: ObjectKind::DroppedWeapon,
                id: d.id,
                at_rest: d.active == 0,
                position: finite3(d.position),
                rotation: d.rotation.to_array(),
                linear_velocity: finite3(d.linear_velocity),
                angular_velocity: finite3(d.angular_velocity),
            };
            count += 1;
        }
        count
    }

    fn poll_events(&mut self, out: &mut [SimEvent]) -> usize {
        let cap = out.len().min(MAX_EVENTS);
        let n = match self.sim.poll_events(&mut self.events[..cap]) {
            Ok(n) => n.min(cap),
            Err(e) => {
                self.report("poll_events", &e);
                return 0;
            }
        };
        let mut written = 0;
        for k in 0..n {
            let event = self.events[k];
            if let Some(converted) = self.convert_event(&event) {
                out[written] = converted;
                written += 1;
            }
        }
        written
    }

    fn taken_pickups(&self, out: &mut [u32]) -> usize {
        // Join-time only: a stack buffer is fine here.
        let mut pickups = [BluePickup::default(); MAX_PICKUPS];
        let n = self.sim.read_pickups(&mut pickups).unwrap_or(0);
        let mut count = 0;
        for p in pickups[..n].iter().filter(|p| p.taken != 0) {
            let Some(slot) = out.get_mut(count) else {
                break;
            };
            *slot = p.instance;
            count += 1;
        }
        count
    }

    fn dropped_weapons(&self, out: &mut [DroppedWeapon]) -> usize {
        let mut count = 0;
        for d in &self.dropped[..self.dropped_count] {
            let Some(slot) = out.get_mut(count) else {
                break;
            };
            *slot = self.dropped_weapon(d.id, d.weapon_key, None);
            count += 1;
        }
        count
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use blue_native::sim::{BlueFirearmState, BlueLoadoutSlot};

    #[test]
    fn ids_are_offset_by_one() {
        assert_eq!(sim_id(0), 1);
        assert_eq!(wire_id(1), Some(0));
        assert_eq!(wire_id(64), Some(63));
        assert_eq!(wire_id(0), None);
        assert_eq!(wire_id(65), None);
        assert_eq!(wire_id(u32::MAX), None);
    }

    #[test]
    fn own_state_maps_and_saturates() {
        let mut p = BluePlayerState {
            id: 3,
            position: BlueVec3 {
                x: 1.0,
                y: 2.0,
                z: f32::NAN,
            },
            jump_age: 1000,
            jump_cooldown: -5,
            alive: 1,
            on_ground: 1,
            active_slot: 1,
            selected_slot: -1,
            raise: 0.25,
            deaths: 7,
            jump_speed: 4.0,
            wait_for_release: 1,
            ..BluePlayerState::default()
        };
        p.slots[1] = BlueLoadoutSlot {
            present: 1,
            weapon_key: 0xABCD,
            firearm: BlueFirearmState {
                fire_mode: 2,
                magazine: 400,
                incoming_magazine: -1,
                spare_count: 2,
                spares: [30, -3, 9, 0, 0, 0, 0, 0],
                pull_used: 1,
                reload_stage: 9,
                ..BlueFirearmState::default()
            },
            scope_magnification: 4.0,
            scope_zero: 300.0,
            ..BlueLoadoutSlot::default()
        };
        let s = own_state(&p);
        assert_eq!(s.position, [1.0, 2.0, 0.0]);
        assert_eq!(s.jump_age, 255);
        assert_eq!(s.jump_cooldown, 0);
        assert_eq!(s.flags, flags::ALIVE | flags::ON_GROUND);
        assert_eq!(s.deaths, 7);
        assert!(s.wait_for_release);
        assert!(!s.slots[0].present);
        let slot = s.slots[1];
        assert!(slot.present);
        assert_eq!(slot.weapon_key, 0xABCD);
        assert_eq!(slot.magazine_rounds, 255);
        assert_eq!(slot.spare_magazines, [30, 0, 0, 0, 0, 0, 0, 0]);
        assert_eq!(slot.incoming_magazine, -1);
        assert_eq!(slot.reload_stage, 7);
        assert!(slot.pull_used);
        assert_eq!(active_weapon(&p), 0xABCD);
        // The mapped state always encodes.
        let mut buf = [0u8; 256];
        let mut state = s;
        assert!(
            state
                .serialize(&mut blue_protocol::bits::BitWriter::new(&mut buf))
                .is_ok()
        );
    }

    #[test]
    fn unit_vectors() {
        let v = unit(BlueVec3 {
            x: 0.0,
            y: 3.0,
            z: 4.0,
        });
        assert!((v[1] - 0.6).abs() < 1e-6 && (v[2] - 0.8).abs() < 1e-6);
        assert_eq!(unit(BlueVec3::default()), [0.0, 0.0, -1.0]);
    }
}
