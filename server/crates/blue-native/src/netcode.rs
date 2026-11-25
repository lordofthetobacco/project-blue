//! netcode.io (`mas-bandwidth/netcode` v1.5.0): hand-written FFI declarations
//! and RAII wrappers for the server, the client and connect-token generation.
//!
//! Time is always passed explicitly (seconds, monotonically non-decreasing
//! per object). Received payloads are copied into caller buffers and the
//! library's packet is freed immediately, so no borrow of library memory
//! escapes a call.

use std::ffi::{CStr, CString, c_char, c_int, c_void};
use std::fmt;
use std::ptr;
use std::sync::Mutex;
use std::sync::atomic::{AtomicI32, Ordering};

/// Bytes of a connect token.
pub const CONNECT_TOKEN_BYTES: usize = 2048;
/// Bytes of a private key.
pub const KEY_BYTES: usize = 32;
/// Bytes of the user data carried in a connect token.
pub const USER_DATA_BYTES: usize = 256;
/// Largest payload netcode carries.
pub const MAX_PACKET_SIZE: usize = 1200;
/// Most clients one server can hold.
pub const MAX_CLIENTS: usize = 256;
const MAX_ADDRESS_STRING_LENGTH: usize = 256;
const NETCODE_OK: c_int = 1;

/// netcode log levels.
pub mod log_level {
    /// Silent (default here: netcode prints with `printf`, which would mix
    /// unstructured lines into the server's structured log).
    pub const NONE: i32 = 0;
    /// Errors only.
    pub const ERROR: i32 = 1;
    /// Informational.
    pub const INFO: i32 = 2;
    /// Everything.
    pub const DEBUG: i32 = 3;
}

#[repr(C)]
#[derive(Clone, Copy)]
struct RawAddress {
    data: [u16; 8],
    port: u16,
    kind: u8,
}

/// Mirror of `struct netcode_client_config_t` (LP64). Function pointers are
/// stored as opaque pointers: Rust never calls or sets them except through
/// `netcode_default_client_config`.
#[repr(C)]
struct RawClientConfig {
    allocator_context: *mut c_void,
    allocate_function: *mut c_void,
    free_function: *mut c_void,
    network_simulator: *mut c_void,
    callback_context: *mut c_void,
    state_change_callback: *mut c_void,
    send_loopback_packet_callback: *mut c_void,
    override_send_and_receive: c_int,
    send_packet_override: *mut c_void,
    receive_packet_override: *mut c_void,
}

/// Mirror of `struct netcode_server_config_t` (LP64).
#[repr(C)]
struct RawServerConfig {
    protocol_id: u64,
    private_key: [u8; KEY_BYTES],
    allocator_context: *mut c_void,
    allocate_function: *mut c_void,
    free_function: *mut c_void,
    network_simulator: *mut c_void,
    callback_context: *mut c_void,
    connect_disconnect_callback: *mut c_void,
    send_loopback_packet_callback: *mut c_void,
    override_send_and_receive: c_int,
    send_packet_override: *mut c_void,
    receive_packet_override: *mut c_void,
    max_connect_token_lifetime: c_int,
}

const _: () = assert!(size_of::<RawAddress>() == 20);
const _: () = assert!(size_of::<RawClientConfig>() == 80);
const _: () = assert!(size_of::<RawServerConfig>() == 128);

#[repr(C)]
struct RawServer {
    _opaque: [u8; 0],
}

#[repr(C)]
struct RawClient {
    _opaque: [u8; 0],
}

