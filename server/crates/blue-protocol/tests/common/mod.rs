//! Shared test helpers: a deterministic RNG and random value generators.

#![allow(dead_code, clippy::unwrap_used)]

use blue_protocol::command::{MAX_SLOT, interact};
use blue_protocol::message::{
    DroppedWeapon, HitZone, MAX_PLAYER_ID, Message, Pose, RejectReason, SYNC_PICKUPS_PER_MESSAGE,
};
use blue_protocol::player::{MAX_SPARE_MAGAZINES, WeaponSlotState};
use blue_protocol::{Name, ObjectKind, PlayerState, RemotePlayer, UserCommand, WorldObject};

/// `SplitMix64`: tiny, fast, good enough for tests, fully deterministic.
pub struct Rng(u64);

impl Rng {
    pub fn new(seed: u64) -> Self {
        Self(seed)
    }

    pub fn next_u64(&mut self) -> u64 {
        self.0 = self.0.wrapping_add(0x9E37_79B9_7F4A_7C15);
        let mut z = self.0;
        z = (z ^ (z >> 30)).wrapping_mul(0xBF58_476D_1CE4_E5B9);
        z = (z ^ (z >> 27)).wrapping_mul(0x94D0_49BB_1331_11EB);
        z ^ (z >> 31)
    }

    pub fn u32(&mut self) -> u32 {
        (self.next_u64() >> 32) as u32
    }

    pub fn below(&mut self, n: u64) -> u64 {
        self.next_u64() % n
    }

    pub fn chance(&mut self, p: f64) -> bool {
        ((self.next_u64() >> 11) as f64 / (1u64 << 53) as f64) < p
    }

    pub fn f32_in(&mut self, lo: f32, hi: f32) -> f32 {
        let t = (self.next_u64() >> 40) as f32 / (1u64 << 24) as f32;
        lo + (hi - lo) * t
    }

    pub fn vec3(&mut self, lo: f32, hi: f32) -> [f32; 3] {
        [
            self.f32_in(lo, hi),
            self.f32_in(lo, hi),
            self.f32_in(lo, hi),
        ]
    }

    pub fn player_id(&mut self) -> u8 {
        self.below(u64::from(MAX_PLAYER_ID) + 1) as u8
    }

    pub fn bytes(&mut self, out: &mut [u8]) {
        for b in out {
            *b = self.next_u64() as u8;
        }
    }
}

pub fn name(rng: &mut Rng) -> Name {
    const ALPHABET: &[&str] = &["a", "Z", "0", "_", " ", "é", "ż", "€", "日"];
    let mut text = String::new();
    loop {
        let piece = ALPHABET[rng.below(ALPHABET.len() as u64) as usize];
        if text.len() + piece.len() > 32 || rng.chance(0.1) {
            break;
        }
        text.push_str(piece);
    }
    Name::new(&text).unwrap()
}

pub fn command(rng: &mut Rng, tick: u32) -> UserCommand {
    let interact_kind = if rng.chance(0.7) {
        interact::NONE
    } else {
        rng.below(16) as u8
    };
    UserCommand {
        tick,
        held: rng.u32() & 0xFF,
        pressed: if rng.chance(0.5) { 0 } else { rng.u32() & 0xFF },
        yaw: rng.f32_in(-20.0, 20.0),
        pitch: rng.f32_in(-1.7, 1.7),
        select_slot: if rng.chance(0.7) {
            -1
        } else {
            rng.below(MAX_SLOT as u64 + 1) as i8
        },
        interact_kind,
        interact_id: if interact_kind == interact::NONE {
            0
        } else {
            rng.u32()
        },
    }
}

