//! Lag compensation: a ring of every player's capsule over the last 64
//! ticks, rewinding to a shooter's view time, and segment-vs-capsule
//! intersection.
//!
//! Capsules are vertical (Y up): the axis runs from `feet + radius` to
//! `feet + height - radius`. Projectile segments are tested against the
//! capsules as the shooter saw them: at `server tick - view delay`, linearly
//! interpolated between the two recorded ticks around that time.

use blue_protocol::message::MAX_PLAYERS;

use crate::sim::{Capsule, PlayerHit, Segment};

/// Ticks of history kept.
pub const HISTORY_TICKS: usize = 64;

type Vec3 = [f32; 3];

fn sub(a: Vec3, b: Vec3) -> Vec3 {
    [a[0] - b[0], a[1] - b[1], a[2] - b[2]]
}
fn dot(a: Vec3, b: Vec3) -> f32 {
    a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
}
fn add_scaled(a: Vec3, d: Vec3, t: f32) -> Vec3 {
    [a[0] + d[0] * t, a[1] + d[1] * t, a[2] + d[2] * t]
}
fn lerp(a: f32, b: f32, t: f32) -> f32 {
    a + (b - a) * t
}

/// Axis end points of a capsule (bottom and top sphere centres).
fn axis(c: &Capsule) -> (Vec3, Vec3, f32) {
    let r = c.radius.max(0.0);
    let half = (c.height * 0.5).max(0.0);
    let r_eff = r.min(half);
    let a = [c.feet[0], c.feet[1] + r_eff, c.feet[2]];
    let b = [
        c.feet[0],
        c.feet[1] + (c.height - r_eff).max(r_eff),
        c.feet[2],
    ];
    (a, b, r)
}

/// Squared distance from `p` to the segment `a`-`b`.
fn point_segment_dist2(p: Vec3, a: Vec3, b: Vec3) -> f32 {
    let ab = sub(b, a);
    let len2 = dot(ab, ab);
    let t = if len2 > 0.0 {
        (dot(sub(p, a), ab) / len2).clamp(0.0, 1.0)
    } else {
        0.0
    };
    let q = add_scaled(a, ab, t);
    let d = sub(p, q);
    dot(d, d)
}

/// Smallest `t >= 0` where the ray `o + t*d` (`d` unit length) enters the
/// sphere, if any.
fn ray_sphere(o: Vec3, d: Vec3, centre: Vec3, r: f32) -> Option<f32> {
    let oc = sub(o, centre);
    let b = dot(oc, d);
    let c = dot(oc, oc) - r * r;
    let h = b * b - c;
    if h < 0.0 {
        return None;
    }
    let t = -b - h.sqrt();
    (t >= 0.0).then_some(t)
}

/// Smallest `t >= 0` where the ray enters the finite cylinder body (side
/// surface only; the end discs lie inside the cap spheres).
fn ray_cylinder(o: Vec3, d: Vec3, a: Vec3, b: Vec3, r: f32) -> Option<f32> {
    let ba = sub(b, a);
    let oa = sub(o, a);
    let baba = dot(ba, ba);
    if baba <= 0.0 {
        return None;
    }
    let bard = dot(ba, d);
    let baoa = dot(ba, oa);
    let rdoa = dot(d, oa);
    let oaoa = dot(oa, oa);
    let qa = baba - bard * bard;
    // Parallel to the axis (qa ~ 0): only the caps can be entered first.
    if qa <= 1e-9 * baba {
        return None;
    }
    let qb = baba * rdoa - baoa * bard;
    let qc = baba * oaoa - baoa * baoa - r * r * baba;
    let h = qb * qb - qa * qc;
    if h < 0.0 {
        return None;
    }
    let t = (-qb - h.sqrt()) / qa;
    let y = baoa + t * bard;
    (t >= 0.0 && y >= 0.0 && y <= baba).then_some(t)
}

/// Fraction `0..=1` along `from`-`to` where the segment first touches the
/// capsule, or `None`. A segment starting inside hits at 0; a zero-length
/// segment hits at 0 if its point is inside.
#[must_use]
pub fn segment_capsule(from: Vec3, to: Vec3, capsule: &Capsule) -> Option<f32> {
    let (a, b, r) = axis(capsule);
    if r.is_nan() || r <= 0.0 || !from.iter().chain(to.iter()).all(|v| v.is_finite()) {
        return None;
    }
    if point_segment_dist2(from, a, b) <= r * r {
        return Some(0.0);
    }
    let delta = sub(to, from);
    let len = dot(delta, delta).sqrt();
    if len <= 1e-9 {
        return None;
    }
    let d = [delta[0] / len, delta[1] / len, delta[2] / len];
    let candidates = [
        ray_cylinder(from, d, a, b, r),
        ray_sphere(from, d, a, r),
        ray_sphere(from, d, b, r),
    ];
    let t = candidates
        .into_iter()
        .flatten()
        .fold(f32::INFINITY, f32::min);
    (t <= len).then(|| (t / len).clamp(0.0, 1.0))
}

