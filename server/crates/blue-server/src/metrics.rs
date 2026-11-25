//! Server metrics: lock-free counters written by the tick thread and
//! rendered in the Prometheus text format by the HTTP thread.

use std::fmt::Write as _;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};

const MAX_BUCKETS: usize = 12;

/// A fixed-bucket histogram of `u64` observations.
#[derive(Debug)]
pub struct Histogram {
    bounds: &'static [u64],
    counts: [AtomicU64; MAX_BUCKETS],
    sum: AtomicU64,
    count: AtomicU64,
}

impl Histogram {
    /// A histogram with upper bucket bounds `bounds` (ascending, at most
    /// 11; `+Inf` is implicit).
    #[must_use]
    pub const fn new(bounds: &'static [u64]) -> Self {
        Self {
            bounds,
            counts: [const { AtomicU64::new(0) }; MAX_BUCKETS],
            sum: AtomicU64::new(0),
            count: AtomicU64::new(0),
        }
    }

    /// Records one observation.
    pub fn observe(&self, value: u64) {
        let limit = self.bounds.len().min(MAX_BUCKETS - 1);
        let bucket = self.bounds[..limit]
            .iter()
            .position(|&b| value <= b)
            .unwrap_or(limit);
        self.counts[bucket].fetch_add(1, Ordering::Relaxed);
        self.sum.fetch_add(value, Ordering::Relaxed);
        self.count.fetch_add(1, Ordering::Relaxed);
    }

    /// Observations recorded.
    #[must_use]
    pub fn count(&self) -> u64 {
        self.count.load(Ordering::Relaxed)
    }

    /// Renders as a Prometheus histogram; `scale` divides raw values (e.g.
    /// 1e9 for nanoseconds rendered as seconds).
    fn render(&self, out: &mut String, name: &str, help: &str, scale: f64) {
        let _ = writeln!(out, "# HELP {name} {help}");
        let _ = writeln!(out, "# TYPE {name} histogram");
        let limit = self.bounds.len().min(MAX_BUCKETS - 1);
        let mut cumulative = 0u64;
        for (i, bound) in self.bounds[..limit].iter().enumerate() {
            cumulative += self.counts[i].load(Ordering::Relaxed);
            let le = *bound as f64 / scale;
            let _ = writeln!(out, "{name}_bucket{{le=\"{le}\"}} {cumulative}");
        }
        cumulative += self.counts[limit].load(Ordering::Relaxed);
        let _ = writeln!(out, "{name}_bucket{{le=\"+Inf\"}} {cumulative}");
        let sum = self.sum.load(Ordering::Relaxed) as f64 / scale;
        let _ = writeln!(out, "{name}_sum {sum}");
        let _ = writeln!(out, "{name}_count {cumulative}");
    }
}

/// Tick duration buckets, nanoseconds (0.25 ms .. 33 ms).
const TICK_BOUNDS_NS: &[u64] = &[
    250_000, 500_000, 1_000_000, 2_000_000, 4_000_000, 8_000_000, 12_000_000, 16_666_667,
    25_000_000, 33_333_333,
];
/// Snapshot size buckets, bytes.
const SNAPSHOT_BOUNDS: &[u64] = &[64, 128, 256, 384, 512, 768, 1024, 1100, 1200];

/// All server metrics.
#[derive(Debug)]
pub struct Metrics {
    /// Tick processing time, ns.
    pub tick_duration: Histogram,
    /// Snapshot payload sizes, bytes.
    pub snapshot_size: Histogram,
    /// Ticks whose processing took longer than one period.
    pub overruns: AtomicU64,
    /// Ticks that started at least one period late.
    pub late_ticks: AtomicU64,
    /// Ticks dropped by resynchronisation after a stall.
    pub skipped_ticks: AtomicU64,
    /// Simulated ticks.
    pub ticks: AtomicU64,
    /// Welcomed players.
    pub players: AtomicU64,
    /// Dropped weapons in the world (at the last snapshot).
    pub dropped_weapons: AtomicU64,
    /// World objects (bodies + dropped weapons) at the last snapshot.
    pub world_objects: AtomicU64,
    /// netcode connections (including ones still in the handshake).
    pub connections: AtomicU64,
    /// Packets received.
    pub packets_in: AtomicU64,
    /// Payload bytes received.
    pub bytes_in: AtomicU64,
    /// Packets sent.
    pub packets_out: AtomicU64,
    /// Payload bytes sent.
    pub bytes_out: AtomicU64,
    /// Packets dropped without a violation (duplicate, stale, rate-limited
    /// before welcome).
    pub dropped_packets: AtomicU64,
    /// Packets or contents that violated the protocol.
    pub invalid_packets: AtomicU64,
    /// Ticks a player ran without a fresh command.
    pub missing_commands: AtomicU64,
    /// Commands that arrived for a tick already simulated (resends).
    pub late_commands: AtomicU64,
    /// Clients disconnected for violations.
    pub kicked: AtomicU64,
    /// Player hits found by lag compensation.
    pub hits: AtomicU64,
    /// Simulation events forwarded as reliable messages.
    pub sim_events: AtomicU64,
    /// Projectile segments swept.
    pub segments: AtomicU64,
    /// Tick duration percentiles over the last 10 s, ns: p50, p90, p99, max.
    pub tick_percentiles: [AtomicU64; 4],
    /// Scene loaded and socket bound.
    pub ready: AtomicBool,
}

