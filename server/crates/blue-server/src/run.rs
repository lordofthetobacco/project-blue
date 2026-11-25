//! Starting and stopping a server: simulation, netcode socket, HTTP
//! listener and the dedicated 60 Hz tick thread.

use std::net::SocketAddr;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, mpsc};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

use blue_native::netcode::{self, NetcodeServer, ServerSettings as NetSettings};

use crate::config::{Config, SimKind};
use crate::log;
use crate::metrics::{Metrics, PercentileWindow};
use crate::native_sim::NativeSim;
use crate::server::{Server, ServerSettings};
use crate::sim::Simulation;
use crate::standin::StandInSim;
use crate::tick::{Due, TickScheduler, wait_until};
use crate::{MAX_CONNECT_TOKEN_LIFETIME, NETCODE_PROTOCOL_ID, TICK_RATE};

/// Ticks a stalled loop may run back to back before resynchronising.
const MAX_CATCH_UP: u32 = 4;
/// Tick-duration samples for the percentile gauges (10 s).
const PERCENTILE_WINDOW: usize = 10 * TICK_RATE as usize;
/// Stack of the tick thread (the server keeps large fixed buffers).
const TICK_STACK_BYTES: usize = 8 * 1024 * 1024;

/// Optional extras for tests.
#[derive(Clone, Copy, Debug, Default)]
pub struct RunOptions {
    /// Called on the tick thread after every tick with the tick number.
    pub tick_hook: Option<fn(u32)>,
}

/// A running server.
#[derive(Debug)]
pub struct ServerHandle {
    stop: Arc<AtomicBool>,
    tick_thread: Option<JoinHandle<()>>,
    http_thread: Option<JoinHandle<()>>,
    /// Live metrics.
    pub metrics: Arc<Metrics>,
    /// Address the HTTP listener is bound to, if enabled.
    pub metrics_addr: Option<SocketAddr>,
    /// UDP port netcode is bound to.
    pub port: u16,
}

impl ServerHandle {
    /// Asks the server to shut down (non-blocking).
    pub fn stop(&self) {
        self.stop.store(true, Ordering::SeqCst);
    }

    /// True once the tick thread has exited.
    #[must_use]
    pub fn finished(&self) -> bool {
        self.tick_thread
            .as_ref()
            .is_none_or(JoinHandle::is_finished)
    }

    /// Stops and waits for every thread.
    ///
    /// # Errors
    /// If a thread panicked.
    pub fn join(mut self) -> Result<(), String> {
        self.stop();
        let mut result = Ok(());
        if let Some(t) = self.tick_thread.take()
            && t.join().is_err()
        {
            result = Err("tick thread panicked".to_owned());
        }
        if let Some(t) = self.http_thread.take()
            && t.join().is_err()
        {
            result = Err("http thread panicked".to_owned());
        }
        result
    }
}

impl Drop for ServerHandle {
    fn drop(&mut self) {
        self.stop();
        if let Some(t) = self.tick_thread.take() {
            let _ = t.join();
        }
        if let Some(t) = self.http_thread.take() {
            let _ = t.join();
        }
    }
}

/// Builds the simulation on the tick thread.
pub type SimFactory = Box<dyn FnOnce() -> Result<Box<dyn Simulation>, String> + Send>;

fn create_sim(config: &Config) -> Result<Box<dyn Simulation>, String> {
    match config.sim {
        SimKind::StandIn => Ok(Box::new(StandInSim::new())),
        SimKind::Native => {
            let sim = NativeSim::create(&config.asset_dir, &config.scene, config.max_players)?;
            Ok(Box::new(sim))
        }
    }
}

fn hex(bytes: u64) -> String {
    format!("{bytes:016x}")
}

/// Loads the simulation, binds the socket, starts the HTTP listener and the
/// tick thread.
///
/// # Errors
/// Simulation, socket or listener failures, as a message.
pub fn start(config: &Config, options: RunOptions) -> Result<ServerHandle, String> {
    let sim_config = config.clone();
    start_with_sim(config, options, Box::new(move || create_sim(&sim_config)))
}