/// Rewind amount in ticks for a client: `server_tick - (view_tick +
/// view_fraction / 256)`, clamped to `0..=max_ticks`.
#[must_use]
pub fn rewind_ticks(server_tick: u32, view_tick: u32, view_fraction: u8, max_ticks: f64) -> f64 {
    // Signed difference on the tick circle.
    let whole = server_tick.wrapping_sub(view_tick) as i32;
    let delay = f64::from(whole) - f64::from(view_fraction) / 256.0;
    delay.clamp(0.0, max_ticks.max(0.0))
}

/// Capsules of every player for the last [`HISTORY_TICKS`] ticks.
#[derive(Clone, Debug)]
pub struct CapsuleHistory {
    ticks: [Option<u32>; HISTORY_TICKS],
    capsules: [[Option<Capsule>; MAX_PLAYERS]; HISTORY_TICKS],
}

impl Default for CapsuleHistory {
    fn default() -> Self {
        Self {
            ticks: [None; HISTORY_TICKS],
            capsules: [[None; MAX_PLAYERS]; HISTORY_TICKS],
        }
    }
}

impl CapsuleHistory {
    /// Records the capsules at the end of `tick`; `capsule(id)` is called
    /// for every player id.
    pub fn record(&mut self, tick: u32, mut capsule: impl FnMut(u8) -> Option<Capsule>) {
        let slot = tick as usize % HISTORY_TICKS;
        self.ticks[slot] = Some(tick);
        for (id, entry) in self.capsules[slot].iter_mut().enumerate() {
            *entry = capsule(id as u8);
        }
    }

    fn at_tick(&self, tick: u32, player: u8) -> Option<Capsule> {
        let slot = tick as usize % HISTORY_TICKS;
        if self.ticks[slot] != Some(tick) {
            return None;
        }
        self.capsules[slot]
            .get(usize::from(player))
            .copied()
            .flatten()
    }

    /// Player `player`'s capsule `rewind` ticks before `now` (fractional:
    /// interpolated between the recorded ticks around it). Falls back to the
    /// nearer recorded tick if only one exists; `None` if neither does.
    #[must_use]
    pub fn rewound(&self, now: u32, rewind: f64, player: u8) -> Option<Capsule> {
        let rewind = rewind.clamp(0.0, (HISTORY_TICKS - 1) as f64);
        let back = rewind.ceil();
        let older_tick = now.wrapping_sub(back as u32);
        let newer_tick = older_tick.wrapping_add(1);
        // Weight of the newer sample.
        let t = if back > 0.0 {
            (back - rewind) as f32
        } else {
            0.0
        };
        let older = self.at_tick(older_tick, player);
        let newer = if back > 0.0 {
            self.at_tick(newer_tick, player)
        } else {
            None
        };
        match (older, newer) {
            (Some(o), Some(n)) => Some(Capsule {
                feet: [
                    lerp(o.feet[0], n.feet[0], t),
                    lerp(o.feet[1], n.feet[1], t),
                    lerp(o.feet[2], n.feet[2], t),
                ],
                height: lerp(o.height, n.height, t),
                radius: lerp(o.radius, n.radius, t),
                // Hittable only if alive at the nearer sample.
                alive: if t >= 0.5 { n.alive } else { o.alive },
            }),
            (Some(c), None) | (None, Some(c)) => Some(c),
            (None, None) => None,
        }
    }
}

/// Tests every segment against the capsules of all players except its
/// shooter, rewound by `rewind_of(shooter)` ticks from `now`, keeping the
/// nearest hit before the segment's world hit. Writes hits into `out` and
/// returns their count.
pub fn find_hits(
    history: &CapsuleHistory,
    now: u32,
    segments: &[Segment],
    mut rewind_of: impl FnMut(u8) -> f64,
    out: &mut [PlayerHit],
) -> usize {
    let mut count = 0;
    for (index, segment) in segments.iter().enumerate() {
        if count >= out.len() {
            break;
        }
        let rewind = rewind_of(segment.shooter);
        let limit = segment.world_fraction.clamp(0.0, 1.0);
        let mut best: Option<PlayerHit> = None;
        for player in 0..MAX_PLAYERS as u8 {
            if player == segment.shooter {
                continue;
            }
            let Some(capsule) = history.rewound(now, rewind, player) else {
                continue;
            };
            if !capsule.alive {
                continue;
            }
            let Some(fraction) = segment_capsule(segment.from, segment.to, &capsule) else {
                continue;
            };
            if fraction > limit || best.is_some_and(|b| b.fraction <= fraction) {
                continue;
            }
            let point = add_scaled(segment.from, sub(segment.to, segment.from), fraction);
            let height_ratio = if capsule.height > 0.0 {
                ((point[1] - capsule.feet[1]) / capsule.height).clamp(0.0, 1.0)
            } else {
                0.0
            };
            best = Some(PlayerHit {
                segment: index as u32,
                projectile: segment.projectile,
                victim: player,
                fraction,
                point,
                height_ratio,
            });
        }
        if let Some(hit) = best {
            out[count] = hit;
            count += 1;
        }
    }
    count
}

