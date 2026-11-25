//! The message catalogue: handshake messages (sent in `Control` packets) and
//! game events (sent on the reliable-ordered channel).

use crate::bits::{BitReader, BitWriter, Measure, Stream};
use crate::error::Error;
use crate::text::BoundedStr;

/// Maximum bytes of a player name.
pub const NAME_MAX_BYTES: usize = 32;
/// A player name.
pub type Name = BoundedStr<NAME_MAX_BYTES>;

/// Largest player id + 1. Player ids are 6 bits.
pub const MAX_PLAYERS: usize = 64;
/// Default player cap of a server.
pub const DEFAULT_MAX_PLAYERS: u8 = 16;
/// Largest player id.
pub const MAX_PLAYER_ID: u8 = (MAX_PLAYERS - 1) as u8;

/// Taken-pickup ids per [`Message::WorldSyncPickupsTaken`].
pub const SYNC_PICKUPS_PER_MESSAGE: usize = 16;

/// Message type ids (8 bits on the wire). Unlisted values are reserved:
/// 4..=15 for handshake / control, 30..=127 for future game messages,
/// 128..=255 for experiments. 0 is never valid.
pub mod id {
    /// [`super::Message::ClientHello`].
    pub const CLIENT_HELLO: u8 = 1;
    /// [`super::Message::ServerWelcome`].
    pub const SERVER_WELCOME: u8 = 2;
    /// [`super::Message::ServerReject`].
    pub const SERVER_REJECT: u8 = 3;
    /// [`super::Message::PlayerJoined`].
    pub const PLAYER_JOINED: u8 = 16;
    /// [`super::Message::PlayerLeft`].
    pub const PLAYER_LEFT: u8 = 17;
    /// [`super::Message::Damage`].
    pub const DAMAGE: u8 = 18;
    /// [`super::Message::Death`].
    pub const DEATH: u8 = 19;
    /// [`super::Message::Respawn`].
    pub const RESPAWN: u8 = 20;
    /// [`super::Message::Shot`].
    pub const SHOT: u8 = 21;
    /// [`super::Message::Impact`].
    pub const IMPACT: u8 = 22;
    /// [`super::Message::WeaponDropped`].
    pub const WEAPON_DROPPED: u8 = 23;
    /// [`super::Message::DroppedTaken`].
    pub const DROPPED_TAKEN: u8 = 24;
    /// [`super::Message::PickupTaken`].
    pub const PICKUP_TAKEN: u8 = 25;
    /// [`super::Message::WorldSyncPickupsTaken`].
    pub const WORLD_SYNC_PICKUPS_TAKEN: u8 = 26;
    /// [`super::Message::WorldSyncDroppedWeapon`].
    pub const WORLD_SYNC_DROPPED_WEAPON: u8 = 27;
    /// [`super::Message::WorldSyncDone`].
    pub const WORLD_SYNC_DONE: u8 = 28;
    /// [`super::Message::DroppedRemoved`].
    pub const DROPPED_REMOVED: u8 = 29;
}

/// Why a server refused a client. Unknown codes are kept as-is.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub struct RejectReason(pub u8);

impl RejectReason {
    /// `protocol_version` differs.
    pub const PROTOCOL_MISMATCH: Self = Self(1);
    /// `content_hash` differs (scene, weapon table, catalogue).
    pub const CONTENT_MISMATCH: Self = Self(2);
    /// No free player slot.
    pub const SERVER_FULL: Self = Self(3);
    /// The name is empty or not acceptable.
    pub const BAD_NAME: Self = Self(4);
    /// The server is shutting down.
    pub const SHUTTING_DOWN: Self = Self(5);
}

/// Body hit zone of a [`Message::Damage`] (3 bits).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub struct HitZone(pub u8);

impl HitZone {
    /// Head (4x).
    pub const HEAD: Self = Self(0);
    /// Torso (1x).
    pub const TORSO: Self = Self(1);
    /// Legs (0.6x).
    pub const LEGS: Self = Self(2);
    /// Not a body hit (world damage).
    pub const OTHER: Self = Self(3);
    /// Largest encodable value.
    pub const MAX: u8 = 7;
}

/// Position and rotation `[x, y, z, w]` at full precision.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Pose {
    /// Position, metres.
    pub position: [f32; 3],
    /// Rotation quaternion `[x, y, z, w]`.
    pub rotation: [f32; 4],
}

/// A dropped weapon as announced by [`Message::WeaponDropped`] and
/// [`Message::WorldSyncDroppedWeapon`].
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct DroppedWeapon {
    /// Id of the dropped weapon (key of its world-object state).
    pub dropped_id: u32,
    /// Stable weapon key (FNV-1a of the preset path).
    pub weapon_key: u32,
    /// Rounds left in its magazine.
    pub rounds_in_magazine: u8,
    /// Initial pose.
    pub pose: Pose,
}

