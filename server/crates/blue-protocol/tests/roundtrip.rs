//! Round trips of every type, boundary values and maximum-size packets.

#![allow(clippy::unwrap_used, clippy::panic)]

mod common;

use blue_protocol::bits::{BitReader, BitWriter, Stream};
use blue_protocol::command::MAX_COMMANDS_PER_PACKET;
use blue_protocol::message::{MAX_PLAYERS, Message};
use blue_protocol::packet::{
    INPUT_DEPTH_MAX, INPUT_DEPTH_MIN, MAX_BASELINE_AGE, decode_control, encode_control,
};
use blue_protocol::player::WeaponSlotState;
use blue_protocol::quant::{POSITION_MAX, POSITION_MIN, VELOCITY_MAX, VELOCITY_MIN};
use blue_protocol::reliable::MAX_MESSAGES_PER_PACKET;
use blue_protocol::{
    ClientInput, CommandList, Error, MAX_PAYLOAD_BYTES, PacketHeader, PlayerSet, PlayerState,
    ReliableSection, RemotePlayerQ, ServerSnapshot, SnapshotInfo, SnapshotWriter, UserCommand,
};
use common::Rng;

const ITERATIONS: usize = 2000;

fn round_trip_message(message: &Message) {
    let mut buf = [0u8; 256];
    let len = message.encode_standalone(&mut buf).unwrap();
    assert_eq!(len, message.bit_len().unwrap().div_ceil(8));
    assert_eq!(Message::decode_standalone(&buf[..len]).unwrap(), *message);
}

#[test]
fn messages_round_trip() {
    let mut rng = Rng::new(1);
    for i in 0..ITERATIONS as u64 {
        round_trip_message(&common::message_of(&mut rng, i));
    }
}

#[test]
fn control_round_trip_and_rejects_game_messages() {
    let mut rng = Rng::new(2);
    let mut buf = [0u8; MAX_PAYLOAD_BYTES];
    for i in 0..3 {
        let m = common::message_of(&mut rng, i);
        let len = encode_control(&m, &mut buf).unwrap();
        assert_eq!(decode_control(&buf[..len]).unwrap(), m);
    }
    let m = Message::PlayerLeft { player_id: 3 };
    assert_eq!(encode_control(&m, &mut buf), Err(Error::UnexpectedMessage));
}

#[test]
fn message_field_ranges_are_enforced() {
    let mut buf = [0u8; 64];
    let bad = Message::PlayerLeft { player_id: 64 };
    assert_eq!(bad.encode_standalone(&mut buf), Err(Error::OutOfRange));
    let bad = Message::ServerWelcome {
        player_id: 0,
        tick_rate: 0,
        server_tick: 0,
        max_players: 1,
    };
    assert_eq!(bad.encode_standalone(&mut buf), Err(Error::OutOfRange));
    let bad = Message::Respawn {
        player_id: 0,
        position: [f32::NAN, 0.0, 0.0],
        yaw: 0.0,
    };
    assert_eq!(bad.encode_standalone(&mut buf), Err(Error::OutOfRange));
}

fn command_list(rng: &mut Rng, count: usize) -> CommandList {
    let mut list = CommandList::default();
    let tick = rng.u32();
    let mut c = common::command(rng, tick);
    list.push(c).unwrap();
    for _ in 1..count {
        c = common::older_command(rng, &c);
        list.push(c).unwrap();
    }
    list
}

#[test]
fn commands_round_trip() {
    let mut rng = Rng::new(3);
    for _ in 0..ITERATIONS {
        let count = 1 + rng.below(MAX_COMMANDS_PER_PACKET as u64) as usize;
        let mut list = command_list(&mut rng, count);
        let expected = list.canonical();
        let mut buf = [0u8; 1024];
        let mut w = BitWriter::new(&mut buf);
        list.serialize(&mut w).unwrap();
        let len = w.finish().unwrap();
        let mut decoded = CommandList::default();
        let mut r = BitReader::new(&buf[..len]);
        decoded.serialize(&mut r).unwrap();
        r.finish().unwrap();
        assert_eq!(decoded, expected);
        // Canonical values are a fixed point.
        assert_eq!(expected.canonical(), expected);
    }
}

