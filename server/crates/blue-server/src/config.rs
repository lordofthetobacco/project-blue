//! Server configuration from command-line flags and environment variables.
//!
//! Every setting has an environment variable and a flag; a flag wins over
//! the variable, the variable over the default. Parsing is a pure function of
//! the arguments and an environment lookup, so it is unit tested without
//! touching the process environment.

use std::fmt::Write as _;

use crate::DEV_PRIVATE_KEY;
use crate::log::Format;

/// Which simulation the server runs.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SimKind {
    /// Pure-Rust kinematic stand-in (tests, development).
    StandIn,
    /// The C++ simulation core through `blue_sim_c`.
    Native,
}

/// Complete server configuration.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Config {
    /// UDP port (`BLUE_PORT`, `--port`), default 27015.
    pub port: u16,
    /// Bind address (`BLUE_BIND`, `--bind`), default `0.0.0.0`. netcode
    /// always binds the wildcard address of the public address' family;
    /// another value only produces a warning.
    pub bind: String,
    /// Address clients connect to and connect tokens name
    /// (`BLUE_PUBLIC_ADDR`, `--public-addr`), default `127.0.0.1:<port>`.
    pub public_addr: String,
    /// Whether `public_addr` was given explicitly.
    pub public_addr_given: bool,
    /// Player cap 1..=64 (`BLUE_MAX_PLAYERS`, `--max-players`), default 16.
    pub max_players: u8,
    /// Scene path relative to the asset directory (`BLUE_SCENE`, `--scene`).
    pub scene: String,
    /// Asset directory (`BLUE_ASSET_DIR`, `--asset-dir`).
    pub asset_dir: String,
    /// netcode private key (`BLUE_PRIVATE_KEY`, `--private-key`, 64 hex
    /// digits); the development key when absent.
    pub private_key: [u8; 32],
    /// True when `private_key` is the built-in development key.
    pub dev_key: bool,
    /// Lag-compensation rewind cap, ms (`BLUE_MAX_REWIND_MS`,
    /// `--max-rewind-ms`), default 200, at most 1000.
    pub max_rewind_ms: u32,
    /// Snapshots per second per client, 1..=60 (`BLUE_SNAPSHOT_RATE`,
    /// `--snapshot-rate`), default 60.
    pub snapshot_rate: u32,
    /// Metrics/health HTTP listener (`BLUE_METRICS_ADDR`, `--metrics-addr`),
    /// default `0.0.0.0:9100`; `off` disables it.
    pub metrics_addr: Option<String>,
    /// Log format (`BLUE_LOG_FORMAT`, `--log-format`): `json` (default) or
    /// `text`.
    pub log_format: Format,
    /// Simulation (`BLUE_SIM`, `--sim`): `standin` (default) or `native`.
    pub sim: SimKind,
    /// `BLUE_ENV`/`--env`; `production` refuses the development key.
    pub environment: String,
    /// netcode library log level 0..=3 (`BLUE_NETCODE_LOG`,
    /// `--netcode-log`), default 0 (silent).
    pub netcode_log: i32,
}

impl Default for Config {
    fn default() -> Self {
        Self {
            port: 27015,
            bind: "0.0.0.0".to_owned(),
            public_addr: "127.0.0.1:27015".to_owned(),
            public_addr_given: false,
            max_players: 16,
            scene: "range.mscn".to_owned(),
            asset_dir: "assets".to_owned(),
            private_key: DEV_PRIVATE_KEY,
            dev_key: true,
            max_rewind_ms: 200,
            snapshot_rate: 60,
            metrics_addr: Some("0.0.0.0:9100".to_owned()),
            log_format: Format::Json,
            sim: SimKind::StandIn,
            environment: "development".to_owned(),
            netcode_log: 0,
        }
    }
}

/// Result of parsing.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Parsed {
    /// Run with this configuration.
    Run(Box<Config>),
    /// `--help` was given; print [`usage`].
    Help,
}

/// (flag, environment variable, value placeholder, description)
const OPTIONS: &[(&str, &str, &str, &str)] = &[
    ("--port", "BLUE_PORT", "PORT", "UDP port (27015)"),
    ("--bind", "BLUE_BIND", "ADDR", "bind address (0.0.0.0)"),
    (
        "--public-addr",
        "BLUE_PUBLIC_ADDR",
        "HOST:PORT",
        "address in connect tokens (127.0.0.1:PORT)",
    ),
    (
        "--max-players",
        "BLUE_MAX_PLAYERS",
        "N",
        "player cap, 1..=64 (16)",
    ),
    ("--scene", "BLUE_SCENE", "PATH", "scene file (range.mscn)"),
    (
        "--asset-dir",
        "BLUE_ASSET_DIR",
        "DIR",
        "asset directory (assets)",
    ),
    (
        "--private-key",
        "BLUE_PRIVATE_KEY",
        "HEX",
        "64 hex digits (built-in development key)",
    ),
    (
        "--max-rewind-ms",
        "BLUE_MAX_REWIND_MS",
        "MS",
        "lag compensation cap, 0..=1000 (200)",
    ),
    (
        "--snapshot-rate",
        "BLUE_SNAPSHOT_RATE",
        "HZ",
        "snapshots per second, 1..=60 (60)",
    ),
    (
        "--metrics-addr",
        "BLUE_METRICS_ADDR",
        "ADDR|off",
        "HTTP metrics/health listener (0.0.0.0:9100)",
    ),
    (
        "--log-format",
        "BLUE_LOG_FORMAT",
        "json|text",
        "log format (json)",
    ),
    (
        "--sim",
        "BLUE_SIM",
        "standin|native",
        "simulation (standin)",
    ),
    (
        "--env",
        "BLUE_ENV",
        "NAME",
        "environment; production refuses the dev key",
    ),
    (
        "--netcode-log",
        "BLUE_NETCODE_LOG",
        "0..3",
        "netcode library log level (0)",
    ),
];