unsafe extern "C" {
    fn netcode_init() -> c_int;
    fn netcode_term();
    fn netcode_log_level(level: c_int);
    fn netcode_address_to_string(address: *mut RawAddress, buffer: *mut c_char) -> *mut c_char;

    fn netcode_default_client_config(config: *mut RawClientConfig);
    fn netcode_client_create(
        address: *const c_char,
        config: *const RawClientConfig,
        time: f64,
    ) -> *mut RawClient;
    fn netcode_client_create_error() -> c_int;
    fn netcode_client_destroy(client: *mut RawClient);
    fn netcode_client_connect(client: *mut RawClient, connect_token: *mut u8);
    fn netcode_client_update(client: *mut RawClient, time: f64);
    fn netcode_client_send_packet(client: *mut RawClient, data: *const u8, bytes: c_int);
    fn netcode_client_receive_packet(
        client: *mut RawClient,
        bytes: *mut c_int,
        sequence: *mut u64,
    ) -> *mut u8;
    fn netcode_client_free_packet(client: *mut RawClient, packet: *mut c_void);
    fn netcode_client_disconnect(client: *mut RawClient);
    fn netcode_client_state(client: *mut RawClient) -> c_int;
    fn netcode_client_index(client: *mut RawClient) -> c_int;

    fn netcode_generate_connect_token(
        num_server_addresses: c_int,
        public_server_addresses: *const *const c_char,
        internal_server_addresses: *const *const c_char,
        expire_seconds: c_int,
        timeout_seconds: c_int,
        client_id: u64,
        protocol_id: u64,
        private_key: *const u8,
        user_data: *mut u8,
        connect_token: *mut u8,
    ) -> c_int;

    fn netcode_default_server_config(config: *mut RawServerConfig);
    fn netcode_server_create(
        address: *const c_char,
        config: *const RawServerConfig,
        time: f64,
    ) -> *mut RawServer;
    fn netcode_server_create_error() -> c_int;
    fn netcode_server_destroy(server: *mut RawServer);
    fn netcode_server_start(server: *mut RawServer, max_clients: c_int);
    fn netcode_server_stop(server: *mut RawServer);
    fn netcode_server_update(server: *mut RawServer, time: f64);
    fn netcode_server_client_connected(server: *mut RawServer, client_index: c_int) -> c_int;
    fn netcode_server_client_id(server: *mut RawServer, client_index: c_int) -> u64;
    fn netcode_server_client_address(
        server: *mut RawServer,
        client_index: c_int,
    ) -> *mut RawAddress;
    fn netcode_server_disconnect_client(server: *mut RawServer, client_index: c_int);
    fn netcode_server_disconnect_all_clients(server: *mut RawServer);
    fn netcode_server_send_packet(
        server: *mut RawServer,
        client_index: c_int,
        data: *const u8,
        bytes: c_int,
    );
    fn netcode_server_receive_packet(
        server: *mut RawServer,
        client_index: c_int,
        bytes: *mut c_int,
        sequence: *mut u64,
    ) -> *mut u8;
    fn netcode_server_free_packet(server: *mut RawServer, packet: *mut c_void);
    fn netcode_server_num_connected_clients(server: *mut RawServer) -> c_int;
    fn netcode_server_get_port(server: *mut RawServer) -> u16;
}

/// Errors of this module.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum NetcodeError {
    /// `netcode_init` failed (libsodium could not initialise).
    Init,
    /// An address string contains a NUL byte.
    BadAddress,
    /// `netcode_server_create` failed; the value is its create-error code
    /// (`5` = the port is already in use).
    ServerCreate(i32),
    /// `netcode_client_create` failed; the value is its create-error code.
    ClientCreate(i32),
    /// Connect token generation failed (bad address or arguments).
    Token,
    /// A payload is empty or larger than [`MAX_PACKET_SIZE`].
    PayloadSize,
    /// A client index is outside `0..max_clients`.
    BadClientIndex,
}

impl fmt::Display for NetcodeError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Init => write!(f, "netcode_init failed"),
            Self::BadAddress => write!(f, "address contains a NUL byte"),
            Self::ServerCreate(5) => write!(f, "cannot bind the server port (already in use?)"),
            Self::ServerCreate(code) => write!(f, "netcode_server_create failed (error {code})"),
            Self::ClientCreate(code) => write!(f, "netcode_client_create failed (error {code})"),
            Self::Token => write!(f, "connect token generation failed"),
            Self::PayloadSize => write!(f, "payload size out of range"),
            Self::BadClientIndex => write!(f, "client index out of range"),
        }
    }
}

impl std::error::Error for NetcodeError {}

// Process-wide netcode initialisation. `netcode_init`'s own reference count is
// not thread safe, so every acquire/release goes through this mutex.
static USERS: Mutex<usize> = Mutex::new(0);
static LOG_LEVEL: AtomicI32 = AtomicI32::new(log_level::NONE);

