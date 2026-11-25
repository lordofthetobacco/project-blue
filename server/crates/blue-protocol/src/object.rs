//! World objects (dynamic rigid bodies and dropped weapons), sent as absolute
//! quantised state.

use crate::bits::Stream;
use crate::error::Error;
use crate::quant::{
    POSITION_MAX, POSITION_MIN, POSITION_SCALE, QUAT_COMPONENT_MAX, QuatQ, VELOCITY_MAX,
    VELOCITY_MIN, VELOCITY_SCALE, dequantise_fixed, dequantise_quat, quantise_position,
    quantise_quat, quantise_velocity,
};

/// Which id space a world object's id belongs to.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub enum ObjectKind {
    /// A dynamic rigid body keyed by its scene `instance_id`.
    #[default]
    Body,
    /// A dropped weapon keyed by its `dropped_id`.
    DroppedWeapon,
}

/// State of one world object.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct WorldObject {
    /// Id space.
    pub kind: ObjectKind,
    /// `instance_id` or `dropped_id`.
    pub id: u32,
    /// At rest: velocities are not sent and decode as zero.
    pub at_rest: bool,
    /// Position, metres.
    pub position: [f32; 3],
    /// Rotation `[x, y, z, w]`.
    pub rotation: [f32; 4],
    /// Linear velocity, m/s.
    pub linear_velocity: [f32; 3],
    /// Angular velocity, rad/s.
    pub angular_velocity: [f32; 3],
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
struct ObjectQ {
    kind: ObjectKind,
    id: u32,
    at_rest: bool,
    position: [i32; 3],
    rotation: QuatQ,
    linear_velocity: [i32; 3],
    angular_velocity: [i32; 3],
}

impl ObjectQ {
    fn from_object(o: &WorldObject) -> Self {
        let (linear_velocity, angular_velocity) = if o.at_rest {
            ([0; 3], [0; 3])
        } else {
            (
                o.linear_velocity.map(quantise_velocity),
                o.angular_velocity.map(quantise_velocity),
            )
        };
        Self {
            kind: o.kind,
            id: o.id,
            at_rest: o.at_rest,
            position: o.position.map(quantise_position),
            rotation: quantise_quat(o.rotation),
            linear_velocity,
            angular_velocity,
        }
    }

    fn to_object(self) -> WorldObject {
        WorldObject {
            kind: self.kind,
            id: self.id,
            at_rest: self.at_rest,
            position: self.position.map(|q| dequantise_fixed(q, POSITION_SCALE)),
            rotation: dequantise_quat(self.rotation),
            linear_velocity: self
                .linear_velocity
                .map(|q| dequantise_fixed(q, VELOCITY_SCALE)),
            angular_velocity: self
                .angular_velocity
                .map(|q| dequantise_fixed(q, VELOCITY_SCALE)),
        }
    }

    fn serialize<S: Stream>(&mut self, s: &mut S) -> Result<(), Error> {
        let mut dropped = self.kind == ObjectKind::DroppedWeapon;
        s.bool(&mut dropped)?;
        self.kind = if dropped {
            ObjectKind::DroppedWeapon
        } else {
            ObjectKind::Body
        };
        s.varuint(&mut self.id)?;
        s.bool(&mut self.at_rest)?;
        for c in &mut self.position {
            s.bounded_i32(c, POSITION_MIN, POSITION_MAX)?;
        }
        let mut largest = u32::from(self.rotation.largest);
        s.serialize_bits(&mut largest, 2)?;
        self.rotation.largest = u8::try_from(largest).map_err(|_| Error::OutOfRange)?;
        for c in &mut self.rotation.components {
            s.bounded_u32(c, 0, QUAT_COMPONENT_MAX)?;
        }
        if self.at_rest {
            self.linear_velocity = [0; 3];
            self.angular_velocity = [0; 3];
            return Ok(());
        }
        for c in &mut self.linear_velocity {
            s.bounded_i32(c, VELOCITY_MIN, VELOCITY_MAX)?;
        }
        for c in &mut self.angular_velocity {
            s.bounded_i32(c, VELOCITY_MIN, VELOCITY_MAX)?;
        }
        Ok(())
    }
}

impl WorldObject {
    /// The object as a receiver will decode it (quantised and back).
    #[must_use]
    pub fn quantised(&self) -> Self {
        ObjectQ::from_object(self).to_object()
    }

    /// Writes the object (quantising it).
    ///
    /// # Errors
    /// Stream errors.
    pub(crate) fn write<S: Stream>(&self, s: &mut S) -> Result<(), Error> {
        ObjectQ::from_object(self).serialize(s)
    }

    /// Reads an object.
    ///
    /// # Errors
    /// Stream errors.
    pub(crate) fn read<S: Stream>(s: &mut S) -> Result<Self, Error> {
        let mut q = ObjectQ::default();
        q.serialize(s)?;
        Ok(q.to_object())
    }
}
