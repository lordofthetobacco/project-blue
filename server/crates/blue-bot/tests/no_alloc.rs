//! The steady-state tick path does not allocate.
//!
//! A counting global allocator counts allocations made on the tick thread
//! (flagged through the server's tick hook). Four bots connect; once they
//! are all in, the count must not grow over three seconds of ticks.

#![allow(clippy::unwrap_used, clippy::panic)]

use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};

use blue_bot::RunConfig;
use blue_native::alloc_counter::{CountingAllocator, track_current_thread, tracked_allocations};
use blue_server::config::Config;
use blue_server::run::{self, RunOptions};

#[global_allocator]
static GLOBAL: CountingAllocator = CountingAllocator;

static AT_START: AtomicU64 = AtomicU64::new(u64::MAX);
static AT_END: AtomicU64 = AtomicU64::new(u64::MAX);

/// Ticks 1..: track the tick thread. Tick 2: one deliberate allocation. Tick 240 (4 s, bots connected at
/// ~1 s): snapshot the count. Tick 420 (7 s): snapshot again.
fn hook(tick: u32) {
    track_current_thread(true);
    match tick {
        // Proves the counter sees the tick thread: exactly one allocation.
        2 => drop(std::hint::black_box(vec![0u8; 16])),
        240 => AT_START.store(tracked_allocations(), Ordering::SeqCst),
        420 => AT_END.store(tracked_allocations(), Ordering::SeqCst),
        _ => {}
    }
}

#[test]
fn steady_state_tick_does_not_allocate() {
    let port = 45_000 + (std::process::id() % 5_000) as u16;
    let config = Config {
        port,
        public_addr: format!("127.0.0.1:{port}"),
        public_addr_given: true,
        metrics_addr: None,
        ..Config::default()
    };
    let options = RunOptions {
        tick_hook: Some(hook),
    };
    let server = run::start(&config, options).unwrap();
    let bots = RunConfig {
        server: format!("127.0.0.1:{port}"),
        count: 4,
        duration: 8.0,
        latency_ms: 20.0,
        loss_pct: 2.0,
        ..RunConfig::default()
    };
    let summary = blue_bot::run(&bots, &AtomicBool::new(false)).unwrap();
    assert!(summary.ok(), "{}", summary.report());
    let start = AT_START.load(Ordering::SeqCst);
    let end = AT_END.load(Ordering::SeqCst);
    assert_ne!(start, u64::MAX, "tick 240 not reached");
    assert_ne!(end, u64::MAX, "tick 420 not reached");
    println!("tick-thread allocations: {start} by tick 240, {end} by tick 420");
    assert!(
        start >= 1,
        "the deliberate allocation at tick 2 was not counted"
    );
    assert_eq!(start, end, "the tick thread allocated during steady state");
    server.join().unwrap();
}