/// Sets netcode's log level for this process (takes effect immediately if
/// netcode is initialised, otherwise at initialisation).
pub fn set_log_level(level: i32) {
    LOG_LEVEL.store(level, Ordering::Relaxed);
    let users = USERS
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner);
    if *users > 0 {
        // SAFETY: netcode is initialised (users > 0, guarded by the mutex);
        // netcode_log_level only stores an int.
        unsafe { netcode_log_level(level) };
    }
}

/// Keeps netcode initialised while alive. Every server and client owns one.
struct LibraryRef;

impl LibraryRef {
    fn acquire() -> Result<Self, NetcodeError> {
        let mut users = USERS
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        if *users == 0 {
            // SAFETY: called with the mutex held, so no other thread runs
            // netcode_init/netcode_term concurrently.
            let ok = unsafe { netcode_init() };
            if ok != NETCODE_OK {
                return Err(NetcodeError::Init);
            }
            // SAFETY: as above; only stores an int.
            unsafe { netcode_log_level(LOG_LEVEL.load(Ordering::Relaxed)) };
        }
        *users += 1;
        Ok(Self)
    }
}

impl Drop for LibraryRef {
    fn drop(&mut self) {
        let mut users = USERS
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        *users = users.saturating_sub(1);
        if *users == 0 {
            // SAFETY: the last server/client is gone (they drop their raw
            // object before this ref), and the mutex serialises init/term.
            unsafe { netcode_term() };
        }
    }
}

fn c_string(text: &str) -> Result<CString, NetcodeError> {
    CString::new(text).map_err(|_| NetcodeError::BadAddress)
}

fn payload_len(payload: &[u8]) -> Result<c_int, NetcodeError> {
    if payload.is_empty() || payload.len() > MAX_PACKET_SIZE {
        return Err(NetcodeError::PayloadSize);
    }
    c_int::try_from(payload.len()).map_err(|_| NetcodeError::PayloadSize)
}

/// Generates a connect token: a client with it may connect to any of
/// `server_addresses` (as `"host:port"`, the server's public address).
///
/// `expire_seconds` is the token's lifetime, `timeout_seconds` the
/// connection timeout the client and server will use.
///
/// # Errors
/// [`NetcodeError::BadAddress`], [`NetcodeError::Token`] (unparsable
/// address, no or more than 32 addresses).
#[allow(clippy::too_many_arguments)]
pub fn generate_connect_token(
    server_addresses: &[&str],
    expire_seconds: i32,
    timeout_seconds: i32,
    client_id: u64,
    protocol_id: u64,
    private_key: &[u8; KEY_BYTES],
    user_data: &[u8; USER_DATA_BYTES],
) -> Result<[u8; CONNECT_TOKEN_BYTES], NetcodeError> {
    if server_addresses.is_empty() || server_addresses.len() > 32 {
        return Err(NetcodeError::Token);
    }
    let _lib = LibraryRef::acquire()?;
    let owned = server_addresses
        .iter()
        .map(|a| c_string(a))
        .collect::<Result<Vec<_>, _>>()?;
    let pointers: Vec<*const c_char> = owned.iter().map(|c| c.as_ptr()).collect();
    let count = c_int::try_from(pointers.len()).map_err(|_| NetcodeError::Token)?;
    let mut user = *user_data;
    let mut token = [0u8; CONNECT_TOKEN_BYTES];
    // SAFETY: `pointers` holds `count` valid NUL-terminated strings that
    // outlive the call (owned by `owned`); netcode only reads them. The key
    // is KEY_BYTES long, `user` USER_DATA_BYTES and `token`
    // CONNECT_TOKEN_BYTES, as netcode expects; netcode is initialised.
    let ok = unsafe {
        netcode_generate_connect_token(
            count,
            pointers.as_ptr(),
            pointers.as_ptr(),
            expire_seconds,
            timeout_seconds,
            client_id,
            protocol_id,
            private_key.as_ptr(),
            user.as_mut_ptr(),
            token.as_mut_ptr(),
        )
    };
    if ok == NETCODE_OK {
        Ok(token)
    } else {
        Err(NetcodeError::Token)
    }
}

