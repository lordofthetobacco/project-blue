//! Quantisation of floats to integers and back.
//!
//! All arithmetic is done in IEEE-754 binary64 with the exact operation
//! order written here (no fused multiply-add), so a C++ implementation that
//! follows `docs/netcode/protocol.md` produces identical integers. Rounding is
//! half away from zero (`f64::round`, C++ `std::round`). Quantisers are total:
//! NaN maps to 0 and out-of-range values clamp.

use core::f64::consts::{FRAC_1_SQRT_2, SQRT_2, TAU};

/// Positions: 1/512 m per unit.
pub const POSITION_SCALE: f64 = 512.0;
/// Smallest quantised position (-16384 m).
pub const POSITION_MIN: i32 = -(1 << 23);
/// Largest quantised position (just under +16384 m).
pub const POSITION_MAX: i32 = (1 << 23) - 1;

/// Linear and angular velocities: 1/64 m/s (rad/s) per unit.
pub const VELOCITY_SCALE: f64 = 64.0;
/// Smallest quantised velocity (-128 m/s).
pub const VELOCITY_MIN: i32 = -(1 << 13);
/// Largest quantised velocity (just under +128 m/s).
pub const VELOCITY_MAX: i32 = (1 << 13) - 1;

/// Bits of a fine angle (commands): a full turn is `1 << 20` units.
pub const FINE_ANGLE_BITS: u32 = 20;
/// Bits of a coarse angle (remote players): a full turn is `1 << 16` units.
pub const COARSE_ANGLE_BITS: u32 = 16;

/// Bits per component of a smallest-three quaternion.
pub const QUAT_COMPONENT_BITS: u32 = 10;
/// Steps per unit of a smallest-three component: a component `c` in
/// `[-1/sqrt(2), 1/sqrt(2)]` is sent as `round(c * sqrt(2) * 511) + 511`, so
/// 0 is exact (identity rotations stay exact).
pub const QUAT_COMPONENT_SCALE: u32 = 511;
/// Largest valid quantised smallest-three component (1023 is invalid).
pub const QUAT_COMPONENT_MAX: u32 = 2 * QUAT_COMPONENT_SCALE;

/// `round(x * scale)` clamped to `min..=max`; NaN gives 0.
#[must_use]
pub fn quantise_fixed(x: f32, scale: f64, min: i32, max: i32) -> i32 {
    if x.is_nan() {
        return 0;
    }
    let q = (f64::from(x) * scale).round();
    if q <= f64::from(min) {
        min
    } else if q >= f64::from(max) {
        max
    } else {
        q as i32
    }
}

/// `q / scale` rounded to binary32.
#[must_use]
pub fn dequantise_fixed(q: i32, scale: f64) -> f32 {
    (f64::from(q) / scale) as f32
}

/// Quantises a position component (1/512 m).
#[must_use]
pub fn quantise_position(x: f32) -> i32 {
    quantise_fixed(x, POSITION_SCALE, POSITION_MIN, POSITION_MAX)
}

/// Quantises a velocity component (1/64 m/s or rad/s).
#[must_use]
pub fn quantise_velocity(x: f32) -> i32 {
    quantise_fixed(x, VELOCITY_SCALE, VELOCITY_MIN, VELOCITY_MAX)
}

fn units_per_radian(bits: u32) -> f64 {
    f64::from(1u32 << bits) / TAU
}

fn radians_per_unit(bits: u32) -> f64 {
    TAU / f64::from(1u32 << bits)
}

/// Yaw to `0..(1 << bits)` units of a full turn, wrapping. NaN and
/// infinities give 0.
#[must_use]
pub fn quantise_yaw(yaw: f32, bits: u32) -> u32 {
    if !yaw.is_finite() {
        return 0;
    }
    let turn = f64::from(1u32 << bits);
    let mut q = (f64::from(yaw) * units_per_radian(bits)).round() % turn;
    if q < 0.0 {
        q += turn;
    }
    // q is an integer in [0, turn); turn <= 2^20 so the cast is exact.
    q as u32
}

