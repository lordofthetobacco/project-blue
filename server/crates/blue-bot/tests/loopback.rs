//! In-process server (stand-in sim) plus four bots over real netcode
//! sockets on loopback, with 60 ms latency and 5% loss each way.

#![allow(clippy::unwrap_used, clippy::panic)]

use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, Instant};

use blue_bot::RunConfig;
use blue_server::config::Config;
use blue_server::run::{self, RunOptions};

#[test]
fn four_bots_with_loss_and_latency() {
    let port = 40_000 + (std::process::id() % 5_000) as u16;
    let config = Config {
        port,
        public_addr: format!("127.0.0.1:{port}"),
        public_addr_given: true,
        metrics_addr: Some("127.0.0.1:0".to_owned()),
        ..Config::default()
    };
    let server = run::start(&config, RunOptions::default()).unwrap();
    assert!(server.metrics.ready.load(Ordering::SeqCst));

    let bots = RunConfig {
        server: format!("127.0.0.1:{port}"),
        count: 4,
        duration: 5.0,
        latency_ms: 60.0,
        loss_pct: 5.0,
        seed: 11,
        ..RunConfig::default()
    };
    let summary = blue_bot::run(&bots, &AtomicBool::new(false)).unwrap();
    println!("{}", summary.report());
    assert!(summary.ok(), "{}", summary.report());
    assert!(summary.all_saw_each_other_move(), "{}", summary.report());
    for bot in &summary.bots {
        // Every bot learned about all four players through the reliable
        // channel (late-join sync plus joins).
        assert_eq!(bot.stats.joined.count_ones(), 4, "{}", bot.name);
        // With eight-fold command redundancy, 5% loss never starves the
        // server of commands, so prediction stays exact.
        assert!(
            bot.stats.corrections <= 2,
            "{}: {}",
            bot.name,
            bot.stats.corrections
        );
    }

    let metrics = server.metrics.render();
    assert!(metrics.contains("blue_players 4"), "{metrics}");
    let started = Instant::now();
    server.join().unwrap();
    assert!(started.elapsed() < Duration::from_secs(2));
}
