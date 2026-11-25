# Project Blue wire protocol

Normative specification of everything Project Blue sends inside a netcode.io
payload. The Rust reference implementation is `server/crates/blue-protocol`;
the C++ client codec must follow this document and reproduce the golden vectors
in `server/crates/blue-protocol/tests/vectors/` byte for byte.

**Protocol version: 3** (`PROTOCOL_VERSION`). Layouts marked *provisional*
follow the sim core and will change with it; every change bumps the version
and gets a changelog entry (section 14).

Key words: *must* is a hard requirement; a decoder that meets a violation
rejects the whole payload with an error (section 12).

## 1. Transport and constants

- Transport is netcode.io (`mas-bandwidth/netcode` v1.5.0, protocol 1.03) on
  both sides. It supplies connection, encryption, authentication, replay
  protection, keep-alives and timeouts; this protocol adds none of those.
- One netcode payload carries exactly one packet of this protocol.
- Byte order inside the packet is defined by the bit stream (section 2); there
  are no byte-aligned fields.

| Constant | Value | Meaning |
|---|---|---|
| `PROTOCOL_VERSION` | 3 | sent in `ClientHello` |
| `MAX_PAYLOAD_BYTES` | 1200 | no packet is larger |
| `DEFAULT_TICK_RATE` | 60 | ticks per second (the physics fixed step) |
| `MAX_PLAYERS` | 64 | player ids are `0..=63` |
| `DEFAULT_MAX_PLAYERS` | 16 | default server cap |
| `NAME_MAX_BYTES` | 32 | player names, UTF-8 bytes |
| `MAX_COMMANDS_PER_PACKET` | 16 | commands per `ClientInput` |
| `MAX_MESSAGES_PER_PACKET` | 32 | reliable messages per packet |
| `WINDOW` | 256 | reliable send/receive window (message ids) |
| `SENT_HISTORY` | 256 | sent packets remembered for acks |
| `MAX_BASELINE_AGE` | 255 | oldest encodable baseline, ticks |
| `BASELINE_HISTORY` | 64 | baselines a client keeps; servers use no older |
| `MAX_OBJECTS_PER_PACKET` | 96 | world objects per snapshot |
| `SYNC_PICKUPS_PER_MESSAGE` | 16 | ids per `WorldSyncPickupsTaken` |
| default reliable budget | 300 bytes | per packet, both directions |
| default resend interval | 100 ms | reliable messages |

netcode.io parameters (not part of the payload format, but both sides must
agree):