/// Settings of a [`NetcodeServer`].
#[derive(Clone, Debug)]
pub struct ServerSettings {
    /// Public address clients connect to, `"host:port"`. It must match the
    /// address in connect tokens. netcode binds `0.0.0.0` (or `[::]`) on its
    /// port regardless of the host part.
    pub public_address: String,
    /// Protocol id (shared with clients and the token issuer).
    pub protocol_id: u64,
    /// Private key shared with the token issuer.
    pub private_key: [u8; KEY_BYTES],
    /// Longest lifetime, seconds, of the tokens the issuer mints.
    pub max_connect_token_lifetime: i32,
}

/// A netcode.io server. `Send` (it may move to the tick thread) but not
/// `Sync`: every call needs `&mut self`.
pub struct NetcodeServer {
    raw: *mut RawServer,
    max_clients: usize,
    // Declared last: dropped after `raw` is destroyed in `Drop::drop`.
    _lib: LibraryRef,
}

// SAFETY: the netcode server object has no thread affinity (it owns its
// socket and buffers); it is only touched through `&mut self`, so moving it
// between threads is sound. It is not `Sync` (raw pointer field).
unsafe impl Send for NetcodeServer {}

impl fmt::Debug for NetcodeServer {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("NetcodeServer")
            .field("max_clients", &self.max_clients)
            .finish_non_exhaustive()
    }
}

impl NetcodeServer {
    /// Creates the server and binds its socket (not yet started).
    ///
    /// # Errors
    /// Init, address and create errors.
    pub fn new(settings: &ServerSettings, time: f64) -> Result<Self, NetcodeError> {
        let lib = LibraryRef::acquire()?;
        let address = c_string(&settings.public_address)?;
        let mut config = RawServerConfig {
            protocol_id: 0,
            private_key: [0; KEY_BYTES],
            allocator_context: ptr::null_mut(),
            allocate_function: ptr::null_mut(),
            free_function: ptr::null_mut(),
            network_simulator: ptr::null_mut(),
            callback_context: ptr::null_mut(),
            connect_disconnect_callback: ptr::null_mut(),
            send_loopback_packet_callback: ptr::null_mut(),
            override_send_and_receive: 0,
            send_packet_override: ptr::null_mut(),
            receive_packet_override: ptr::null_mut(),
            max_connect_token_lifetime: 0,
        };
        // SAFETY: `config` is a valid, writable mirror of the C struct
        // (size checked at compile time); the call fills in defaults.
        unsafe { netcode_default_server_config(&raw mut config) };
        config.protocol_id = settings.protocol_id;
        config.private_key = settings.private_key;
        config.max_connect_token_lifetime = settings.max_connect_token_lifetime;
        // SAFETY: `address` is NUL-terminated and outlives the call, the
        // config is fully initialised; netcode copies both.
        let raw = unsafe { netcode_server_create(address.as_ptr(), &raw const config, time) };
        if raw.is_null() {
            // SAFETY: reads a global error code; no preconditions.
            return Err(NetcodeError::ServerCreate(unsafe {
                netcode_server_create_error()
            }));
        }
        Ok(Self {
            raw,
            max_clients: 0,
            _lib: lib,
        })
    }

    /// Starts accepting up to `max_clients` (1..=256) clients.
    pub fn start(&mut self, max_clients: usize) {
        let max = max_clients.clamp(1, MAX_CLIENTS);
        // SAFETY: `raw` is a live server; `max` is within 1..=256 and fits
        // a c_int.
        unsafe { netcode_server_start(self.raw, max as c_int) };
        self.max_clients = max;
    }

    /// Stops the server: disconnects every client, stops accepting.
    pub fn stop(&mut self) {
        // SAFETY: `raw` is a live server; stopping a stopped server is a
        // no-op in netcode.
        unsafe { netcode_server_stop(self.raw) };
        self.max_clients = 0;
    }

    /// Client slots (0 when not started).
    #[must_use]
    pub fn max_clients(&self) -> usize {
        self.max_clients
    }

    /// The UDP port the server is bound to.
    #[must_use]
    pub fn port(&self) -> u16 {
        // SAFETY: `raw` is a live server.
        unsafe { netcode_server_get_port(self.raw) }
    }

