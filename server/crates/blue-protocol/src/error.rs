//! The single error type of the crate.

use core::fmt;

/// Everything that can go wrong while encoding, decoding or running a channel.
///
/// Decoding never panics: every malformed input maps to one of these.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Error {
    /// The writer ran out of room (buffer or byte budget).
    Overflow,
    /// The reader needed more bits than the payload holds.
    Truncated,
    /// A value lies outside the range its field allows (on encode or decode),
    /// or a float is not finite.
    OutOfRange,
    /// A bounded string is not valid UTF-8.
    InvalidUtf8,
    /// The bits decode, but not in the one canonical form the protocol allows
    /// (overlong varint, empty delta, duplicate id, ...).
    NonCanonical,
    /// More than eight bits, or non-zero bits, follow the last field.
    TrailingData,
    /// The packet kind is reserved or unknown.
    UnknownPacketKind,
    /// The message type id is reserved or unknown.
    UnknownMessage,
    /// A known message appeared where it is not allowed (for example a
    /// handshake message in a reliable section).
    UnexpectedMessage,
    /// A delta-encoded section refers to a baseline the receiver does not
    /// have. The rest of the packet is still valid.
    MissingBaseline,
    /// A delta entry contradicts its baseline (delta or removal of a player
    /// the baseline does not contain, result out of range).
    Inconsistent,
    /// The API was used out of order or with contradictory arguments.
    Usage,
    /// The reliable send window is full; the peer is not acknowledging.
    WindowFull,
    /// A message can never fit into a packet's reliable budget.
    MessageTooLarge,
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let text = match self {
            Self::Overflow => "write buffer or budget exhausted",
            Self::Truncated => "payload truncated",
            Self::OutOfRange => "value out of range",
            Self::InvalidUtf8 => "string is not valid UTF-8",
            Self::NonCanonical => "non-canonical encoding",
            Self::TrailingData => "trailing data after the last field",
            Self::UnknownPacketKind => "unknown packet kind",
            Self::UnknownMessage => "unknown message type",
            Self::UnexpectedMessage => "message not allowed here",
            Self::MissingBaseline => "delta baseline not available",
            Self::Inconsistent => "delta contradicts its baseline",
            Self::Usage => "API misuse",
            Self::WindowFull => "reliable send window full",
            Self::MessageTooLarge => "message exceeds the reliable budget",
        };
        f.write_str(text)
    }
}

impl core::error::Error for Error {}
