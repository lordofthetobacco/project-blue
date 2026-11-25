//! Fixed-rate tick scheduling.
//!
//! [`TickScheduler`] is pure (it is fed the current time), so its catch-up
//! and resynchronisation rules are tested with a fake clock. Deadlines are
//! absolute multiples of the period from the start, so rounding never
//! accumulates into drift. [`wait_until`] sleeps until about a millisecond
//! before a deadline and spins the rest.

use std::time::{Duration, Instant};

/// What the caller should do now.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Due {
    /// Run one tick now.
    Run {
        /// The tick started at least one period after its deadline.
        late: bool,
        /// Ticks dropped by a resynchronisation just before this one.
        skipped: u32,
    },
    /// Nothing is due before this time (since the scheduler's epoch).
    Wait(Duration),
}

/// Deadline-based fixed-rate scheduler.
#[derive(Clone, Debug)]
pub struct TickScheduler {
    period: Duration,
    /// Index of the next tick; its deadline is `base + index * period`.
    index: u64,
    base: Duration,
    max_catch_up: u32,
}

impl TickScheduler {
    /// A scheduler whose first tick is due at time 0. When it falls more
    /// than `max_catch_up` periods behind, it drops the missed ticks and
    /// restarts its deadlines from the current time.
    #[must_use]
    pub fn new(period: Duration, max_catch_up: u32) -> Self {
        Self {
            period,
            index: 0,
            base: Duration::ZERO,
            max_catch_up: max_catch_up.max(1),
        }
    }

    /// The same scheduler with its first tick due at `start` instead of 0
    /// (for loops that begin after some setup time).
    #[must_use]
    pub fn starting_at(mut self, start: Duration) -> Self {
        self.base = start;
        self.index = 0;
        self
    }

    /// Period.
    #[must_use]
    pub fn period(&self) -> Duration {
        self.period
    }

    /// Deadline of the next tick.
    #[must_use]
    pub fn next_deadline(&self) -> Duration {
        self.deadline()
    }

    fn deadline(&self) -> Duration {
        // `index` stays below 2^32 for over two years at 60 Hz; the base is
        // moved forward on every resync, keeping the multiplier small.
        let index = u32::try_from(self.index).unwrap_or(u32::MAX);
        self.base + self.period * index
    }

    /// Decides at time `now` (since the epoch).
    pub fn poll(&mut self, now: Duration) -> Due {
        let deadline = self.deadline();
        if now < deadline {
            return Due::Wait(deadline);
        }
        let lateness = now - deadline;
        if lateness >= self.period * self.max_catch_up {
            // Too far behind to catch up: drop the backlog, restart the
            // deadline grid at `now`.
            let skipped = (lateness.as_nanos() / self.period.as_nanos().max(1)) as u32;
            self.base = now;
            self.index = 1;
            return Due::Run {
                late: true,
                skipped,
            };
        }
        self.index += 1;
        if self.index >= u64::from(u32::MAX / 2) {
            self.base = self.deadline();
            self.index = 0;
        }
        Due::Run {
            late: lateness >= self.period,
            skipped: 0,
        }
    }
}

/// Blocks until `epoch + deadline`: sleeps until about 1 ms before it, then
/// spins.
pub fn wait_until(epoch: Instant, deadline: Duration) {
    const SPIN: Duration = Duration::from_micros(1000);
    let target = epoch + deadline;
    loop {
        let now = Instant::now();
        if now >= target {
            return;
        }
        let remaining = target - now;
        if remaining > SPIN + Duration::from_micros(200) {
            std::thread::sleep(remaining - SPIN);
        } else {
            std::hint::spin_loop();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const P: Duration = Duration::from_nanos(16_666_667);

    fn ms(v: f64) -> Duration {
        Duration::from_secs_f64(v / 1000.0)
    }

    #[test]
    fn on_time_ticks_follow_the_grid() {
        let mut s = TickScheduler::new(P, 4);
        assert_eq!(
            s.poll(Duration::ZERO),
            Due::Run {
                late: false,
                skipped: 0
            }
        );
        assert_eq!(s.poll(ms(1.0)), Due::Wait(P));
        // Waking up a little late does not shift later deadlines.
        assert!(matches!(s.poll(P + ms(2.0)), Due::Run { late: false, .. }));
        assert_eq!(s.poll(P + ms(3.0)), Due::Wait(P * 2));
    }

    #[test]
    fn no_drift_over_a_minute() {
        let mut s = TickScheduler::new(P, 4);
        let mut now = Duration::ZERO;
        let mut ticks = 0u32;
        // Fake clock: each tick takes 3 ms, the "sleep" lands exactly on the
        // deadline.
        while now < Duration::from_secs(60) {
            match s.poll(now) {
                Due::Run { .. } => {
                    ticks += 1;
                    now += ms(3.0);
                }
                Due::Wait(d) => now = d,
            }
        }
        assert_eq!(ticks, 3600);
    }

    #[test]
    fn catches_up_a_few_ticks() {
        let mut s = TickScheduler::new(P, 4);
        assert!(matches!(s.poll(Duration::ZERO), Due::Run { .. }));
        // One tick took 2.5 periods: the two due ticks run back to back.
        let now = P * 2 + P / 2;
        assert_eq!(
            s.poll(now),
            Due::Run {
                late: true,
                skipped: 0
            }
        );
        assert!(matches!(s.poll(now), Due::Run { late: false, .. }));
        assert_eq!(s.poll(now), Due::Wait(P * 3));
    }

    #[test]
    fn resyncs_instead_of_spiralling() {
        let mut s = TickScheduler::new(P, 4);
        assert!(matches!(s.poll(Duration::ZERO), Due::Run { .. }));
        // A 500 ms stall: drop the backlog instead of running 30 ticks.
        let now = ms(500.0);
        let Due::Run { late, skipped } = s.poll(now) else {
            unreachable!()
        };
        assert!(late);
        // 483.3 ms of lateness: 28 whole periods dropped.
        assert_eq!(skipped, 28);
        assert_eq!(s.poll(now), Due::Wait(now + P));
        assert!(matches!(s.poll(now + P), Due::Run { late: false, .. }));
    }

    #[test]
    fn delayed_start() {
        let mut s = TickScheduler::new(P, 4).starting_at(ms(200.0));
        assert_eq!(s.poll(ms(100.0)), Due::Wait(ms(200.0)));
        assert_eq!(
            s.poll(ms(200.0)),
            Due::Run {
                late: false,
                skipped: 0
            }
        );
    }

    #[test]
    fn real_wait_is_accurate() {
        let epoch = Instant::now();
        let deadline = Duration::from_millis(5);
        wait_until(epoch, deadline);
        let elapsed = epoch.elapsed();
        assert!(elapsed >= deadline);
        assert!(elapsed < deadline + Duration::from_millis(3), "{elapsed:?}");
    }
}