#[test]
fn command_boundaries() {
    let extremes = [
        UserCommand {
            tick: u32::MAX,
            held: 0xFF,
            pressed: 0xFF,
            yaw: -0.0,
            pitch: core::f32::consts::FRAC_PI_2,
            select_slot: 7,
            interact_kind: 15,
            interact_id: u32::MAX,
        },
        UserCommand {
            tick: 0,
            held: 0,
            pressed: 0,
            yaw: 1e30,
            pitch: -1e30,
            select_slot: -1,
            interact_kind: 0,
            interact_id: 0,
        },
    ];
    // Tick wraps from 0 to u32::MAX inside one list.
    let mut list = CommandList::default();
    list.push(UserCommand {
        tick: 0,
        ..extremes[1]
    })
    .unwrap();
    list.push(extremes[0]).unwrap();
    let mut buf = [0u8; 128];
    let mut w = BitWriter::new(&mut buf);
    let expected = list.canonical();
    list.serialize(&mut w).unwrap();
    let len = w.finish().unwrap();
    let mut decoded = CommandList::default();
    decoded.serialize(&mut BitReader::new(&buf[..len])).unwrap();
    assert_eq!(decoded, expected);

    // Out-of-range fields are refused.
    for bad in [
        UserCommand {
            held: 0x100,
            ..UserCommand::default()
        },
        UserCommand {
            pressed: 0x100,
            ..UserCommand::default()
        },
        UserCommand {
            select_slot: 8,
            ..UserCommand::default()
        },
        UserCommand {
            select_slot: -2,
            ..UserCommand::default()
        },
        UserCommand {
            interact_kind: 16,
            ..UserCommand::default()
        },
    ] {
        let mut list = CommandList::from_slice(&[bad]).unwrap();
        let mut w = BitWriter::new(&mut buf);
        assert_eq!(list.serialize(&mut w), Err(Error::OutOfRange));
    }
    // Non-consecutive ticks are refused by the list.
    let mut list = CommandList::default();
    list.push(UserCommand {
        tick: 10,
        ..UserCommand::default()
    })
    .unwrap();
    assert_eq!(
        list.push(UserCommand {
            tick: 8,
            ..UserCommand::default()
        }),
        Err(Error::Usage)
    );
}

#[test]
fn player_state_round_trip() {
    let mut rng = Rng::new(4);
    for _ in 0..ITERATIONS {
        let state = common::player_state(&mut rng);
        let mut buf = [0u8; 256];
        let mut w = BitWriter::new(&mut buf);
        let mut copy = state;
        copy.serialize(&mut w).unwrap();
        let len = w.finish().unwrap();
        let mut decoded = PlayerState::default();
        let mut r = BitReader::new(&buf[..len]);
        decoded.serialize(&mut r).unwrap();
        r.finish().unwrap();
        assert_eq!(decoded, state);
    }
}

#[test]
fn player_state_rejects_non_canonical_slots() {
    let mut buf = [0u8; 256];
    let mut state = PlayerState::default();
    state.slots[0] = WeaponSlotState {
        present: false,
        weapon_key: 1,
        ..WeaponSlotState::default()
    };
    assert_eq!(
        state.serialize(&mut BitWriter::new(&mut buf)),
        Err(Error::NonCanonical)
    );
    let mut state = PlayerState::default();
    state.slots[1].present = true;
    state.slots[1].spare_count = 1;
    state.slots[1].spare_magazines[3] = 9;
    assert_eq!(
        state.serialize(&mut BitWriter::new(&mut buf)),
        Err(Error::NonCanonical)
    );
}

#[test]
fn player_state_incoming_magazine_range() {
    let mut buf = [0u8; 256];
    for (value, ok) in [(-2i16, false), (-1, true), (255, true), (256, false)] {
        let mut state = PlayerState::default();
        state.slots[0].present = true;
        state.slots[0].incoming_magazine = value;
        let result = state.serialize(&mut BitWriter::new(&mut buf));
        assert_eq!(result.is_ok(), ok, "{value}");
        if !ok {
            assert_eq!(result, Err(Error::OutOfRange));
        }
    }
}

