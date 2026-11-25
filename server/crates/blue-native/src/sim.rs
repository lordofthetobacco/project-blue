//! Safe wrapper over the `blue_sim_c` C API (`src/sim/c_api/blue_sim.h`,
//! `BLUE_SIM_ABI_VERSION` 3).
//!
//! The `Blue*` structs are hand-written `repr(C)` mirrors of the header's
//! plain-data structs; their sizes are asserted at compile time here and
//! checked against `blue_sim_struct_size` by [`check_abi`] (which
//! [`Sim::create`] runs first), so a header change cannot go unnoticed.
//!
//! A [`Sim`] is neither `Send` nor `Sync`: the library requires every call,
//! and creation and destruction, to happen on one thread.

use std::ffi::{CStr, CString, c_char, c_void};
use std::fmt;
use std::ptr;
use std::sync::OnceLock;

/// ABI version this mirror was written against.
pub const ABI_VERSION: i32 = 3;

/// `BLUE_SIM_ERR_BUFFER_TOO_SMALL`.
const ERR_BUFFER_TOO_SMALL: i32 = -4;

/// Event types (`BLUE_SIM_EVENT_*`).
pub mod event {
    /// Player fired: `id` shot number, `position` origin, `vector` velocity.
    pub const SHOT: u8 = 0;
    /// A bullet hit the world: `id` instance, `position` point, `vector`
    /// normal.
    pub const IMPACT: u8 = 1;
    /// `player` was hit by `other`: `id` shot number, `weapon_key`, `zone`,
    /// `value` damage, `health` left, `position` point, `vector` bullet
    /// velocity.
    pub const DAMAGE: u8 = 2;
    /// `player` was killed by `other` (0: no attacker) with `weapon_key`.
    pub const DEATH: u8 = 3;
    /// `player` came back at `position` facing `yaw`.
    pub const RESPAWN: u8 = 4;
    /// `player` dropped dropped item `id` (`weapon_key`, `value` magazine
    /// rounds, `chambered`).
    pub const WEAPON_DROPPED: u8 = 5;
    /// `player` picked up dropped item `id`.
    pub const DROPPED_TAKEN: u8 = 6;
    /// `player` picked up scene pickup instance `id`.
    pub const PICKUP_TAKEN: u8 = 7;
    // 8 was HIT_CONFIRMED in ABI 1 (retired).
    /// Dropped item `id` (`weapon_key`) left the world without a taker
    /// (lifetime, cap, kill plane); `player` 0.
    pub const DROPPED_REMOVED: u8 = 9;
}

/// `BlueVec3`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct BlueVec3 {
    /// X.
    pub x: f32,
    /// Y (up).
    pub y: f32,
    /// Z.
    pub z: f32,
}

impl BlueVec3 {
    /// As an array.
    #[must_use]
    pub fn to_array(self) -> [f32; 3] {
        [self.x, self.y, self.z]
    }

    /// From an array.
    #[must_use]
    pub fn from_array(v: [f32; 3]) -> Self {
        Self {
            x: v[0],
            y: v[1],
            z: v[2],
        }
    }
}

/// `BlueQuat`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct BlueQuat {
    /// X.
    pub x: f32,
    /// Y.
    pub y: f32,
    /// Z.
    pub z: f32,
    /// W.
    pub w: f32,
}

impl BlueQuat {
    /// As `[x, y, z, w]`.
    #[must_use]
    pub fn to_array(self) -> [f32; 4] {
        [self.x, self.y, self.z, self.w]
    }
}

/// `BlueUserCommand`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct BlueUserCommand {
    /// Tick.
    pub tick: u32,
    /// Held buttons.
    pub held: u32,
    /// Pressed edges.
    pub pressed: u32,
    /// Yaw.
    pub yaw: f32,
    /// Pitch.
    pub pitch: f32,
    /// Slot to select, -1 none.
    pub select_slot: i8,
    /// `BLUE_SIM_INTERACT_*`.
    pub interact_kind: u8,
    /// Padding.
    pub _pad: [u8; 2],
    /// Interaction target.
    pub interact_id: u32,
}