    /// Advances to `time` (seconds): receives packets, sends keep-alives,
    /// times out clients.
    pub fn update(&mut self, time: f64) {
        // SAFETY: `raw` is a live server.
        unsafe { netcode_server_update(self.raw, time) };
    }

    fn index(&self, client: usize) -> Result<c_int, NetcodeError> {
        if client < self.max_clients {
            c_int::try_from(client).map_err(|_| NetcodeError::BadClientIndex)
        } else {
            Err(NetcodeError::BadClientIndex)
        }
    }

    /// Whether slot `client` holds a connected client.
    #[must_use]
    pub fn client_connected(&self, client: usize) -> bool {
        match self.index(client) {
            // SAFETY: live server, index within 0..max_clients.
            Ok(i) => unsafe { netcode_server_client_connected(self.raw, i) != 0 },
            Err(_) => false,
        }
    }

    /// The client id from the connect token of slot `client`.
    #[must_use]
    pub fn client_id(&self, client: usize) -> u64 {
        match self.index(client) {
            // SAFETY: live server, index within 0..max_clients.
            Ok(i) => unsafe { netcode_server_client_id(self.raw, i) },
            Err(_) => 0,
        }
    }

    /// Writes the address of slot `client` into `buf` and returns it.
    pub fn client_address<'b>(&self, client: usize, buf: &'b mut [u8; 256]) -> &'b str {
        let Ok(i) = self.index(client) else {
            return "";
        };
        // SAFETY: live server, valid index; netcode returns a pointer into
        // its own client table (or null for a bad index).
        let address = unsafe { netcode_server_client_address(self.raw, i) };
        if address.is_null() {
            return "";
        }
        // SAFETY: `address` is valid for this call (we hold &self); `buf`
        // is NETCODE_MAX_ADDRESS_STRING_LENGTH bytes and netcode always
        // NUL-terminates within it.
        unsafe { netcode_address_to_string(address, buf.as_mut_ptr().cast::<c_char>()) };
        CStr::from_bytes_until_nul(&buf[..MAX_ADDRESS_STRING_LENGTH])
            .ok()
            .and_then(|c| c.to_str().ok())
            .unwrap_or("")
    }

    /// Disconnects slot `client` (sends disconnect packets).
    pub fn disconnect_client(&mut self, client: usize) {
        if let Ok(i) = self.index(client)
            && self.client_connected(client)
        {
            // SAFETY: live server, valid and connected index.
            unsafe { netcode_server_disconnect_client(self.raw, i) };
        }
    }

    /// Disconnects every client.
    pub fn disconnect_all(&mut self) {
        // SAFETY: live server.
        unsafe { netcode_server_disconnect_all_clients(self.raw) };
    }

    /// Number of connected clients.
    #[must_use]
    pub fn num_connected(&self) -> usize {
        // SAFETY: live server.
        let n = unsafe { netcode_server_num_connected_clients(self.raw) };
        usize::try_from(n).unwrap_or(0)
    }

    /// Sends one payload (1..=1200 bytes) to slot `client`.
    ///
    /// # Errors
    /// [`NetcodeError::PayloadSize`], [`NetcodeError::BadClientIndex`].
    pub fn send(&mut self, client: usize, payload: &[u8]) -> Result<(), NetcodeError> {
        let i = self.index(client)?;
        let len = payload_len(payload)?;
        // SAFETY: live server, valid index, `payload` is readable for `len`
        // (1..=1200) bytes; netcode copies it before returning.
        unsafe { netcode_server_send_packet(self.raw, i, payload.as_ptr(), len) };
        Ok(())
    }

    /// Takes the next received payload of slot `client` into `buf` and
    /// returns its length; `None` when there is none. A payload longer than
    /// `buf` is discarded (`Some(0)` is never returned for it: it yields
    /// `Some(usize::MAX)` so the caller can count it as invalid).
    pub fn receive(&mut self, client: usize, buf: &mut [u8]) -> Option<usize> {
        let i = self.index(client).ok()?;
        let mut bytes: c_int = 0;
        let mut sequence: u64 = 0;
        // SAFETY: live server, valid index, out-pointers are valid.
        let packet = unsafe {
            netcode_server_receive_packet(self.raw, i, &raw mut bytes, &raw mut sequence)
        };
        if packet.is_null() {
            return None;
        }
        let len = usize::try_from(bytes).unwrap_or(0);
        let result = if len <= buf.len() {
            // SAFETY: netcode returned a packet of `bytes` readable bytes
            // that stays valid until freed below; `buf` has room for `len`.
            unsafe { ptr::copy_nonoverlapping(packet, buf.as_mut_ptr(), len) };
            Some(len)
        } else {
            Some(usize::MAX)
        };
        // SAFETY: `packet` came from netcode_server_receive_packet on this
        // server and is freed exactly once.
        unsafe { netcode_server_free_packet(self.raw, packet.cast::<c_void>()) };
        result
    }
}

