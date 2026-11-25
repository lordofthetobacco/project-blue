//! Top-level packets: everything that travels in one netcode.io payload.
//!
//! Every packet starts with a 4-bit kind. `Control` packets carry one
//! handshake message; `ClientInput` and `ServerSnapshot` packets carry the
//! sequenced game traffic. All packets end with zero padding to a byte.

use crate::MAX_PAYLOAD_BYTES;
use crate::bits::{BitReader, BitWriter, Stream};
use crate::command::CommandList;
use crate::error::Error;
use crate::message::Message;
use crate::object::WorldObject;
use crate::player::{PlayerSet, PlayerState, PlayerUpdates, apply_entry, write_player_entry};
use crate::reliable::ReliableSection;
use crate::seq::PacketHeader;

/// Bits of the packet kind field. Frozen across protocol versions.
pub const KIND_BITS: u32 = 4;

/// Packet kinds. 3..=15 are reserved.
pub mod kind {
    /// Client -> server, once per tick.
    pub const CLIENT_INPUT: u32 = 0;
    /// Server -> client, once per tick.
    pub const SERVER_SNAPSHOT: u32 = 1;
    /// Either direction: one handshake message (frozen layout).
    pub const CONTROL: u32 = 2;
}

/// Reads the kind of a payload without decoding it.
///
/// # Errors
/// [`Error::Truncated`] for an empty payload,
/// [`Error::UnknownPacketKind`] for reserved kinds.
pub fn peek_kind(payload: &[u8]) -> Result<u32, Error> {
    let mut r = BitReader::new(payload);
    let k = r.read_bits(KIND_BITS)?;
    match k {
        kind::CLIENT_INPUT | kind::SERVER_SNAPSHOT | kind::CONTROL => Ok(k),
        _ => Err(Error::UnknownPacketKind),
    }
}

fn capacity_bits(buf: &[u8]) -> usize {
    buf.len().min(MAX_PAYLOAD_BYTES) * 8
}

fn expect_kind(r: &mut BitReader<'_>, expected: u32) -> Result<(), Error> {
    let k = r.read_bits(KIND_BITS)?;
    if k == expected {
        Ok(())
    } else if k <= kind::CONTROL {
        Err(Error::Usage)
    } else {
        Err(Error::UnknownPacketKind)
    }
}

fn optional_u32<S: Stream>(s: &mut S, value: &mut Option<u32>) -> Result<(), Error> {
    let mut present = value.is_some();
    s.bool(&mut present)?;
    if present {
        let mut v = value.unwrap_or(0);
        s.u32(&mut v)?;
        *value = Some(v);
    } else {
        *value = None;
    }
    Ok(())
}

/// Encodes a `Control` packet carrying one handshake message.
///
/// # Errors
/// [`Error::UnexpectedMessage`] for non-handshake messages; write errors.
pub fn encode_control(message: &Message, buf: &mut [u8]) -> Result<usize, Error> {
    if !message.is_handshake() {
        return Err(Error::UnexpectedMessage);
    }
    let limit = capacity_bits(buf);
    let mut w = BitWriter::new(buf);
    w.set_limit(limit);
    w.write_bits(kind::CONTROL, KIND_BITS)?;
    let mut copy = *message;
    copy.serialize(&mut w)?;
    w.finish()
}

/// Decodes a `Control` packet.
///
/// # Errors
/// Any decode error; [`Error::UnexpectedMessage`] for non-handshake
/// messages; [`Error::Usage`] if the payload is another packet kind.
pub fn decode_control(payload: &[u8]) -> Result<Message, Error> {
    if payload.len() > MAX_PAYLOAD_BYTES {
        return Err(Error::OutOfRange);
    }
    let mut r = BitReader::new(payload);
    expect_kind(&mut r, kind::CONTROL)?;
    let mut message = Message::default();
    message.serialize(&mut r)?;
    if !message.is_handshake() {
        return Err(Error::UnexpectedMessage);
    }
    r.finish()?;
    Ok(message)
}