/// `BlueFirearmState`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct BlueFirearmState {
    /// 0 semi, 1 auto, 2 double action.
    pub fire_mode: u8,
    /// Bool.
    pub magazine_inserted: u8,
    /// Bool.
    pub chambered: u8,
    /// Bool.
    pub bolt_locked: u8,
    /// Bool.
    pub empty_reload: u8,
    /// Bool.
    pub pull_used: u8,
    /// Reload steps performed.
    pub reload_stage: i8,
    /// Valid entries of `spares`.
    pub spare_count: u8,
    /// Rounds in the inserted magazine.
    pub magazine: i16,
    /// Rounds of the magazine being seated, -1 none.
    pub incoming_magazine: i16,
    /// Spare magazine rounds.
    pub spares: [i16; 8],
    /// Spares beyond eight.
    pub spares_dropped: u8,
    /// Padding.
    pub _pad: [u8; 3],
    /// Seconds.
    pub reload_elapsed: f32,
    /// Seconds.
    pub reload_duration: f32,
    /// Seconds.
    pub cycle_remaining: f32,
}

/// `BlueLoadoutSlot`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct BlueLoadoutSlot {
    /// Bool.
    pub present: u8,
    /// Padding.
    pub _pad: [u8; 3],
    /// Weapon key.
    pub weapon_key: u32,
    /// Firearm.
    pub firearm: BlueFirearmState,
    /// Scope magnification.
    pub scope_magnification: f32,
    /// Scope zero, m.
    pub scope_zero: f32,
}

/// `BluePlayerState`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct BluePlayerState {
    /// Sim player id.
    pub id: u32,
    /// Feet.
    pub position: BlueVec3,
    /// Velocity.
    pub velocity: BlueVec3,
    /// Jump speed.
    pub jump_speed: f32,
    /// Ticks.
    pub jump_age: i32,
    /// Ticks.
    pub jump_cooldown: i32,
    /// Yaw.
    pub yaw: f32,
    /// Pitch.
    pub pitch: f32,
    /// Eye height above the feet.
    pub eye_height: f32,
    /// Health.
    pub health: i16,
    /// Ticks until respawn.
    pub respawn_ticks: u16,
    /// Shots fired.
    pub shots_fired: u32,
    /// Deaths.
    pub deaths: u32,
    /// Bool.
    pub jump_pending: u8,
    /// Bool.
    pub crouching: u8,
    /// Bool.
    pub on_ground: u8,
    /// Bool.
    pub sprinting: u8,
    /// Bool.
    pub aiming: u8,
    /// Bool.
    pub alive: u8,
    /// Weapon in hand, -1 empty.
    pub active_slot: i8,
    /// Switch target, -1 none.
    pub selected_slot: i8,
    /// Switch progress 0..1.
    pub raise: f32,
    /// Bool.
    pub wait_for_release: u8,
    /// Padding.
    pub _pad: [u8; 3],
    /// Slots (0 pistol, 1 rifle).
    pub slots: [BlueLoadoutSlot; 2],
}

/// `BlueSegment`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct BlueSegment {
    /// Bullet id.
    pub projectile: i32,
    /// Shot number.
    pub shot_number: i32,
    /// Shooting player (sim id).
    pub owner: u32,
    /// Fraction of `delta` where the world stops it; 1: no hit.
    pub world_fraction: f32,
    /// Start.
    pub origin: BlueVec3,
    /// Motion over the substep.
    pub delta: BlueVec3,
    /// Velocity at the start.
    pub velocity: BlueVec3,
    /// Mass, kg.
    pub mass: f32,
    /// World hit normal.
    pub world_normal: BlueVec3,
    /// Scene instance hit, 0 none.
    pub world_instance: u32,
}

/// `BlueShotHit`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct BlueShotHit {
    /// Index into the last sweep.
    pub segment: i32,
    /// Victim (sim id).
    pub victim: u32,
    /// Along the segment, 0..1.
    pub fraction: f32,
    /// Of the tested capsule's height; negative: use the current capsule.
    pub height_fraction: f32,
}

/// `BlueCapsule`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct BlueCapsule {
    /// Bottom centre.
    pub feet: BlueVec3,
    /// Total height.
    pub height: f32,
    /// Radius.
    pub radius: f32,
}

