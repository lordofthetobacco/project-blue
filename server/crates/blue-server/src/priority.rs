//! World-object priority accumulator (per client).
//!
//! Every snapshot each object's accumulator grows by its weight (awake and
//! near objects weigh more); objects are offered to the packet in
//! descending accumulator order and an object that made it in resets to
//! zero. An object left out keeps growing until it outranks the others, so
//! nothing starves however small its weight.

use blue_protocol::WorldObject;
use blue_protocol::object::ObjectKind;

/// Most world objects tracked per client.
pub const MAX_OBJECTS: usize = 512;

/// Weight of an object at rest relative to a moving one.
const REST_WEIGHT: f32 = 0.1;
/// Distance (m) at which an object's weight halves.
const HALF_DISTANCE: f32 = 25.0;
/// Smallest weight: bounds how long the least important object waits
/// (about `max weight / MIN_WEIGHT` times as long as the most important).
const MIN_WEIGHT: f32 = 0.02;

/// Stable key of an object across ticks.
#[must_use]
pub fn object_key(object: &WorldObject) -> u64 {
    let kind = match object.kind {
        ObjectKind::Body => 0u64,
        ObjectKind::DroppedWeapon => 1u64,
    };
    (kind << 32) | u64::from(object.id)
}

/// Priority weight of `object` for a viewer at `viewer`.
#[must_use]
pub fn weight(object: &WorldObject, viewer: Option<[f32; 3]>) -> f32 {
    let base = if object.at_rest { REST_WEIGHT } else { 1.0 };
    let Some(v) = viewer else {
        return base.max(MIN_WEIGHT);
    };
    let d = [
        object.position[0] - v[0],
        object.position[1] - v[1],
        object.position[2] - v[2],
    ];
    let distance = (d[0] * d[0] + d[1] * d[1] + d[2] * d[2]).sqrt();
    let scaled = if distance.is_finite() {
        base * HALF_DISTANCE / (HALF_DISTANCE + distance)
    } else {
        0.0
    };
    scaled.max(MIN_WEIGHT)
}

/// Accumulators of one client, indexed like the server's object read-out.
#[derive(Clone, Debug)]
pub struct PriorityAccumulator {
    keys: [u64; MAX_OBJECTS],
    values: [f32; MAX_OBJECTS],
}

impl Default for PriorityAccumulator {
    fn default() -> Self {
        Self {
            keys: [u64::MAX; MAX_OBJECTS],
            values: [0.0; MAX_OBJECTS],
        }
    }
}

impl PriorityAccumulator {
    /// Forgets everything.
    pub fn reset(&mut self) {
        self.keys = [u64::MAX; MAX_OBJECTS];
        self.values = [0.0; MAX_OBJECTS];
    }

    /// Adds this snapshot's weights and writes the object indices into
    /// `order`, highest priority first. `objects` and `order` are truncated
    /// to [`MAX_OBJECTS`]. Returns the number of indices written.
    pub fn rank(
        &mut self,
        objects: &[WorldObject],
        viewer: Option<[f32; 3]>,
        order: &mut [u16],
    ) -> usize {
        let n = objects.len().min(MAX_OBJECTS).min(order.len());
        for (i, object) in objects.iter().take(n).enumerate() {
            let key = object_key(object);
            if self.keys[i] != key {
                // The read-out changed shape (an object appeared or went):
                // restart this slot.
                self.keys[i] = key;
                self.values[i] = 0.0;
            }
            self.values[i] += weight(object, viewer);
            order[i] = i as u16;
        }
        let values = &self.values;
        // sort_unstable_by does not allocate.
        order[..n]
            .sort_unstable_by(|&a, &b| values[usize::from(b)].total_cmp(&values[usize::from(a)]));
        n
    }

    /// Marks object `index` as sent.
    pub fn sent(&mut self, index: u16) {
        if let Some(v) = self.values.get_mut(usize::from(index)) {
            *v = 0.0;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn object(id: u32, x: f32, at_rest: bool) -> WorldObject {
        WorldObject {
            id,
            at_rest,
            position: [x, 0.0, 0.0],
            rotation: [0.0, 0.0, 0.0, 1.0],
            ..WorldObject::default()
        }
    }

    #[test]
    fn near_and_awake_first() {
        let objects = [
            object(1, 100.0, false),
            object(2, 1.0, true),
            object(3, 1.0, false),
        ];
        let mut acc = PriorityAccumulator::default();
        let mut order = [0u16; 8];
        let n = acc.rank(&objects, Some([0.0; 3]), &mut order);
        assert_eq!(n, 3);
        assert_eq!(order[0], 2);
        assert_eq!(order[1], 0);
        assert_eq!(order[2], 1);
    }

    #[test]
    fn no_starvation() {
        // 20 objects, one fits per snapshot; the far, resting one is sent
        // too within a bounded number of rounds.
        let mut objects = [object(0, 0.0, false); 20];
        for (i, o) in objects.iter_mut().enumerate() {
            *o = object(i as u32, i as f32, false);
        }
        objects[19] = object(19, 1000.0, true);
        let mut acc = PriorityAccumulator::default();
        let mut order = [0u16; 20];
        let mut last_sent = [0u32; 20];
        for round in 1..=4000u32 {
            acc.rank(&objects, Some([0.0; 3]), &mut order);
            let pick = order[0];
            acc.sent(pick);
            last_sent[usize::from(pick)] = round;
        }
        assert!(last_sent.iter().all(|&r| r > 2000), "{last_sent:?}");
    }

    #[test]
    fn shape_change_resets_slot() {
        let mut acc = PriorityAccumulator::default();
        let mut order = [0u16; 2];
        acc.rank(&[object(1, 0.0, false)], None, &mut order);
        acc.rank(&[object(1, 0.0, false)], None, &mut order);
        assert!((acc.values[0] - 2.0).abs() < 1e-6);
        acc.rank(&[object(2, 0.0, false)], None, &mut order);
        assert!((acc.values[0] - 1.0).abs() < 1e-6);
        let mut dropped = object(2, 0.0, false);
        dropped.kind = ObjectKind::DroppedWeapon;
        assert_ne!(object_key(&dropped), object_key(&object(2, 0.0, false)));
    }
}