/// Every message of the protocol. See `docs/netcode/protocol.md` for the
/// exact layouts. All variants are `Copy` and fixed size.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub enum Message {
    /// Client -> server, first thing sent (frozen layout).
    ClientHello {
        /// [`crate::PROTOCOL_VERSION`] of the client.
        protocol_version: u16,
        /// Hash of the client's content (scene, weapon table, catalogue).
        content_hash: u64,
        /// Requested display name.
        name: Name,
    },
    /// Server -> client: accepted.
    ServerWelcome {
        /// Id assigned to the client (0..=63).
        player_id: u8,
        /// Simulation ticks per second (1..=65535).
        tick_rate: u16,
        /// Current server tick.
        server_tick: u32,
        /// Player cap of this server (1..=64).
        max_players: u8,
    },
    /// Server -> client: refused (frozen layout).
    ServerReject {
        /// Why.
        reason: RejectReason,
        /// The server's [`crate::PROTOCOL_VERSION`].
        server_protocol_version: u16,
    },
    /// A player joined (also sent for every present player to a newcomer).
    PlayerJoined {
        /// Id of the player.
        player_id: u8,
        /// Display name.
        name: Name,
    },
    /// A player left.
    PlayerLeft {
        /// Id of the player.
        player_id: u8,
    },
    /// A player took damage.
    Damage {
        /// Who was hit.
        victim: u8,
        /// Who caused it, `None` for the world.
        attacker: Option<u8>,
        /// Health removed.
        amount: i16,
        /// Hit zone.
        zone: HitZone,
        /// Victim health after the hit.
        health_after: i16,
    },
    /// A player died.
    Death {
        /// Who died.
        victim: u8,
        /// Who killed, `None` for the world.
        killer: Option<u8>,
        /// Weapon used, 0 for none.
        weapon_key: u32,
    },
    /// A player respawned.
    Respawn {
        /// Who.
        player_id: u8,
        /// Feet position, metres.
        position: [f32; 3],
        /// Facing, radians.
        yaw: f32,
    },
    /// A shot was fired (for effects on other clients).
    Shot {
        /// Who fired.
        shooter: u8,
        /// Muzzle origin (the sim eye).
        origin: [f32; 3],
        /// Unit direction.
        direction: [f32; 3],
        /// Weapon fired.
        weapon_key: u32,
        /// The shooter's `shots_fired` for this shot.
        shot_number: u32,
    },
    /// A projectile hit the world.
    Impact {
        /// Hit point.
        point: [f32; 3],
        /// Surface normal.
        normal: [f32; 3],
    },
    /// A weapon was dropped into the world.
    WeaponDropped(DroppedWeapon),
    /// A dropped weapon was picked up.
    DroppedTaken {
        /// Which dropped weapon.
        dropped_id: u32,
        /// Who took it.
        player_id: u8,
    },
    /// A scene pickup was taken.
    PickupTaken {
        /// Scene instance id of the pickup.
        instance_id: u32,
        /// Who took it.
        player_id: u8,
    },
    /// Late-join sync: pickups already taken.
    WorldSyncPickupsTaken {
        /// Number of valid entries in `instance_ids` (0..=16).
        count: u8,
        /// Instance ids; entries past `count` are 0.
        instance_ids: [u32; SYNC_PICKUPS_PER_MESSAGE],
    },
    /// Late-join sync: a dropped weapon that already exists.
    WorldSyncDroppedWeapon(DroppedWeapon),
    /// Late-join sync complete.
    #[default]
    WorldSyncDone,
    /// A dropped weapon left the world without a taker (lifetime expired,
    /// count cap, kill plane).
    DroppedRemoved {
        /// Which dropped weapon.
        dropped_id: u32,
    },
}

fn player_id<S: Stream>(s: &mut S, value: &mut u8) -> Result<(), Error> {
    s.bounded_u8(value, MAX_PLAYER_ID)
}

fn optional_player_id<S: Stream>(s: &mut S, value: &mut Option<u8>) -> Result<(), Error> {
    let mut present = value.is_some();
    s.bool(&mut present)?;
    if present {
        let mut id = value.unwrap_or(0);
        player_id(s, &mut id)?;
        *value = Some(id);
    } else {
        *value = None;
    }
    Ok(())
}

fn dropped_weapon<S: Stream>(s: &mut S, value: &mut DroppedWeapon) -> Result<(), Error> {
    s.u32(&mut value.dropped_id)?;
    s.u32(&mut value.weapon_key)?;
    s.u8(&mut value.rounds_in_magazine)?;
    s.vec3(&mut value.pose.position)?;
    for c in &mut value.pose.rotation {
        s.f32(c)?;
    }
    Ok(())
}