fn client_input(rng: &mut Rng, commands: usize, messages: usize) -> ClientInput {
    let mut reliable = ReliableSection::default();
    let mut id = rng.u32() as u16;
    for _ in 0..messages {
        reliable.push(id, common::game_message(rng)).unwrap();
        id = id.wrapping_add(1 + rng.below(3) as u16);
    }
    ClientInput {
        header: PacketHeader {
            sequence: rng.u32() as u16,
            ack: rng.u32() as u16,
            ack_bits: rng.u32(),
        },
        snapshot_ack: if rng.chance(0.5) {
            Some(rng.u32())
        } else {
            None
        },
        view_tick: rng.u32(),
        view_fraction: rng.u32() as u8,
        commands: command_list(rng, commands),
        reliable,
    }
}

#[test]
fn client_input_round_trip() {
    let mut rng = Rng::new(5);
    for _ in 0..ITERATIONS {
        let commands = 1 + rng.below(MAX_COMMANDS_PER_PACKET as u64) as usize;
        let messages = rng.below(4) as usize;
        let input = client_input(&mut rng, commands, messages);
        let mut buf = [0u8; MAX_PAYLOAD_BYTES];
        let len = input.encode(&mut buf).unwrap();
        let mut decoded = ClientInput::default();
        ClientInput::decode(&buf[..len], &mut decoded).unwrap();
        let mut expected = input;
        expected.commands = input.commands.canonical();
        assert_eq!(decoded, expected);
    }
}

#[test]
fn oversized_client_input_fails_cleanly() {
    // 16 commands plus 32 large messages cannot fit 1200 bytes.
    let mut rng = Rng::new(6);
    let mut input = client_input(&mut rng, MAX_COMMANDS_PER_PACKET, 0);
    for i in 0..MAX_MESSAGES_PER_PACKET as u16 {
        let message = common::message_of(&mut rng, 13);
        input.reliable.push(i, message).unwrap();
    }
    let mut buf = [0u8; 4096];
    match input.encode(&mut buf) {
        Err(Error::Overflow) => {}
        Ok(len) => assert!(len <= MAX_PAYLOAD_BYTES),
        Err(e) => panic!("unexpected {e:?}"),
    }
}

fn random_set(rng: &mut Rng, fill: f64) -> PlayerSet {
    let mut set = PlayerSet::default();
    for id in 0..MAX_PLAYERS as u8 {
        if rng.chance(fill) {
            set.set(id, Some(common::remote_player(rng).quantise()))
                .unwrap();
        }
    }
    set
}

/// `baseline` with some players moved, changed, removed and added.
fn evolve(rng: &mut Rng, baseline: &PlayerSet) -> PlayerSet {
    let mut set = *baseline;
    for id in 0..MAX_PLAYERS as u8 {
        match (baseline.get(id), rng.below(10)) {
            (Some(_), 0) => set.set(id, None).unwrap(),
            (Some(p), 1..=6) => {
                let mut p = *p;
                if rng.chance(0.8) {
                    for c in &mut p.position {
                        *c = (*c + rng.below(2001) as i32 - 1000).clamp(POSITION_MIN, POSITION_MAX);
                    }
                }
                if rng.chance(0.5) {
                    p.velocity[0] = rng.below(16384) as i32 + VELOCITY_MIN;
                }
                if rng.chance(0.3) {
                    p.yaw = rng.u32() as u16;
                }
                if rng.chance(0.2) {
                    p.health = rng.u32() as i16;
                }
                if rng.chance(0.1) {
                    p.weapon_key = rng.u32();
                }
                set.set(id, Some(p)).unwrap();
            }
            (None, 0) => set
                .set(id, Some(common::remote_player(rng).quantise()))
                .unwrap(),
            _ => {}
        }
    }
    set
}

fn all_ids() -> Vec<u8> {
    (0..MAX_PLAYERS as u8).collect()
}