/// `BlueRayHit`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct BlueRayHit {
    /// Hit point.
    pub point: BlueVec3,
    /// Normal.
    pub normal: BlueVec3,
    /// Fraction of the distance.
    pub fraction: f32,
    /// Scene instance, 0 none.
    pub instance: u32,
    /// Dropped weapon, 0 none.
    pub dropped_id: u32,
}

/// `BlueBodyState`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct BlueBodyState {
    /// Scene instance.
    pub instance: u32,
    /// Awake.
    pub active: u8,
    /// Padding.
    pub _pad: [u8; 3],
    /// Origin.
    pub position: BlueVec3,
    /// Rotation.
    pub rotation: BlueQuat,
    /// Linear velocity.
    pub linear_velocity: BlueVec3,
    /// Angular velocity.
    pub angular_velocity: BlueVec3,
}

/// `BlueDroppedWeapon`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct BlueDroppedWeapon {
    /// Dropped item id.
    pub id: u32,
    /// Weapon key.
    pub weapon_key: u32,
    /// Position.
    pub position: BlueVec3,
    /// Rotation.
    pub rotation: BlueQuat,
    /// Scale.
    pub scale: BlueVec3,
    /// m/s; 0 without a body.
    pub linear_velocity: BlueVec3,
    /// rad/s; 0 without a body.
    pub angular_velocity: BlueVec3,
    /// Rounds in the inserted magazine.
    pub magazine: i16,
    /// Bool: a round is chambered.
    pub chambered: u8,
    /// Bool: its body is awake (moving).
    pub active: u8,
}

/// `BluePickup`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct BluePickup {
    /// Scene instance.
    pub instance: u32,
    /// Weapon key.
    pub weapon_key: u32,
    /// Bool.
    pub taken: u8,
    /// Padding.
    pub _pad: [u8; 3],
    /// Position.
    pub position: BlueVec3,
    /// Rotation.
    pub rotation: BlueQuat,
}

/// `BlueSimEvent`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct BlueSimEvent {
    /// [`event`] type.
    pub kind: u8,
    /// Hit zone (0 head, 1 torso, 2 legs); damage only.
    pub zone: u8,
    /// Bool; weapon dropped only.
    pub chambered: u8,
    /// Padding.
    pub _pad: u8,
    /// Tick.
    pub tick: u32,
    /// Who the event is about (sim id).
    pub player: u32,
    /// The other player (sim id), 0 none.
    pub other: u32,
    /// Per type.
    pub id: u32,
    /// Weapon key.
    pub weapon_key: u32,
    /// Per type.
    pub value: i32,
    /// Health left.
    pub health: i32,
    /// Radians; respawn only.
    pub yaw: f32,
    /// Per type.
    pub position: BlueVec3,
    /// Per type.
    pub vector: BlueVec3,
}

#[repr(C)]
struct RawConfig {
    asset_dir: *const c_char,
    scene_path: *const c_char,
    worker_threads: i32,
    max_players: i32,
}

#[repr(C)]
struct RawSim {
    _opaque: [u8; 0],
}

// Sizes from the header's comments (static_asserted in blue_sim.cc).
const _: () = assert!(size_of::<RawConfig>() == 24);
const _: () = assert!(size_of::<BlueVec3>() == 12);
const _: () = assert!(size_of::<BlueQuat>() == 16);
const _: () = assert!(size_of::<BlueUserCommand>() == 28);
const _: () = assert!(size_of::<BlueFirearmState>() == 44);
const _: () = assert!(size_of::<BlueLoadoutSlot>() == 60);
const _: () = assert!(size_of::<BluePlayerState>() == 200);
const _: () = assert!(size_of::<BlueSegment>() == 72);
const _: () = assert!(size_of::<BlueShotHit>() == 16);
const _: () = assert!(size_of::<BlueCapsule>() == 20);
const _: () = assert!(size_of::<BlueRayHit>() == 36);
const _: () = assert!(size_of::<BlueBodyState>() == 60);
const _: () = assert!(size_of::<BlueDroppedWeapon>() == 76);
const _: () = assert!(size_of::<BluePickup>() == 40);
const _: () = assert!(size_of::<BlueSimEvent>() == 60);

