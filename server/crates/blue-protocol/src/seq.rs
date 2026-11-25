//! Sequence numbers, the packet header and packet-level acknowledgement
//! (Gaffer on Games "reliable ordered messages" scheme).

use crate::bits::Stream;
use crate::error::Error;

/// True if `a` is newer than `b` on the 16-bit circle (a half-circle window).
#[must_use]
pub const fn seq_greater(a: u16, b: u16) -> bool {
    ((a > b) && (a - b <= 32768)) || ((a < b) && (b - a > 32768))
}

/// True if `a` is older than `b` on the 16-bit circle.
#[must_use]
pub const fn seq_less(a: u16, b: u16) -> bool {
    seq_greater(b, a)
}

/// True if tick `a` is newer than tick `b` on the 32-bit circle.
#[must_use]
pub const fn tick_greater(a: u32, b: u32) -> bool {
    a != b && a.wrapping_sub(b) < (1 << 31)
}

/// Header at the start of every `ClientInput` and `ServerSnapshot` packet.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub struct PacketHeader {
    /// Sequence number of this packet.
    pub sequence: u16,
    /// Newest sequence received from the peer (`0xFFFF` before any).
    pub ack: u16,
    /// Bit `i` (LSB = 0) set: packet `ack - 1 - i` was received too.
    pub ack_bits: u32,
}

impl PacketHeader {
    /// Serialises the header: sequence (16), ack (16), ack bits (32).
    ///
    /// # Errors
    /// Stream errors.
    pub fn serialize<S: Stream>(&mut self, s: &mut S) -> Result<(), Error> {
        s.u16(&mut self.sequence)?;
        s.u16(&mut self.ack)?;
        s.u32(&mut self.ack_bits)
    }
}

/// Window of the received-packet history carried in a header.
pub const ACK_BITS: u16 = 32;
/// Number of sent packets remembered for acknowledgement.
pub const SENT_HISTORY: usize = 256;

/// Outcome of [`AckTracker::on_receive`].
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Receipt {
    /// First time seen: process the whole packet.
    Fresh,
    /// Already received: discard the packet.
    Duplicate,
    /// Older than the ack window: discard the packet (it is never acked, so
    /// its reliable messages will be resent).
    Stale,
}

/// Sequence numbers newly acknowledged by one received header.
#[derive(Clone, Copy, Debug)]
pub struct AckedList {
    seqs: [u16; ACK_BITS as usize + 1],
    len: usize,
}

impl Default for AckedList {
    fn default() -> Self {
        Self {
            seqs: [0; ACK_BITS as usize + 1],
            len: 0,
        }
    }
}

impl AckedList {
    /// The acknowledged sequence numbers, newest first.
    #[must_use]
    pub fn as_slice(&self) -> &[u16] {
        &self.seqs[..self.len]
    }

    fn push(&mut self, seq: u16) {
        if let Some(slot) = self.seqs.get_mut(self.len) {
            *slot = seq;
            self.len += 1;
        }
    }
}

#[derive(Clone, Copy, Debug, Default)]
struct SentRecord {
    seq: u16,
    valid: bool,
    acked: bool,
}

/// Per-connection sequence and acknowledgement state for one direction pair.
///
/// Call [`AckTracker::next_header`] for every outgoing packet and
/// [`AckTracker::on_receive`] only after an incoming packet decoded
/// completely; process the packet only when it returns [`Receipt::Fresh`].
#[derive(Clone, Debug)]
pub struct AckTracker {
    next_seq: u16,
    newest_received: Option<u16>,
    received_bits: u32,
    sent: [SentRecord; SENT_HISTORY],
}

impl Default for AckTracker {
    fn default() -> Self {
        Self::new()
    }
}

impl AckTracker {
    /// Starts at sequence 0.
    #[must_use]
    pub fn new() -> Self {
        Self::with_initial_sequence(0)
    }

    /// Starts at `seq` (tests exercise wrap-around with this).
    #[must_use]
    pub fn with_initial_sequence(seq: u16) -> Self {
        Self {
            next_seq: seq,
            newest_received: None,
            received_bits: 0,
            sent: [SentRecord::default(); SENT_HISTORY],
        }
    }

    /// Sequence the next outgoing packet will use.
    #[must_use]
    pub fn next_sequence(&self) -> u16 {
        self.next_seq
    }

    /// Header for the next outgoing packet; records it as sent.
    pub fn next_header(&mut self) -> PacketHeader {
        let seq = self.next_seq;
        self.next_seq = self.next_seq.wrapping_add(1);
        self.sent[usize::from(seq) % SENT_HISTORY] = SentRecord {
            seq,
            valid: true,
            acked: false,
        };
        let (ack, ack_bits) = match self.newest_received {
            Some(newest) => (newest, self.received_bits),
            None => (0xFFFF, 0),
        };
        PacketHeader {
            sequence: seq,
            ack,
            ack_bits,
        }
    }

    /// Registers a received header. Its acks are applied in every case
    /// (they are idempotent); newly acknowledged sequences go to `acked`.
    pub fn on_receive(&mut self, header: &PacketHeader, acked: &mut AckedList) -> Receipt {
        self.apply_acks(header, acked);
        let seq = header.sequence;
        let Some(newest) = self.newest_received else {
            self.newest_received = Some(seq);
            self.received_bits = 0;
            return Receipt::Fresh;
        };
        if seq_greater(seq, newest) {
            let distance = u32::from(seq.wrapping_sub(newest));
            let shifted = if distance > u32::from(ACK_BITS) {
                0
            } else {
                let wide = (u64::from(self.received_bits) << distance) | (1u64 << (distance - 1));
                (wide & 0xFFFF_FFFF) as u32
            };
            self.received_bits = shifted;
            self.newest_received = Some(seq);
            return Receipt::Fresh;
        }
        if seq == newest {
            return Receipt::Duplicate;
        }
        let distance = newest.wrapping_sub(seq);
        if distance > ACK_BITS {
            return Receipt::Stale;
        }
        let bit = 1u32 << (distance - 1);
        if self.received_bits & bit != 0 {
            return Receipt::Duplicate;
        }
        self.received_bits |= bit;
        Receipt::Fresh
    }

    fn apply_acks(&mut self, header: &PacketHeader, acked: &mut AckedList) {
        self.ack_one(header.ack, acked);
        for i in 0..ACK_BITS {
            if header.ack_bits & (1 << i) != 0 {
                self.ack_one(header.ack.wrapping_sub(1 + i), acked);
            }
        }
    }

    fn ack_one(&mut self, seq: u16, acked: &mut AckedList) {
        let record = &mut self.sent[usize::from(seq) % SENT_HISTORY];
        if record.valid && record.seq == seq && !record.acked {
            record.acked = true;
            acked.push(seq);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn wrap_compare() {
        assert!(seq_greater(1, 0));
        assert!(seq_greater(0, 65535));
        assert!(seq_greater(100, 65500));
        assert!(!seq_greater(65500, 100));
        assert!(seq_less(65535, 0));
        assert!(!seq_greater(5, 5));
        assert!(tick_greater(0, u32::MAX));
        assert!(!tick_greater(u32::MAX, 0));
    }
}