impl Default for Metrics {
    fn default() -> Self {
        Self::new()
    }
}

impl Metrics {
    /// All zero, not ready.
    #[must_use]
    pub const fn new() -> Self {
        Self {
            tick_duration: Histogram::new(TICK_BOUNDS_NS),
            snapshot_size: Histogram::new(SNAPSHOT_BOUNDS),
            overruns: AtomicU64::new(0),
            late_ticks: AtomicU64::new(0),
            skipped_ticks: AtomicU64::new(0),
            ticks: AtomicU64::new(0),
            players: AtomicU64::new(0),
            dropped_weapons: AtomicU64::new(0),
            world_objects: AtomicU64::new(0),
            connections: AtomicU64::new(0),
            packets_in: AtomicU64::new(0),
            bytes_in: AtomicU64::new(0),
            packets_out: AtomicU64::new(0),
            bytes_out: AtomicU64::new(0),
            dropped_packets: AtomicU64::new(0),
            invalid_packets: AtomicU64::new(0),
            missing_commands: AtomicU64::new(0),
            late_commands: AtomicU64::new(0),
            kicked: AtomicU64::new(0),
            hits: AtomicU64::new(0),
            sim_events: AtomicU64::new(0),
            segments: AtomicU64::new(0),
            tick_percentiles: [const { AtomicU64::new(0) }; 4],
            ready: AtomicBool::new(false),
        }
    }

    /// Adds `n` to a counter.
    pub fn add(counter: &AtomicU64, n: u64) {
        counter.fetch_add(n, Ordering::Relaxed);
    }

    /// Prometheus text exposition format.
    #[must_use]
    pub fn render(&self) -> String {
        let mut out = String::with_capacity(4096);
        self.tick_duration.render(
            &mut out,
            "blue_tick_duration_seconds",
            "Time spent processing one tick.",
            1e9,
        );
        let quantiles = ["0.5", "0.9", "0.99", "1"];
        let _ = writeln!(
            out,
            "# HELP blue_tick_duration_recent_seconds Tick duration quantiles over the last 10 s."
        );
        let _ = writeln!(out, "# TYPE blue_tick_duration_recent_seconds gauge");
        for (q, value) in quantiles.iter().zip(&self.tick_percentiles) {
            let v = value.load(Ordering::Relaxed) as f64 / 1e9;
            let _ = writeln!(
                out,
                "blue_tick_duration_recent_seconds{{quantile=\"{q}\"}} {v}"
            );
        }
        self.snapshot_size.render(
            &mut out,
            "blue_snapshot_bytes",
            "Snapshot payload size.",
            1.0,
        );
        let counters: [(&str, &str, &AtomicU64); 20] = [
            ("blue_ticks_total", "Simulated ticks.", &self.ticks),
            (
                "blue_tick_overruns_total",
                "Ticks that took longer than one period.",
                &self.overruns,
            ),
            (
                "blue_ticks_late_total",
                "Ticks started at least one period late.",
                &self.late_ticks,
            ),
            (
                "blue_ticks_skipped_total",
                "Ticks dropped by resynchronisation.",
                &self.skipped_ticks,
            ),
            (
                "blue_packets_in_total",
                "Packets received.",
                &self.packets_in,
            ),
            (
                "blue_bytes_in_total",
                "Payload bytes received.",
                &self.bytes_in,
            ),
            ("blue_packets_out_total", "Packets sent.", &self.packets_out),
            (
                "blue_bytes_out_total",
                "Payload bytes sent.",
                &self.bytes_out,
            ),
            (
                "blue_packets_dropped_total",
                "Packets dropped (duplicate, stale, rate limited).",
                &self.dropped_packets,
            ),
            (
                "blue_packets_invalid_total",
                "Protocol violations.",
                &self.invalid_packets,
            ),
            (
                "blue_commands_missing_total",
                "Player ticks run without a fresh command.",
                &self.missing_commands,
            ),
            (
                "blue_commands_late_total",
                "Commands received for ticks already simulated.",
                &self.late_commands,
            ),
            (
                "blue_clients_kicked_total",
                "Clients disconnected for violations.",
                &self.kicked,
            ),
            (
                "blue_hits_total",
                "Lag-compensated player hits.",
                &self.hits,
            ),
            (
                "blue_sim_events_total",
                "Simulation events forwarded.",
                &self.sim_events,
            ),
            (
                "blue_projectile_segments_total",
                "Projectile segments swept.",
                &self.segments,
            ),
            ("blue_players", "Welcomed players.", &self.players),
            (
                "blue_dropped_weapons",
                "Dropped weapons in the world.",
                &self.dropped_weapons,
            ),
            (
                "blue_world_objects",
                "World objects (bodies and dropped weapons).",
                &self.world_objects,
            ),
            (
                "blue_connections",
                "netcode connections including handshakes.",
                &self.connections,
            ),
        ];
        for (name, help, value) in counters {
            let kind = if name.ends_with("_total") {
                "counter"
            } else {
                "gauge"
            };
            let _ = writeln!(out, "# HELP {name} {help}");
            let _ = writeln!(out, "# TYPE {name} {kind}");
            let _ = writeln!(out, "{name} {}", value.load(Ordering::Relaxed));
        }
        let _ = writeln!(out, "# HELP blue_ready Scene loaded and socket bound.");
        let _ = writeln!(out, "# TYPE blue_ready gauge");
        let _ = writeln!(
            out,
            "blue_ready {}",
            u8::from(self.ready.load(Ordering::Relaxed))
        );
        out
    }
}