impl Message {
    /// The message's type id.
    #[must_use]
    pub fn type_id(&self) -> u8 {
        match self {
            Self::ClientHello { .. } => id::CLIENT_HELLO,
            Self::ServerWelcome { .. } => id::SERVER_WELCOME,
            Self::ServerReject { .. } => id::SERVER_REJECT,
            Self::PlayerJoined { .. } => id::PLAYER_JOINED,
            Self::PlayerLeft { .. } => id::PLAYER_LEFT,
            Self::Damage { .. } => id::DAMAGE,
            Self::Death { .. } => id::DEATH,
            Self::Respawn { .. } => id::RESPAWN,
            Self::Shot { .. } => id::SHOT,
            Self::Impact { .. } => id::IMPACT,
            Self::WeaponDropped(_) => id::WEAPON_DROPPED,
            Self::DroppedTaken { .. } => id::DROPPED_TAKEN,
            Self::PickupTaken { .. } => id::PICKUP_TAKEN,
            Self::WorldSyncPickupsTaken { .. } => id::WORLD_SYNC_PICKUPS_TAKEN,
            Self::WorldSyncDroppedWeapon(_) => id::WORLD_SYNC_DROPPED_WEAPON,
            Self::WorldSyncDone => id::WORLD_SYNC_DONE,
            Self::DroppedRemoved { .. } => id::DROPPED_REMOVED,
        }
    }

    /// True for the messages that travel in `Control` packets only.
    #[must_use]
    pub fn is_handshake(&self) -> bool {
        matches!(
            self,
            Self::ClientHello { .. } | Self::ServerWelcome { .. } | Self::ServerReject { .. }
        )
    }

    /// A message of type `type_id` with default fields, to decode into.
    ///
    /// # Errors
    /// [`Error::UnknownMessage`] for reserved ids.
    pub fn blank(type_id: u8) -> Result<Self, Error> {
        Ok(match type_id {
            id::CLIENT_HELLO => Self::ClientHello {
                protocol_version: 0,
                content_hash: 0,
                name: Name::EMPTY,
            },
            id::SERVER_WELCOME => Self::ServerWelcome {
                player_id: 0,
                tick_rate: 1,
                server_tick: 0,
                max_players: 1,
            },
            id::SERVER_REJECT => Self::ServerReject {
                reason: RejectReason(0),
                server_protocol_version: 0,
            },
            id::PLAYER_JOINED => Self::PlayerJoined {
                player_id: 0,
                name: Name::EMPTY,
            },
            id::PLAYER_LEFT => Self::PlayerLeft { player_id: 0 },
            id::DAMAGE => Self::Damage {
                victim: 0,
                attacker: None,
                amount: 0,
                zone: HitZone::HEAD,
                health_after: 0,
            },
            id::DEATH => Self::Death {
                victim: 0,
                killer: None,
                weapon_key: 0,
            },
            id::RESPAWN => Self::Respawn {
                player_id: 0,
                position: [0.0; 3],
                yaw: 0.0,
            },
            id::SHOT => Self::Shot {
                shooter: 0,
                origin: [0.0; 3],
                direction: [0.0; 3],
                weapon_key: 0,
                shot_number: 0,
            },
            id::IMPACT => Self::Impact {
                point: [0.0; 3],
                normal: [0.0; 3],
            },
            id::WEAPON_DROPPED => Self::WeaponDropped(DroppedWeapon::default()),
            id::DROPPED_TAKEN => Self::DroppedTaken {
                dropped_id: 0,
                player_id: 0,
            },
            id::PICKUP_TAKEN => Self::PickupTaken {
                instance_id: 0,
                player_id: 0,
            },
            id::WORLD_SYNC_PICKUPS_TAKEN => Self::WorldSyncPickupsTaken {
                count: 0,
                instance_ids: [0; SYNC_PICKUPS_PER_MESSAGE],
            },
            id::WORLD_SYNC_DROPPED_WEAPON => Self::WorldSyncDroppedWeapon(DroppedWeapon::default()),
            id::WORLD_SYNC_DONE => Self::WorldSyncDone,
            id::DROPPED_REMOVED => Self::DroppedRemoved { dropped_id: 0 },
            _ => return Err(Error::UnknownMessage),
        })
    }