/// Client -> server packet, one per tick.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct ClientInput {
    /// Sequence and acks.
    pub header: PacketHeader,
    /// Server tick of the newest snapshot fully decoded (delta baseline).
    pub snapshot_ack: Option<u32>,
    /// Server tick the client is rendering remote entities at.
    pub view_tick: u32,
    /// Fraction of a tick past `view_tick`, in 1/256.
    pub view_fraction: u8,
    /// 1..=16 commands, newest first, consecutive ticks.
    pub commands: CommandList,
    /// Reliable messages.
    pub reliable: ReliableSection,
}

impl ClientInput {
    fn serialize<S: Stream>(&mut self, s: &mut S) -> Result<(), Error> {
        self.header.serialize(s)?;
        optional_u32(s, &mut self.snapshot_ack)?;
        s.u32(&mut self.view_tick)?;
        s.u8(&mut self.view_fraction)?;
        self.commands.serialize(s)?;
        self.reliable.serialize(s)
    }

    /// Encodes into `buf` (at most 1200 bytes are used).
    ///
    /// # Errors
    /// [`Error::Overflow`] if it does not fit; field errors.
    pub fn encode(&self, buf: &mut [u8]) -> Result<usize, Error> {
        let limit = capacity_bits(buf);
        let mut w = BitWriter::new(buf);
        w.set_limit(limit);
        w.write_bits(kind::CLIENT_INPUT, KIND_BITS)?;
        let mut copy = *self;
        copy.serialize(&mut w)?;
        w.finish()
    }

    /// Decodes `payload` into `out` (no allocation). `out` is unspecified
    /// on error.
    ///
    /// # Errors
    /// Any decode error.
    pub fn decode(payload: &[u8], out: &mut Self) -> Result<(), Error> {
        if payload.len() > MAX_PAYLOAD_BYTES {
            return Err(Error::OutOfRange);
        }
        let mut r = BitReader::new(payload);
        expect_kind(&mut r, kind::CLIENT_INPUT)?;
        out.serialize(&mut r)?;
        r.finish()
    }
}

/// Smallest and largest input buffer depth.
pub const INPUT_DEPTH_MIN: i8 = -32;
/// Largest input buffer depth.
pub const INPUT_DEPTH_MAX: i8 = 31;
/// Oldest baseline a snapshot can name, in ticks before `server_tick`.
pub const MAX_BASELINE_AGE: u32 = 255;
/// Snapshot player sets a client keeps for use as baselines; servers must
/// not name a baseline older than this many ticks.
pub const BASELINE_HISTORY: u32 = 64;
/// Most world objects in one snapshot.
pub const MAX_OBJECTS_PER_PACKET: usize = 96;

/// Per-snapshot timing information.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct SnapshotInfo {
    /// Tick this snapshot describes.
    pub server_tick: u32,
    /// Newest command tick consumed for this client, `None` before any.
    pub last_command_tick: Option<u32>,
    /// Commands buffered beyond the one consumed this tick; negative when
    /// the client's commands arrive late (ticks run without one).
    pub input_buffer_depth: i8,
    /// Tick of the snapshot the players section is delta-encoded against
    /// (1..=255 ticks before `server_tick`), `None` for a full section.
    pub baseline_tick: Option<u32>,
}

impl SnapshotInfo {
    fn serialize<S: Stream>(&mut self, s: &mut S) -> Result<(), Error> {
        s.u32(&mut self.server_tick)?;
        optional_u32(s, &mut self.last_command_tick)?;
        s.bounded_i8(
            &mut self.input_buffer_depth,
            INPUT_DEPTH_MIN,
            INPUT_DEPTH_MAX,
        )?;
        let mut present = self.baseline_tick.is_some();
        s.bool(&mut present)?;
        if present {
            let mut age = self
                .server_tick
                .wrapping_sub(self.baseline_tick.unwrap_or(0));
            s.bounded_u32(&mut age, 1, MAX_BASELINE_AGE)?;
            self.baseline_tick = Some(self.server_tick.wrapping_sub(age));
        } else {
            self.baseline_tick = None;
        }
        Ok(())
    }
}

fn own_player<S: Stream>(s: &mut S, value: &mut Option<PlayerState>) -> Result<(), Error> {
    let mut present = value.is_some();
    s.bool(&mut present)?;
    if present {
        let mut state = value.unwrap_or_default();
        state.serialize(s)?;
        *value = Some(state);
    } else {
        *value = None;
    }
    Ok(())
}