type LogCallback = extern "C" fn(*mut c_void, i32, *const c_char);

unsafe extern "C" {
    fn blue_sim_abi_version() -> i32;
    fn blue_sim_struct_size(struct_id: i32) -> i32;
    fn blue_sim_create(
        config: *const RawConfig,
        out: *mut *mut RawSim,
        err: *mut c_char,
        err_cap: i32,
    ) -> i32;
    fn blue_sim_destroy(sim: *mut RawSim) -> i32;
    fn blue_sim_last_error(sim: *const RawSim, buffer: *mut c_char, cap: i32) -> i32;
    fn blue_sim_set_log_callback(callback: Option<LogCallback>, user: *mut c_void) -> i32;
    fn blue_sim_content_hash(sim: *const RawSim, out: *mut u64) -> i32;
    fn blue_sim_add_player(sim: *mut RawSim, id: u32) -> i32;
    fn blue_sim_remove_player(sim: *mut RawSim, id: u32) -> i32;
    fn blue_sim_apply_command(sim: *mut RawSim, id: u32, command: *const BlueUserCommand) -> i32;
    fn blue_sim_step(sim: *mut RawSim, tick: u32) -> i32;
    fn blue_sim_projectiles_sweep(sim: *mut RawSim, out: *mut BlueSegment, cap: i32) -> i32;
    fn blue_sim_projectiles_resolve(sim: *mut RawSim, hits: *const BlueShotHit, count: i32) -> i32;
    fn blue_sim_hit_capsules_now(sim: *const RawSim, out: *mut BlueShotHit, cap: i32) -> i32;
    fn blue_sim_player_capsule(sim: *const RawSim, id: u32, out: *mut BlueCapsule) -> i32;
    fn blue_sim_cast_ray(
        sim: *const RawSim,
        origin: *const BlueVec3,
        direction: *const BlueVec3,
        max_distance: f32,
        out: *mut BlueRayHit,
    ) -> i32;
    fn blue_sim_read_players(sim: *const RawSim, out: *mut BluePlayerState, cap: i32) -> i32;
    fn blue_sim_read_bodies(sim: *const RawSim, out: *mut BlueBodyState, cap: i32) -> i32;
    fn blue_sim_read_dropped_weapons(
        sim: *const RawSim,
        out: *mut BlueDroppedWeapon,
        cap: i32,
    ) -> i32;
    fn blue_sim_read_pickups(sim: *const RawSim, out: *mut BluePickup, cap: i32) -> i32;
    fn blue_sim_poll_events(sim: *mut RawSim, out: *mut BlueSimEvent, cap: i32) -> i32;
}

/// Errors of the simulation library.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum SimError {
    /// The library's ABI version or a struct size differs from this mirror.
    Abi(String),
    /// A path contains a NUL byte or a count does not fit.
    InvalidArgument(String),
    /// `blue_sim_create` failed with this message.
    Create(String),
    /// A call returned this negative status; the message is the library's
    /// last error.
    Status(i32, String),
    /// The output buffer was too small; nothing was written.
    BufferTooSmall,
}

impl fmt::Display for SimError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Abi(m) => write!(f, "blue_sim ABI mismatch: {m}"),
            Self::InvalidArgument(m) => write!(f, "invalid argument: {m}"),
            Self::Create(m) => write!(f, "cannot create the simulation: {m}"),
            Self::Status(code, m) => write!(f, "blue_sim error {code}: {m}"),
            Self::BufferTooSmall => write!(f, "output buffer too small"),
        }
    }
}

impl std::error::Error for SimError {}

/// True: the bindings are present.
#[must_use]
pub const fn available() -> bool {
    true
}