    /// Serialises the body (everything after the 8-bit type id).
    ///
    /// # Errors
    /// Stream errors; [`Error::OutOfRange`] for fields outside their range.
    #[allow(clippy::too_many_lines)]
    pub fn serialize_body<S: Stream>(&mut self, s: &mut S) -> Result<(), Error> {
        match self {
            Self::ClientHello {
                protocol_version,
                content_hash,
                name,
            } => {
                s.u16(protocol_version)?;
                s.u64(content_hash)?;
                s.string(name)
            }
            Self::ServerWelcome {
                player_id: id,
                tick_rate,
                server_tick,
                max_players,
            } => {
                player_id(s, id)?;
                let mut rate = u32::from(*tick_rate);
                s.bounded_u32(&mut rate, 1, u32::from(u16::MAX))?;
                *tick_rate = u16::try_from(rate).map_err(|_| Error::OutOfRange)?;
                s.u32(server_tick)?;
                let mut cap = u32::from(*max_players);
                s.bounded_u32(&mut cap, 1, MAX_PLAYERS as u32)?;
                *max_players = u8::try_from(cap).map_err(|_| Error::OutOfRange)?;
                Ok(())
            }
            Self::ServerReject {
                reason,
                server_protocol_version,
            } => {
                s.u8(&mut reason.0)?;
                s.u16(server_protocol_version)
            }
            Self::PlayerJoined {
                player_id: id,
                name,
            } => {
                player_id(s, id)?;
                s.string(name)
            }
            Self::PlayerLeft { player_id: id } => player_id(s, id),
            Self::Damage {
                victim,
                attacker,
                amount,
                zone,
                health_after,
            } => {
                player_id(s, victim)?;
                optional_player_id(s, attacker)?;
                s.i16(amount)?;
                s.bounded_u8(&mut zone.0, HitZone::MAX)?;
                s.i16(health_after)
            }
            Self::Death {
                victim,
                killer,
                weapon_key,
            } => {
                player_id(s, victim)?;
                optional_player_id(s, killer)?;
                s.u32(weapon_key)
            }
            Self::Respawn {
                player_id: id,
                position,
                yaw,
            } => {
                player_id(s, id)?;
                s.vec3(position)?;
                s.f32(yaw)
            }
            Self::Shot {
                shooter,
                origin,
                direction,
                weapon_key,
                shot_number,
            } => {
                player_id(s, shooter)?;
                s.vec3(origin)?;
                s.vec3(direction)?;
                s.u32(weapon_key)?;
                s.u32(shot_number)
            }
            Self::Impact { point, normal } => {
                s.vec3(point)?;
                s.vec3(normal)
            }
            Self::WeaponDropped(w) | Self::WorldSyncDroppedWeapon(w) => dropped_weapon(s, w),
            Self::DroppedTaken {
                dropped_id,
                player_id: id,
            } => {
                s.u32(dropped_id)?;
                player_id(s, id)
            }
            Self::PickupTaken {
                instance_id,
                player_id: id,
            } => {
                s.u32(instance_id)?;
                player_id(s, id)
            }
            Self::WorldSyncPickupsTaken {
                count,
                instance_ids,
            } => {
                s.bounded_u8(count, SYNC_PICKUPS_PER_MESSAGE as u8)?;
                let used = usize::from(*count);
                if S::WRITING && instance_ids[used..].iter().any(|&v| v != 0) {
                    return Err(Error::NonCanonical);
                }
                for value in &mut instance_ids[..used] {
                    s.u32(value)?;
                }
                if !S::WRITING {
                    instance_ids[used..].fill(0);
                }
                Ok(())
            }
            Self::WorldSyncDone => Ok(()),
            Self::DroppedRemoved { dropped_id } => s.u32(dropped_id),
        }
    }

    /// Serialises type id + body.
    ///
    /// # Errors
    /// Stream errors, [`Error::UnknownMessage`] when reading a reserved id.
    pub fn serialize<S: Stream>(&mut self, s: &mut S) -> Result<(), Error> {
        let mut type_id = self.type_id();
        s.u8(&mut type_id)?;
        if !S::WRITING {
            *self = Self::blank(type_id)?;
        }
        self.serialize_body(s)
    }

    /// Bits of type id + body.
    ///
    /// # Errors
    /// [`Error::OutOfRange`] if a field is outside its range.
    pub fn bit_len(&self) -> Result<usize, Error> {
        let mut m = Measure::new();
        let mut copy = *self;
        copy.serialize(&mut m)?;
        Ok(m.bits())
    }

    /// Encodes type id + body alone, zero padded to a byte (used by the
    /// golden vectors). Returns the number of bytes written.
    ///
    /// # Errors
    /// Write errors.
    pub fn encode_standalone(&self, buf: &mut [u8]) -> Result<usize, Error> {
        let mut w = BitWriter::new(buf);
        let mut copy = *self;
        copy.serialize(&mut w)?;
        w.finish()
    }

    /// Decodes what [`Message::encode_standalone`] wrote.
    ///
    /// # Errors
    /// Any decode error, including [`Error::TrailingData`].
    pub fn decode_standalone(payload: &[u8]) -> Result<Self, Error> {
        let mut r = BitReader::new(payload);
        let mut message = Self::default();
        message.serialize(&mut r)?;
        r.finish()?;
        Ok(message)
    }
}
