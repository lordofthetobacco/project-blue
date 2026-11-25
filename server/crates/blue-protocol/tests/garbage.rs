//! Decoding random, truncated and bit-flipped input returns `Err` (or a
//! valid value) and never panics.

#![allow(clippy::unwrap_used)]

mod common;

use blue_protocol::message::Message;
use blue_protocol::packet::{decode_control, encode_control, peek_kind};
use blue_protocol::reliable::ReliableSection;
use blue_protocol::{
    ClientInput, CommandList, MAX_PAYLOAD_BYTES, PacketHeader, PlayerSet, ServerSnapshot,
    SnapshotInfo, SnapshotWriter,
};
use common::Rng;

fn decode_all(bytes: &[u8]) -> [bool; 4] {
    let mut input = ClientInput::default();
    let mut snapshot = ServerSnapshot::default();
    let a = ClientInput::decode(bytes, &mut input).is_ok();
    let b = ServerSnapshot::decode(bytes, &mut snapshot).is_ok();
    if b {
        // Applying must not panic either, with or without a baseline.
        let mut set = PlayerSet::default();
        let _ = snapshot.players.apply(None, &mut set);
        let base = PlayerSet::default();
        let _ = snapshot.players.apply(Some(&base), &mut set);
    }
    let c = decode_control(bytes).is_ok();
    let d = Message::decode_standalone(bytes).is_ok();
    let _ = peek_kind(bytes);
    [a, b, c, d]
}

#[test]
fn random_bytes_never_panic() {
    let mut rng = Rng::new(100);
    let mut buf = vec![0u8; MAX_PAYLOAD_BYTES + 16];
    for _ in 0..50_000 {
        let len = rng.below(buf.len() as u64 + 1) as usize;
        rng.bytes(&mut buf[..len]);
        decode_all(&buf[..len]);
    }
}

#[test]
fn random_bytes_with_valid_kinds_never_panic() {
    // Force the kind nibble so decoding goes deep into each packet type.
    let mut rng = Rng::new(101);
    let mut buf = [0u8; 256];
    for i in 0..60_000u32 {
        let len = 1 + rng.below(buf.len() as u64) as usize;
        rng.bytes(&mut buf[..len]);
        buf[0] = (buf[0] & 0xF0) | (i % 3) as u8;
        decode_all(&buf[..len]);
    }
}

fn valid_packets(rng: &mut Rng) -> Vec<Vec<u8>> {
    let mut out = Vec::new();
    for i in 0..3 {
        let mut buf = [0u8; MAX_PAYLOAD_BYTES];
        let len = encode_control(&common::message_of(rng, i), &mut buf).unwrap();
        out.push(buf[..len].to_vec());
    }
    for _ in 0..20 {
        let mut commands = CommandList::default();
        let tick = rng.u32();
        let mut c = common::command(rng, tick);
        commands.push(c).unwrap();
        for _ in 0..rng.below(8) {
            c = common::older_command(rng, &c);
            commands.push(c).unwrap();
        }
        let mut reliable = ReliableSection::default();
        for id in 0..rng.below(4) as u16 {
            reliable.push(id, common::game_message(rng)).unwrap();
        }
        let input = ClientInput {
            header: PacketHeader {
                sequence: 1,
                ack: 2,
                ack_bits: 3,
            },
            snapshot_ack: Some(5),
            view_tick: 4,
            view_fraction: 128,
            commands,
            reliable,
        };
        let mut buf = [0u8; MAX_PAYLOAD_BYTES];
        let len = input.encode(&mut buf).unwrap();
        out.push(buf[..len].to_vec());

        let own = common::player_state(rng);
        let mut current = PlayerSet::default();
        let mut baseline = PlayerSet::default();
        for id in 0..10u8 {
            current
                .set(id, Some(common::remote_player(rng).quantise()))
                .unwrap();
            if rng.chance(0.5) {
                baseline
                    .set(id, Some(common::remote_player(rng).quantise()))
                    .unwrap();
            }
        }
        baseline
            .set(12, Some(common::remote_player(rng).quantise()))
            .unwrap();
        let info = SnapshotInfo {
            server_tick: 50,
            baseline_tick: Some(45),
            ..SnapshotInfo::default()
        };
        let mut buf = [0u8; MAX_PAYLOAD_BYTES];
        let mut w =
            SnapshotWriter::begin(&mut buf, &input.header, &info, Some(&own), &reliable).unwrap();
        let mut record = PlayerSet::default();
        let ids: Vec<u8> = (0..64).collect();
        w.write_players(&current, Some(&baseline), &ids, 0, &mut record)
            .unwrap();
        for _ in 0..rng.below(10) {
            w.try_add_object(&common::world_object(rng)).unwrap();
        }
        let len = w.finish().unwrap();
        out.push(buf[..len].to_vec());
    }
    out
}

#[test]
fn every_truncation_is_rejected() {
    let mut rng = Rng::new(102);
    for packet in valid_packets(&mut rng) {
        assert!(
            decode_all(&packet).iter().any(|&ok| ok),
            "the full packet decodes"
        );
        for len in 0..packet.len() {
            let ok = decode_all(&packet[..len]);
            assert_eq!(ok[..3], [false; 3], "prefix {len}/{} decoded", packet.len());
        }
        // Extra bytes are rejected too.
        let mut longer = packet.clone();
        longer.push(0);
        assert_eq!(decode_all(&longer)[..3], [false; 3]);
    }
}

#[test]
fn bit_flips_never_panic() {
    let mut rng = Rng::new(103);
    for packet in valid_packets(&mut rng) {
        for _ in 0..500 {
            let mut mutated = packet.clone();
            for _ in 0..1 + rng.below(4) {
                let bit = rng.below(mutated.len() as u64 * 8) as usize;
                mutated[bit / 8] ^= 1 << (bit % 8);
            }
            decode_all(&mutated);
        }
    }
}
