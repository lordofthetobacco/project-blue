//! A counting global allocator for allocation tests.
//!
//! Install it in a test binary with
//! `#[global_allocator] static A: CountingAllocator = CountingAllocator;`,
//! mark the threads to watch with [`track_current_thread`], and read
//! [`tracked_allocations`]. It forwards everything to the system allocator.

use std::alloc::{GlobalAlloc, Layout, System};
use std::cell::Cell;
use std::sync::atomic::{AtomicU64, Ordering};

thread_local! {
    static TRACKED: Cell<bool> = const { Cell::new(false) };
}

static ALLOCATIONS: AtomicU64 = AtomicU64::new(0);

/// The allocator (zero-sized).
#[derive(Clone, Copy, Debug, Default)]
pub struct CountingAllocator;

/// Starts or stops counting allocations made by the calling thread.
pub fn track_current_thread(on: bool) {
    TRACKED.with(|t| t.set(on));
}

/// Allocations (including reallocations) made by tracked threads so far.
#[must_use]
pub fn tracked_allocations() -> u64 {
    ALLOCATIONS.load(Ordering::SeqCst)
}

fn count() {
    // `try_with`: the thread-local may already be gone during thread exit.
    if TRACKED.try_with(Cell::get).unwrap_or(false) {
        ALLOCATIONS.fetch_add(1, Ordering::Relaxed);
    }
}

// SAFETY: every method forwards to `System` with the caller's arguments, so
// the GlobalAlloc contract is upheld exactly as `System` upholds it. The
// bookkeeping (an atomic add and a const-initialised thread-local Cell)
// never allocates, so it cannot recurse into the allocator.
unsafe impl GlobalAlloc for CountingAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        count();
        // SAFETY: the caller upholds `alloc`'s contract (non-zero size).
        unsafe { System.alloc(layout) }
    }

    unsafe fn alloc_zeroed(&self, layout: Layout) -> *mut u8 {
        count();
        // SAFETY: as for `alloc`.
        unsafe { System.alloc_zeroed(layout) }
    }

    unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
        // SAFETY: `ptr` was allocated by `System` (through this type) with
        // `layout`, as the caller guarantees.
        unsafe { System.dealloc(ptr, layout) }
    }

    unsafe fn realloc(&self, ptr: *mut u8, layout: Layout, new_size: usize) -> *mut u8 {
        count();
        // SAFETY: as for `dealloc`, plus the caller's `new_size` rules.
        unsafe { System.realloc(ptr, layout, new_size) }
    }
}
