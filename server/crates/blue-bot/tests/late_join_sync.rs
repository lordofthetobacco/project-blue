//! A late joiner is never kicked by an oversized late-join sync: with more
//! dropped weapons than the reliable window holds, the server caps the sync
//! and still sends `PlayerJoined` and `WorldSyncDone`.

#![allow(clippy::unwrap_used, clippy::panic)]

use std::sync::atomic::{AtomicBool, Ordering};

use blue_bot::RunConfig;
use blue_protocol::message::{DroppedWeapon, Pose};
use blue_protocol::{PlayerState, RemotePlayer, UserCommand, WorldObject};
use blue_server::config::Config;
use blue_server::run::{self, RunOptions};
use blue_server::sim::{Capsule, PlayerHit, Segment, SimEvent, Simulation};
use blue_server::standin::StandInSim;

/// The stand-in world plus 256 dropped weapons lying around.
struct ManyDropped(StandInSim);

impl Simulation for ManyDropped {
    fn content_hash(&self) -> u64 {
        self.0.content_hash()
    }
    fn add_player(&mut self, id: u8) -> bool {
        self.0.add_player(id)
    }
    fn remove_player(&mut self, id: u8) {
        self.0.remove_player(id);
    }
    fn apply_command(&mut self, id: u8, command: &UserCommand) {
        self.0.apply_command(id, command);
    }
    fn step(&mut self, tick: u32) {
        self.0.step(tick);
    }
    fn own_state(&self, id: u8) -> Option<PlayerState> {
        self.0.own_state(id)
    }
    fn remote_state(&self, id: u8) -> Option<RemotePlayer> {
        self.0.remote_state(id)
    }
    fn capsule(&self, id: u8) -> Option<Capsule> {
        self.0.capsule(id)
    }
    fn sweep_projectiles(&mut self, out: &mut [Segment]) -> usize {
        self.0.sweep_projectiles(out)
    }
    fn resolve_projectiles(&mut self, hits: &[PlayerHit]) {
        self.0.resolve_projectiles(hits);
    }
    fn read_objects(&self, out: &mut [WorldObject]) -> usize {
        self.0.read_objects(out)
    }
    fn poll_events(&mut self, out: &mut [SimEvent]) -> usize {
        self.0.poll_events(out)
    }
    fn taken_pickups(&self, out: &mut [u32]) -> usize {
        self.0.taken_pickups(out)
    }
    fn dropped_weapons(&self, out: &mut [DroppedWeapon]) -> usize {
        for (i, slot) in out.iter_mut().enumerate() {
            *slot = DroppedWeapon {
                dropped_id: 1 + i as u32,
                weapon_key: 7,
                rounds_in_magazine: 30,
                pose: Pose {
                    position: [i as f32, 0.1, 5.0],
                    rotation: [0.0, 0.0, 0.0, 1.0],
                },
            };
        }
        out.len()
    }
}

#[test]
fn oversized_sync_is_capped_not_kicked() {
    let port = 25_000 + (std::process::id() % 5_000) as u16;
    let config = Config {
        port,
        public_addr: format!("127.0.0.1:{port}"),
        public_addr_given: true,
        metrics_addr: None,
        ..Config::default()
    };
    let handle = run::start_with_sim(
        &config,
        RunOptions::default(),
        Box::new(|| Ok(Box::new(ManyDropped(StandInSim::new())) as Box<dyn Simulation>)),
    )
    .unwrap();
    let bots = RunConfig {
        server: format!("127.0.0.1:{port}"),
        count: 3,
        duration: 3.0,
        latency_ms: 40.0,
        ..RunConfig::default()
    };
    let summary = blue_bot::run(&bots, &AtomicBool::new(false)).unwrap();
    // ok() includes "WorldSyncDone arrived" and "still connected".
    assert!(summary.ok(), "{}", summary.report());
    for bot in &summary.bots {
        assert_eq!(bot.stats.joined.count_ones(), 3, "{}", bot.name);
        assert!(bot.stats.sync_done);
    }
    assert_eq!(handle.metrics.kicked.load(Ordering::SeqCst), 0);
    handle.join().unwrap();
}