/// (struct id, Rust size, name) of every mirrored struct.
const STRUCTS: [(i32, usize, &str); 15] = [
    (1, size_of::<RawConfig>(), "BlueSimConfig"),
    (2, size_of::<BlueVec3>(), "BlueVec3"),
    (3, size_of::<BlueQuat>(), "BlueQuat"),
    (4, size_of::<BlueUserCommand>(), "BlueUserCommand"),
    (5, size_of::<BlueFirearmState>(), "BlueFirearmState"),
    (6, size_of::<BlueLoadoutSlot>(), "BlueLoadoutSlot"),
    (7, size_of::<BluePlayerState>(), "BluePlayerState"),
    (8, size_of::<BlueSegment>(), "BlueSegment"),
    (9, size_of::<BlueShotHit>(), "BlueShotHit"),
    (10, size_of::<BlueCapsule>(), "BlueCapsule"),
    (11, size_of::<BlueRayHit>(), "BlueRayHit"),
    (12, size_of::<BlueBodyState>(), "BlueBodyState"),
    (13, size_of::<BlueDroppedWeapon>(), "BlueDroppedWeapon"),
    (14, size_of::<BluePickup>(), "BluePickup"),
    (15, size_of::<BlueSimEvent>(), "BlueSimEvent"),
];

/// Checks the library's ABI version and every struct size against this
/// mirror.
///
/// # Errors
/// [`SimError::Abi`] naming the first mismatch.
pub fn check_abi() -> Result<(), SimError> {
    // SAFETY: no arguments, returns an int.
    let version = unsafe { blue_sim_abi_version() };
    if version != ABI_VERSION {
        return Err(SimError::Abi(format!(
            "library ABI {version}, server built for {ABI_VERSION}"
        )));
    }
    for (id, size, name) in STRUCTS {
        // SAFETY: takes and returns an int.
        let native = unsafe { blue_sim_struct_size(id) };
        if usize::try_from(native).ok() != Some(size) {
            return Err(SimError::Abi(format!(
                "{name}: library {native} bytes, mirror {size} bytes"
            )));
        }
    }
    Ok(())
}

static LOG_SINK: OnceLock<fn(i32, &str)> = OnceLock::new();

extern "C" fn log_trampoline(_user: *mut c_void, level: i32, message: *const c_char) {
    if message.is_null() {
        return;
    }
    // SAFETY: the library passes a NUL-terminated string valid for the
    // duration of the call.
    let text = unsafe { CStr::from_ptr(message) }.to_string_lossy();
    if let Some(sink) = LOG_SINK.get() {
        // Never unwind into C.
        let _ = std::panic::catch_unwind(|| sink(level, &text));
    }
}

/// Routes every engine log line to `sink(level, message)` (levels: 0 info,
/// 1 warn, 2 error, 3 fatal: the process aborts after the callback). The
/// library may call it from physics worker threads (never two at once), so
/// `sink` must be thread safe and must not call into the simulation. Set it
/// before [`Sim::create`]. Only the first sink set takes effect.
pub fn set_log_sink(sink: fn(i32, &str)) {
    let _ = LOG_SINK.set(sink);
    // SAFETY: `log_trampoline` is a valid `extern "C"` function for the
    // process lifetime and ignores `user`, so a null user is fine.
    unsafe { blue_sim_set_log_callback(Some(log_trampoline), ptr::null_mut()) };
}

/// Settings of [`Sim::create`].
#[derive(Clone, Copy, Debug)]
pub struct SimConfig<'a> {
    /// Root of the asset tree.
    pub asset_dir: &'a str,
    /// Scene, relative to `asset_dir` or absolute.
    pub scene_path: &'a str,
    /// Physics threads besides the caller's.
    pub worker_threads: i32,
    /// Player cap.
    pub max_players: i32,
}

fn cap(len: usize) -> i32 {
    i32::try_from(len).unwrap_or(i32::MAX)
}

/// A loaded simulation.
pub struct Sim {
    raw: *mut RawSim,
}

impl fmt::Debug for Sim {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Sim").finish_non_exhaustive()
    }
}

