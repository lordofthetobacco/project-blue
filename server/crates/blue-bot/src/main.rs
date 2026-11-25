//! `blue-bot`: run headless clients against a server and report.
//!
//! Exits 0 only if every bot stayed connected and every invariant held.

#![forbid(unsafe_code)]

use std::process::ExitCode;
use std::sync::atomic::AtomicBool;

fn main() -> ExitCode {
    let config = match blue_bot::parse_args(std::env::args().skip(1)) {
        Ok(Some(config)) => config,
        Ok(None) => {
            print!("{}", blue_bot::USAGE);
            return ExitCode::SUCCESS;
        }
        Err(message) => {
            eprintln!("blue-bot: {message}\n{}", blue_bot::USAGE);
            return ExitCode::from(2);
        }
    };
    let stop = AtomicBool::new(false);
    match blue_bot::run(&config, &stop) {
        Ok(summary) => {
            print!("{}", summary.report());
            if summary.ok() {
                ExitCode::SUCCESS
            } else {
                ExitCode::FAILURE
            }
        }
        Err(message) => {
            eprintln!("blue-bot: {message}");
            ExitCode::FAILURE
        }
    }
}