impl Drop for NetcodeServer {
    fn drop(&mut self) {
        // SAFETY: `raw` is live and destroyed exactly once; `_lib` (dropped
        // after this) keeps netcode initialised until then.
        unsafe { netcode_server_destroy(self.raw) };
    }
}

/// netcode client states.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ClientState {
    /// The connect token expired.
    ConnectTokenExpired,
    /// The server refused the connect token.
    InvalidConnectToken,
    /// The connection timed out after being established.
    ConnectionTimedOut,
    /// No challenge response arrived.
    ConnectionResponseTimedOut,
    /// No answer to the connection request.
    ConnectionRequestTimedOut,
    /// The server is full or refused the client.
    ConnectionDenied,
    /// Not connected.
    Disconnected,
    /// Sending connection requests.
    SendingConnectionRequest,
    /// Sending challenge responses.
    SendingConnectionResponse,
    /// Connected.
    Connected,
}

impl ClientState {
    fn from_raw(state: c_int) -> Self {
        match state {
            -6 => Self::ConnectTokenExpired,
            -5 => Self::InvalidConnectToken,
            -4 => Self::ConnectionTimedOut,
            -3 => Self::ConnectionResponseTimedOut,
            -2 => Self::ConnectionRequestTimedOut,
            -1 => Self::ConnectionDenied,
            1 => Self::SendingConnectionRequest,
            2 => Self::SendingConnectionResponse,
            3 => Self::Connected,
            _ => Self::Disconnected,
        }
    }

    /// True for the terminal error states.
    #[must_use]
    pub fn is_error(self) -> bool {
        matches!(
            self,
            Self::ConnectTokenExpired
                | Self::InvalidConnectToken
                | Self::ConnectionTimedOut
                | Self::ConnectionResponseTimedOut
                | Self::ConnectionRequestTimedOut
                | Self::ConnectionDenied
        )
    }
}

/// A netcode.io client. `Send` but not `Sync`.
pub struct NetcodeClient {
    raw: *mut RawClient,
    _lib: LibraryRef,
}

// SAFETY: as for NetcodeServer: no thread affinity, `&mut self` access only.
unsafe impl Send for NetcodeClient {}

impl fmt::Debug for NetcodeClient {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("NetcodeClient").finish_non_exhaustive()
    }
}

impl NetcodeClient {
    /// Creates a client bound to `bind_address` (e.g. `"0.0.0.0"` for any
    /// local port).
    ///
    /// # Errors
    /// Init, address and create errors.
    pub fn new(bind_address: &str, time: f64) -> Result<Self, NetcodeError> {
        let lib = LibraryRef::acquire()?;
        let address = c_string(bind_address)?;
        let mut config = RawClientConfig {
            allocator_context: ptr::null_mut(),
            allocate_function: ptr::null_mut(),
            free_function: ptr::null_mut(),
            network_simulator: ptr::null_mut(),
            callback_context: ptr::null_mut(),
            state_change_callback: ptr::null_mut(),
            send_loopback_packet_callback: ptr::null_mut(),
            override_send_and_receive: 0,
            send_packet_override: ptr::null_mut(),
            receive_packet_override: ptr::null_mut(),
        };
        // SAFETY: `config` is a valid writable mirror of the C struct.
        unsafe { netcode_default_client_config(&raw mut config) };
        // SAFETY: NUL-terminated address and initialised config, both copied.
        let raw = unsafe { netcode_client_create(address.as_ptr(), &raw const config, time) };
        if raw.is_null() {
            // SAFETY: reads a global error code.
            return Err(NetcodeError::ClientCreate(unsafe {
                netcode_client_create_error()
            }));
        }
        Ok(Self { raw, _lib: lib })
    }

