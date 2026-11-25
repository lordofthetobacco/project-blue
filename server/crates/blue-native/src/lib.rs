//! Safe wrappers over the native C libraries the server links:
//! netcode.io ([`netcode`]), the simulation core `blue_sim_c` ([`sim`]) and
//! process signals ([`signal`]), plus a counting allocator for allocation
//! tests ([`alloc_counter`]).
//!
//! This is the only crate of the workspace that contains `unsafe` code. Every
//! `unsafe` block states the invariant that makes it sound; everything
//! exported is safe to call.

#![allow(unsafe_code)]

pub mod alloc_counter;
pub mod netcode;
pub mod signal;
pub mod sim;