/// The `--help` text.
#[must_use]
pub fn usage() -> String {
    let mut text = String::from("Usage: blue-server [OPTIONS]\n\nOptions (flag / env):\n");
    for (flag, env, value, help) in OPTIONS {
        let left = format!("{flag} {value}");
        let _ = writeln!(text, "  {left:<32} {env:<20} {help}");
    }
    text.push_str("  --help\n");
    text
}

fn parse_number<T: std::str::FromStr>(name: &str, value: &str) -> Result<T, String> {
    value
        .trim()
        .parse::<T>()
        .map_err(|_| format!("{name}: not a valid number: {value:?}"))
}

/// Parses 64 hex digits into a key.
///
/// # Errors
/// A message naming the problem.
pub fn parse_key(text: &str) -> Result<[u8; 32], String> {
    let text = text.trim();
    if text.len() != 64 {
        return Err(format!(
            "private key: expected 64 hex digits, got {}",
            text.len()
        ));
    }
    let mut key = [0u8; 32];
    for (i, byte) in key.iter_mut().enumerate() {
        let pair = text
            .get(i * 2..i * 2 + 2)
            .ok_or_else(|| "private key: not hex".to_owned())?;
        *byte = u8::from_str_radix(pair, 16).map_err(|_| "private key: not hex".to_owned())?;
    }
    Ok(key)
}

fn apply(config: &mut Config, key: &str, value: &str) -> Result<(), String> {
    match key {
        "--port" => config.port = parse_number("port", value)?,
        "--bind" => config.bind = value.to_owned(),
        "--public-addr" => {
            config.public_addr = value.to_owned();
            config.public_addr_given = true;
        }
        "--max-players" => {
            let n: u8 = parse_number("max players", value)?;
            if !(1..=64).contains(&n) {
                return Err(format!("max players must be 1..=64, got {n}"));
            }
            config.max_players = n;
        }
        "--scene" => config.scene = value.to_owned(),
        "--asset-dir" => config.asset_dir = value.to_owned(),
        "--private-key" => {
            config.private_key = parse_key(value)?;
            config.dev_key = config.private_key == DEV_PRIVATE_KEY;
        }
        "--max-rewind-ms" => {
            let ms: u32 = parse_number("max rewind", value)?;
            if ms > 1000 {
                return Err(format!("max rewind must be 0..=1000 ms, got {ms}"));
            }
            config.max_rewind_ms = ms;
        }
        "--snapshot-rate" => {
            let hz: u32 = parse_number("snapshot rate", value)?;
            if !(1..=60).contains(&hz) {
                return Err(format!("snapshot rate must be 1..=60, got {hz}"));
            }
            config.snapshot_rate = hz;
        }
        "--metrics-addr" => {
            config.metrics_addr = if value.eq_ignore_ascii_case("off") || value.is_empty() {
                None
            } else {
                Some(value.to_owned())
            };
        }
        "--log-format" => {
            config.log_format = match value {
                "json" => Format::Json,
                "text" => Format::Text,
                _ => return Err(format!("log format must be json or text, got {value:?}")),
            };
        }
        "--sim" => {
            config.sim = match value {
                "standin" => SimKind::StandIn,
                "native" => SimKind::Native,
                _ => return Err(format!("sim must be standin or native, got {value:?}")),
            };
        }
        "--env" => config.environment = value.to_owned(),
        "--netcode-log" => {
            let level: i32 = parse_number("netcode log", value)?;
            if !(0..=3).contains(&level) {
                return Err(format!("netcode log level must be 0..=3, got {level}"));
            }
            config.netcode_log = level;
        }
        _ => return Err(format!("unknown option {key}")),
    }
    Ok(())
}