#[cfg(test)]
mod tests {
    #![allow(clippy::unwrap_used)]

    use super::*;

    const C: Capsule = Capsule {
        feet: [0.0, 0.0, 0.0],
        height: 1.8,
        radius: 0.4,
        alive: true,
    };

    fn near(a: f32, b: f32) -> bool {
        (a - b).abs() < 1e-4
    }

    #[test]
    fn straight_through_the_body() {
        // From x=-10 to x=10 at chest height: enters at x=-0.4 -> 9.6/20.
        let f = segment_capsule([-10.0, 1.0, 0.0], [10.0, 1.0, 0.0], &C).unwrap();
        assert!(near(f, 0.48), "{f}");
    }

    #[test]
    fn graze() {
        // Tangent lines at distance r - 1 mm hit, r + 1 mm miss.
        assert!(segment_capsule([-10.0, 1.0, 0.399], [10.0, 1.0, 0.399], &C).is_some());
        assert!(segment_capsule([-10.0, 1.0, 0.401], [10.0, 1.0, 0.401], &C).is_none());
    }

    #[test]
    fn end_caps() {
        // Straight down onto the head: top of the capsule at y=1.8.
        let f = segment_capsule([0.0, 5.0, 0.0], [0.0, -5.0, 0.0], &C).unwrap();
        assert!(near(f, (5.0 - 1.8) / 10.0), "{f}");
        // Straight up into the feet: bottom at y=0.
        let f = segment_capsule([0.0, -5.0, 0.0], [0.0, 5.0, 0.0], &C).unwrap();
        assert!(near(f, 0.5), "{f}");
        // Diagonal clip of the top cap only (above the cylinder body).
        let f = segment_capsule([-5.0, 1.65, 0.0], [5.0, 1.65, 0.0], &C).unwrap();
        // Sphere centre y=1.4, r=0.4: half chord = sqrt(0.16 - 0.0625).
        let half = (0.16f32 - 0.0625).sqrt();
        assert!(near(f, (5.0 - half) / 10.0), "{f}");
        // Just over the head misses.
        assert!(segment_capsule([-5.0, 1.81, 0.0], [5.0, 1.81, 0.0], &C).is_none());
    }

    #[test]
    fn inside_start() {
        assert_eq!(
            segment_capsule([0.1, 1.0, 0.0], [10.0, 1.0, 0.0], &C),
            Some(0.0)
        );
        // Inside a cap sphere.
        assert_eq!(
            segment_capsule([0.0, 1.7, 0.0], [0.0, 10.0, 0.0], &C),
            Some(0.0)
        );
    }

    #[test]
    fn parallel_to_axis() {
        // Vertical line beside the capsule never touches it.
        assert!(segment_capsule([1.0, -5.0, 0.0], [1.0, 5.0, 0.0], &C).is_none());
        // Vertical line through the side volume enters the bottom cap.
        let f = segment_capsule([0.3, -5.0, 0.0], [0.3, 5.0, 0.0], &C).unwrap();
        let y = 0.4 - (0.16f32 - 0.09).sqrt();
        assert!(near(f, (5.0 + y) / 10.0), "{f}");
    }

    #[test]
    fn zero_length() {
        assert_eq!(
            segment_capsule([0.0, 1.0, 0.0], [0.0, 1.0, 0.0], &C),
            Some(0.0)
        );
        assert_eq!(segment_capsule([2.0, 1.0, 0.0], [2.0, 1.0, 0.0], &C), None);
    }

    #[test]
    fn segment_ends_short_or_points_away() {
        assert!(segment_capsule([-10.0, 1.0, 0.0], [-1.0, 1.0, 0.0], &C).is_none());
        assert!(segment_capsule([-1.0, 1.0, 0.0], [-10.0, 1.0, 0.0], &C).is_none());
        // Ends exactly at the surface: a hit at fraction 1.
        let f = segment_capsule([-10.0, 1.0, 0.0], [-0.4, 1.0, 0.0], &C).unwrap();
        assert!(near(f, 1.0), "{f}");
    }