    /// Starts connecting with `token`.
    pub fn connect(&mut self, token: &[u8; CONNECT_TOKEN_BYTES]) {
        let mut copy = *token;
        // SAFETY: live client; netcode reads CONNECT_TOKEN_BYTES from the
        // (mutable, owned) copy and does not keep the pointer.
        unsafe { netcode_client_connect(self.raw, copy.as_mut_ptr()) };
    }

    /// Advances to `time` (seconds).
    pub fn update(&mut self, time: f64) {
        // SAFETY: live client.
        unsafe { netcode_client_update(self.raw, time) };
    }

    /// Current state.
    #[must_use]
    pub fn state(&self) -> ClientState {
        // SAFETY: live client.
        ClientState::from_raw(unsafe { netcode_client_state(self.raw) })
    }

    /// Slot index on the server once connected.
    #[must_use]
    pub fn index(&self) -> i32 {
        // SAFETY: live client.
        unsafe { netcode_client_index(self.raw) }
    }

    /// Sends one payload (1..=1200 bytes) if connected.
    ///
    /// # Errors
    /// [`NetcodeError::PayloadSize`].
    pub fn send(&mut self, payload: &[u8]) -> Result<(), NetcodeError> {
        let len = payload_len(payload)?;
        // SAFETY: live client; `payload` readable for `len` bytes, copied.
        unsafe { netcode_client_send_packet(self.raw, payload.as_ptr(), len) };
        Ok(())
    }

    /// Takes the next received payload into `buf`; see
    /// [`NetcodeServer::receive`] for the oversize convention.
    pub fn receive(&mut self, buf: &mut [u8]) -> Option<usize> {
        let mut bytes: c_int = 0;
        let mut sequence: u64 = 0;
        // SAFETY: live client, valid out-pointers.
        let packet =
            unsafe { netcode_client_receive_packet(self.raw, &raw mut bytes, &raw mut sequence) };
        if packet.is_null() {
            return None;
        }
        let len = usize::try_from(bytes).unwrap_or(0);
        let result = if len <= buf.len() {
            // SAFETY: `packet` holds `len` readable bytes until freed; `buf`
            // has room for them.
            unsafe { ptr::copy_nonoverlapping(packet, buf.as_mut_ptr(), len) };
            Some(len)
        } else {
            Some(usize::MAX)
        };
        // SAFETY: freed exactly once, on the client that returned it.
        unsafe { netcode_client_free_packet(self.raw, packet.cast::<c_void>()) };
        result
    }

    /// Disconnects (sends disconnect packets).
    pub fn disconnect(&mut self) {
        // SAFETY: live client; a no-op when not connected.
        unsafe { netcode_client_disconnect(self.raw) };
    }
}

impl Drop for NetcodeClient {
    fn drop(&mut self) {
        // SAFETY: live client destroyed exactly once, before `_lib` drops.
        unsafe { netcode_client_destroy(self.raw) };
    }
}

#[cfg(test)]
mod tests {
    #![allow(clippy::unwrap_used, clippy::panic)]

    use super::*;

    const KEY: [u8; KEY_BYTES] = [7; KEY_BYTES];
    const PROTOCOL: u64 = 0x1234_5678;

    #[test]
    fn default_server_config_layout() {
        let _lib = LibraryRef::acquire().ok();
        let mut config = RawServerConfig {
            protocol_id: 0,
            private_key: [0; KEY_BYTES],
            allocator_context: ptr::null_mut(),
            allocate_function: ptr::null_mut(),
            free_function: ptr::null_mut(),
            network_simulator: ptr::null_mut(),
            callback_context: ptr::null_mut(),
            connect_disconnect_callback: ptr::null_mut(),
            send_loopback_packet_callback: ptr::null_mut(),
            override_send_and_receive: 7,
            send_packet_override: ptr::null_mut(),
            receive_packet_override: ptr::null_mut(),
            max_connect_token_lifetime: 0,
        };
        // SAFETY: valid writable mirror.
        unsafe { netcode_default_server_config(&raw mut config) };
        // The last field sits where C put it: the default is 30 s.
        assert_eq!(config.max_connect_token_lifetime, 30);
        assert_eq!(config.override_send_and_receive, 0);
        assert!(!config.allocate_function.is_null());
        assert!(!config.free_function.is_null());
        assert!(config.connect_disconnect_callback.is_null());
    }

