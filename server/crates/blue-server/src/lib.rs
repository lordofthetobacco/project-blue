//! Project Blue authoritative dedicated server.
//!
//! A dedicated thread runs the simulation at exactly 60 Hz ([`tick`]); each
//! tick pumps netcode, validates and buffers client commands ([`input`],
//! [`validate`]), steps the [`sim::Simulation`], tests projectiles against
//! lag-compensated player capsules ([`lagcomp`]), turns simulation events
//! into reliable messages and sends one snapshot per client ([`server`]).
//! Operations: configuration ([`config`]), structured logs ([`log`]),
//! Prometheus metrics and health endpoints ([`metrics`], [`http`]).
//!
//! The binary is a thin wrapper over [`run::start`], which tests and the
//! soak bot also use to run a server in-process.

#![forbid(unsafe_code)]

pub mod config;
pub mod cpu;
pub mod http;
pub mod input;
pub mod lagcomp;
pub mod log;
pub mod metrics;
pub mod native_sim;
pub mod priority;
pub mod run;
pub mod server;
pub mod session;
pub mod sim;
pub mod standin;
pub mod tick;
pub mod validate;

/// netcode.io protocol id of Project Blue ("`BLUE_MP1`"); shared by the
/// server, the game client and the token issuer.
pub const NETCODE_PROTOCOL_ID: u64 = 0x424C_5545_5F4D_5031;

/// Built-in development private key. Anyone can mint tokens with it, so the
/// server warns on every start that uses it and refuses it in production.
pub const DEV_PRIVATE_KEY: [u8; 32] = *b"blue-dev-key-do-not-use-in-prod!";

/// Longest connect-token lifetime the server expects from issuers, seconds.
pub const MAX_CONNECT_TOKEN_LIFETIME: i32 = 30;

/// Lifetime of self-minted development tokens, seconds. Longer than
/// [`MAX_CONNECT_TOKEN_LIFETIME`]: netcode refuses tokens whose expiry minus
/// that lifetime is not after the server's start, so a token minted in the
/// same second the server started needs the extra margin.
pub const DEV_TOKEN_EXPIRE_SECONDS: i32 = 60;

/// Connection timeout carried in connect tokens, seconds.
pub const TOKEN_TIMEOUT_SECONDS: i32 = 5;

/// Simulation ticks per second.
pub const TICK_RATE: u32 = 60;

/// Seconds per tick.
pub const TICK_SECONDS: f64 = 1.0 / TICK_RATE as f64;