/// Fixed-capacity list of decoded world objects.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ObjectList {
    objects: [WorldObject; MAX_OBJECTS_PER_PACKET],
    len: usize,
}

impl Default for ObjectList {
    fn default() -> Self {
        Self {
            objects: [WorldObject::default(); MAX_OBJECTS_PER_PACKET],
            len: 0,
        }
    }
}

impl ObjectList {
    /// Objects in wire order (later entries for the same id win).
    #[must_use]
    pub fn as_slice(&self) -> &[WorldObject] {
        &self.objects[..self.len]
    }
}

/// A decoded server -> client packet.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct ServerSnapshot {
    /// Sequence and acks.
    pub header: PacketHeader,
    /// Timing.
    pub info: SnapshotInfo,
    /// The receiving client's own player, full precision.
    pub own: Option<PlayerState>,
    /// Other players; apply to the baseline named by `info.baseline_tick`.
    pub players: PlayerUpdates,
    /// Reliable messages.
    pub reliable: ReliableSection,
    /// World objects chosen for this packet.
    pub objects: ObjectList,
}

impl ServerSnapshot {
    /// Decodes `payload` into `out` (no allocation). The whole packet is
    /// validated; the players section is applied separately with
    /// [`PlayerUpdates::apply`]. `out` is unspecified on error.
    ///
    /// # Errors
    /// Any decode error.
    pub fn decode(payload: &[u8], out: &mut Self) -> Result<(), Error> {
        if payload.len() > MAX_PAYLOAD_BYTES {
            return Err(Error::OutOfRange);
        }
        let mut r = BitReader::new(payload);
        expect_kind(&mut r, kind::SERVER_SNAPSHOT)?;
        out.header.serialize(&mut r)?;
        out.info.serialize(&mut r)?;
        own_player(&mut r, &mut out.own)?;
        out.reliable.serialize(&mut r)?;
        out.players.read(out.info.baseline_tick.is_some(), &mut r)?;
        out.objects.len = 0;
        loop {
            let mut more = false;
            r.bool(&mut more)?;
            if !more {
                break;
            }
            let object = WorldObject::read(&mut r)?;
            let slot = out
                .objects
                .objects
                .get_mut(out.objects.len)
                .ok_or(Error::NonCanonical)?;
            *slot = object;
            out.objects.len += 1;
        }
        r.finish()
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Stage {
    Players,
    Objects,
}

/// Builds a `ServerSnapshot` packet section by section, never exceeding
/// 1200 bytes (or the buffer, if smaller).
///
/// Order: [`SnapshotWriter::begin`] (header, info, own player, reliable
/// section), [`SnapshotWriter::write_players`] once, any number of
/// [`SnapshotWriter::try_add_object`], then [`SnapshotWriter::finish`].
#[derive(Debug)]
pub struct SnapshotWriter<'a> {
    w: BitWriter<'a>,
    capacity: usize,
    stage: Stage,
    objects: usize,
    has_baseline: bool,
}

impl<'a> SnapshotWriter<'a> {
    /// Writes everything up to the players section.
    ///
    /// # Errors
    /// [`Error::Overflow`] if that alone does not fit; field errors.
    pub fn begin(
        buf: &'a mut [u8],
        header: &PacketHeader,
        info: &SnapshotInfo,
        own: Option<&PlayerState>,
        reliable: &ReliableSection,
    ) -> Result<Self, Error> {
        let capacity = capacity_bits(buf);
        let mut w = BitWriter::new(buf);
        // Two terminator bits (players, objects) are always reserved.
        w.set_limit(capacity.saturating_sub(2));
        w.write_bits(kind::SERVER_SNAPSHOT, KIND_BITS)?;
        let mut header = *header;
        header.serialize(&mut w)?;
        let mut info = *info;
        info.serialize(&mut w)?;
        let mut own = own.copied();
        own_player(&mut w, &mut own)?;
        let mut reliable = *reliable;
        reliable.serialize(&mut w)?;
        Ok(Self {
            w,
            capacity,
            stage: Stage::Players,
            objects: 0,
            has_baseline: info.baseline_tick.is_some(),
        })
    }

