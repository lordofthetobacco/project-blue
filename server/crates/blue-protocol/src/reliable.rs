//! Reliable-ordered message channel riding on packet acknowledgements.
//!
//! Each message gets a 16-bit id. The sender keeps it until a packet that
//! carried it is acknowledged and resends it (at most once per
//! `resend_interval`) in later packets meanwhile. The receiver buffers out of
//! order arrivals and hands messages out strictly in id order, each once.

use crate::bits::{Stream, bits_required};
use crate::error::Error;
use crate::message::Message;
use crate::seq::seq_less;

/// Messages per packet at most.
pub const MAX_MESSAGES_PER_PACKET: usize = 32;
/// Send and receive window, in message ids.
pub const WINDOW: usize = 256;
/// Packets remembered for mapping an ack back to its messages.
pub const SENT_PACKET_HISTORY: usize = 256;
/// Default reliable byte budget per packet (of 1200).
pub const DEFAULT_BUDGET_BYTES: usize = 300;
/// Default resend interval, seconds.
pub const DEFAULT_RESEND_INTERVAL: f64 = 0.1;

const COUNT_BITS: u32 = bits_required(MAX_MESSAGES_PER_PACKET as u32);
const FIRST_ID_BITS: usize = 16;

/// One message with its id.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct ReliableEntry {
    /// Message id.
    pub id: u16,
    /// The message.
    pub message: Message,
}

/// The reliable section of one packet.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ReliableSection {
    entries: [ReliableEntry; MAX_MESSAGES_PER_PACKET],
    len: usize,
}

impl Default for ReliableSection {
    fn default() -> Self {
        Self {
            entries: [ReliableEntry::default(); MAX_MESSAGES_PER_PACKET],
            len: 0,
        }
    }
}

impl ReliableSection {
    /// Entries in wire order.
    #[must_use]
    pub fn entries(&self) -> &[ReliableEntry] {
        &self.entries[..self.len]
    }

    /// Number of messages.
    #[must_use]
    pub fn len(&self) -> usize {
        self.len
    }

    /// True when empty.
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.len == 0
    }

    /// Removes every entry.
    pub fn clear(&mut self) {
        self.len = 0;
    }

    /// Appends an entry (ids must increase, each by 1..=255).
    ///
    /// # Errors
    /// [`Error::Usage`] if full or the id does not follow the previous one;
    /// [`Error::UnexpectedMessage`] for handshake messages.
    pub fn push(&mut self, id: u16, message: Message) -> Result<(), Error> {
        if self.len >= MAX_MESSAGES_PER_PACKET {
            return Err(Error::Usage);
        }
        if message.is_handshake() {
            return Err(Error::UnexpectedMessage);
        }
        if let Some(last) = self.entries().last() {
            let gap = id.wrapping_sub(last.id);
            if gap == 0 || usize::from(gap) >= WINDOW {
                return Err(Error::Usage);
            }
        }
        self.entries[self.len] = ReliableEntry { id, message };
        self.len += 1;
        Ok(())
    }

    /// Serialises: count `bounded(0, 32)`; then the first id in 16 bits and
    /// each later id as `varuint(id - previous - 1)`; each id followed by its
    /// message (type id + body).
    ///
    /// # Errors
    /// Stream errors; [`Error::UnexpectedMessage`] for handshake messages;
    /// [`Error::OutOfRange`] for an id gap of 256 or more.
    pub fn serialize<S: Stream>(&mut self, s: &mut S) -> Result<(), Error> {
        let mut count = u32::try_from(self.len).map_err(|_| Error::Usage)?;
        s.bounded_u32(&mut count, 0, MAX_MESSAGES_PER_PACKET as u32)?;
        let count = count as usize;
        let mut previous: u16 = 0;
        for i in 0..count {
            let entry = &mut self.entries[i];
            if i == 0 {
                s.u16(&mut entry.id)?;
            } else {
                let mut gap = u32::from(entry.id.wrapping_sub(previous).wrapping_sub(1));
                s.varuint(&mut gap)?;
                if gap as usize >= WINDOW - 1 {
                    return Err(Error::OutOfRange);
                }
                let step = u16::try_from(gap + 1).map_err(|_| Error::OutOfRange)?;
                entry.id = previous.wrapping_add(step);
            }
            previous = entry.id;
            entry.message.serialize(s)?;
            if entry.message.is_handshake() {
                return Err(Error::UnexpectedMessage);
            }
        }
        if !S::WRITING {
            self.len = count;
        }
        Ok(())
    }
}