#[test]
fn snapshot_delta_reconstructs_current() {
    let mut rng = Rng::new(7);
    let priority = all_ids();
    for i in 0..500 {
        let baseline = random_set(&mut rng, if i % 2 == 0 { 0.25 } else { 0.05 });
        let current = evolve(&mut rng, &baseline);
        let info = SnapshotInfo {
            server_tick: rng.u32(),
            last_command_tick: Some(rng.u32()),
            input_buffer_depth: 2,
            baseline_tick: None,
        };
        let info = SnapshotInfo {
            baseline_tick: Some(info.server_tick.wrapping_sub(1 + rng.below(255) as u32)),
            ..info
        };
        let mut buf = [0u8; MAX_PAYLOAD_BYTES];
        let mut w = SnapshotWriter::begin(
            &mut buf,
            &PacketHeader::default(),
            &info,
            None,
            &ReliableSection::default(),
        )
        .unwrap();
        let mut record = PlayerSet::default();
        w.write_players(&current, Some(&baseline), &priority, 0, &mut record)
            .unwrap();
        let len = w.finish().unwrap();
        assert!(len <= MAX_PAYLOAD_BYTES);

        let mut snapshot = ServerSnapshot::default();
        ServerSnapshot::decode(&buf[..len], &mut snapshot).unwrap();
        assert_eq!(snapshot.info, info);
        let mut decoded = PlayerSet::default();
        snapshot
            .players
            .apply(Some(&baseline), &mut decoded)
            .unwrap();
        assert_eq!(decoded, record);
        if record != current {
            // Only a packet too small for every entry may leave players stale.
            assert!(
                len > MAX_PAYLOAD_BYTES - 64,
                "record differs although {len} bytes were free"
            );
        }
        // Missing baseline is reported, not mis-decoded.
        assert_eq!(
            snapshot.players.apply(None, &mut decoded),
            Err(Error::MissingBaseline)
        );
    }
}

#[test]
fn snapshot_delta_of_quantised_floats() {
    // decode(encode(state, baseline), baseline) == quantise(state).
    let mut rng = Rng::new(8);
    for _ in 0..200 {
        let mut baseline = PlayerSet::default();
        let mut current = PlayerSet::default();
        for id in 0..16u8 {
            let b = common::remote_player(&mut rng);
            let mut c = b;
            c.position[0] += rng.f32_in(-2.0, 2.0);
            c.velocity[2] = rng.f32_in(-10.0, 10.0);
            c.pitch = rng.f32_in(-1.5, 1.5);
            baseline.set(id, Some(b.quantise())).unwrap();
            current.set(id, Some(c.quantise())).unwrap();
        }
        let info = SnapshotInfo {
            server_tick: 100,
            baseline_tick: Some(99),
            ..SnapshotInfo::default()
        };
        let mut buf = [0u8; MAX_PAYLOAD_BYTES];
        let mut w = SnapshotWriter::begin(
            &mut buf,
            &PacketHeader::default(),
            &info,
            None,
            &ReliableSection::default(),
        )
        .unwrap();
        let mut record = PlayerSet::default();
        w.write_players(&current, Some(&baseline), &all_ids(), 0, &mut record)
            .unwrap();
        let len = w.finish().unwrap();
        let mut snapshot = ServerSnapshot::default();
        ServerSnapshot::decode(&buf[..len], &mut snapshot).unwrap();
        let mut decoded = PlayerSet::default();
        snapshot
            .players
            .apply(Some(&baseline), &mut decoded)
            .unwrap();
        assert_eq!(decoded, current);
        assert_eq!(record, current);
        // Dequantise / quantise is a fixed point.
        for (_, p) in decoded.iter() {
            assert_eq!(p.dequantise().quantise(), *p);
        }
    }
}

#[test]
fn full_snapshot_with_64_players_is_split_by_budget() {
    let mut rng = Rng::new(9);
    let current = random_set(&mut rng, 1.0);
    assert_eq!(current.len(), MAX_PLAYERS);
    let mut known = PlayerSet::default();
    let mut baseline: Option<PlayerSet> = None;
    // Without a baseline not all 64 fit; successive deltas fill the rest.
    for tick in 1..=4u32 {
        let info = SnapshotInfo {
            server_tick: tick,
            baseline_tick: baseline.map(|_| tick - 1),
            ..SnapshotInfo::default()
        };
        let mut buf = [0u8; MAX_PAYLOAD_BYTES];
        let own = PlayerState::default();
        let mut w = SnapshotWriter::begin(
            &mut buf,
            &PacketHeader::default(),
            &info,
            Some(&own),
            &ReliableSection::default(),
        )
        .unwrap();
        let mut record = PlayerSet::default();
        w.write_players(&current, baseline.as_ref(), &all_ids(), 100, &mut record)
            .unwrap();
        assert!(w.bytes_left() >= 99);
        let len = w.finish().unwrap();
        let mut snapshot = ServerSnapshot::default();
        ServerSnapshot::decode(&buf[..len], &mut snapshot).unwrap();
        snapshot
            .players
            .apply(baseline.as_ref(), &mut known)
            .unwrap();
        assert_eq!(known, record);
        baseline = Some(record);
    }
    assert_eq!(known, current);
}

