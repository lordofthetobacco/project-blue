//! `blue-server`: the Project Blue dedicated server binary.
//!
//! Configuration comes from flags and `BLUE_*` environment variables (see
//! `--help`). SIGINT/SIGTERM stop it gracefully: clients are disconnected
//! and the process exits 0 within two seconds.

#![forbid(unsafe_code)]

use std::process::ExitCode;
use std::time::{Duration, Instant};

use blue_server::config::{self, Parsed};
use blue_server::log;
use blue_server::run::{self, RunOptions};

fn main() -> ExitCode {
    let parsed = config::parse(std::env::args().skip(1), |k| std::env::var(k).ok());
    let config = match parsed {
        Ok(Parsed::Run(config)) => config,
        Ok(Parsed::Help) => {
            print!("{}", config::usage());
            return ExitCode::SUCCESS;
        }
        Err(message) => {
            log::error("bad_config", &[("message", message.as_str().into())]);
            eprintln!("{}", config::usage());
            return ExitCode::from(2);
        }
    };
    log::set_format(config.log_format);
    if !blue_native::signal::install_shutdown_handler() {
        log::warn("no_signal_handler", &[]);
    }
    let handle = match run::start(&config, RunOptions::default()) {
        Ok(handle) => handle,
        Err(message) => {
            log::error("start_failed", &[("message", message.as_str().into())]);
            return ExitCode::FAILURE;
        }
    };
    while !blue_native::signal::shutdown_requested() && !handle.finished() {
        std::thread::sleep(Duration::from_millis(20));
    }
    let crashed = handle.finished() && !blue_native::signal::shutdown_requested();
    log::info("shutdown_requested", &[]);
    let started = Instant::now();
    let result = handle.join();
    let elapsed_ms = started.elapsed().as_millis() as u64;
    match result {
        Ok(()) if !crashed => {
            log::info("shutdown_complete", &[("elapsed_ms", elapsed_ms.into())]);
            ExitCode::SUCCESS
        }
        Ok(()) => {
            log::error("tick_thread_exited", &[]);
            ExitCode::FAILURE
        }
        Err(message) => {
            log::error("shutdown_failed", &[("message", message.as_str().into())]);
            ExitCode::FAILURE
        }
    }
}