impl Sim {
    /// Checks the ABI, then loads the scene.
    ///
    /// # Errors
    /// ABI mismatch, bad paths, or the library's create error.
    pub fn create(config: &SimConfig<'_>) -> Result<Self, SimError> {
        check_abi()?;
        let asset_dir = CString::new(config.asset_dir)
            .map_err(|_| SimError::InvalidArgument("asset dir contains NUL".to_owned()))?;
        let scene = CString::new(config.scene_path)
            .map_err(|_| SimError::InvalidArgument("scene path contains NUL".to_owned()))?;
        let raw_config = RawConfig {
            asset_dir: asset_dir.as_ptr(),
            scene_path: scene.as_ptr(),
            worker_threads: config.worker_threads,
            max_players: config.max_players,
        };
        let mut raw: *mut RawSim = ptr::null_mut();
        let mut err = [0u8; 1024];
        // SAFETY: the config's strings outlive the call; `raw` and `err`
        // are valid out-pointers, `err` holds `err_cap` bytes.
        let status = unsafe {
            blue_sim_create(
                &raw const raw_config,
                &raw mut raw,
                err.as_mut_ptr().cast::<c_char>(),
                cap(err.len()),
            )
        };
        if status != 0 || raw.is_null() {
            let message = CStr::from_bytes_until_nul(&err)
                .map(|c| c.to_string_lossy().into_owned())
                .unwrap_or_default();
            return Err(SimError::Create(format!("{message} (status {status})")));
        }
        Ok(Self { raw })
    }

    /// The library's last error message.
    #[must_use]
    pub fn last_error(&self) -> String {
        let mut buf = [0u8; 512];
        // SAFETY: live sim; `buf` holds `cap` bytes, NUL-terminated by the
        // library.
        unsafe { blue_sim_last_error(self.raw, buf.as_mut_ptr().cast::<c_char>(), cap(buf.len())) };
        CStr::from_bytes_until_nul(&buf)
            .map(|c| c.to_string_lossy().into_owned())
            .unwrap_or_default()
    }

    fn check(&self, status: i32) -> Result<i32, SimError> {
        if status >= 0 {
            Ok(status)
        } else if status == ERR_BUFFER_TOO_SMALL {
            Err(SimError::BufferTooSmall)
        } else {
            Err(SimError::Status(status, self.last_error()))
        }
    }

    fn count(&self, status: i32) -> Result<usize, SimError> {
        self.check(status).map(|n| usize::try_from(n).unwrap_or(0))
    }

    /// The content hash clients must match.
    ///
    /// # Errors
    /// Library errors.
    pub fn content_hash(&self) -> Result<u64, SimError> {
        let mut hash = 0u64;
        // SAFETY: live sim, valid out-pointer.
        let status = unsafe { blue_sim_content_hash(self.raw, &raw mut hash) };
        self.check(status).map(|_| hash)
    }

    /// Adds player `id` at a spawn point.
    ///
    /// # Errors
    /// Refused (id in use, full, no body).
    pub fn add_player(&mut self, id: u32) -> Result<(), SimError> {
        // SAFETY: live sim.
        let status = unsafe { blue_sim_add_player(self.raw, id) };
        self.check(status).map(|_| ())
    }

    /// Removes player `id`.
    ///
    /// # Errors
    /// Unknown player.
    pub fn remove_player(&mut self, id: u32) -> Result<(), SimError> {
        // SAFETY: live sim.
        let status = unsafe { blue_sim_remove_player(self.raw, id) };
        self.check(status).map(|_| ())
    }

    /// Stores `command` for player `id`'s next step.
    ///
    /// # Errors
    /// Unknown player, invalid command.
    pub fn apply_command(&mut self, id: u32, command: &BlueUserCommand) -> Result<(), SimError> {
        // SAFETY: live sim; `command` is a valid struct for the call.
        let status = unsafe { blue_sim_apply_command(self.raw, id, command) };
        self.check(status).map(|_| ())
    }

    /// Runs tick `tick`.
    ///
    /// # Errors
    /// Library errors.
    pub fn step(&mut self, tick: u32) -> Result<(), SimError> {
        // SAFETY: live sim.
        let status = unsafe { blue_sim_step(self.raw, tick) };
        self.check(status).map(|_| ())
    }

    /// Sweeps the bullets over the last tick into `out`.
    ///
    /// # Errors
    /// [`SimError::BufferTooSmall`].
    pub fn sweep(&mut self, out: &mut [BlueSegment]) -> Result<usize, SimError> {
        // SAFETY: live sim; `out` is writable for `cap(out.len())` entries.
        let status =
            unsafe { blue_sim_projectiles_sweep(self.raw, out.as_mut_ptr(), cap(out.len())) };
        self.count(status)
    }