    #[test]
    fn default_client_config_layout() {
        let _lib = LibraryRef::acquire().ok();
        let mut config = RawClientConfig {
            allocator_context: ptr::null_mut(),
            allocate_function: ptr::null_mut(),
            free_function: ptr::null_mut(),
            network_simulator: ptr::null_mut(),
            callback_context: ptr::null_mut(),
            state_change_callback: ptr::null_mut(),
            send_loopback_packet_callback: ptr::null_mut(),
            override_send_and_receive: 7,
            send_packet_override: ptr::null_mut(),
            receive_packet_override: ptr::null_mut(),
        };
        // SAFETY: valid writable mirror.
        unsafe { netcode_default_client_config(&raw mut config) };
        assert_eq!(config.override_send_and_receive, 0);
        assert!(!config.allocate_function.is_null());
        assert!(config.receive_packet_override.is_null());
    }

    #[test]
    fn token_rejects_bad_input() {
        let user = [0u8; USER_DATA_BYTES];
        assert_eq!(
            generate_connect_token(&[], 30, 5, 1, PROTOCOL, &KEY, &user),
            Err(NetcodeError::Token)
        );
        assert_eq!(
            generate_connect_token(&["not an address"], 30, 5, 1, PROTOCOL, &KEY, &user),
            Err(NetcodeError::Token)
        );
        assert_eq!(
            generate_connect_token(&["a\0b"], 30, 5, 1, PROTOCOL, &KEY, &user),
            Err(NetcodeError::BadAddress)
        );
        assert!(
            generate_connect_token(&["127.0.0.1:40000"], 30, 5, 1, PROTOCOL, &KEY, &user).is_ok()
        );
    }

    #[test]
    fn loopback_round_trip() {
        let address = "127.0.0.1:40917";
        let settings = ServerSettings {
            public_address: address.to_owned(),
            protocol_id: PROTOCOL,
            private_key: KEY,
            max_connect_token_lifetime: 30,
        };
        let mut time = 0.0;
        let Ok(mut server) = NetcodeServer::new(&settings, time) else {
            panic!("server create");
        };
        server.start(4);
        assert_eq!(server.max_clients(), 4);
        assert_eq!(server.port(), 40917);
        let user = [0u8; USER_DATA_BYTES];
        let Ok(token) = generate_connect_token(&[address], 60, 5, 42, PROTOCOL, &KEY, &user) else {
            panic!("token");
        };
        let Ok(mut client) = NetcodeClient::new("0.0.0.0", time) else {
            panic!("client create");
        };
        client.connect(&token);
        let mut buf = [0u8; MAX_PACKET_SIZE];
        let mut got_server = false;
        let mut got_client = false;
        for _ in 0..400 {
            time += 0.01;
            client.update(time);
            server.update(time);
            if client.state() == ClientState::Connected {
                let _ = client.send(b"ping");
                if server.client_connected(0) {
                    assert_eq!(server.client_id(0), 42);
                    let _ = server.send(0, b"pong");
                }
            }
            while let Some(n) = server.receive(0, &mut buf) {
                got_server |= &buf[..n] == b"ping";
            }
            while let Some(n) = client.receive(&mut buf) {
                got_client |= &buf[..n] == b"pong";
            }
            if got_server && got_client {
                break;
            }
            std::thread::sleep(std::time::Duration::from_millis(1));
        }
        assert!(got_server && got_client);
        assert_eq!(server.num_connected(), 1);
        let mut addr = [0u8; 256];
        assert!(server.client_address(0, &mut addr).starts_with("127.0.0.1"));
        assert_eq!(server.client_address(99, &mut addr), "");
        assert_eq!(server.send(9, b"x"), Err(NetcodeError::BadClientIndex));
        assert_eq!(server.send(0, &[]), Err(NetcodeError::PayloadSize));
        assert_eq!(client.send(&[0; 1201]), Err(NetcodeError::PayloadSize));
        client.disconnect();
        server.disconnect_all();
        server.stop();
    }
}