#[test]
fn snapshot_info_boundaries() {
    for info in [
        SnapshotInfo {
            server_tick: 5,
            last_command_tick: Some(u32::MAX),
            input_buffer_depth: INPUT_DEPTH_MIN,
            baseline_tick: Some(5u32.wrapping_sub(MAX_BASELINE_AGE)),
        },
        SnapshotInfo {
            server_tick: u32::MAX,
            last_command_tick: None,
            input_buffer_depth: INPUT_DEPTH_MAX,
            baseline_tick: None,
        },
    ] {
        let mut buf = [0u8; MAX_PAYLOAD_BYTES];
        let mut w = SnapshotWriter::begin(
            &mut buf,
            &PacketHeader {
                sequence: 65535,
                ack: 0,
                ack_bits: u32::MAX,
            },
            &info,
            None,
            &ReliableSection::default(),
        )
        .unwrap();
        let mut record = PlayerSet::default();
        let baseline = info.baseline_tick.map(|_| PlayerSet::default());
        w.write_players(
            &PlayerSet::default(),
            baseline.as_ref(),
            &[],
            0,
            &mut record,
        )
        .unwrap();
        let len = w.finish().unwrap();
        let mut snapshot = ServerSnapshot::default();
        ServerSnapshot::decode(&buf[..len], &mut snapshot).unwrap();
        assert_eq!(snapshot.info, info);
    }
    // Baseline older than 255 ticks or equal to the tick is refused.
    for age in [0u32, 256] {
        let info = SnapshotInfo {
            server_tick: 1000,
            baseline_tick: Some(1000 - age),
            ..SnapshotInfo::default()
        };
        let mut buf = [0u8; 64];
        let result = SnapshotWriter::begin(
            &mut buf,
            &PacketHeader::default(),
            &info,
            None,
            &ReliableSection::default(),
        );
        assert!(matches!(result, Err(Error::OutOfRange)));
    }
}

#[test]
fn remote_player_extremes_round_trip() {
    let extreme = RemotePlayerQ {
        position: [POSITION_MIN, POSITION_MAX, 0],
        velocity: [VELOCITY_MIN, VELOCITY_MAX, -1],
        yaw: u16::MAX,
        pitch: -16384,
        flags: 0x3F,
        health: i16::MIN,
        weapon_key: u32::MAX,
    };
    let mut baseline = PlayerSet::default();
    baseline
        .set(
            0,
            Some(RemotePlayerQ {
                position: [POSITION_MAX, POSITION_MIN, 0],
                ..extreme
            }),
        )
        .unwrap();
    let mut current = PlayerSet::default();
    current.set(0, Some(extreme)).unwrap();
    current.set(63, Some(extreme)).unwrap();
    let info = SnapshotInfo {
        server_tick: 9,
        baseline_tick: Some(8),
        ..SnapshotInfo::default()
    };
    let mut buf = [0u8; MAX_PAYLOAD_BYTES];
    let mut w = SnapshotWriter::begin(
        &mut buf,
        &PacketHeader::default(),
        &info,
        None,
        &ReliableSection::default(),
    )
    .unwrap();
    let mut record = PlayerSet::default();
    w.write_players(&current, Some(&baseline), &all_ids(), 0, &mut record)
        .unwrap();
    let len = w.finish().unwrap();
    let mut snapshot = ServerSnapshot::default();
    ServerSnapshot::decode(&buf[..len], &mut snapshot).unwrap();
    let mut decoded = PlayerSet::default();
    snapshot
        .players
        .apply(Some(&baseline), &mut decoded)
        .unwrap();
    assert_eq!(decoded, current);
}

#[test]
fn objects_round_trip_quantised() {
    let mut rng = Rng::new(10);
    for _ in 0..200 {
        let mut buf = [0u8; MAX_PAYLOAD_BYTES];
        let mut w = SnapshotWriter::begin(
            &mut buf,
            &PacketHeader::default(),
            &SnapshotInfo::default(),
            None,
            &ReliableSection::default(),
        )
        .unwrap();
        let mut record = PlayerSet::default();
        w.write_players(&PlayerSet::default(), None, &[], 0, &mut record)
            .unwrap();
        let mut sent = Vec::new();
        for _ in 0..rng.below(20) {
            let object = common::world_object(&mut rng);
            assert!(w.try_add_object(&object).unwrap());
            sent.push(object.quantised());
        }
        let len = w.finish().unwrap();
        let mut snapshot = ServerSnapshot::default();
        ServerSnapshot::decode(&buf[..len], &mut snapshot).unwrap();
        assert_eq!(snapshot.objects.as_slice(), sent.as_slice());
    }
}