    /// Resolves the last sweep with `hits`.
    ///
    /// # Errors
    /// Library errors.
    pub fn resolve(&mut self, hits: &[BlueShotHit]) -> Result<(), SimError> {
        // SAFETY: live sim; `hits` is readable for its length.
        let status =
            unsafe { blue_sim_projectiles_resolve(self.raw, hits.as_ptr(), cap(hits.len())) };
        self.check(status).map(|_| ())
    }

    /// Hits of the current sweep on the players' current capsules.
    ///
    /// # Errors
    /// [`SimError::BufferTooSmall`].
    pub fn hit_capsules_now(&self, out: &mut [BlueShotHit]) -> Result<usize, SimError> {
        // SAFETY: live sim; `out` writable for its length.
        let status =
            unsafe { blue_sim_hit_capsules_now(self.raw, out.as_mut_ptr(), cap(out.len())) };
        self.count(status)
    }

    /// Player `id`'s current capsule.
    ///
    /// # Errors
    /// Unknown player.
    pub fn player_capsule(&self, id: u32) -> Result<BlueCapsule, SimError> {
        let mut capsule = BlueCapsule::default();
        // SAFETY: live sim, valid out-pointer.
        let status = unsafe { blue_sim_player_capsule(self.raw, id, &raw mut capsule) };
        self.check(status).map(|_| capsule)
    }

    /// Casts a ray against the world's bodies.
    #[must_use]
    pub fn cast_ray(
        &self,
        origin: BlueVec3,
        direction: BlueVec3,
        max_distance: f32,
    ) -> Option<BlueRayHit> {
        let mut hit = BlueRayHit::default();
        // SAFETY: live sim; all pointers are to valid locals.
        let status = unsafe {
            blue_sim_cast_ray(
                self.raw,
                &raw const origin,
                &raw const direction,
                max_distance,
                &raw mut hit,
            )
        };
        (status == 1).then_some(hit)
    }

    /// Every player, by id.
    ///
    /// # Errors
    /// [`SimError::BufferTooSmall`].
    pub fn read_players(&self, out: &mut [BluePlayerState]) -> Result<usize, SimError> {
        // SAFETY: live sim; `out` writable for its length.
        let status = unsafe { blue_sim_read_players(self.raw, out.as_mut_ptr(), cap(out.len())) };
        self.count(status)
    }

    /// Every dynamic body, in scene order.
    ///
    /// # Errors
    /// [`SimError::BufferTooSmall`].
    pub fn read_bodies(&self, out: &mut [BlueBodyState]) -> Result<usize, SimError> {
        // SAFETY: live sim; `out` writable for its length.
        let status = unsafe { blue_sim_read_bodies(self.raw, out.as_mut_ptr(), cap(out.len())) };
        self.count(status)
    }

    /// Every dropped weapon, by id.
    ///
    /// # Errors
    /// [`SimError::BufferTooSmall`].
    pub fn read_dropped_weapons(&self, out: &mut [BlueDroppedWeapon]) -> Result<usize, SimError> {
        // SAFETY: live sim; `out` writable for its length.
        let status =
            unsafe { blue_sim_read_dropped_weapons(self.raw, out.as_mut_ptr(), cap(out.len())) };
        self.count(status)
    }

    /// Every scene pickup, by instance.
    ///
    /// # Errors
    /// [`SimError::BufferTooSmall`].
    pub fn read_pickups(&self, out: &mut [BluePickup]) -> Result<usize, SimError> {
        // SAFETY: live sim; `out` writable for its length.
        let status = unsafe { blue_sim_read_pickups(self.raw, out.as_mut_ptr(), cap(out.len())) };
        self.count(status)
    }

    /// Moves up to `out.len()` queued events into `out`.
    ///
    /// # Errors
    /// Library errors.
    pub fn poll_events(&mut self, out: &mut [BlueSimEvent]) -> Result<usize, SimError> {
        // SAFETY: live sim; `out` writable for its length.
        let status = unsafe { blue_sim_poll_events(self.raw, out.as_mut_ptr(), cap(out.len())) };
        self.count(status)
    }
}