    #[test]
    fn degenerate_inputs() {
        let mut c = C;
        c.radius = 0.0;
        assert!(segment_capsule([-1.0, 1.0, 0.0], [1.0, 1.0, 0.0], &c).is_none());
        assert!(segment_capsule([f32::NAN, 1.0, 0.0], [1.0, 1.0, 0.0], &C).is_none());
        // Height below 2r: a sphere.
        c = C;
        c.height = 0.5;
        let f = segment_capsule([-10.0, 0.25, 0.0], [10.0, 0.25, 0.0], &c).unwrap();
        assert!(near(f, 0.48), "{f}");
    }

    #[test]
    fn rewind_amounts() {
        assert!(near(rewind_ticks(100, 94, 128, 12.0) as f32, 5.5));
        assert!(near(rewind_ticks(100, 50, 0, 12.0) as f32, 12.0));
        // View ahead of the server (bogus) clamps to zero.
        assert!(near(rewind_ticks(100, 105, 0, 12.0) as f32, 0.0));
        assert!(near(rewind_ticks(2, u32::MAX - 1, 0, 12.0) as f32, 4.0));
    }

    fn moving_history() -> CapsuleHistory {
        let mut h = CapsuleHistory::default();
        // Player 1 walks +x one metre per tick; player 0 stands at z=-5.
        for tick in 0..100u32 {
            h.record(tick, |id| match id {
                0 => Some(Capsule {
                    feet: [0.0, 0.0, -5.0],
                    ..C
                }),
                1 => Some(Capsule {
                    feet: [tick as f32, 0.0, 0.0],
                    ..C
                }),
                _ => None,
            });
        }
        h
    }

    #[test]
    fn rewinding_interpolates() {
        let h = moving_history();
        let c = h.rewound(99, 2.25, 1).unwrap();
        assert!(near(c.feet[0], 96.75), "{}", c.feet[0]);
        let c = h.rewound(99, 0.0, 1).unwrap();
        assert!(near(c.feet[0], 99.0));
        // Older than the history: clamped to the oldest kept tick.
        let c = h.rewound(99, 500.0, 1).unwrap();
        assert!(near(c.feet[0], 36.0), "{}", c.feet[0]);
        assert!(h.rewound(99, 0.0, 7).is_none());
    }

    #[test]
    fn hits_use_the_shooters_view() {
        let h = moving_history();
        // Player 0 shoots along +z at x=95: the target is there 4 ticks ago.
        let segments = [Segment {
            projectile: 7,
            shooter: 0,
            from: [95.0, 1.0, -4.0],
            to: [95.0, 1.0, 4.0],
            world_fraction: 1.0,
        }];
        let mut out = [PlayerHit::default(); 4];
        assert_eq!(find_hits(&h, 99, &segments, |_| 4.0, &mut out), 1);
        assert_eq!(out[0].victim, 1);
        assert_eq!(out[0].projectile, 7);
        assert!(near(out[0].fraction, 3.6 / 8.0));
        assert!(near(out[0].height_ratio, 1.0 / 1.8));
        // Without rewind the target is at x=99: a miss.
        assert_eq!(find_hits(&h, 99, &segments, |_| 0.0, &mut out), 0);
        // A wall before the target stops it.
        let mut blocked = segments;
        blocked[0].world_fraction = 0.3;
        assert_eq!(find_hits(&h, 99, &blocked, |_| 4.0, &mut out), 0);
    }

    #[test]
    fn shooter_and_dead_players_are_skipped() {
        let mut h = CapsuleHistory::default();
        h.record(10, |id| match id {
            0 => Some(C),
            1 => Some(Capsule {
                feet: [0.0, 0.0, 3.0],
                alive: false,
                ..C
            }),
            2 => Some(Capsule {
                feet: [0.0, 0.0, 6.0],
                ..C
            }),
            _ => None,
        });
        let segments = [Segment {
            projectile: 1,
            shooter: 0,
            from: [0.0, 1.0, 0.0],
            to: [0.0, 1.0, 10.0],
            world_fraction: 1.0,
        }];
        let mut out = [PlayerHit::default(); 1];
        assert_eq!(find_hits(&h, 10, &segments, |_| 0.0, &mut out), 1);
        assert_eq!(out[0].victim, 2);
        // Output capacity is respected.
        assert_eq!(find_hits(&h, 10, &segments, |_| 0.0, &mut []), 0);
    }
}
