//! Golden vectors shared with the C++ codec.
//!
//! `tests/vectors/<name>.bin` holds the exact bytes and `vectors.md` the
//! logical content of every vector. A normal run compares both; run with
//! `BLUE_BLESS=1` to regenerate them after an intentional layout change (and
//! bump `PROTOCOL_VERSION`).

#![allow(clippy::unwrap_used, clippy::panic)]

use std::fmt::Write as _;
use std::path::PathBuf;

use blue_protocol::command::{held, interact, pressed};
use blue_protocol::message::{DroppedWeapon, HitZone, Message, Pose, RejectReason};
use blue_protocol::packet::{decode_control, encode_control};
use blue_protocol::player::{WeaponSlotState, flags};
use blue_protocol::{
    ClientInput, CommandList, MAX_PAYLOAD_BYTES, Name, ObjectKind, PROTOCOL_VERSION, PacketHeader,
    PlayerSet, PlayerState, ReliableSection, RemotePlayer, ServerSnapshot, SnapshotInfo,
    SnapshotWriter, UserCommand, WorldObject,
};

struct Vector {
    name: String,
    bytes: Vec<u8>,
    description: String,
}

fn hex(bytes: &[u8]) -> String {
    let mut out = String::new();
    for (i, b) in bytes.iter().enumerate() {
        if i > 0 {
            out.push(if i % 16 == 0 { '\n' } else { ' ' });
        }
        write!(out, "{b:02x}").unwrap();
    }
    out
}

fn messages() -> Vec<(&'static str, Message)> {
    let pose = Pose {
        position: [12.5, -3.25, 100.0],
        rotation: [0.0, 0.6, 0.0, 0.8],
    };
    let dropped = DroppedWeapon {
        dropped_id: 70_000,
        weapon_key: 0xDEAD_BEEF,
        rounds_in_magazine: 17,
        pose,
    };
    let mut instance_ids = [0; 16];
    instance_ids[..3].copy_from_slice(&[1, 300, 0xFFFF_FFFF]);
    vec![
        (
            "client_hello",
            Message::ClientHello {
                protocol_version: PROTOCOL_VERSION,
                content_hash: 0x0123_4567_89AB_CDEF,
                name: Name::new("Zoë").unwrap(),
            },
        ),
        (
            "server_welcome",
            Message::ServerWelcome {
                player_id: 5,
                tick_rate: 60,
                server_tick: 123_456,
                max_players: 16,
            },
        ),
        (
            "server_reject",
            Message::ServerReject {
                reason: RejectReason::PROTOCOL_MISMATCH,
                server_protocol_version: PROTOCOL_VERSION,
            },
        ),
        (
            "player_joined",
            Message::PlayerJoined {
                player_id: 63,
                name: Name::new("bot_7").unwrap(),
            },
        ),
        ("player_left", Message::PlayerLeft { player_id: 2 }),
        (
            "damage",
            Message::Damage {
                victim: 3,
                attacker: Some(4),
                amount: 37,
                zone: HitZone::HEAD,
                health_after: -12,
            },
        ),
        (
            "death",
            Message::Death {
                victim: 3,
                killer: None,
                weapon_key: 0,
            },
        ),
        (
            "respawn",
            Message::Respawn {
                player_id: 3,
                position: [-8.0, 0.5, 16.25],
                yaw: 3.0,
            },
        ),
        (
            "shot",
            Message::Shot {
                shooter: 4,
                origin: [1.0, 1.75, -2.0],
                direction: [0.0, 0.0, -1.0],
                weapon_key: 0x1234_5678,
                shot_number: 42,
            },
        ),
        (
            "impact",
            Message::Impact {
                point: [5.5, 0.0, -30.125],
                normal: [0.0, 1.0, 0.0],
            },
        ),
        ("weapon_dropped", Message::WeaponDropped(dropped)),
        (
            "dropped_taken",
            Message::DroppedTaken {
                dropped_id: 70_000,
                player_id: 9,
            },
        ),
        (
            "pickup_taken",
            Message::PickupTaken {
                instance_id: 12,
                player_id: 0,
            },
        ),
        (
            "world_sync_pickups_taken",
            Message::WorldSyncPickupsTaken {
                count: 3,
                instance_ids,
            },
        ),
        (
            "world_sync_dropped_weapon",
            Message::WorldSyncDroppedWeapon(dropped),
        ),
        ("world_sync_done", Message::WorldSyncDone),
        (
            "dropped_removed",
            Message::DroppedRemoved { dropped_id: 70_001 },
        ),
    ]
}

