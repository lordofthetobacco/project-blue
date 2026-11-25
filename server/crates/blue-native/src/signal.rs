//! SIGINT/SIGTERM handling for graceful shutdown.
//!
//! The handler only stores into an atomic flag (async-signal-safe); the tick
//! loop polls [`shutdown_requested`].

use std::sync::atomic::{AtomicBool, Ordering};

static SHUTDOWN: AtomicBool = AtomicBool::new(false);

#[cfg(unix)]
mod imp {
    use super::{Ordering, SHUTDOWN};
    use std::ffi::c_int;

    const SIGINT: c_int = 2;
    const SIGTERM: c_int = 15;
    const SIG_ERR: usize = usize::MAX;

    unsafe extern "C" {
        // `sighandler_t signal(int, sighandler_t)`; glibc gives it BSD
        // semantics (handler stays installed, syscalls restart).
        fn signal(signum: c_int, handler: extern "C" fn(c_int)) -> usize;
    }

    extern "C" fn on_signal(_signum: c_int) {
        SHUTDOWN.store(true, Ordering::SeqCst);
    }

    pub(super) fn install() -> bool {
        // SAFETY: `on_signal` is an `extern "C"` function that only performs
        // an atomic store, which is async-signal-safe; SIGINT and SIGTERM
        // are valid signal numbers on Linux.
        let a = unsafe { signal(SIGINT, on_signal) };
        // SAFETY: as above.
        let b = unsafe { signal(SIGTERM, on_signal) };
        a != SIG_ERR && b != SIG_ERR
    }
}

#[cfg(not(unix))]
mod imp {
    pub(super) fn install() -> bool {
        false
    }
}

/// Installs SIGINT and SIGTERM handlers that set the shutdown flag.
/// Returns false if they could not be installed (or on non-Unix targets).
pub fn install_shutdown_handler() -> bool {
    imp::install()
}

/// True once SIGINT or SIGTERM arrived (or [`request_shutdown`] was called).
#[must_use]
pub fn shutdown_requested() -> bool {
    SHUTDOWN.load(Ordering::SeqCst)
}

/// Sets the shutdown flag as if a signal had arrived.
pub fn request_shutdown() {
    SHUTDOWN.store(true, Ordering::SeqCst);
}