/// A command that differs from `newer` in a few fields (realistic history).
pub fn older_command(rng: &mut Rng, newer: &UserCommand) -> UserCommand {
    let mut c = *newer;
    c.tick = newer.tick.wrapping_sub(1);
    if rng.chance(0.3) {
        c.held = rng.u32() & 0xFF;
    }
    if rng.chance(0.3) {
        c.pressed = rng.u32() & 0xFF;
    }
    if rng.chance(0.6) {
        c.yaw += rng.f32_in(-0.05, 0.05);
    }
    if rng.chance(0.6) {
        c.pitch = (c.pitch + rng.f32_in(-0.05, 0.05)).clamp(-1.5, 1.5);
    }
    if rng.chance(0.2) {
        let other = command(rng, c.tick);
        c.select_slot = other.select_slot;
        c.interact_kind = other.interact_kind;
        c.interact_id = other.interact_id;
    }
    c
}

fn optional_player(rng: &mut Rng) -> Option<u8> {
    if rng.chance(0.5) {
        Some(rng.player_id())
    } else {
        None
    }
}

fn pose(rng: &mut Rng) -> Pose {
    Pose {
        position: rng.vec3(-500.0, 500.0),
        rotation: [
            rng.f32_in(-1.0, 1.0),
            rng.f32_in(-1.0, 1.0),
            rng.f32_in(-1.0, 1.0),
            0.5,
        ],
    }
}

fn dropped(rng: &mut Rng) -> DroppedWeapon {
    DroppedWeapon {
        dropped_id: rng.u32(),
        weapon_key: rng.u32(),
        rounds_in_magazine: rng.u32() as u8,
        pose: pose(rng),
    }
}

/// A random message of type `index % 17` (handshake types included).
pub fn message_of(rng: &mut Rng, index: u64) -> Message {
    match index % 17 {
        0 => Message::ClientHello {
            protocol_version: rng.u32() as u16,
            content_hash: rng.next_u64(),
            name: name(rng),
        },
        1 => Message::ServerWelcome {
            player_id: rng.player_id(),
            tick_rate: (rng.u32() as u16).max(1),
            server_tick: rng.u32(),
            max_players: rng.below(64) as u8 + 1,
        },
        2 => Message::ServerReject {
            reason: RejectReason(rng.u32() as u8),
            server_protocol_version: rng.u32() as u16,
        },
        3 => Message::PlayerJoined {
            player_id: rng.player_id(),
            name: name(rng),
        },
        4 => Message::PlayerLeft {
            player_id: rng.player_id(),
        },
        5 => Message::Damage {
            victim: rng.player_id(),
            attacker: optional_player(rng),
            amount: rng.u32() as i16,
            zone: HitZone(rng.below(8) as u8),
            health_after: rng.u32() as i16,
        },
        6 => Message::Death {
            victim: rng.player_id(),
            killer: optional_player(rng),
            weapon_key: rng.u32(),
        },
        7 => Message::Respawn {
            player_id: rng.player_id(),
            position: rng.vec3(-1000.0, 1000.0),
            yaw: rng.f32_in(-7.0, 7.0),
        },
        8 => Message::Shot {
            shooter: rng.player_id(),
            origin: rng.vec3(-1000.0, 1000.0),
            direction: rng.vec3(-1.0, 1.0),
            weapon_key: rng.u32(),
            shot_number: rng.u32(),
        },
        9 => Message::Impact {
            point: rng.vec3(-1000.0, 1000.0),
            normal: rng.vec3(-1.0, 1.0),
        },
        10 => Message::WeaponDropped(dropped(rng)),
        11 => Message::DroppedTaken {
            dropped_id: rng.u32(),
            player_id: rng.player_id(),
        },
        12 => Message::PickupTaken {
            instance_id: rng.u32(),
            player_id: rng.player_id(),
        },
        13 => {
            let count = rng.below(SYNC_PICKUPS_PER_MESSAGE as u64 + 1) as u8;
            let mut instance_ids = [0; SYNC_PICKUPS_PER_MESSAGE];
            for id in &mut instance_ids[..usize::from(count)] {
                *id = rng.u32();
            }
            Message::WorldSyncPickupsTaken {
                count,
                instance_ids,
            }
        }
        14 => Message::WorldSyncDroppedWeapon(dropped(rng)),
        15 => Message::DroppedRemoved {
            dropped_id: rng.u32(),
        },
        _ => Message::WorldSyncDone,
    }
}

