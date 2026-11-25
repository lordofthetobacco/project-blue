//! Project Blue wire protocol.
//!
//! Pure codecs and state machines, no IO: bit packing ([`bits`]),
//! quantisation ([`quant`]), sequence numbers and acks ([`seq`]), the
//! message catalogue ([`message`]), user commands ([`command`]), player and
//! world-object state ([`player`], [`object`]), the reliable-ordered channel
//! ([`reliable`]) and the top-level packets ([`packet`]).
//!
//! The normative specification is `docs/netcode/protocol.md` at the
//! repository root; the C++ client codec is written from it and checked
//! against the golden vectors in `tests/vectors/`.
//!
//! Encoding writes into caller-provided buffers and decoding fills
//! caller-provided fixed-capacity values: nothing allocates. Decoding never
//! panics; every malformed input is an [`Error`].

#![forbid(unsafe_code)]

pub mod bits;
pub mod command;
mod error;
pub mod message;
pub mod object;
pub mod packet;
pub mod player;
pub mod quant;
pub mod reliable;
pub mod seq;
mod text;

pub use command::{CommandList, UserCommand};
pub use error::Error;
pub use message::{Message, Name};
pub use object::{ObjectKind, WorldObject};
pub use packet::{ClientInput, ServerSnapshot, SnapshotInfo, SnapshotWriter};
pub use player::{PlayerSet, PlayerState, PlayerUpdates, RemotePlayer, RemotePlayerQ};
pub use reliable::{ReliableConfig, ReliableEndpoint, ReliableSection};
pub use seq::{AckTracker, PacketHeader, Receipt};
pub use text::BoundedStr;

/// Wire protocol version. Bumped on every layout change (see the changelog
/// in `docs/netcode/protocol.md`). Sent in `ClientHello`; a mismatch is
/// answered with `ServerReject`.
pub const PROTOCOL_VERSION: u16 = 3;

/// Largest payload of one netcode.io packet, bytes.
pub const MAX_PAYLOAD_BYTES: usize = 1200;

/// Default simulation rate, ticks per second (the physics fixed step).
pub const DEFAULT_TICK_RATE: u16 = 60;
