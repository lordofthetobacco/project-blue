//! Headless Project Blue clients for soak and integration tests.
//!
//! Each [`bot::Bot`] mints its own connect token with the (development)
//! key, connects over a real netcode socket, does the handshake, sends
//! scripted random commands at 60 Hz, decodes snapshots and checks
//! invariants. A deterministic [`conditioner`] adds latency, jitter, loss,
//! duplication and reordering between each bot and its socket.

#![forbid(unsafe_code)]

pub mod bot;
pub mod conditioner;

use std::fmt::Write as _;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, Instant};

use blue_server::config::parse_key;
use blue_server::standin::STANDIN_CONTENT_HASH;
use blue_server::tick::{Due, TickScheduler, wait_until};
use blue_server::{DEV_PRIVATE_KEY, TICK_RATE};

use crate::bot::{Bot, BotSettings, BotStats};
use crate::conditioner::{ConditionerStats, Conditions};

/// Settings of a bot run.
#[derive(Clone, Debug, PartialEq)]
pub struct RunConfig {
    /// Server `host:port`.
    pub server: String,
    /// Bots.
    pub count: usize,
    /// Seconds to run after starting.
    pub duration: f64,
    /// Added one-way delay per direction, ms.
    pub latency_ms: f64,
    /// Delay variation either way, ms.
    pub jitter_ms: f64,
    /// Loss per direction, percent.
    pub loss_pct: f64,
    /// Duplication per direction, percent.
    pub duplicate_pct: f64,
    /// Extra reordering per direction, percent.
    pub reorder_pct: f64,
    /// Seed of every random choice.
    pub seed: u64,
    /// Content hash to present.
    pub content_hash: u64,
    /// Token key.
    pub private_key: [u8; 32],
    /// Name prefix.
    pub name: String,
    /// Aim at the nearest player and fire in bursts.
    pub aim: bool,
}

impl Default for RunConfig {
    fn default() -> Self {
        Self {
            server: "127.0.0.1:27015".to_owned(),
            count: 1,
            duration: 10.0,
            latency_ms: 0.0,
            jitter_ms: 0.0,
            loss_pct: 0.0,
            duplicate_pct: 0.0,
            reorder_pct: 0.0,
            seed: 1,
            content_hash: STANDIN_CONTENT_HASH,
            private_key: DEV_PRIVATE_KEY,
            name: "bot".to_owned(),
            aim: false,
        }
    }
}