/// Like [`start`], with the simulation built by `factory` (on the tick
/// thread) instead of from `config.sim` (tests use it to inject a sim).
///
/// # Errors
/// As [`start`].
pub fn start_with_sim(
    config: &Config,
    options: RunOptions,
    factory: SimFactory,
) -> Result<ServerHandle, String> {
    netcode::set_log_level(config.netcode_log);
    if config.dev_key {
        for _ in 0..3 {
            log::warn(
                "dev_key_in_use",
                &[(
                    "message",
                    "RUNNING WITH THE BUILT-IN DEVELOPMENT KEY: anyone can mint connect \
                     tokens. Set BLUE_PRIVATE_KEY for any non-local deployment."
                        .into(),
                )],
            );
        }
    }
    if config.bind != "0.0.0.0" {
        log::warn(
            "bind_ignored",
            &[
                ("bind", config.bind.as_str().into()),
                (
                    "message",
                    "netcode binds the wildcard address on the public address' port".into(),
                ),
            ],
        );
    }
    if !config.public_addr_given {
        log::warn(
            "public_addr_default",
            &[("public_addr", config.public_addr.as_str().into())],
        );
    }

    let epoch = Instant::now();
    let net_settings = NetSettings {
        public_address: config.public_addr.clone(),
        protocol_id: NETCODE_PROTOCOL_ID,
        private_key: config.private_key,
        max_connect_token_lifetime: MAX_CONNECT_TOKEN_LIFETIME,
    };
    let mut net = NetcodeServer::new(&net_settings, 0.0)
        .map_err(|e| format!("cannot start netcode on {}: {e}", config.public_addr))?;
    net.start(usize::from(config.max_players));
    let port = net.port();

    let metrics = Arc::new(Metrics::new());
    let stop = Arc::new(AtomicBool::new(false));
    let settings = ServerSettings {
        max_players: config.max_players,
        snapshot_rate: config.snapshot_rate,
        max_rewind_ms: config.max_rewind_ms,
    };

    // The simulation is created, used and dropped on the tick thread (the
    // native library requires one thread); it reports the load result back.
    let (loaded_tx, loaded_rx) = mpsc::channel::<Result<u64, String>>();
    let thread_stop = Arc::clone(&stop);
    let thread_metrics = Arc::clone(&metrics);
    let tick_thread = std::thread::Builder::new()
        .name("blue-tick".to_owned())
        .stack_size(TICK_STACK_BYTES)
        .spawn(move || {
            let sim = match factory() {
                Ok(sim) => sim,
                Err(message) => {
                    let _ = loaded_tx.send(Err(message));
                    return;
                }
            };
            let _ = loaded_tx.send(Ok(sim.content_hash()));
            let server = Server::new(net, sim, settings, Arc::clone(&thread_metrics));
            tick_loop(server, epoch, &thread_stop, &thread_metrics, options);
        })
        .map_err(|e| format!("cannot spawn the tick thread: {e}"))?;
    let content_hash = match loaded_rx.recv() {
        Ok(Ok(hash)) => hash,
        Ok(Err(message)) => {
            let _ = tick_thread.join();
            return Err(message);
        }
        Err(_) => {
            let _ = tick_thread.join();
            return Err("the tick thread exited while loading the simulation".to_owned());
        }
    };
    let mut handle = ServerHandle {
        stop: Arc::clone(&stop),
        tick_thread: Some(tick_thread),
        http_thread: None,
        metrics: Arc::clone(&metrics),
        metrics_addr: None,
        port,
    };
    if let Some(address) = &config.metrics_addr {
        // On error `handle` drops here, which stops the tick thread.
        let (http, local) = crate::http::spawn(address, Arc::clone(&metrics), Arc::clone(&stop))
            .map_err(|e| format!("cannot bind metrics listener {address}: {e}"))?;
        handle.http_thread = Some(http);
        handle.metrics_addr = Some(local);
    }
    metrics.ready.store(true, Ordering::SeqCst);
    log::info(
        "server_started",
        &[
            ("port", port.into()),
            ("public_addr", config.public_addr.as_str().into()),
            ("max_players", config.max_players.into()),
            (
                "sim",
                if config.sim == SimKind::StandIn {
                    "standin"
                } else {
                    "native"
                }
                .into(),
            ),
            ("content_hash", hex(content_hash).as_str().into()),
            ("snapshot_rate", config.snapshot_rate.into()),
            ("max_rewind_ms", config.max_rewind_ms.into()),
            (
                "metrics_addr",
                handle
                    .metrics_addr
                    .map(|a| a.to_string())
                    .unwrap_or_else(|| "off".to_owned())
                    .as_str()
                    .into(),
            ),
        ],
    );
    Ok(handle)
}

fn tick_loop(
    mut server: Server,
    epoch: Instant,
    stop: &AtomicBool,
    metrics: &Metrics,
    options: RunOptions,
) {
    let period = Duration::from_nanos(1_000_000_000 / u64::from(TICK_RATE));
    // The grid starts now, after the simulation loaded.
    let mut scheduler = TickScheduler::new(period, MAX_CATCH_UP).starting_at(epoch.elapsed());
    let mut window = PercentileWindow::new(PERCENTILE_WINDOW);
    let mut since_percentiles = 0u32;
    while !stop.load(Ordering::Relaxed) {
        match scheduler.poll(epoch.elapsed()) {
            Due::Wait(deadline) => wait_until(epoch, deadline),
            Due::Run { late, skipped } => {
                if late {
                    Metrics::add(&metrics.late_ticks, 1);
                }
                if skipped > 0 {
                    Metrics::add(&metrics.skipped_ticks, u64::from(skipped));
                    log::warn("tick_resync", &[("skipped", skipped.into())]);
                }
                let started = Instant::now();
                let tick = server.next_tick();
                server.tick(epoch.elapsed().as_secs_f64());
                let took = started.elapsed();
                let nanos = u64::try_from(took.as_nanos()).unwrap_or(u64::MAX);
                metrics.tick_duration.observe(nanos);
                if took > period {
                    Metrics::add(&metrics.overruns, 1);
                }
                window.push(nanos);
                since_percentiles += 1;
                if since_percentiles >= TICK_RATE {
                    since_percentiles = 0;
                    for (gauge, value) in metrics.tick_percentiles.iter().zip(window.percentiles())
                    {
                        gauge.store(value, Ordering::Relaxed);
                    }
                }
                if let Some(hook) = options.tick_hook {
                    hook(tick);
                }
            }
        }
    }
    log::info(
        "server_stopping",
        &[("players", metrics.players.load(Ordering::Relaxed).into())],
    );
    metrics.ready.store(false, Ordering::SeqCst);
    // netcode sends its disconnect packets synchronously here.
    server.shutdown();
    drop(server);
    log::info("server_stopped", &[]);
}