/// Tunables of a [`ReliableEndpoint`].
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ReliableConfig {
    /// Seconds before an unacknowledged message is sent again.
    pub resend_interval: f64,
    /// Bits a reliable section may take per packet, count field included.
    pub budget_bits: usize,
}

impl Default for ReliableConfig {
    fn default() -> Self {
        Self {
            resend_interval: DEFAULT_RESEND_INTERVAL,
            budget_bits: DEFAULT_BUDGET_BYTES * 8,
        }
    }
}

#[derive(Clone, Copy, Debug)]
struct SendSlot {
    id: u16,
    occupied: bool,
    last_sent: f64,
    bits: usize,
    message: Message,
}

impl Default for SendSlot {
    fn default() -> Self {
        Self {
            id: 0,
            occupied: false,
            last_sent: f64::NEG_INFINITY,
            bits: 0,
            message: Message::default(),
        }
    }
}

#[derive(Clone, Copy, Debug, Default)]
struct SentPacket {
    seq: u16,
    valid: bool,
    len: usize,
    ids: [u16; MAX_MESSAGES_PER_PACKET],
}

#[derive(Clone, Copy, Debug, Default)]
struct RecvSlot {
    id: u16,
    occupied: bool,
    message: Message,
}

/// One side of a reliable-ordered channel (sends and receives).
///
/// Fixed size, no allocation after construction. Both peers must start
/// with the same ids (0 by default).
#[derive(Clone, Debug)]
pub struct ReliableEndpoint {
    config: ReliableConfig,
    send: [SendSlot; WINDOW],
    oldest_unacked: u16,
    next_send_id: u16,
    sent_packets: [SentPacket; SENT_PACKET_HISTORY],
    recv: [RecvSlot; WINDOW],
    next_recv_id: u16,
}

impl ReliableEndpoint {
    /// A new endpoint with message ids starting at 0.
    #[must_use]
    pub fn new(config: ReliableConfig) -> Self {
        Self::with_initial_ids(config, 0, 0)
    }

    /// A new endpoint whose first sent id is `send_id` and first expected id
    /// is `recv_id` (tests exercise wrap-around with this).
    #[must_use]
    pub fn with_initial_ids(config: ReliableConfig, send_id: u16, recv_id: u16) -> Self {
        Self {
            config,
            send: [SendSlot::default(); WINDOW],
            oldest_unacked: send_id,
            next_send_id: send_id,
            sent_packets: [SentPacket::default(); SENT_PACKET_HISTORY],
            recv: [RecvSlot::default(); WINDOW],
            next_recv_id: recv_id,
        }
    }

    /// Messages queued and not yet acknowledged.
    #[must_use]
    pub fn unacked(&self) -> usize {
        usize::from(self.next_send_id.wrapping_sub(self.oldest_unacked))
    }

    /// Queues a message and returns its id.
    ///
    /// # Errors
    /// [`Error::WindowFull`] when 256 messages are unacknowledged;
    /// [`Error::UnexpectedMessage`] for handshake messages;
    /// [`Error::MessageTooLarge`] if it can never fit the budget;
    /// field range errors.
    pub fn send(&mut self, message: Message) -> Result<u16, Error> {
        if message.is_handshake() {
            return Err(Error::UnexpectedMessage);
        }
        if self.unacked() >= WINDOW {
            return Err(Error::WindowFull);
        }
        let bits = message.bit_len()?;
        if COUNT_BITS as usize + FIRST_ID_BITS + bits > self.config.budget_bits {
            return Err(Error::MessageTooLarge);
        }
        let id = self.next_send_id;
        self.send[usize::from(id) % WINDOW] = SendSlot {
            id,
            occupied: true,
            last_sent: f64::NEG_INFINITY,
            bits,
            message,
        };
        self.next_send_id = id.wrapping_add(1);
        Ok(id)
    }