fn command(tick: u32) -> UserCommand {
    UserCommand {
        tick,
        held: held::FORWARD | held::SPRINT,
        pressed: 0,
        yaw: 1.5,
        pitch: -0.25,
        select_slot: -1,
        interact_kind: interact::NONE,
        interact_id: 0,
    }
}

fn client_inputs() -> Vec<(&'static str, ClientInput)> {
    let single = ClientInput {
        header: PacketHeader {
            sequence: 10,
            ack: 20,
            ack_bits: 0x0000_0005,
        },
        snapshot_ack: None,
        view_tick: 0,
        view_fraction: 0,
        commands: CommandList::from_slice(&[command(1000)]).unwrap(),
        reliable: ReliableSection::default(),
    };
    let newest = UserCommand {
        pressed: pressed::JUMP | pressed::INTERACT,
        interact_kind: interact::PICKUP,
        interact_id: 77,
        ..command(70_001)
    };
    let older = [
        newest,
        UserCommand {
            yaw: 1.5 - 0.001,
            ..command(70_000)
        },
        UserCommand {
            pitch: 0.125,
            held: held::FIRE | held::AIM,
            ..command(69_999)
        },
        UserCommand {
            select_slot: 1,
            ..command(69_998)
        },
    ];
    let mut reliable = ReliableSection::default();
    reliable
        .push(65_535, Message::PlayerLeft { player_id: 1 })
        .unwrap();
    reliable
        .push(
            2,
            Message::PickupTaken {
                instance_id: 5,
                player_id: 1,
            },
        )
        .unwrap();
    let multi = ClientInput {
        header: PacketHeader {
            sequence: 65_535,
            ack: 0,
            ack_bits: 0xFFFF_FFFF,
        },
        snapshot_ack: Some(69_990),
        view_tick: 69_988,
        view_fraction: 192,
        commands: CommandList::from_slice(&older).unwrap(),
        reliable,
    };
    vec![
        ("client_input_single", single),
        ("client_input_multi", multi),
    ]
}

fn own_state() -> PlayerState {
    let mut rifle = WeaponSlotState {
        present: true,
        weapon_key: 0xCAFE_F00D,
        fire_mode: 1,
        magazine_rounds: 29,
        magazine_inserted: true,
        chambered: true,
        bolt_locked: false,
        empty_reload: false,
        reload_stage: 0,
        reload_elapsed: 0.0,
        reload_duration: 0.0,
        cycle_remaining: 0.0625,
        spare_count: 3,
        spare_magazines: [0; 8],
        incoming_magazine: -1,
        pull_used: true,
        scope_magnification: 4.0,
        scope_zero: 300.0,
        spares_dropped: 0,
    };
    rifle.spare_magazines[..3].copy_from_slice(&[30, 30, 12]);
    PlayerState {
        position: [10.0, 0.0, -4.5],
        velocity: [0.0, -9.8125, 6.0],
        yaw: 0.75,
        pitch: -0.125,
        eye_height: 1.625,
        flags: flags::ALIVE | flags::SPRINTING,
        jump_age: 200,
        jump_cooldown: 0,
        health: 100,
        respawn_ticks: 0,
        shots_fired: 1234,
        active_slot: 1,
        selected_slot: 1,
        raise: 0.5,
        jump_speed: 4.5,
        deaths: 3,
        wait_for_release: true,
        slots: [WeaponSlotState::default(), rifle],
    }
}

fn remote(x: f32, health: i16) -> RemotePlayer {
    RemotePlayer {
        position: [x, 0.0, 2.5],
        velocity: [1.0, 0.0, -0.5],
        yaw: 1.0,
        pitch: 0.0,
        flags: flags::ALIVE | flags::ON_GROUND,
        health,
        weapon_key: 0xCAFE_F00D,
    }
}

fn objects() -> Vec<WorldObject> {
    vec![
        WorldObject {
            kind: ObjectKind::Body,
            id: 3,
            at_rest: true,
            position: [1.0, 0.5, -2.0],
            rotation: [0.0, 0.0, 0.0, 1.0],
            linear_velocity: [0.0; 3],
            angular_velocity: [0.0; 3],
        },
        WorldObject {
            kind: ObjectKind::DroppedWeapon,
            id: 70_000,
            at_rest: false,
            position: [12.5, 3.0, 100.0],
            rotation: [0.0, 0.6, 0.0, 0.8],
            linear_velocity: [0.5, -4.0, 0.25],
            angular_velocity: [0.0, 3.0, -1.5],
        },
    ]
}

struct SnapshotCase {
    header: PacketHeader,
    info: SnapshotInfo,
    own: Option<PlayerState>,
    reliable: ReliableSection,
    current: Vec<(u8, RemotePlayer)>,
    baseline: Option<Vec<(u8, RemotePlayer)>>,
    objects: Vec<WorldObject>,
}