| Parameter | Value |
|---|---|
| `protocol_id` | `0x424C55455F4D5031` ("BLUE_MP1") |
| server `max_connect_token_lifetime` | 30 s |
| development token lifetime (`expire_seconds`) | 60 s (longer than 30 s so a token minted in the server's first second is accepted) |
| token `timeout_seconds` | 5 |
| development private key | the 32 ASCII bytes `blue-dev-key-do-not-use-in-prod!` (refused when `BLUE_ENV=production`) |
| server address in tokens | the server's public address (`BLUE_PUBLIC_ADDR`) |

The server gives each client the player id equal to its netcode client index
(`0..max_players`).

## 2. Bit stream

- **Bit order.** Stream bit `i` is bit `i mod 8` (value `1 << (i mod 8)`) of
  byte `i / 8`. A field of `n` bits is written least significant bit first:
  bit `k` of the value goes to stream bit `p + k`, where `p` is the current
  position. Example: writing 5 in 3 bits, then 1 in 1 bit, then `0xAB` in 8
  bits yields bytes `BD 0A`.
- **Padding.** After the last field the writer appends zero bits up to the next
  byte boundary. The packet length is `ceil(bits / 8)`.
- **End check.** After decoding the last field, fewer than 8 bits must remain
  and all of them must be 0; otherwise the payload is rejected
  (`TrailingData`).
- Every read is bounds checked; reading past the end rejects the payload
  (`Truncated`).

## 3. Primitive encodings

`bits_required(r)` = number of bits needed for `0..=r` = `32 - clz32(r)`
(`bits_required(0) = 0`).

| Notation | Encoding |
|---|---|
| `bool` | 1 bit, 1 = true |
| `uint(n)` | `n` bits (`n <= 32`), unsigned |
| `bounded(min, max)` | `value - min` in `bits_required(max - min)` bits. Decoders reject a raw value above `max - min` (`OutOfRange`). Encoders refuse values outside `min..=max`. |
| `u8` / `u16` / `u32` | `uint(8)` / `uint(16)` / `uint(32)` |
| `i16` | `uint(16)` of the two's-complement pattern |
| `u64` | low 32 bits as `u32`, then high 32 bits as `u32` |
| `varuint` | see below |
| `varint` | zig-zag (`(v << 1) ^ (v >> 31)`, i.e. `0,-1,1,-2,…` -> `0,1,2,3,…`), then `varuint` |
| `f32` | the 32-bit IEEE-754 binary32 pattern as `u32`. Must be finite: encoders refuse and decoders reject NaN and infinities (`OutOfRange`). |
| `vec3` | `f32` x, `f32` y, `f32` z |
| `quat_f32` | `f32` x, y, z, w |
| `string<N>` | byte length `bounded(0, N)`, then that many bytes as `u8`. Must be valid UTF-8 (`InvalidUtf8`). |
| `optional<T>` | `bool` present, then `T` only if present |
| `player_id` | `bounded(0, 63)` (6 bits) |

**varuint.** The value is split into 6-bit groups, least significant group
first. Each group is written as `uint(6)` followed by a `bool` *continuation*
bit (1 = another group follows). Encoders write the minimum number of groups
(at least one). Decoders read at most 6 groups and reject:
- a 6th group above 3, or a 6th group with continuation 1 (`OutOfRange`);
- a final group of 0 that is not the first group (`NonCanonical`).

Examples: 0 -> `0000000` (7 bits); 63 -> group 63, stop; 64 -> group 0 more,
group 1 stop (14 bits).

## 4. Quantisation

All quantisation arithmetic is IEEE-754 binary64 in exactly the order written,
without fused multiply-add (MSVC `/fp:precise`, GCC/Clang
`-ffp-contract=off`). `round` is round-half-away-from-zero (C++ `std::round`).
`f64(x)` widens a binary32; `f32(x)` rounds a binary64 to nearest binary32.
Quantisers are total: NaN gives 0 and values outside the range clamp.

Constants (binary64): `TAU = 6.283185307179586`,
`SQRT_2 = 1.4142135623730951`, `FRAC_1_SQRT_2 = 0.7071067811865476`.

### 4.1 Fixed point

```
fixed(x, scale, min, max):
    if x is NaN: return 0
    q = round(f64(x) * scale)
    if q <= min: return min
    if q >= max: return max
    return (int32) q
unfixed(q, scale) = f32(f64(q) / scale)
```

| Quantity | scale | range (wire) | bits |
|---|---|---|---|
| position (m) | 512 | `bounded(-8388608, 8388607)` (+-16384 m) | 24 |
| velocity (m/s), angular velocity (rad/s) | 64 | `bounded(-8192, 8191)` (+-128) | 14 |

### 4.2 Angles

An angle with `b` bits uses `2^b` units per full turn.
`upr(b) = f64(2^b) / TAU` (units per radian) and `rpu(b) = TAU / f64(2^b)`.

```
yaw_q(yaw, b):                         // result in 0 .. 2^b - 1
    if yaw is not finite: return 0
    q = fmod(round(f64(yaw) * upr(b)), f64(2^b))   // C fmod: sign of dividend
    if q < 0: q = q + f64(2^b)
    return (uint32) q
yaw_f(q, b)   = f32(f64(q) * rpu(b))   // radians in [0, 2*pi)

pitch_q(pitch, b) = fixed(pitch, upr(b), -2^(b-2), 2^(b-2))   // +- quarter turn
pitch_f(q, b)     = f32(f64(q) * rpu(b))
```

| Use | b | yaw wire | pitch wire | resolution |
|---|---|---|---|---|
| `UserCommand` (fine) | 20 | `uint(20)` | `bounded(-262144, 262144)` (20 bits) | 0.000343 deg = 6.0 microrad: 0.6 mm at 100 m, 6 mm at 1 km |
| remote player (coarse) | 16 | `u16` | `bounded(-16384, 16384)` (16 bits) | 0.0055 deg |

Why 20 bits for commands: aim is server-authoritative and the client predicts
with the quantised angles, so quantisation error is a direct aim error. 16
bits (0.0055 deg, 9.6 mm at 100 m) is coarser than one mouse count of a low
sensitivity setting under a magnified scope (an 8x scope at 0.022 deg/count
moves 0.0028 deg per count); 20 bits is 8x finer than that while costing
nothing in steady state, because older commands are delta-encoded (section
8.2) and an unchanged angle costs one bit.

Clients **must** simulate (predict) with the dequantised command
(`UserCommand::canonical()` in Rust) so prediction and authority see the same
angles. The sim must accept yaw anywhere in `[0, 2*pi)` (it is a wrapped
value) and pitch clamped to `[-pi/2, pi/2]`.

### 4.3 Smallest-three quaternion

Used for world-object rotations `[x, y, z, w]` (indices 0..3).

```
quat_q(q):
    v = f64 components of q
    n = sqrt(((v0*v0 + v1*v1) + v2*v2) + v3*v3)
    if n is not finite or n < 1e-6: v = [0, 0, 0, 1]
    else: v[i] = v[i] / n for each i
    largest = first index i with |v[i]| strictly greater than every earlier |v[j]|
              (i.e. the first index of the maximum magnitude)
    if v[largest] < 0: v[i] = -v[i] for each i
    for each other index i in increasing order:
        s = clamp(round((v[i] * SQRT_2) * 511.0), -511.0, 511.0)
        component = (uint32)(s + 511.0)          // 0 ..= 1022
quat_f(largest, c0, c1, c2):
    sum = 0
    for each other index i in increasing order, with its component c:
        v[i] = ((f64(c) - 511.0) / 511.0) * FRAC_1_SQRT_2
        sum = sum + v[i] * v[i]
    v[largest] = sqrt(max(0, 1 - sum))
    result = f32 of each v[i]
```

Wire: `uint(2)` largest, then the three components each as `bounded(0, 1022)`
(10 bits; 1023 is rejected). 0 is exact, so the identity rotation round-trips
exactly. Maximum angular error is about 0.16 deg.

## 5. Packet kinds

Every packet starts with `uint(4)` **kind**. The kind field and the `Control`
packet layout are **frozen** across all protocol versions.

| Kind | Name | Direction | Section |
|---|---|---|---|
| 0 | `ClientInput` | client -> server, one per tick | 8 |
| 1 | `ServerSnapshot` | server -> client, one per tick per client | 9 |
| 2 | `Control` | both | 6 |
| 3..15 | reserved | | rejected (`UnknownPacketKind`) |

## 6. Control packet and handshake

```
Control:
    uint(4)  kind = 2
    u8       message type id      // must be 1, 2 or 3
    ...      message body         // section 10
    padding
```

`Control` packets have no sequence number and are not acknowledged. They carry
only `ClientHello` (1), `ServerWelcome` (2) and `ServerReject` (3); any other
type rejects the packet (`UnexpectedMessage`). The `ClientHello` and
`ServerReject` bodies are frozen across versions so that a version mismatch is
always detectable.

Handshake:
1. After netcode reports *connected*, the client sends `Control(ClientHello)`
   every 100 ms until it receives `ServerWelcome` or `ServerReject` (the
   application gives up after its own timeout, e.g. 5 s).
2. The server answers every `ClientHello`:
   - `protocol_version != PROTOCOL_VERSION` -> `ServerReject(PROTOCOL_MISMATCH)`;
   - `content_hash` differs -> `ServerReject(CONTENT_MISMATCH)`;
   - no free slot -> `ServerReject(SERVER_FULL)`;
   - name empty or unacceptable -> `ServerReject(BAD_NAME)`;
   - otherwise `ServerWelcome`. Repeated hellos from a welcomed client get the
     same welcome again.
   After a reject the server disconnects the netcode client about 1 s later.
3. Until it receives the first `ClientInput` from a welcomed client, the server
   also re-sends `ServerWelcome` every 100 ms.
4. On `ServerWelcome` the client starts sending `ClientInput` every tick. The
   server starts sending `ServerSnapshot` after the welcome. A client discards
   `ServerSnapshot` packets that arrive before its welcome (they are never
   acknowledged, so nothing is lost).
5. Sequenced state starts fresh at the welcome: packet sequence numbers start
   at 0 in each direction, reliable message ids start at 0 in each direction.

## 7. Sequencing and acknowledgement

### 7.1 Packet header

Every `ClientInput` and `ServerSnapshot` contains:

```
PacketHeader:
    u16  sequence    // of this packet
    u16  ack         // newest sequence received from the peer, 0xFFFF before any
    u32  ack_bits    // bit i (LSB = 0) set: packet (ack - 1 - i) mod 2^16 received
```

### 7.2 Wrap-around comparison

```
seq_greater(a, b) = (a > b and a - b <= 32768) or (a < b and b - a > 32768)   // u16
tick_greater(a, b) = a != b and (a - b) mod 2^32 < 2^31                       // u32
```

Ticks are `u32` and compared with `tick_greater` everywhere.

### 7.3 Receiving rules

A packet is *registered* only after it decoded completely. Registration:
1. Apply its acks (always; idempotent): `ack` and every `ack - 1 - i` with bit
   `i` set are acknowledged if they are in the sent history (256 entries,
   indexed by `sequence mod 256`, matched by full sequence) and not yet acked.
   Each newly acknowledged sequence is passed to the reliable channel.
2. Classify its `sequence` against the receive history (newest received
   `newest`, plus a 32-bit mask of the 32 sequences before it):
   - newer than `newest` by `d`: mask = `(mask << d) | (1 << (d - 1))`
     (mask = 0 if `d > 32`), `newest = sequence` -> **fresh**;
   - equal to `newest`, or older by `d <= 32` with its bit already set ->
     **duplicate**;
   - older by `d <= 32` with its bit clear: set the bit -> **fresh**;
   - older by `d > 32` -> **stale**.
3. Process the packet's content only if fresh. Duplicate and stale packets are
   discarded whole. A stale packet is never acknowledged, so its reliable
   messages will be resent.

The header of every outgoing packet carries the current `newest` and mask.

## 8. ClientInput (kind 0)

```
ClientInput:
    uint(4)            kind = 0
    PacketHeader
    optional<u32>      snapshot_ack       // server_tick of the newest snapshot fully applied
    u32                view_tick
    u8                 view_fraction      // 1/256 of a tick
    CommandList
    ReliableSection                        // section 11
    padding
```

- `snapshot_ack`: the newest (by `tick_greater`) `server_tick` whose snapshot
  the client decoded and whose players section it applied successfully. The
  server uses it as the delta baseline. It never moves backwards.
- `view_tick + view_fraction / 256`: the server time, in ticks, at which the
  client currently renders remote players (its interpolation time). The server
  rewinds lag-compensation history to this time (capped, default 200 ms).
  Before the first snapshot both are 0.

### 8.1 UserCommand

```
UserCommand { tick: u32, held: u32, pressed: u32, yaw: f32, pitch: f32,
              select_slot: i8, interact_kind: u8, interact_id: u32 }
```

`tick` is the server tick the command is for. `held` bits (8 on the wire,
higher bits reserved, must be 0):

| bit | held | pressed |
|---|---|---|
| 0 | forward | jump |
| 1 | back | reload |
| 2 | left | fire_mode |
| 3 | right | drop_weapon |
| 4 | crouch | interact |
| 5 | sprint | zero_up |
| 6 | fire | zero_down |
| 7 | aim | (reserved, 0) |

`pressed` bits are edges: set on exactly one command (the command builder
resolves toggles and guarantees no lost or doubled edges). `select_slot` is
`-1` (none) or `0..=7`. `interact_kind`: 0 none, 1 pickup (`interact_id` =
`instance_id`), 2 dropped weapon (`interact_id` = `dropped_id`), 3..15
reserved; when it is 0, `interact_id` is not sent and decodes as 0 (senders
must set it to 0).

Wire form of the angles: `yaw_q(yaw, 20)`, `pitch_q(pitch, 20)` (section 4.2).

Full encoding:
```
    u32                        tick
    uint(8)                    held
    uint(8)                    pressed
    uint(20)                   yaw_q
    bounded(-262144, 262144)   pitch_q
    Actions
Actions:
    bool                       has_slot
    if has_slot: bounded(0, 7) select_slot          // else select_slot = -1
    bounded(0, 15)             interact_kind
    if interact_kind != 0: u32 interact_id          // else interact_id = 0
```

### 8.2 CommandList

```
CommandList:
    bounded(1, 16)   count
    command[0]       full encoding                  // the newest
    command[1..]     delta encoding against the previous (one tick newer) command
```

Commands are for consecutive ticks, newest first:
`command[i].tick = command[0].tick - i` (mod 2^32); the tick of a delta command
is not sent. The client sends every tick its newest command plus the previous
ones it still considers useful (at least 100 ms worth: send 8 by default).

Delta encoding of command `c` against the newer command `n` (all angle values
are the quantised integers):
```
    bool changed_held;     if set: uint(8) held                  // else held = n.held
    bool changed_pressed;  if set: uint(8) pressed               // else pressed = n.pressed
    bool changed_yaw;      if set: varint d                      // else yaw = n.yaw
        // encoder: d = (c.yaw - n.yaw) mod 2^20, then d -= 2^20 if d >= 2^19
        // decoder: requires d != 0 and -2^19 <= d < 2^19; yaw = (n.yaw + d) mod 2^20
    bool changed_pitch;    if set: varint d                      // else pitch = n.pitch
        // d = c.pitch - n.pitch; decoder: d != 0, n.pitch + d within +-262144
    bool changed_actions;  if set: Actions                       // else copied from n
        // decoder: the decoded Actions must differ from n's (NonCanonical otherwise)
```
A `changed_*` bit is 1 exactly when the field differs; decoders reject a set
bit whose value equals the newer command's (`NonCanonical`).

## 9. ServerSnapshot (kind 1)

```
ServerSnapshot:
    uint(4)            kind = 1
    PacketHeader
    SnapshotInfo
    optional<PlayerState>   own           // the receiving client's own player
    ReliableSection                       // section 11
    PlayersSection
    ObjectsSection
    padding
```

The reliable section precedes the state sections so the server can fill the
remainder of the 1200 bytes by priority (players, then objects).

### 9.1 SnapshotInfo

```
    u32                 server_tick
    optional<u32>       last_command_tick
    bounded(-32, 31)    input_buffer_depth
    bool                has_baseline
    if has_baseline: bounded(1, 255) baseline_age   // baseline_tick = server_tick - age (mod 2^32)
```

- The own state and all other state are the state at the end of server tick
  `server_tick`.
- `last_command_tick`: the newest command tick the server has consumed for
  this client (`server_tick` when the client's command for that tick was on
  time; absent before the first command).
- `input_buffer_depth`: commands buffered for this client beyond the one
  consumed this tick; negative values count recent ticks that ran without a
  fresh command (the client is late). The client dilates its clock to keep it
  small and positive. Exact definition (server, after consuming the command
  for `server_tick` = T): let N be the newest command tick the server has
  ever accepted from this client.
  - if `tick_greater(N, T)`: depth = `min(N - T, 31)` (how far ahead the
    client's commands run, even if some tick in between was lost);
  - else if the command for T was fresh: depth = 0;
  - else: depth = `-min(M, 32)`, where M is the number of consecutive ticks
    up to and including T that ran without a fresh command.

  The server accepts a command only for ticks T..=T+60 relative to the next
  tick to simulate (older ones are late resends and dropped silently, newer
  ones are a violation), keeps the first copy per tick (a different second
  copy is a violation), and on a missing command repeats the previous
  command's `held`, `yaw` and `pitch` with `pressed` = 0, `select_slot` = -1
  and no interaction. A client should aim for a depth of about 2 (plus its
  jitter in ticks).

### 9.2 PlayerState (own player, full precision) — *provisional*

```
PlayerState:
    vec3              position          // feet, m
    vec3              velocity          // m/s
    f32               yaw
    f32               pitch
    f32               eye_height
    uint(6)           flags             // section 9.3
    u8                jump_age          // ticks
    u8                jump_cooldown     // ticks
    i16               health
    u16               respawn_ticks
    u32               shots_fired
    bounded(-1, 7)    active_slot       // -1 empty-handed
    bounded(-1, 7)    selected_slot     // -1 none
    f32               raise             // switch progress, linear fraction 0..1
    f32               jump_speed        // m/s of the pending (or last) jump
    u32               deaths            // picks the respawn point
    bool              wait_for_release  // trigger must be released after a switch
    WeaponSlot        slots[0]
    WeaponSlot        slots[1]
WeaponSlot:
    bool              present
    if present:
        u32               weapon_key
        bounded(0, 15)    fire_mode
        u8                magazine_rounds
        bool              magazine_inserted
        bool              chambered
        bool              bolt_locked
        bool              empty_reload
        bounded(0, 7)     reload_stage
        f32               reload_elapsed     // s
        f32               reload_duration    // s, 0 when not reloading
        f32               cycle_remaining    // s
        bounded(0, 8)     spare_count
        u8                spare_magazines[spare_count]   // rounds per spare
        bounded(-1, 255)  incoming_magazine  // rounds of the magazine being seated, -1 none
        bool              pull_used          // the current trigger pull fired or clicked
        f32               scope_magnification
        f32               scope_zero         // m
        u8                spares_dropped     // spares beyond the 8 sent (0 for stock weapons)
```

The layout mirrors `sim::PlayerState` (`src/sim/player_state.h`) with its
`weapon::LoadoutState` / `LoadoutSlotState` / `FirearmState`, so the own
state round-trips exactly. Mapping and narrowing rules:
- `flags` carries `crouching`, `sprinting`, `aiming`, `alive`, `on_ground`,
  `jump_pending` (section 9.3).
- `jump_age` and `jump_cooldown` (C++ `int32_t`) saturate to `0..=255`.
- `magazine_rounds` (C++ `magazine`, `int16_t`) and each spare's rounds
  saturate to `0..=255`; `incoming_magazine` must be -1..=255.
- `active_slot` / `selected_slot` are the `std::optional<WeaponSlot>` index,
  -1 for none; `weapon_key` is `slot_keys[i]`; `fire_mode` is the
  `FireMode` enum value.
- `raise` is the loadout's linear switch progress in `[0, 1]`, not seconds.

An absent slot decodes with every field 0/false (including
`incoming_magazine` = 0, `scope_*` = 0); spare entries past `spare_count`
decode as 0. Encoders refuse a non-canonical value (data in an absent slot
or past `spare_count`). Floats are raw and must be finite.

### 9.3 Player flags

| bit | flag |
|---|---|
| 0 | crouching |
| 1 | sprinting |
| 2 | aiming |
| 3 | alive |
| 4 | on_ground |
| 5 | jump_pending |

### 9.4 PlayersSection (remote players, delta-encoded)

Remote players exclude the receiving client. A remote player in wire units
(`RemotePlayerQ`):

| Field | Quantisation | Wire (full) | Group |
|---|---|---|---|
| position x, y, z | `fixed(v, 512, …)` | 3 x `bounded(-8388608, 8388607)` | 0 position |
| velocity x, y, z | `fixed(v, 64, …)` | 3 x `bounded(-8192, 8191)` | 1 velocity |
| yaw | `yaw_q(yaw, 16)` | `u16` | 2 orientation |
| pitch | `pitch_q(pitch, 16)` | `bounded(-16384, 16384)` | 2 orientation |
| flags | `flags & 0x3F` | `uint(6)` | 3 status |
| health | as is | `i16` | 3 status |
| weapon_key | as is (0 = none) | `u32` | 4 weapon |

A full entry is 200 bits of state.

```
PlayersSection:
    repeat: bool 1, Entry
    bool 0                                   // end of section
Entry:
    player_id         id
    uint(2)           kind                   // 0 delta, 1 full, 2 removed, 3 rejected
    kind 1 (full):    every field above, absolute, in table order
    kind 0 (delta):   uint(5) groups (bit g = group g changed; must be non-zero)
                      group 0: 3 x varint (position - baseline position), per axis
                      group 1: velocity, absolute (3 x bounded)
                      group 2: yaw u16, pitch bounded
                      group 3: flags uint(6), health i16
                      group 4: weapon_key u32
    kind 2 (removed): nothing
```

Rules:
- At most 64 entries; ids within a section are distinct (`NonCanonical`).
- Without a baseline only *full* entries are allowed (`NonCanonical`).
- The section is parsed without knowing the baseline, so a packet whose
  baseline the client lacks still yields its header, own state, reliable
  messages and objects.

Reconstruction (`PlayerUpdates::apply`): start from a copy of the baseline set
(empty if `has_baseline` is false; `MissingBaseline` if it is true and the
client does not have that tick's set), then apply entries in order:
- full: set the player;
- removed: the player must exist (else `Inconsistent`); remove it;
- delta: the player must exist (else `Inconsistent`); add the position
  differences (each result must stay within the position range, else
  `Inconsistent`) and replace the fields of each set group.

Players present in the baseline and not mentioned keep their baseline state.
Players not in the baseline and not mentioned are absent.

Encoder (`SnapshotWriter::write_players`): writes *removed* entries for every
baseline player missing from the current set, then considers player ids in the
server's priority order, writing a *full* entry for players new relative to the
baseline (all players when there is no baseline) and a *delta* entry with the
changed groups for players that changed. Entries that would not fit the space
left (minus a reserve the server keeps for objects) are skipped. Because the
section may therefore be partial, the server records, per client and per
snapshot tick, the set the client will reconstruct (`record` = baseline +
written entries) and uses that record when the client later acknowledges the
tick. This makes 64 players encodable: what does not fit is sent in the next
snapshots against the acknowledged record.

Baseline protocol:
- The client keeps the reconstructed sets of at least the last
  `BASELINE_HISTORY` (64) snapshot ticks it applied.
- The server encodes against the record of the client's `snapshot_ack` tick
  if it is at most 64 ticks old and still stored, otherwise without a
  baseline.

### 9.5 ObjectsSection (world objects)

```
ObjectsSection:
    repeat: bool 1, Object            // at most 96
    bool 0
Object:
    bool               kind               // 0 = dynamic body (id = instance_id), 1 = dropped weapon (id = dropped_id)
    varuint            id
    bool               at_rest
    3 x bounded(-8388608, 8388607)   position, fixed(v, 512)
    uint(2)            rotation largest index
    3 x bounded(0, 1022)             rotation components (section 4.3)
    if not at_rest:
        3 x bounded(-8192, 8191)     linear velocity, fixed(v, 64)
        3 x bounded(-8192, 8191)     angular velocity, fixed(v, 64)
```

Objects are absolute state (no baseline). The server chooses objects by
priority until the packet is full; an object may appear in some packets and
not others. If the same object appears twice, the later entry wins. At rest
objects decode with zero velocities. The smallest object is 114 bits, so at
most 83 fit in practice; 96 is a hard decoder limit.

### 9.6 Size guarantee

`SnapshotWriter` never produces more than 1200 bytes (or the buffer size, if
smaller). Two bits are always reserved for the two section terminators;
`try_add_object` writes nothing and returns false when an object does not fit.

## 10. Messages

Every message is `u8` type id followed by its body. A reserved id rejects the
payload (`UnknownMessage`).

| Id | Name | Where |
|---|---|---|
| 0 | invalid | |
| 1 | `ClientHello` | Control only (frozen) |
| 2 | `ServerWelcome` | Control only |
| 3 | `ServerReject` | Control only (frozen) |
| 4..15 | reserved: handshake / control | |
| 16 | `PlayerJoined` | reliable |
| 17 | `PlayerLeft` | reliable |
| 18 | `Damage` | reliable |
| 19 | `Death` | reliable |
| 20 | `Respawn` | reliable |
| 21 | `Shot` | reliable |
| 22 | `Impact` | reliable |
| 23 | `WeaponDropped` | reliable |
| 24 | `DroppedTaken` | reliable |
| 25 | `PickupTaken` | reliable |
| 26 | `WorldSyncPickupsTaken` | reliable |
| 27 | `WorldSyncDroppedWeapon` | reliable |
| 28 | `WorldSyncDone` | reliable |
| 29 | `DroppedRemoved` | reliable |
| 30..127 | reserved: future game messages | |
| 128..255 | reserved: experiments | |

Bodies:

```
1  ClientHello      u16 protocol_version; u64 content_hash; string<32> name
2  ServerWelcome    player_id player_id; bounded(1, 65535) tick_rate;
                    u32 server_tick; bounded(1, 64) max_players
3  ServerReject     u8 reason; u16 server_protocol_version
16 PlayerJoined     player_id player_id; string<32> name
17 PlayerLeft       player_id player_id
18 Damage           player_id victim; optional<player_id> attacker; i16 amount;
                    bounded(0, 7) zone; i16 health_after
19 Death            player_id victim; optional<player_id> killer; u32 weapon_key
20 Respawn          player_id player_id; vec3 position; f32 yaw
21 Shot             player_id shooter; vec3 origin; vec3 direction; u32 weapon_key;
                    u32 shot_number
22 Impact           vec3 point; vec3 normal
23 WeaponDropped    DroppedWeapon
24 DroppedTaken     u32 dropped_id; player_id player_id
25 PickupTaken      u32 instance_id; player_id player_id
26 WorldSyncPickupsTaken  bounded(0, 16) count; u32 instance_ids[count]
27 WorldSyncDroppedWeapon DroppedWeapon
28 WorldSyncDone    (empty)
29 DroppedRemoved   u32 dropped_id
DroppedWeapon:      u32 dropped_id; u32 weapon_key; u8 rounds_in_magazine;
                    vec3 position; quat_f32 rotation
```

Enumerations:
- `ServerReject.reason`: 1 protocol mismatch, 2 content mismatch, 3 server
  full, 4 bad name, 5 shutting down; other values are carried through
  (display as "unknown").
- `Damage.zone`: 0 head, 1 torso, 2 legs, 3 other (world); 4..7 reserved.
- `attacker` / `killer` absent: world damage. `weapon_key` 0: none.
- `WorldSyncPickupsTaken` entries past `count` are 0 in the decoded value.
- `DroppedRemoved`: a dropped weapon left the world without a taker (its
  lifetime expired, the world's dropped-weapon cap evicted it, or it fell
  below the kill plane). Clients remove it like `DroppedTaken`, without a
  pickup. A player falling below the kill plane gets `Death` with no killer
  and `weapon_key` 0, then `Respawn` as usual.

Late join: after the welcome the server queues `PlayerJoined` for every present
player, then `WorldSyncPickupsTaken` messages (16 ids each) for taken pickups,
`WorldSyncDroppedWeapon` for every existing dropped weapon, then
`WorldSyncDone`. Live events queued after that are ordered after the sync.
The sync must fit the reliable window: if it would not, the server sends only
the `WorldSyncDroppedWeapon` messages that fit (and logs a warning), never
dropping a `PlayerJoined` or `WorldSyncDone`; a weapon left out still
appears in the objects section of snapshots.

## 11. Reliable-ordered channel

### 11.1 ReliableSection (wire)

```
ReliableSection:
    bounded(0, 32)   count
    for each message i:
        i == 0: u16 id
        i > 0:  varuint gap            // id = previous id + gap + 1 (mod 2^16); gap <= 254
        Message                        // u8 type id + body; handshake types rejected
```

Ids within a section strictly increase (on the 16-bit circle) by 1..=255.

### 11.2 Sender

- `send(message)` assigns the next id (starting at 0). At most 256 messages
  may be unacknowledged (`WindowFull`; the connection should be dropped). A
  message larger than the budget minus the 6-bit count and 16-bit id can never
  be sent (`MessageTooLarge`).
- For every outgoing packet, the sender walks ids from the oldest
  unacknowledged to the newest and includes each message that was never sent
  or was last sent at least `resend_interval` (100 ms) ago, if it fits in the
  remaining budget (default 300 bytes = 2400 bits, including the count field
  and each message's id field); messages that do not fit are skipped for this
  packet. At most 32 per packet. It records which ids packet `sequence`
  carried (256-entry history indexed by `sequence mod 256`).
- When packet `sequence` is acknowledged (section 7.3), every message it
  carried is delivered; the oldest-unacknowledged id advances past delivered
  messages.

### 11.3 Receiver

- Only sections of **fresh** packets are processed.
- An id older than the next expected id (by `seq_greater`) is a duplicate and
  ignored. An id 256 or more ahead of the next expected id is a protocol
  violation (drop the connection). Others are buffered (first copy wins).
- Messages are handed to the application strictly in id order, each once.

## 12. Decoder obligations

Decoders must never crash, loop unboundedly or allocate based on input. A
payload is rejected whole if any of these hold: longer than 1200 bytes;
truncated; trailing data or non-zero padding; reserved packet kind or message
id; a handshake message outside a `Control` packet or a game message inside
one; a `bounded` raw value above its range; a non-finite `f32`; invalid UTF-8;
a non-canonical `varuint`; non-canonical deltas (section 8.2, empty players
delta, delta/removed entries without a baseline, duplicate player ids); more
than 64 player entries or 96 objects; a reliable id gap above 254.

Semantic validation is the server's job, beyond the codec: command rate and
tick window, finite and clamped angles (already guaranteed by quantisation),
interaction reach and line of sight, name contents.

## 13. Golden vectors

`server/crates/blue-protocol/tests/vectors/` holds one `<name>.bin` per vector
and `vectors.md` with the logical value of each (Rust `Debug` output, plus a hex
dump). Vectors: `msg_<name>` for every message type (type id + body, zero
padded), `control_client_hello`, `control_server_welcome`,
`control_server_reject`, `client_input_single`, `client_input_multi`
(several delta-encoded commands, sequence and message-id wrap),
`server_snapshot_full` (own state, reliable messages, three players without
baseline, two objects) and `server_snapshot_delta` (moved, changed, removed,
new and unchanged players against a baseline; one moving object).

`cargo test` compares; `BLUE_BLESS=1 cargo test` regenerates (only together
with a `PROTOCOL_VERSION` bump). The C++ tests must encode the same logical
values to identical bytes and decode the bytes to the same values.

## 14. Changelog

- **1** (2026-11-24): initial version.
- (2026-11-24, no wire change, version stays 1): section 1 lists the netcode
  parameters; section 9.1 defines `input_buffer_depth` and the server's
  command window exactly.
- **2** (2026-11-24): own `PlayerState` mirrors `sim::PlayerState` exactly:
  `raise_timer` (seconds) becomes `raise` (linear fraction 0..1); new
  `jump_speed`, `deaths`, `wait_for_release` after it; per present weapon
  slot new `incoming_magazine`, `pull_used`, `scope_magnification`,
  `scope_zero`, `spares_dropped` after the spare magazines. Narrowing rules
  in section 9.2. Golden vectors regenerated.
- **3** (2026-11-24): new reliable message 29 `DroppedRemoved { u32
  dropped_id }` (dropped weapons now despawn: lifetime, cap, kill plane).
  Late-join sync is capped to the reliable window (section 10). Golden
  vectors regenerated (new `msg_dropped_removed`).

## 15. Contract notes for the sim core and the C++ client

- `UserCommand` button bits are fixed by section 8.1 (same as the C++ sim);
  adding a button uses a reserved bit and bumps the version.
- Commands carry quantised angles: predict with the dequantised values; yaw is
  delivered in `[0, 2*pi)`.
- The own `PlayerState` layout is provisional: the sim (T6) owns the final
  field list. Changing it is one line per field in
  `player.rs::PlayerState::serialize` and in the C++ codec, plus a version
  bump. Since version 2 it mirrors `sim::PlayerState` field for field
  (section 9.2).
- `reload_stage` must fit 0..=7, `fire_mode` 0..=15, `spare_count` 0..=8,
  `active_slot`/`selected_slot` -1..=7, `magazine_rounds` and spare rounds
  0..=255.
- Remote positions are limited to +-16384 m and velocities to +-128 m/s
  (they clamp); world objects likewise.
- Reconciliation reads the own state as the state at the end of
  `server_tick`; the client replays its commands with `tick` newer than
  `server_tick`.
