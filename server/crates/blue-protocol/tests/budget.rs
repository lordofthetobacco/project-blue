//! The snapshot writer fills objects until the byte budget and never
//! produces more than 1200 bytes.

#![allow(clippy::unwrap_used)]

mod common;

use blue_protocol::packet::MAX_OBJECTS_PER_PACKET;
use blue_protocol::{
    MAX_PAYLOAD_BYTES, PacketHeader, PlayerSet, ReliableSection, ServerSnapshot, SnapshotInfo,
    SnapshotWriter,
};
use common::Rng;

fn fill(
    buf: &mut [u8],
    seed: u64,
    players: usize,
    messages: u16,
) -> (usize, Vec<blue_protocol::WorldObject>) {
    let mut rng = Rng::new(seed);
    let own = common::player_state(&mut rng);
    let mut reliable = ReliableSection::default();
    for id in 0..messages {
        reliable.push(id, common::game_message(&mut rng)).unwrap();
    }
    let mut current = PlayerSet::default();
    for id in 0..players as u8 {
        current
            .set(id, Some(common::remote_player(&mut rng).quantise()))
            .unwrap();
    }
    let info = SnapshotInfo {
        server_tick: 7,
        ..SnapshotInfo::default()
    };
    let mut w =
        SnapshotWriter::begin(buf, &PacketHeader::default(), &info, Some(&own), &reliable).unwrap();
    let mut record = PlayerSet::default();
    let ids: Vec<u8> = (0..64).collect();
    w.write_players(&current, None, &ids, 0, &mut record)
        .unwrap();
    let mut sent = Vec::new();
    loop {
        let object = common::world_object(&mut rng);
        let before = w.bytes_left();
        if !w.try_add_object(&object).unwrap() {
            // A refused object leaves the writer unchanged.
            assert_eq!(w.bytes_left(), before);
            break;
        }
        sent.push(object.quantised());
    }
    // Smaller objects may still fit after a refusal; the writer stays valid.
    let at_rest = blue_protocol::WorldObject {
        at_rest: true,
        ..blue_protocol::WorldObject::default()
    };
    while sent.len() < MAX_OBJECTS_PER_PACKET && w.try_add_object(&at_rest).unwrap() {
        sent.push(at_rest.quantised());
    }
    assert!(w.bytes_left() < 16, "{} bytes left unused", w.bytes_left());
    let len = w.finish().unwrap();
    (len, sent)
}

#[test]
fn fills_to_the_limit_and_decodes() {
    for (seed, players, messages) in [(1, 0, 0), (2, 16, 3), (3, 5, 10), (4, 30, 0)] {
        let mut buf = vec![0u8; 4096];
        let (len, sent) = fill(&mut buf, seed, players, messages);
        assert!(len <= MAX_PAYLOAD_BYTES);
        assert!(len > MAX_PAYLOAD_BYTES - 16 || sent.len() == MAX_OBJECTS_PER_PACKET);
        let mut snapshot = ServerSnapshot::default();
        ServerSnapshot::decode(&buf[..len], &mut snapshot).unwrap();
        assert_eq!(snapshot.objects.as_slice(), sent.as_slice());
    }
}

#[test]
fn smaller_buffer_is_respected() {
    let mut buf = vec![0u8; 300];
    let (len, sent) = fill(&mut buf, 5, 2, 1);
    assert!(len <= 300);
    let mut snapshot = ServerSnapshot::default();
    ServerSnapshot::decode(&buf[..len], &mut snapshot).unwrap();
    assert_eq!(snapshot.objects.as_slice(), sent.as_slice());
}

#[test]
fn smallest_objects_fill_exactly() {
    // Prelude: kind 4 + header 64 + tick 32 + last command 1 + depth 6 +
    // baseline 1 + own 1 + reliable count 6 + players end 1 = 116 bits.
    // An at-rest object with id 0: more 1 + kind 1 + id 7 + rest 1 +
    // position 72 + rotation 32 = 114 bits. Objects end: 1 bit.
    const EXPECTED: usize = (MAX_PAYLOAD_BYTES * 8 - 116 - 1) / 114;
    let mut buf = vec![0u8; MAX_PAYLOAD_BYTES];
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
    let object = blue_protocol::WorldObject {
        at_rest: true,
        ..blue_protocol::WorldObject::default()
    };
    while w.try_add_object(&object).unwrap() {}
    let count = w.object_count();
    assert_eq!(count, EXPECTED.min(MAX_OBJECTS_PER_PACKET));
    let len = w.finish().unwrap();
    assert!(len <= MAX_PAYLOAD_BYTES);
    let mut snapshot = ServerSnapshot::default();
    ServerSnapshot::decode(&buf[..len], &mut snapshot).unwrap();
    assert_eq!(snapshot.objects.as_slice().len(), count);
}