fn set_of(players: &[(u8, RemotePlayer)]) -> PlayerSet {
    let mut set = PlayerSet::default();
    for (id, p) in players {
        set.set(*id, Some(p.quantise())).unwrap();
    }
    set
}

fn snapshots() -> Vec<(&'static str, SnapshotCase)> {
    let mut reliable = ReliableSection::default();
    reliable
        .push(
            0,
            Message::PlayerJoined {
                player_id: 2,
                name: Name::new("Ann").unwrap(),
            },
        )
        .unwrap();
    reliable
        .push(
            1,
            Message::Damage {
                victim: 0,
                attacker: Some(2),
                amount: 25,
                zone: HitZone::TORSO,
                health_after: 75,
            },
        )
        .unwrap();
    let full = SnapshotCase {
        header: PacketHeader {
            sequence: 300,
            ack: 299,
            ack_bits: 0x8000_0001,
        },
        info: SnapshotInfo {
            server_tick: 5000,
            last_command_tick: Some(5002),
            input_buffer_depth: 2,
            baseline_tick: None,
        },
        own: Some(own_state()),
        reliable,
        current: vec![
            (1, remote(-3.0, 100)),
            (2, remote(4.0, 80)),
            (40, remote(0.001, 1)),
        ],
        baseline: None,
        objects: objects(),
    };
    let moved = RemotePlayer {
        position: [-2.75, 0.0, 2.5],
        ..remote(-3.0, 100)
    };
    let hurt = RemotePlayer {
        health: 55,
        yaw: 2.0,
        ..remote(4.0, 80)
    };
    let delta = SnapshotCase {
        header: PacketHeader {
            sequence: 301,
            ack: 300,
            ack_bits: 0x0000_0003,
        },
        info: SnapshotInfo {
            server_tick: 5001,
            last_command_tick: Some(5003),
            input_buffer_depth: -1,
            baseline_tick: Some(4996),
        },
        own: None,
        reliable: ReliableSection::default(),
        current: vec![
            (1, moved),
            (2, hurt),
            (5, remote(7.0, 100)),
            (40, remote(0.001, 1)),
        ],
        baseline: Some(vec![
            (1, remote(-3.0, 100)),
            (2, remote(4.0, 80)),
            (3, remote(9.0, 10)),
            (40, remote(0.001, 1)),
        ]),
        objects: vec![objects()[1]],
    };
    vec![
        ("server_snapshot_full", full),
        ("server_snapshot_delta", delta),
    ]
}