/// Inverse of [`quantise_yaw`]: radians in `[0, 2*pi)`.
#[must_use]
pub fn dequantise_yaw(q: u32, bits: u32) -> f32 {
    (f64::from(q) * radians_per_unit(bits)) as f32
}

/// Largest quantised pitch magnitude: a quarter turn.
#[must_use]
pub const fn pitch_limit(bits: u32) -> i32 {
    1 << (bits - 2)
}

/// Pitch to units of `1 << bits` per turn, clamped to a quarter turn either
/// way. NaN gives 0.
#[must_use]
pub fn quantise_pitch(pitch: f32, bits: u32) -> i32 {
    let limit = pitch_limit(bits);
    quantise_fixed(pitch, units_per_radian(bits), -limit, limit)
}

/// Inverse of [`quantise_pitch`].
#[must_use]
pub fn dequantise_pitch(q: i32, bits: u32) -> f32 {
    (f64::from(q) * radians_per_unit(bits)) as f32
}

/// A smallest-three quaternion: index of the dropped (largest) component and
/// the other three in index order, each in `0..=QUAT_COMPONENT_MAX`.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct QuatQ {
    /// Index (0..=3, order x, y, z, w) of the component that is not sent.
    pub largest: u8,
    /// The remaining three components, quantised.
    pub components: [u32; 3],
}

/// Quantises a rotation `[x, y, z, w]`. The input is normalised first; a
/// zero-length or non-finite input becomes the identity.
#[must_use]
pub fn quantise_quat(q: [f32; 4]) -> QuatQ {
    let mut v = q.map(f64::from);
    let norm = (v[0] * v[0] + v[1] * v[1] + v[2] * v[2] + v[3] * v[3]).sqrt();
    if !norm.is_finite() || norm < 1e-6 {
        v = [0.0, 0.0, 0.0, 1.0];
    } else {
        v = v.map(|c| c / norm);
    }
    let mut largest = 0;
    for i in 1..4 {
        if v[i].abs() > v[largest].abs() {
            largest = i;
        }
    }
    if v[largest] < 0.0 {
        v = v.map(|c| -c);
    }
    let mut out = QuatQ {
        largest: largest as u8,
        components: [0; 3],
    };
    let scale = f64::from(QUAT_COMPONENT_SCALE);
    let mut slot = 0;
    for (i, c) in v.iter().enumerate() {
        if i == largest {
            continue;
        }
        let scaled = (c * SQRT_2 * scale).round().clamp(-scale, scale);
        out.components[slot] = (scaled + scale) as u32;
        slot += 1;
    }
    out
}

/// Inverse of [`quantise_quat`]. Out-of-range input fields are clamped.
#[must_use]
pub fn dequantise_quat(q: QuatQ) -> [f32; 4] {
    let scale = f64::from(QUAT_COMPONENT_SCALE);
    let largest = usize::from(q.largest.min(3));
    let mut v = [0.0f64; 4];
    let mut sum = 0.0;
    let mut slot = 0;
    for (i, out) in v.iter_mut().enumerate() {
        if i == largest {
            continue;
        }
        let raw = f64::from(q.components[slot].min(QUAT_COMPONENT_MAX));
        let c = (raw - scale) / scale * FRAC_1_SQRT_2;
        *out = c;
        sum += c * c;
        slot += 1;
    }
    v[largest] = (1.0 - sum).max(0.0).sqrt();
    v.map(|c| c as f32)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn yaw_wraps() {
        assert_eq!(quantise_yaw(0.0, 20), 0);
        assert_eq!(quantise_yaw(core::f32::consts::TAU, 20), 0);
        assert_eq!(quantise_yaw(-core::f32::consts::FRAC_PI_2, 20), 3 << 18);
        assert_eq!(quantise_yaw(f32::NAN, 20), 0);
    }

    #[test]
    fn pitch_clamps() {
        assert_eq!(quantise_pitch(10.0, 20), 1 << 18);
        assert_eq!(quantise_pitch(-10.0, 20), -(1 << 18));
    }

    #[test]
    fn quat_identity() {
        let q = quantise_quat([0.0, 0.0, 0.0, 1.0]);
        assert_eq!(q.largest, 3);
        assert_eq!(dequantise_quat(q)[3], 1.0);
    }
}