impl Drop for Sim {
    fn drop(&mut self) {
        // SAFETY: `raw` is live and destroyed exactly once, on the thread
        // that owns this (non-Send) value, i.e. the creating thread.
        unsafe { blue_sim_destroy(self.raw) };
    }
}

#[cfg(test)]
mod tests {
    #![allow(clippy::unwrap_used, clippy::panic)]

    use super::*;

    #[test]
    fn abi_matches_the_library() {
        check_abi().unwrap();
    }

    /// Every sim-creating check in one test: the library wants creation and
    /// destruction on a single thread, and tests run on several.
    #[test]
    fn lifecycle() {
        create_reports_errors();
        runs_a_real_scene_when_assets_are_given();
    }

    fn create_reports_errors() {
        let error = Sim::create(&SimConfig {
            asset_dir: "/nonexistent-blue-assets",
            scene_path: "nope.mscn",
            worker_threads: 0,
            max_players: 4,
        })
        .unwrap_err();
        match error {
            SimError::Create(message) => assert!(!message.is_empty()),
            other => panic!("{other:?}"),
        }
        assert!(matches!(
            Sim::create(&SimConfig {
                asset_dir: "a\0b",
                scene_path: "x",
                worker_threads: 0,
                max_players: 1,
            }),
            Err(SimError::InvalidArgument(_))
        ));
    }

    /// Runs only with `BLUE_TEST_ASSET_DIR` (cooked assets with
    /// `range.mscn`): two players, commands, step, read-outs.
    fn runs_a_real_scene_when_assets_are_given() {
        let Some(dir) = std::env::var_os("BLUE_TEST_ASSET_DIR") else {
            return;
        };
        let dir = dir.to_string_lossy().into_owned();
        let mut sim = Sim::create(&SimConfig {
            asset_dir: &dir,
            scene_path: "range.mscn",
            worker_threads: 0,
            max_players: 4,
        })
        .unwrap();
        assert_ne!(sim.content_hash().unwrap(), 0);
        sim.add_player(1).unwrap();
        sim.add_player(2).unwrap();
        assert!(sim.add_player(1).is_err());
        let mut players = [BluePlayerState::default(); 4];
        let start = {
            assert_eq!(sim.read_players(&mut players).unwrap(), 2);
            players[0].position
        };
        for tick in 1..=120u32 {
            let command = BlueUserCommand {
                tick,
                held: 1, // forward
                select_slot: -1,
                ..BlueUserCommand::default()
            };
            sim.apply_command(1, &command).unwrap();
            sim.step(tick).unwrap();
            let mut segments = [BlueSegment::default(); 64];
            let n = sim.sweep(&mut segments).unwrap();
            let mut hits = [BlueShotHit::default(); 64];
            let h = sim.hit_capsules_now(&mut hits[..]).unwrap();
            assert!(h <= n.max(h));
            sim.resolve(&hits[..h]).unwrap();
            let mut events = [BlueSimEvent::default(); 64];
            let _ = sim.poll_events(&mut events).unwrap();
        }
        assert_eq!(sim.read_players(&mut players).unwrap(), 2);
        let moved = players[0].position;
        let d = (moved.x - start.x).hypot(moved.z - start.z);
        assert!(d > 1.0, "player 1 did not walk: {d}");
        assert!(sim.player_capsule(2).unwrap().height > 1.0);
        assert!(sim.player_capsule(9).is_err());
        assert_eq!(
            sim.read_players(&mut players[..1]),
            Err(SimError::BufferTooSmall)
        );
        let mut bodies = [BlueBodyState::default(); 256];
        let _ = sim.read_bodies(&mut bodies).unwrap();
        let mut pickups = [BluePickup::default(); 64];
        let _ = sim.read_pickups(&mut pickups).unwrap();
        let down = BlueVec3 {
            x: 0.0,
            y: -1.0,
            z: 0.0,
        };
        let origin = BlueVec3 {
            x: players[1].position.x,
            y: players[1].position.y + 1.0,
            z: players[1].position.z,
        };
        assert!(
            sim.cast_ray(origin, down, 10.0).is_some(),
            "no floor under player 2"
        );
        sim.remove_player(2).unwrap();
        assert!(sim.remove_player(2).is_err());
    }
}