fn build_vectors() -> Vec<Vector> {
    let mut out = Vec::new();
    for (name, message) in messages() {
        let mut buf = [0u8; 256];
        let len = message.encode_standalone(&mut buf).unwrap();
        assert_eq!(Message::decode_standalone(&buf[..len]).unwrap(), message);
        out.push(Vector {
            name: format!("msg_{name}"),
            bytes: buf[..len].to_vec(),
            description: format!(
                "Standalone message: type id ({}) + body, zero padded to a byte.\n\n```text\n{message:#?}\n```",
                message.type_id()
            ),
        });
        if message.is_handshake() {
            let mut buf = [0u8; MAX_PAYLOAD_BYTES];
            let len = encode_control(&message, &mut buf).unwrap();
            assert_eq!(decode_control(&buf[..len]).unwrap(), message);
            out.push(Vector {
                name: format!("control_{name}"),
                bytes: buf[..len].to_vec(),
                description: format!(
                    "Control packet (kind 2) carrying the message of `msg_{name}`."
                ),
            });
        }
    }
    for (name, input) in client_inputs() {
        let mut buf = [0u8; MAX_PAYLOAD_BYTES];
        let len = input.encode(&mut buf).unwrap();
        let mut decoded = ClientInput::default();
        ClientInput::decode(&buf[..len], &mut decoded).unwrap();
        let mut expected = input;
        expected.commands = input.commands.canonical();
        assert_eq!(decoded, expected);
        out.push(Vector {
            name: name.to_owned(),
            bytes: buf[..len].to_vec(),
            description: format!(
                "`ClientInput::encode` of the value below. Commands are newest first; their \
                 angles are quantised on encode (canonical values: {:?}).\n\n```text\n{input:#?}\n```",
                expected.commands.as_slice().iter().map(|c| (c.yaw, c.pitch)).collect::<Vec<_>>()
            ),
        });
    }
    for (name, case) in snapshots() {
        let mut buf = [0u8; MAX_PAYLOAD_BYTES];
        let current = set_of(&case.current);
        let baseline = case.baseline.as_deref().map(set_of);
        let mut w = SnapshotWriter::begin(
            &mut buf,
            &case.header,
            &case.info,
            case.own.as_ref(),
            &case.reliable,
        )
        .unwrap();
        let priority: Vec<u8> = (0..64).collect();
        let mut record = PlayerSet::default();
        w.write_players(&current, baseline.as_ref(), &priority, 0, &mut record)
            .unwrap();
        assert_eq!(record, current);
        for object in &case.objects {
            assert!(w.try_add_object(object).unwrap());
        }
        let len = w.finish().unwrap();
        let mut decoded = ServerSnapshot::default();
        ServerSnapshot::decode(&buf[..len], &mut decoded).unwrap();
        let mut set = PlayerSet::default();
        decoded.players.apply(baseline.as_ref(), &mut set).unwrap();
        assert_eq!(set, current);
        let quantised: Vec<WorldObject> = case.objects.iter().map(WorldObject::quantised).collect();
        assert_eq!(decoded.objects.as_slice(), quantised.as_slice());
        let quantised_players = |players: &[(u8, RemotePlayer)]| {
            players
                .iter()
                .map(|(id, p)| (*id, p.quantise()))
                .collect::<Vec<_>>()
        };
        let mut description = String::new();
        writeln!(
            description,
            "`SnapshotWriter::begin(header, info, own, reliable)`, then \
             `write_players(current, baseline, priority = [0, 1, ..., 63], reserve_bytes = 0)`, \
             then `try_add_object` for each object in order, then `finish`.\n"
        )
        .unwrap();
        writeln!(
            description,
            "```text\nheader: {:#?}\ninfo: {:#?}\nown: {:#?}\nreliable: {:#?}",
            case.header,
            case.info,
            case.own,
            case.reliable.entries()
        )
        .unwrap();
        writeln!(description, "current (sim units): {:#?}", case.current).unwrap();
        writeln!(
            description,
            "current (quantised): {:#?}",
            quantised_players(&case.current)
        )
        .unwrap();
        if let Some(b) = &case.baseline {
            writeln!(description, "baseline (sim units): {b:#?}").unwrap();
            writeln!(
                description,
                "baseline (quantised): {:#?}",
                quantised_players(b)
            )
            .unwrap();
        }
        writeln!(description, "objects (sim units): {:#?}", case.objects).unwrap();
        writeln!(description, "objects (as decoded): {quantised:#?}\n```").unwrap();
        out.push(Vector {
            name: name.to_owned(),
            bytes: buf[..len].to_vec(),
            description,
        });
    }
    out
}

fn vectors_dir() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("tests")
        .join("vectors")
}

fn render_markdown(vectors: &[Vector]) -> String {
    let mut md = String::new();
    writeln!(
        md,
        "# Golden vectors (protocol version {PROTOCOL_VERSION})\n"
    )
    .unwrap();
    writeln!(
        md,
        "Generated by `tests/golden.rs` with `BLUE_BLESS=1 cargo test`; do not edit by hand.\n\
         Each `<name>.bin` holds the exact bytes. Values are Rust `Debug` output: floats are\n\
         binary32 printed in shortest round-trip form, `None`/`Some(x)` are optional fields,\n\
         and `PlayerSet`s are listed as `(player_id, state)` pairs. Spec: `docs/netcode/protocol.md`.\n"
    )
    .unwrap();
    for v in vectors {
        writeln!(
            md,
            "## {}\n\n{}\n\n{} bytes:\n\n```text\n{}\n```\n",
            v.name,
            v.description,
            v.bytes.len(),
            hex(&v.bytes)
        )
        .unwrap();
    }
    md
}

#[test]
fn golden_vectors_match() {
    let vectors = build_vectors();
    let dir = vectors_dir();
    let markdown = render_markdown(&vectors);
    if std::env::var_os("BLUE_BLESS").is_some_and(|v| v == "1") {
        std::fs::create_dir_all(&dir).unwrap();
        for v in &vectors {
            std::fs::write(dir.join(format!("{}.bin", v.name)), &v.bytes).unwrap();
        }
        std::fs::write(dir.join("vectors.md"), &markdown).unwrap();
        return;
    }
    for v in &vectors {
        let path = dir.join(format!("{}.bin", v.name));
        let stored = std::fs::read(&path).unwrap_or_else(|e| {
            panic!("{}: {e} (run with BLUE_BLESS=1 to create)", path.display())
        });
        assert_eq!(hex(&stored), hex(&v.bytes), "vector {} differs", v.name);
    }
    let stored = std::fs::read_to_string(dir.join("vectors.md")).unwrap();
    assert_eq!(
        stored.replace("\r\n", "\n"),
        markdown,
        "vectors.md is stale"
    );
}