    /// Writes the players section: removals first, then entries in
    /// `priority` order (ids not in `priority` are not sent), stopping short
    /// of the last `reserve_bytes` of the packet. Entries that do not fit
    /// are skipped. `record` receives the set the client will reconstruct
    /// (`baseline` with the written entries applied); the server must store
    /// it as this tick's baseline candidate. `baseline` must be `Some`
    /// exactly when `info.baseline_tick` was.
    ///
    /// # Errors
    /// [`Error::Usage`] when called twice or `baseline` contradicts
    /// `info.baseline_tick`; field errors.
    pub fn write_players(
        &mut self,
        current: &PlayerSet,
        baseline: Option<&PlayerSet>,
        priority: &[u8],
        reserve_bytes: usize,
        record: &mut PlayerSet,
    ) -> Result<(), Error> {
        if self.stage != Stage::Players || baseline.is_some() != self.has_baseline {
            return Err(Error::Usage);
        }
        *record = baseline.copied().unwrap_or_default();
        self.w.set_limit(
            self.capacity
                .saturating_sub(2)
                .saturating_sub(reserve_bytes * 8),
        );
        let mut done: u64 = 0;
        if let Some(base) = baseline {
            for (id, _) in base.iter() {
                if current.get(id).is_none() {
                    self.try_player(id, current, baseline, record, &mut done)?;
                }
            }
        }
        for &id in priority {
            self.try_player(id, current, baseline, record, &mut done)?;
        }
        self.w.set_limit(self.capacity.saturating_sub(1));
        self.w.write_bits(0, 1)?;
        self.stage = Stage::Objects;
        Ok(())
    }

    fn try_player(
        &mut self,
        id: u8,
        current: &PlayerSet,
        baseline: Option<&PlayerSet>,
        record: &mut PlayerSet,
        done: &mut u64,
    ) -> Result<(), Error> {
        let Some(bit) = 1u64.checked_shl(u32::from(id)) else {
            return Err(Error::OutOfRange);
        };
        if *done & bit != 0 {
            return Ok(());
        }
        *done |= bit;
        let mark = self.w.pos();
        match write_player_entry(id, current, baseline, &mut self.w) {
            Ok(Some(entry)) => apply_entry(&entry, record),
            Ok(None) => Ok(()),
            Err(Error::Overflow) => {
                self.w.rewind(mark);
                Ok(())
            }
            Err(e) => {
                self.w.rewind(mark);
                Err(e)
            }
        }
    }

    /// Whole bytes still free for objects (after the players section).
    #[must_use]
    pub fn bytes_left(&self) -> usize {
        self.w.bits_left() / 8
    }

    /// Objects written so far.
    #[must_use]
    pub fn object_count(&self) -> usize {
        self.objects
    }

    /// Appends one object if it fits; returns `false` (and writes nothing)
    /// when it does not or the object limit is reached.
    ///
    /// # Errors
    /// [`Error::Usage`] before [`SnapshotWriter::write_players`].
    pub fn try_add_object(&mut self, object: &WorldObject) -> Result<bool, Error> {
        if self.stage != Stage::Objects {
            return Err(Error::Usage);
        }
        if self.objects >= MAX_OBJECTS_PER_PACKET {
            return Ok(false);
        }
        let mark = self.w.pos();
        let result = self
            .w
            .write_bits(1, 1)
            .and_then(|()| object.write(&mut self.w));
        match result {
            Ok(()) => {
                self.objects += 1;
                Ok(true)
            }
            Err(Error::Overflow) => {
                self.w.rewind(mark);
                Ok(false)
            }
            Err(e) => {
                self.w.rewind(mark);
                Err(e)
            }
        }
    }

    /// Ends the packet and returns its length in bytes (<= 1200).
    ///
    /// # Errors
    /// [`Error::Usage`] before [`SnapshotWriter::write_players`].
    pub fn finish(mut self) -> Result<usize, Error> {
        if self.stage != Stage::Objects {
            return Err(Error::Usage);
        }
        self.w.set_limit(self.capacity);
        self.w.write_bits(0, 1)?;
        self.w.finish()
    }
}