    /// Fills `out` with the messages due at time `now` (seconds) that fit
    /// the budget, oldest first, and records them as carried by packet
    /// `packet_seq`.
    pub fn fill_section(&mut self, packet_seq: u16, now: f64, out: &mut ReliableSection) {
        out.clear();
        let mut record = SentPacket {
            seq: packet_seq,
            valid: true,
            ..SentPacket::default()
        };
        let mut used = COUNT_BITS as usize;
        let mut previous: Option<u16> = None;
        let mut id = self.oldest_unacked;
        while id != self.next_send_id && out.len < MAX_MESSAGES_PER_PACKET {
            let slot = &mut self.send[usize::from(id) % WINDOW];
            if slot.occupied && slot.id == id && now - slot.last_sent >= self.config.resend_interval
            {
                let id_bits = match previous {
                    None => FIRST_ID_BITS,
                    Some(p) => varuint_bits(u32::from(id.wrapping_sub(p).wrapping_sub(1))),
                };
                let cost = id_bits + slot.bits;
                if used + cost <= self.config.budget_bits && out.push(id, slot.message).is_ok() {
                    used += cost;
                    slot.last_sent = now;
                    record.ids[record.len] = id;
                    record.len += 1;
                    previous = Some(id);
                }
            }
            id = id.wrapping_add(1);
        }
        self.sent_packets[usize::from(packet_seq) % SENT_PACKET_HISTORY] = record;
    }

    /// Marks the messages carried by packet `seq` as delivered.
    pub fn on_packet_acked(&mut self, seq: u16) {
        let record = &mut self.sent_packets[usize::from(seq) % SENT_PACKET_HISTORY];
        if !record.valid || record.seq != seq {
            return;
        }
        record.valid = false;
        for &id in &record.ids[..record.len] {
            let slot = &mut self.send[usize::from(id) % WINDOW];
            if slot.occupied && slot.id == id {
                slot.occupied = false;
            }
        }
        while self.oldest_unacked != self.next_send_id {
            let slot = &self.send[usize::from(self.oldest_unacked) % WINDOW];
            if slot.occupied && slot.id == self.oldest_unacked {
                break;
            }
            self.oldest_unacked = self.oldest_unacked.wrapping_add(1);
        }
    }

    /// Takes in the messages of a received section (duplicates and already
    /// delivered ids are ignored).
    ///
    /// # Errors
    /// [`Error::OutOfRange`] if an id lies beyond the receive window (a
    /// protocol violation); entries before it are kept.
    pub fn receive_section(&mut self, section: &ReliableSection) -> Result<(), Error> {
        for entry in section.entries() {
            if seq_less(entry.id, self.next_recv_id) {
                continue;
            }
            if usize::from(entry.id.wrapping_sub(self.next_recv_id)) >= WINDOW {
                return Err(Error::OutOfRange);
            }
            let slot = &mut self.recv[usize::from(entry.id) % WINDOW];
            if !slot.occupied {
                *slot = RecvSlot {
                    id: entry.id,
                    occupied: true,
                    message: entry.message,
                };
            }
        }
        Ok(())
    }

    /// The next message in order, if it has arrived.
    pub fn pop(&mut self) -> Option<Message> {
        let slot = &mut self.recv[usize::from(self.next_recv_id) % WINDOW];
        if !slot.occupied || slot.id != self.next_recv_id {
            return None;
        }
        slot.occupied = false;
        self.next_recv_id = self.next_recv_id.wrapping_add(1);
        Some(slot.message)
    }
}

fn varuint_bits(value: u32) -> usize {
    let groups = if value == 0 {
        1
    } else {
        bits_required(value).div_ceil(6)
    };
    groups as usize * 7
}
