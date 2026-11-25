//! Packet acks and the reliable-ordered channel under loss, duplication and
//! reordering (deterministic RNG, fixed seeds).

#![allow(clippy::unwrap_used, clippy::panic)]

mod common;

use blue_protocol::bits::{BitReader, BitWriter};
use blue_protocol::message::Message;
use blue_protocol::reliable::WINDOW;
use blue_protocol::seq::AckedList;
use blue_protocol::{
    AckTracker, Error, PacketHeader, Receipt, ReliableConfig, ReliableEndpoint, ReliableSection,
};
use common::Rng;

const DT: f64 = 1.0 / 60.0;

struct Peer {
    acks: AckTracker,
    channel: ReliableEndpoint,
    next_to_send: u32,
    received: Vec<u32>,
}

impl Peer {
    fn new(seq: u16, send_id: u16, recv_id: u16) -> Self {
        Self {
            acks: AckTracker::with_initial_sequence(seq),
            channel: ReliableEndpoint::with_initial_ids(
                ReliableConfig::default(),
                send_id,
                recv_id,
            ),
            next_to_send: 0,
            received: Vec::new(),
        }
    }
}

/// A message carrying `n`, of varying size.
fn numbered(rng: &mut Rng, n: u32) -> Message {
    match rng.below(3) {
        0 => Message::Death {
            victim: 1,
            killer: None,
            weapon_key: n,
        },
        1 => Message::Shot {
            shooter: 2,
            origin: [1.0, 2.0, 3.0],
            direction: [0.0, 0.0, 1.0],
            weapon_key: 7,
            shot_number: n,
        },
        _ => {
            let mut instance_ids = [0; 16];
            instance_ids[0] = n;
            Message::WorldSyncPickupsTaken {
                count: 16,
                instance_ids,
            }
        }
    }
}

fn number_of(message: &Message) -> u32 {
    match message {
        Message::Death { weapon_key, .. } => *weapon_key,
        Message::Shot { shot_number, .. } => *shot_number,
        Message::WorldSyncPickupsTaken { instance_ids, .. } => instance_ids[0],
        other => panic!("unexpected {other:?}"),
    }
}

fn encode(header: &PacketHeader, section: &ReliableSection) -> Vec<u8> {
    let mut buf = [0u8; 1200];
    let mut w = BitWriter::new(&mut buf);
    let mut header = *header;
    header.serialize(&mut w).unwrap();
    let mut section = *section;
    section.serialize(&mut w).unwrap();
    let len = w.finish().unwrap();
    buf[..len].to_vec()
}

fn decode(bytes: &[u8]) -> (PacketHeader, ReliableSection) {
    let mut r = BitReader::new(bytes);
    let mut header = PacketHeader::default();
    header.serialize(&mut r).unwrap();
    let mut section = ReliableSection::default();
    section.serialize(&mut r).unwrap();
    r.finish().unwrap();
    (header, section)
}

struct Network {
    loss: f64,
    duplicate: f64,
    max_delay: u64,
    in_flight: Vec<(u64, usize, Vec<u8>)>,
}

impl Network {
    fn send(&mut self, rng: &mut Rng, now: u64, to: usize, bytes: &[u8]) {
        let copies = if rng.chance(self.duplicate) { 2 } else { 1 };
        for _ in 0..copies {
            if rng.chance(self.loss) {
                continue;
            }
            let at = now + 1 + rng.below(self.max_delay + 1);
            self.in_flight.push((at, to, bytes.to_vec()));
        }
    }

    fn due(&mut self, now: u64) -> Vec<(usize, Vec<u8>)> {
        let mut out = Vec::new();
        self.in_flight.retain(|(at, to, bytes)| {
            if *at <= now {
                out.push((*to, bytes.clone()));
                false
            } else {
                true
            }
        });
        out
    }
}

fn run(seed: u64, loss: f64, duplicate: f64, max_delay: u64, start: (u16, u16), total: u32) {
    let mut rng = Rng::new(seed);
    let mut net = Network {
        loss,
        duplicate,
        max_delay,
        in_flight: Vec::new(),
    };
    let (seq, id) = start;
    let mut peers = [
        Peer::new(seq, id, id.wrapping_add(7)),
        Peer::new(seq ^ 0x5555, id.wrapping_add(7), id),
    ];
    let mut section = ReliableSection::default();
    let mut step: u64 = 0;
    while peers.iter().any(|p| p.received.len() < total as usize) {
        step += 1;
        assert!(
            step < 200_000,
            "channel stalled: {:?}",
            peers.iter().map(|p| p.received.len()).collect::<Vec<_>>()
        );
        let now = step as f64 * DT;
        for (index, peer) in peers.iter_mut().enumerate() {
            // Queue a burst of new messages now and then, as fast as the window allows.
            let burst = if rng.chance(0.2) { rng.below(12) } else { 0 };
            for _ in 0..burst {
                if peer.next_to_send >= total {
                    break;
                }
                match peer.channel.send(numbered(&mut rng, peer.next_to_send)) {
                    Ok(_) => peer.next_to_send += 1,
                    Err(Error::WindowFull) => break,
                    Err(e) => panic!("send failed: {e:?}"),
                }
            }
            let header = peer.acks.next_header();
            peer.channel
                .fill_section(header.sequence, now, &mut section);
            net.send(&mut rng, step, 1 - index, &encode(&header, &section));
        }
        for (to, bytes) in net.due(step) {
            let (header, section) = decode(&bytes);
            let peer = &mut peers[to];
            let mut acked = AckedList::default();
            let receipt = peer.acks.on_receive(&header, &mut acked);
            for &seq in acked.as_slice() {
                peer.channel.on_packet_acked(seq);
            }
            if receipt == Receipt::Fresh {
                peer.channel.receive_section(&section).unwrap();
                while let Some(message) = peer.channel.pop() {
                    peer.received.push(number_of(&message));
                }
            }
        }
    }
    for peer in &peers {
        let expected: Vec<u32> = (0..total).collect();
        assert_eq!(
            peer.received, expected,
            "every message exactly once, in order"
        );
    }
}