/// Parses `args` (without the program name) over the environment given by
/// `env`, then validates the result.
///
/// # Errors
/// A message for the first invalid or unknown setting.
pub fn parse<I, E>(args: I, env: E) -> Result<Parsed, String>
where
    I: IntoIterator<Item = String>,
    E: Fn(&str) -> Option<String>,
{
    let mut config = Config::default();
    for (flag, var, _, _) in OPTIONS {
        if let Some(value) = env(var) {
            apply(&mut config, flag, &value).map_err(|e| format!("{var}: {e}"))?;
        }
    }
    let mut args = args.into_iter();
    while let Some(arg) = args.next() {
        if arg == "--help" || arg == "-h" {
            return Ok(Parsed::Help);
        }
        let (key, value) = match arg.split_once('=') {
            Some((k, v)) if k.starts_with("--") => (k.to_owned(), v.to_owned()),
            _ => {
                if !arg.starts_with("--") {
                    return Err(format!("unexpected argument {arg:?}"));
                }
                let value = args.next().ok_or_else(|| format!("{arg} needs a value"))?;
                (arg, value)
            }
        };
        apply(&mut config, &key, &value)?;
    }
    if !config.public_addr_given {
        config.public_addr = format!("127.0.0.1:{}", config.port);
    }
    if config.environment == "production" && config.dev_key {
        return Err(
            "refusing to run with the built-in development key in production: \
             set BLUE_PRIVATE_KEY"
                .to_owned(),
        );
    }
    Ok(Parsed::Run(Box::new(config)))
}

#[cfg(test)]
mod tests {
    #![allow(clippy::unwrap_used, clippy::panic)]

    use super::*;

    fn run(args: &[&str], env: &[(&str, &str)]) -> Result<Config, String> {
        let owned: Vec<(String, String)> = env
            .iter()
            .map(|(k, v)| ((*k).to_owned(), (*v).to_owned()))
            .collect();
        let lookup = move |k: &str| {
            owned
                .iter()
                .find(|(key, _)| key == k)
                .map(|(_, v)| v.clone())
        };
        match parse(args.iter().map(|s| (*s).to_owned()), lookup)? {
            Parsed::Run(c) => Ok(*c),
            Parsed::Help => Err("help".to_owned()),
        }
    }

    #[test]
    fn defaults() {
        let c = run(&[], &[]).unwrap();
        assert_eq!(c.port, 27015);
        assert_eq!(c.public_addr, "127.0.0.1:27015");
        assert_eq!(c.max_players, 16);
        assert_eq!(c.max_rewind_ms, 200);
        assert_eq!(c.snapshot_rate, 60);
        assert_eq!(c.metrics_addr.as_deref(), Some("0.0.0.0:9100"));
        assert_eq!(c.log_format, Format::Json);
        assert_eq!(c.sim, SimKind::StandIn);
        assert!(c.dev_key);
    }

    #[test]
    fn env_then_flags() {
        let c = run(
            &["--port", "4000", "--sim=native", "--log-format", "text"],
            &[
                ("BLUE_PORT", "3000"),
                ("BLUE_MAX_PLAYERS", "32"),
                ("BLUE_METRICS_ADDR", "off"),
                ("BLUE_SNAPSHOT_RATE", "30"),
            ],
        )
        .unwrap();
        assert_eq!(c.port, 4000);
        assert_eq!(c.public_addr, "127.0.0.1:4000");
        assert_eq!(c.max_players, 32);
        assert_eq!(c.metrics_addr, None);
        assert_eq!(c.snapshot_rate, 30);
        assert_eq!(c.sim, SimKind::Native);
        assert_eq!(c.log_format, Format::Text);
    }

    #[test]
    fn key_parsing() {
        let hex = "00112233445566778899aabbccddeeff00112233445566778899AABBCCDDEEFF";
        let c = run(&["--private-key", hex], &[]).unwrap();
        assert!(!c.dev_key);
        assert_eq!(c.private_key[1], 0x11);
        assert_eq!(c.private_key[31], 0xFF);
        assert!(parse_key("abc").is_err());
        assert!(parse_key(&"zz".repeat(32)).is_err());
        assert!(parse_key(&"é".repeat(32)).is_err());
    }

    #[test]
    fn production_refuses_dev_key() {
        assert!(run(&[], &[("BLUE_ENV", "production")]).is_err());
        let hex = "11".repeat(32);
        assert!(
            run(
                &[],
                &[("BLUE_ENV", "production"), ("BLUE_PRIVATE_KEY", &hex)]
            )
            .is_ok()
        );
    }

    #[test]
    fn rejects_bad_values() {
        assert!(run(&["--port", "70000"], &[]).is_err());
        assert!(run(&["--max-players", "0"], &[]).is_err());
        assert!(run(&["--max-players", "65"], &[]).is_err());
        assert!(run(&["--snapshot-rate", "61"], &[]).is_err());
        assert!(run(&["--max-rewind-ms", "5000"], &[]).is_err());
        assert!(run(&["--sim", "other"], &[]).is_err());
        assert!(run(&["--bogus", "1"], &[]).is_err());
        assert!(run(&["--port"], &[]).is_err());
        assert!(run(&["stray"], &[]).is_err());
        assert!(run(&[], &[("BLUE_PORT", "x")]).is_err());
        assert_eq!(run(&["--help"], &[]), Err("help".to_owned()));
    }

    #[test]
    fn explicit_public_addr() {
        let c = run(&["--public-addr", "203.0.113.5:27015"], &[]).unwrap();
        assert_eq!(c.public_addr, "203.0.113.5:27015");
        assert!(usage().contains("BLUE_PUBLIC_ADDR"));
    }
}
