//! Deterministic network conditioner: latency, jitter, loss, duplication
//! and reordering applied to application payloads between a bot and its
//! netcode socket (one instance per direction).
//!
//! netcode's own control traffic (connection handshake, keep-alives) is not
//! conditioned: netcode 1.5 does not expose its network simulator in the
//! public header, so the conditioner sits on the payload path instead.

/// Small, fast, seedable PRNG (`SplitMix64`).
#[derive(Clone, Debug)]
pub struct Rng(u64);

impl Rng {
    /// A generator from `seed`.
    #[must_use]
    pub fn new(seed: u64) -> Self {
        Self(seed)
    }

    /// Next 64 random bits.
    pub fn next_u64(&mut self) -> u64 {
        self.0 = self.0.wrapping_add(0x9E37_79B9_7F4A_7C15);
        let mut z = self.0;
        z = (z ^ (z >> 30)).wrapping_mul(0xBF58_476D_1CE4_E5B9);
        z = (z ^ (z >> 27)).wrapping_mul(0x94D0_49BB_1331_11EB);
        z ^ (z >> 31)
    }

    /// Uniform in `[0, 1)`.
    pub fn unit(&mut self) -> f64 {
        (self.next_u64() >> 11) as f64 / (1u64 << 53) as f64
    }

    /// Uniform in `[lo, hi)`.
    pub fn range(&mut self, lo: f64, hi: f64) -> f64 {
        lo + (hi - lo) * self.unit()
    }

    /// True with probability `p`.
    pub fn chance(&mut self, p: f64) -> bool {
        p > 0.0 && self.unit() < p
    }
}

/// Conditioner parameters for one direction.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Conditions {
    /// Added one-way delay, seconds.
    pub latency: f64,
    /// Delay varies uniformly by up to this much either way, seconds.
    pub jitter: f64,
    /// Probability a packet is dropped.
    pub loss: f64,
    /// Probability a packet is delivered twice.
    pub duplicate: f64,
    /// Probability a packet is held back extra (arrives after later ones).
    pub reorder: f64,
}

const MAX_PAYLOAD: usize = 1200;

#[derive(Clone)]
struct Delayed {
    due: f64,
    order: u64,
    len: usize,
    data: [u8; MAX_PAYLOAD],
}

/// Counters of one conditioner.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ConditionerStats {
    /// Packets offered.
    pub offered: u64,
    /// Packets dropped.
    pub dropped: u64,
    /// Extra copies created.
    pub duplicated: u64,
    /// Packets held back to reorder.
    pub reordered: u64,
}

/// One direction of the conditioner.
pub struct Conditioner {
    conditions: Conditions,
    rng: Rng,
    queue: Vec<Delayed>,
    order: u64,
    /// Counters.
    pub stats: ConditionerStats,
}

impl std::fmt::Debug for Conditioner {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Conditioner")
            .field("conditions", &self.conditions)
            .field("queued", &self.queue.len())
            .field("stats", &self.stats)
            .finish()
    }
}

impl Conditioner {
    /// A conditioner with its own deterministic random stream.
    #[must_use]
    pub fn new(conditions: Conditions, seed: u64) -> Self {
        Self {
            conditions,
            rng: Rng::new(seed),
            queue: Vec::with_capacity(256),
            order: 0,
            stats: ConditionerStats::default(),
        }
    }

    /// Offers a payload sent at `now` (seconds). Payloads over 1200 bytes
    /// are dropped.
    pub fn push(&mut self, now: f64, payload: &[u8]) {
        self.stats.offered += 1;
        if payload.len() > MAX_PAYLOAD || self.rng.chance(self.conditions.loss) {
            self.stats.dropped += 1;
            return;
        }
        let copies = if self.rng.chance(self.conditions.duplicate) {
            self.stats.duplicated += 1;
            2
        } else {
            1
        };
        for _ in 0..copies {
            let c = self.conditions;
            let mut delay = c.latency + self.rng.range(-c.jitter, c.jitter);
            if self.rng.chance(c.reorder) {
                self.stats.reordered += 1;
                delay += self.rng.range(0.005, 0.005 + 2.0 * c.jitter + 0.02);
            }
            let mut data = [0u8; MAX_PAYLOAD];
            data[..payload.len()].copy_from_slice(payload);
            self.queue.push(Delayed {
                due: now + delay.max(0.0),
                order: self.order,
                len: payload.len(),
                data,
            });
            self.order += 1;
        }
    }

    /// Takes the earliest payload due at `now` into `out`; returns its
    /// length.
    pub fn pop(&mut self, now: f64, out: &mut [u8]) -> Option<usize> {
        let (index, _) = self
            .queue
            .iter()
            .enumerate()
            .filter(|(_, d)| d.due <= now)
            .min_by(|(_, a), (_, b)| a.due.total_cmp(&b.due).then(a.order.cmp(&b.order)))?;
        let item = self.queue.swap_remove(index);
        let len = item.len.min(out.len());
        out[..len].copy_from_slice(&item.data[..len]);
        Some(len)
    }

    /// Payloads waiting.
    #[must_use]
    pub fn queued(&self) -> usize {
        self.queue.len()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn drain(c: &mut Conditioner, now: f64) -> Vec<u8> {
        let mut out = Vec::new();
        let mut buf = [0u8; MAX_PAYLOAD];
        while let Some(n) = c.pop(now, &mut buf) {
            out.push(buf[..n][0]);
        }
        out
    }

    #[test]
    fn pass_through() {
        let mut c = Conditioner::new(Conditions::default(), 1);
        c.push(0.0, &[1]);
        c.push(0.0, &[2]);
        assert_eq!(drain(&mut c, 0.0), vec![1, 2]);
    }

    #[test]
    fn latency_holds_packets() {
        let mut c = Conditioner::new(
            Conditions {
                latency: 0.05,
                ..Conditions::default()
            },
            1,
        );
        c.push(1.0, &[7]);
        assert!(drain(&mut c, 1.049).is_empty());
        assert_eq!(drain(&mut c, 1.05), vec![7]);
    }

    #[test]
    fn loss_rate_and_determinism() {
        let conditions = Conditions {
            loss: 0.05,
            duplicate: 0.02,
            ..Conditions::default()
        };
        let mut a = Conditioner::new(conditions, 42);
        let mut b = Conditioner::new(conditions, 42);
        for i in 0..10_000u32 {
            a.push(0.0, &[(i % 256) as u8]);
            b.push(0.0, &[(i % 256) as u8]);
        }
        assert_eq!(a.stats, b.stats);
        let loss = a.stats.dropped as f64 / 10_000.0;
        assert!((0.04..0.06).contains(&loss), "{loss}");
        let dup = a.stats.duplicated as f64 / 10_000.0;
        assert!((0.01..0.03).contains(&dup), "{dup}");
        assert_eq!(drain(&mut a, 0.0), drain(&mut b, 0.0));
    }

    #[test]
    fn reorder_and_jitter() {
        let mut c = Conditioner::new(
            Conditions {
                latency: 0.03,
                jitter: 0.01,
                reorder: 0.2,
                ..Conditions::default()
            },
            9,
        );
        for i in 0..200u32 {
            c.push(f64::from(i) * 0.001, &[i as u8]);
        }
        let got = drain(&mut c, 10.0);
        assert_eq!(got.len(), 200);
        assert!(got.windows(2).any(|w| w[0] > w[1]), "some reordering");
        assert!(c.stats.reordered > 10);
    }
}