/// Rolling window of tick durations for percentiles (no allocation after
/// construction).
#[derive(Clone, Debug)]
pub struct PercentileWindow {
    samples: Box<[u64]>,
    scratch: Box<[u64]>,
    next: usize,
    filled: usize,
}

impl PercentileWindow {
    /// A window of `capacity` samples.
    #[must_use]
    pub fn new(capacity: usize) -> Self {
        let capacity = capacity.max(1);
        Self {
            samples: vec![0; capacity].into_boxed_slice(),
            scratch: vec![0; capacity].into_boxed_slice(),
            next: 0,
            filled: 0,
        }
    }

    /// Adds a sample.
    pub fn push(&mut self, value: u64) {
        self.samples[self.next] = value;
        self.next = (self.next + 1) % self.samples.len();
        self.filled = (self.filled + 1).min(self.samples.len());
    }

    /// p50, p90, p99 and max of the window (zeros when empty).
    pub fn percentiles(&mut self) -> [u64; 4] {
        let n = self.filled;
        if n == 0 {
            return [0; 4];
        }
        let sorted = &mut self.scratch[..n];
        sorted.copy_from_slice(&self.samples[..n]);
        sorted.sort_unstable();
        let at = |q: f64| sorted[((n as f64 * q).ceil() as usize).clamp(1, n) - 1];
        [at(0.5), at(0.9), at(0.99), sorted[n - 1]]
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn histogram_renders_cumulative_buckets() {
        static H: Histogram = Histogram::new(&[10, 20]);
        H.observe(5);
        H.observe(15);
        H.observe(15);
        H.observe(100);
        let mut out = String::new();
        H.render(&mut out, "x", "help", 1.0);
        assert!(out.contains("x_bucket{le=\"10\"} 1\n"));
        assert!(out.contains("x_bucket{le=\"20\"} 3\n"));
        assert!(out.contains("x_bucket{le=\"+Inf\"} 4\n"));
        assert!(out.contains("x_sum 135\n"));
        assert!(out.contains("x_count 4\n"));
        assert_eq!(H.count(), 4);
    }

    #[test]
    fn render_lists_everything() {
        let m = Metrics::new();
        m.tick_duration.observe(1_500_000);
        Metrics::add(&m.packets_in, 3);
        let text = m.render();
        assert!(text.contains("blue_tick_duration_seconds_bucket{le=\"0.002\"} 1\n"));
        assert!(text.contains("blue_packets_in_total 3\n"));
        assert!(text.contains("# TYPE blue_players gauge\n"));
        assert!(text.contains("blue_ready 0\n"));
        assert!(text.contains("blue_tick_duration_recent_seconds{quantile=\"0.99\"} 0\n"));
    }

    #[test]
    fn percentiles() {
        let mut w = PercentileWindow::new(100);
        assert_eq!(w.percentiles(), [0; 4]);
        for v in 1..=100 {
            w.push(v);
        }
        assert_eq!(w.percentiles(), [50, 90, 99, 100]);
        // The window rolls: 100 more samples of 1000 replace everything.
        for _ in 0..100 {
            w.push(1000);
        }
        assert_eq!(w.percentiles(), [1000; 4]);
    }
}