/// A random non-handshake message.
pub fn game_message(rng: &mut Rng) -> Message {
    let index = 3 + rng.below(14);
    message_of(rng, index)
}

pub fn weapon_slot(rng: &mut Rng) -> WeaponSlotState {
    if rng.chance(0.3) {
        return WeaponSlotState::default();
    }
    let spare_count = rng.below(MAX_SPARE_MAGAZINES as u64 + 1) as u8;
    let mut spare_magazines = [0; MAX_SPARE_MAGAZINES];
    for s in &mut spare_magazines[..usize::from(spare_count)] {
        *s = rng.u32() as u8;
    }
    WeaponSlotState {
        present: true,
        weapon_key: rng.u32(),
        fire_mode: rng.below(16) as u8,
        magazine_rounds: rng.u32() as u8,
        magazine_inserted: rng.chance(0.5),
        chambered: rng.chance(0.5),
        bolt_locked: rng.chance(0.5),
        empty_reload: rng.chance(0.5),
        reload_stage: rng.below(8) as u8,
        reload_elapsed: rng.f32_in(0.0, 3.0),
        reload_duration: rng.f32_in(0.0, 3.0),
        cycle_remaining: rng.f32_in(0.0, 0.2),
        spare_count,
        spare_magazines,
        incoming_magazine: rng.below(257) as i16 - 1,
        pull_used: rng.chance(0.5),
        scope_magnification: rng.f32_in(0.0, 12.0),
        scope_zero: rng.f32_in(0.0, 1000.0),
        spares_dropped: rng.u32() as u8,
    }
}

pub fn player_state(rng: &mut Rng) -> PlayerState {
    PlayerState {
        position: rng.vec3(-5000.0, 5000.0),
        velocity: rng.vec3(-30.0, 30.0),
        yaw: rng.f32_in(-7.0, 7.0),
        pitch: rng.f32_in(-1.6, 1.6),
        eye_height: rng.f32_in(0.8, 1.8),
        flags: (rng.u32() & 0x3F) as u8,
        jump_age: rng.u32() as u8,
        jump_cooldown: rng.u32() as u8,
        health: rng.u32() as i16,
        respawn_ticks: rng.u32() as u16,
        shots_fired: rng.u32(),
        active_slot: rng.below(9) as i8 - 1,
        selected_slot: rng.below(9) as i8 - 1,
        raise: rng.f32_in(0.0, 1.0),
        jump_speed: rng.f32_in(0.0, 8.0),
        deaths: rng.u32(),
        wait_for_release: rng.chance(0.5),
        slots: [weapon_slot(rng), weapon_slot(rng)],
    }
}

pub fn remote_player(rng: &mut Rng) -> RemotePlayer {
    RemotePlayer {
        position: rng.vec3(-20000.0, 20000.0),
        velocity: rng.vec3(-150.0, 150.0),
        yaw: rng.f32_in(-10.0, 10.0),
        pitch: rng.f32_in(-2.0, 2.0),
        flags: rng.u32() as u8,
        health: rng.u32() as i16,
        weapon_key: if rng.chance(0.3) { 0 } else { rng.u32() },
    }
}

pub fn world_object(rng: &mut Rng) -> WorldObject {
    WorldObject {
        kind: if rng.chance(0.5) {
            ObjectKind::Body
        } else {
            ObjectKind::DroppedWeapon
        },
        id: if rng.chance(0.5) {
            rng.below(1000) as u32
        } else {
            rng.u32()
        },
        at_rest: rng.chance(0.3),
        position: rng.vec3(-20000.0, 20000.0),
        rotation: [
            rng.f32_in(-1.0, 1.0),
            rng.f32_in(-1.0, 1.0),
            rng.f32_in(-1.0, 1.0),
            rng.f32_in(-1.0, 1.0),
        ],
        linear_velocity: rng.vec3(-150.0, 150.0),
        angular_velocity: rng.vec3(-150.0, 150.0),
    }
}