#[test]
fn perfect_network_delivers_in_order() {
    run(1, 0.0, 0.0, 0, (0, 0), 2000);
}

#[test]
fn lossy_duplicating_reordering_network() {
    run(2, 0.3, 0.1, 8, (0, 0), 3000);
}

#[test]
fn heavy_loss() {
    run(3, 0.6, 0.2, 3, (100, 1000), 1000);
}

#[test]
fn sequence_and_message_ids_wrap() {
    // Packet sequence starts 50 before the wrap, message ids 300 before.
    run(4, 0.25, 0.1, 6, (65486, 65236), 3000);
}

#[test]
fn window_full_is_reported() {
    let mut channel = ReliableEndpoint::new(ReliableConfig::default());
    for _ in 0..WINDOW {
        channel.send(Message::WorldSyncDone).unwrap();
    }
    assert_eq!(channel.send(Message::WorldSyncDone), Err(Error::WindowFull));
    assert_eq!(
        channel.send(Message::ServerReject {
            reason: blue_protocol::message::RejectReason::SERVER_FULL,
            server_protocol_version: 1
        }),
        Err(Error::UnexpectedMessage)
    );
}

#[test]
fn budget_limits_section() {
    let config = ReliableConfig {
        budget_bits: 400,
        ..ReliableConfig::default()
    };
    let mut channel = ReliableEndpoint::new(config);
    for _ in 0..20 {
        channel.send(Message::PlayerLeft { player_id: 1 }).unwrap();
    }
    let mut section = ReliableSection::default();
    channel.fill_section(0, 0.0, &mut section);
    let mut buf = [0u8; 1200];
    let mut w = BitWriter::new(&mut buf);
    section.serialize(&mut w).unwrap();
    assert!(w.pos() <= 400, "section took {} bits", w.pos());
    assert!(section.len() > 10);
    // Nothing is resent before the interval elapses.
    channel.fill_section(1, 0.05, &mut section);
    assert!(!section.is_empty(), "the messages that did not fit go now");
    let mut again = ReliableSection::default();
    channel.fill_section(2, 0.06, &mut again);
    assert!(again.is_empty());
    // A message that can never fit is refused up front.
    let tiny = ReliableConfig {
        budget_bits: 40,
        ..ReliableConfig::default()
    };
    let mut channel = ReliableEndpoint::new(tiny);
    assert_eq!(
        channel.send(Message::Impact {
            point: [0.0; 3],
            normal: [0.0; 3]
        }),
        Err(Error::MessageTooLarge)
    );
}

#[test]
fn ack_tracker_duplicates_and_stale() {
    let mut a = AckTracker::with_initial_sequence(65530);
    let mut b = AckTracker::new();
    let mut acked = AckedList::default();
    let mut headers = Vec::new();
    for _ in 0..40 {
        headers.push(a.next_header());
    }
    // Deliver every other packet, newest last.
    for h in headers.iter().step_by(2) {
        assert_eq!(b.on_receive(h, &mut acked), Receipt::Fresh);
    }
    assert_eq!(b.on_receive(&headers[38], &mut acked), Receipt::Duplicate);
    assert_eq!(b.on_receive(&headers[37], &mut acked), Receipt::Fresh);
    assert_eq!(b.on_receive(&headers[37], &mut acked), Receipt::Duplicate);
    assert_eq!(b.on_receive(&headers[1], &mut acked), Receipt::Stale);

    // B's reply acknowledges exactly what B received within the window.
    let reply = b.next_header();
    assert_eq!(reply.ack, headers[38].sequence);
    let mut acked = AckedList::default();
    assert_eq!(a.on_receive(&reply, &mut acked), Receipt::Fresh);
    let got: Vec<u16> = acked.as_slice().to_vec();
    let mut expected: Vec<u16> = headers
        .iter()
        .enumerate()
        .filter(|(i, _)| (*i % 2 == 0 || *i == 37) && *i >= 6)
        .map(|(_, h)| h.sequence)
        .collect();
    expected.reverse();
    assert_eq!(got, expected);
    // Acks are applied once.
    let mut acked = AckedList::default();
    a.on_receive(
        &PacketHeader {
            sequence: reply.sequence.wrapping_add(1),
            ..reply
        },
        &mut acked,
    );
    assert!(acked.as_slice().is_empty());
}
