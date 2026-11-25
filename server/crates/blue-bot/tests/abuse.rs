//! Handshake refusals and protocol abuse against an in-process server.

#![allow(clippy::unwrap_used, clippy::panic)]

use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, Instant};

use blue_bot::RunConfig;
use blue_native::netcode::{self, ClientState, NetcodeClient};
use blue_server::config::Config;
use blue_server::run::{self, RunOptions, ServerHandle};
use blue_server::{DEV_PRIVATE_KEY, DEV_TOKEN_EXPIRE_SECONDS, NETCODE_PROTOCOL_ID};

fn server(port: u16) -> ServerHandle {
    let config = Config {
        port,
        public_addr: format!("127.0.0.1:{port}"),
        public_addr_given: true,
        metrics_addr: None,
        ..Config::default()
    };
    run::start(&config, RunOptions::default()).unwrap()
}

#[test]
fn wrong_content_hash_is_rejected() {
    let port = 35_000 + (std::process::id() % 5_000) as u16;
    let handle = server(port);
    let bots = RunConfig {
        server: format!("127.0.0.1:{port}"),
        duration: 1.5,
        content_hash: 0xBAD,
        ..RunConfig::default()
    };
    let summary = blue_bot::run(&bots, &AtomicBool::new(false)).unwrap();
    assert!(!summary.ok());
    let failure = summary.bots[0].failure.clone().unwrap_or_default();
    assert!(failure.contains("rejected (reason 2)"), "{failure}");
    assert_eq!(handle.metrics.players.load(Ordering::SeqCst), 0);
    handle.join().unwrap();
}

#[test]
fn garbage_gets_kicked_and_server_survives() {
    let port = 30_000 + (std::process::id() % 5_000) as u16;
    let handle = server(port);
    let address = format!("127.0.0.1:{port}");
    let token = netcode::generate_connect_token(
        &[address.as_str()],
        DEV_TOKEN_EXPIRE_SECONDS,
        5,
        99,
        NETCODE_PROTOCOL_ID,
        &DEV_PRIVATE_KEY,
        &[0; 256],
    )
    .unwrap();
    let epoch = Instant::now();
    let mut client = NetcodeClient::new("0.0.0.0", 0.0).unwrap();
    client.connect(&token);
    let mut seed = 0x1234_5678_u64;
    let mut kicked = false;
    while epoch.elapsed() < Duration::from_secs(6) {
        let now = epoch.elapsed().as_secs_f64();
        client.update(now);
        match client.state() {
            ClientState::Connected => {
                // Random payloads of random kinds and sizes.
                for _ in 0..8 {
                    seed ^= seed << 13;
                    seed ^= seed >> 7;
                    seed ^= seed << 17;
                    let len = 1 + (seed % 700) as usize;
                    let mut payload = [0u8; 700];
                    for (i, b) in payload[..len].iter_mut().enumerate() {
                        *b = (seed >> ((i % 8) * 8)) as u8 ^ i as u8;
                    }
                    let _ = client.send(&payload[..len]);
                }
            }
            state if state.is_error() || state == ClientState::Disconnected => {
                kicked = epoch.elapsed() > Duration::from_millis(100);
                break;
            }
            _ => {}
        }
        std::thread::sleep(Duration::from_millis(16));
    }
    assert!(kicked, "the server never disconnected the garbage sender");
    assert!(handle.metrics.kicked.load(Ordering::SeqCst) >= 1);
    assert!(handle.metrics.invalid_packets.load(Ordering::SeqCst) >= 60);
    // A well-behaved bot still gets in afterwards.
    let bots = RunConfig {
        server: address,
        duration: 2.0,
        ..RunConfig::default()
    };
    let summary = blue_bot::run(&bots, &AtomicBool::new(false)).unwrap();
    assert!(summary.ok(), "{}", summary.report());
    handle.join().unwrap();
}