#[test]
fn full_snapshot_round_trip() {
    let mut rng = Rng::new(11);
    for _ in 0..300 {
        let own = common::player_state(&mut rng);
        let mut reliable = ReliableSection::default();
        for i in 0..rng.below(5) as u16 {
            reliable.push(i, common::game_message(&mut rng)).unwrap();
        }
        let current = random_set(&mut rng, 0.2);
        let header = PacketHeader {
            sequence: rng.u32() as u16,
            ack: rng.u32() as u16,
            ack_bits: rng.u32(),
        };
        let info = SnapshotInfo {
            server_tick: rng.u32(),
            last_command_tick: None,
            input_buffer_depth: -3,
            baseline_tick: None,
        };
        let mut buf = [0u8; MAX_PAYLOAD_BYTES];
        let mut w = SnapshotWriter::begin(&mut buf, &header, &info, Some(&own), &reliable).unwrap();
        let mut record = PlayerSet::default();
        w.write_players(&current, None, &all_ids(), 0, &mut record)
            .unwrap();
        let len = w.finish().unwrap();
        let mut snapshot = ServerSnapshot::default();
        ServerSnapshot::decode(&buf[..len], &mut snapshot).unwrap();
        assert_eq!(snapshot.header, header);
        assert_eq!(snapshot.own, Some(own));
        assert_eq!(snapshot.reliable, reliable);
        let mut decoded = PlayerSet::default();
        snapshot.players.apply(None, &mut decoded).unwrap();
        assert_eq!(decoded, record);
    }
}

#[test]
fn writer_usage_errors() {
    let mut buf = [0u8; MAX_PAYLOAD_BYTES];
    let info = SnapshotInfo::default();
    let mut w = SnapshotWriter::begin(
        &mut buf,
        &PacketHeader::default(),
        &info,
        None,
        &ReliableSection::default(),
    )
    .unwrap();
    let object = blue_protocol::WorldObject::default();
    assert_eq!(w.try_add_object(&object), Err(Error::Usage));
    let mut record = PlayerSet::default();
    let base = PlayerSet::default();
    assert_eq!(
        w.write_players(&PlayerSet::default(), Some(&base), &[], 0, &mut record),
        Err(Error::Usage)
    );
    w.write_players(&PlayerSet::default(), None, &[], 0, &mut record)
        .unwrap();
    assert_eq!(
        w.write_players(&PlayerSet::default(), None, &[], 0, &mut record),
        Err(Error::Usage)
    );
}

#[test]
fn bounded_primitives() {
    let mut buf = [0u8; 16];
    let mut w = BitWriter::new(&mut buf);
    let mut v = -5;
    w.bounded_i32(&mut v, -5, 5).unwrap();
    let mut v = 5;
    w.bounded_i32(&mut v, -5, 5).unwrap();
    let mut v = 6;
    assert_eq!(w.bounded_i32(&mut v, -5, 5), Err(Error::OutOfRange));
    let mut v = i32::MIN;
    w.bounded_i32(&mut v, i32::MIN, i32::MAX).unwrap();
    assert_eq!(w.pos(), 4 + 4 + 32);
    let len = w.finish().unwrap();
    let mut r = BitReader::new(&buf[..len]);
    let mut out = 0;
    r.bounded_i32(&mut out, -5, 5).unwrap();
    assert_eq!(out, -5);
    r.bounded_i32(&mut out, -5, 5).unwrap();
    assert_eq!(out, 5);
    r.bounded_i32(&mut out, i32::MIN, i32::MAX).unwrap();
    assert_eq!(out, i32::MIN);

    // A raw value above the range is rejected on read.
    let mut buf = [0u8; 1];
    let mut w = BitWriter::new(&mut buf);
    w.write_bits(15, 4).unwrap();
    w.finish().unwrap();
    let mut r = BitReader::new(&buf);
    assert_eq!(r.bounded_i32(&mut out, 0, 10), Err(Error::OutOfRange));
}