/// CLI help.
pub const USAGE: &str = "Usage: blue-bot [OPTIONS]
  --server HOST:PORT   server address (127.0.0.1:27015)
  --count N            bots (1)
  --duration S         seconds (10)
  --latency MS         added one-way delay per direction (0)
  --jitter MS          delay variation either way (0)
  --loss PCT           loss per direction (0)
  --dup PCT            duplication per direction (0)
  --reorder PCT        extra reordering per direction (0)
  --seed N             random seed (1)
  --content-hash HEX   content hash (stand-in sim's)
  --private-key HEX    token key, 64 hex digits (development key)
  --name PREFIX        name prefix (bot)
  --aim                aim at the nearest player and fire in bursts
";

fn number(flag: &str, value: &str) -> Result<f64, String> {
    let v: f64 = value
        .parse()
        .map_err(|_| format!("{flag}: not a number: {value:?}"))?;
    if v.is_finite() && v >= 0.0 {
        Ok(v)
    } else {
        Err(format!("{flag}: must be finite and >= 0"))
    }
}

/// Parses CLI arguments (without the program name). `Ok(None)` for
/// `--help`.
///
/// # Errors
/// A message for the first bad argument.
pub fn parse_args<I: IntoIterator<Item = String>>(args: I) -> Result<Option<RunConfig>, String> {
    let mut c = RunConfig::default();
    let mut args = args.into_iter();
    while let Some(flag) = args.next() {
        if flag == "--help" || flag == "-h" {
            return Ok(None);
        }
        if flag == "--aim" {
            c.aim = true;
            continue;
        }
        let value = args.next().ok_or_else(|| format!("{flag} needs a value"))?;
        match flag.as_str() {
            "--server" => c.server = value,
            "--count" => {
                c.count = value
                    .parse()
                    .map_err(|_| format!("--count: not a number: {value:?}"))?;
                if !(1..=256).contains(&c.count) {
                    return Err("--count must be 1..=256".to_owned());
                }
            }
            "--duration" => c.duration = number(&flag, &value)?,
            "--latency" => c.latency_ms = number(&flag, &value)?,
            "--jitter" => c.jitter_ms = number(&flag, &value)?,
            "--loss" => c.loss_pct = number(&flag, &value)?.min(100.0),
            "--dup" => c.duplicate_pct = number(&flag, &value)?.min(100.0),
            "--reorder" => c.reorder_pct = number(&flag, &value)?.min(100.0),
            "--seed" => {
                c.seed = value
                    .parse()
                    .map_err(|_| format!("--seed: not a number: {value:?}"))?;
            }
            "--content-hash" => {
                c.content_hash = u64::from_str_radix(value.trim_start_matches("0x"), 16)
                    .map_err(|_| format!("--content-hash: not hex: {value:?}"))?;
            }
            "--private-key" => c.private_key = parse_key(&value)?,
            "--name" => c.name = value,
            _ => return Err(format!("unknown option {flag}")),
        }
    }
    Ok(Some(c))
}

/// Result of one bot.
#[derive(Clone, Debug)]
pub struct BotResult {
    /// Name.
    pub name: String,
    /// What it saw.
    pub stats: BotStats,
    /// First invariant violation, `None` if it passed.
    pub failure: Option<String>,
    /// Conditioner counters (up, down).
    pub conditioner: (ConditionerStats, ConditionerStats),
}

/// Result of a run.
#[derive(Clone, Debug)]
pub struct Summary {
    /// Every bot.
    pub bots: Vec<BotResult>,
    /// Wall time, seconds.
    pub elapsed: f64,
}

impl Summary {
    /// True if every bot passed.
    #[must_use]
    pub fn ok(&self) -> bool {
        !self.bots.is_empty() && self.bots.iter().all(|b| b.failure.is_none())
    }

    /// True if every bot saw every other bot move.
    #[must_use]
    pub fn all_saw_each_other_move(&self) -> bool {
        let ids: Vec<u8> = self.bots.iter().filter_map(|b| b.stats.player_id).collect();
        if ids.len() != self.bots.len() {
            return false;
        }
        self.bots.iter().all(|b| {
            ids.iter()
                .all(|&id| Some(id) == b.stats.player_id || b.stats.seen_moving & (1u64 << id) != 0)
        })
    }

    /// Human-readable report.
    #[must_use]
    pub fn report(&self) -> String {
        let mut out = String::new();
        let passed = self.bots.iter().filter(|b| b.failure.is_none()).count();
        let mut rtt: Vec<f64> = self
            .bots
            .iter()
            .flat_map(|b| b.stats.rtt.iter().copied())
            .collect();
        rtt.sort_by(f64::total_cmp);
        let pct = |q: f64| -> f64 {
            if rtt.is_empty() {
                0.0
            } else {
                rtt[((rtt.len() as f64 * q).ceil() as usize).clamp(1, rtt.len()) - 1] * 1000.0
            }
        };
        let mean = if rtt.is_empty() {
            0.0
        } else {
            rtt.iter().sum::<f64>() / rtt.len() as f64 * 1000.0
        };
        let sum = |f: fn(&BotResult) -> u64| self.bots.iter().map(f).sum::<u64>();
        let snapshots = sum(|b| b.stats.snapshots);
        let bytes = sum(|b| b.stats.snapshot_bytes);
        let gaps = sum(|b| b.stats.snapshot_gaps);
        let expected = sum(|b| b.stats.snapshot_expected);
        let corrections = sum(|b| b.stats.corrections);
        let max_snapshot = self
            .bots
            .iter()
            .map(|b| b.stats.max_snapshot)
            .max()
            .unwrap_or(0);
        let depth: i64 = self.bots.iter().map(|b| b.stats.depth_sum).sum();
        let depth_n = sum(|b| b.stats.depth_samples).max(1);
        let up_drop = sum(|b| b.conditioner.0.dropped);
        let up_off = sum(|b| b.conditioner.0.offered).max(1);
        let down_drop = sum(|b| b.conditioner.1.dropped);
        let down_off = sum(|b| b.conditioner.1.offered).max(1);
        let _ = writeln!(out, "blue-bot summary");
        let _ = writeln!(
            out,
            "  bots passed        {passed}/{} ({:.1} s)",
            self.bots.len(),
            self.elapsed
        );
        let _ = writeln!(
            out,
            "  rtt ms             mean {mean:.1}  p50 {:.1}  p95 {:.1}  p99 {:.1}  max {:.1}  ({} samples)",
            pct(0.5),
            pct(0.95),
            pct(0.99),
            pct(1.0),
            rtt.len()
        );
        let reordered = sum(|b| b.stats.reordered);
        let _ = writeln!(
            out,
            "  snapshot loss seen {:.2}% ({gaps} of {expected} sequences), {reordered} reordered",
            100.0 * gaps as f64 / expected.max(1) as f64
        );
        let _ = writeln!(
            out,
            "  conditioner drop   up {:.2}%  down {:.2}%",
            100.0 * up_drop as f64 / up_off as f64,
            100.0 * down_drop as f64 / down_off as f64
        );
        let _ = writeln!(
            out,
            "  snapshots          {snapshots} received, mean {:.0} B, max {max_snapshot} B",
            bytes as f64 / snapshots.max(1) as f64
        );
        let _ = writeln!(
            out,
            "  input depth        mean {:.2} ticks",
            depth as f64 / depth_n as f64
        );
        let _ = writeln!(
            out,
            "  corrections        {corrections} (prediction uses the stand-in model)"
        );
        let _ = writeln!(
            out,
            "  reliable messages  {} (shots {}, damage {}, deaths {})",
            sum(|b| b.stats.messages),
            sum(|b| b.stats.shots),
            sum(|b| b.stats.damage),
            sum(|b| b.stats.deaths)
        );
        let lowest = self
            .bots
            .iter()
            .filter_map(|b| b.stats.lowest_y)
            .fold(f32::INFINITY, f32::min);
        let _ = writeln!(
            out,
            "  world deaths       {} (own deaths without a killer), lowest own y {:.1} m",
            sum(|b| b.stats.world_deaths),
            lowest
        );
        let _ = writeln!(
            out,
            "  dropped removed    {} DroppedRemoved messages",
            sum(|b| b.stats.dropped_removed)
        );
        let _ = writeln!(
            out,
            "  everyone moved     {}",
            if self.all_saw_each_other_move() {
                "yes"
            } else {
                "no"
            }
        );
        for b in &self.bots {
            if let Some(f) = &b.failure {
                let _ = writeln!(out, "  FAIL {}: {f}", b.name);
            }
        }
        out
    }
}

fn conditions(config: &RunConfig) -> Conditions {
    Conditions {
        latency: config.latency_ms / 1000.0,
        jitter: config.jitter_ms / 1000.0,
        loss: config.loss_pct / 100.0,
        duplicate: config.duplicate_pct / 100.0,
        reorder: config.reorder_pct / 100.0,
    }
}

/// Runs `config.count` bots on this thread until the duration elapses or
/// `stop` is set.
///
/// # Errors
/// If a bot cannot be created (token or socket).
pub fn run(config: &RunConfig, stop: &AtomicBool) -> Result<Summary, String> {
    let epoch = Instant::now();
    let unique = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map_or(0, |d| d.as_nanos() as u64);
    let mut bots = Vec::with_capacity(config.count);
    for i in 0..config.count {
        let settings = BotSettings {
            server: config.server.clone(),
            name: format!("{}{}", config.name, i + 1),
            client_id: unique.wrapping_mul(1000).wrapping_add(i as u64 + 1),
            private_key: config.private_key,
            content_hash: config.content_hash,
            up: conditions(config),
            down: conditions(config),
            seed: config.seed.wrapping_mul(1_000_003).wrapping_add(i as u64),
            aim: config.aim,
        };
        bots.push(Bot::new(settings, epoch.elapsed().as_secs_f64())?);
    }
    let period = Duration::from_nanos(1_000_000_000 / u64::from(TICK_RATE));
    let mut scheduler = TickScheduler::new(period, 4);
    let end = Duration::from_secs_f64(config.duration);
    while epoch.elapsed() < end && !stop.load(Ordering::Relaxed) {
        match scheduler.poll(epoch.elapsed()) {
            Due::Wait(deadline) => wait_until(epoch, deadline),
            Due::Run { .. } => {
                let now = epoch.elapsed().as_secs_f64();
                for bot in &mut bots {
                    bot.frame(now);
                }
            }
        }
    }
    let results = bots
        .iter()
        .enumerate()
        .map(|(i, b)| BotResult {
            name: format!("{}{}", config.name, i + 1),
            stats: b.stats.clone(),
            failure: b.verdict(),
            conditioner: b.conditioner_stats(),
        })
        .collect();
    for bot in &mut bots {
        bot.disconnect();
    }
    Ok(Summary {
        bots: results,
        elapsed: epoch.elapsed().as_secs_f64(),
    })
}

#[cfg(test)]
mod tests {
    #![allow(clippy::unwrap_used)]

    use super::*;

    fn parse(args: &[&str]) -> Result<Option<RunConfig>, String> {
        parse_args(args.iter().map(|s| (*s).to_owned()))
    }

    #[test]
    fn cli() {
        let c = parse(&[
            "--server",
            "10.0.0.1:1",
            "--count",
            "16",
            "--duration",
            "60",
            "--latency",
            "80",
            "--jitter",
            "20",
            "--loss",
            "5",
            "--seed",
            "7",
        ])
        .unwrap()
        .unwrap();
        assert_eq!(c.server, "10.0.0.1:1");
        assert_eq!(c.count, 16);
        assert!((c.loss_pct - 5.0).abs() < 1e-9);
        assert_eq!(c.seed, 7);
        assert_eq!(c.content_hash, STANDIN_CONTENT_HASH);
        assert!(parse(&["--help"]).unwrap().is_none());
        assert!(parse(&["--aim", "--count", "2"]).unwrap().unwrap().aim);
        assert!(parse(&["--count", "0"]).is_err());
        assert!(parse(&["--loss", "-1"]).is_err());
        assert!(parse(&["--latency"]).is_err());
        assert!(parse(&["--what", "1"]).is_err());
        assert_eq!(
            parse(&["--content-hash", "0x10"])
                .unwrap()
                .unwrap()
                .content_hash,
            16
        );
    }
}
