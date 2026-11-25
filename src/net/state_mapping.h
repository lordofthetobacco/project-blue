#ifndef BLUE_NET_STATE_MAPPING_H_
#define BLUE_NET_STATE_MAPPING_H_

#include "net/players.h"
#include "sim/player_state.h"

namespace blue::net {

// Conversion between the simulation's sim::PlayerState and the own-player
// state of a ServerSnapshot (net::WirePlayerState, section 9.2 of
// docs/netcode/protocol.md), field by field. Every state the simulation
// produces round-trips exactly: FromWire(ToWire(s)) == s.

// Returns `state` as the server sends it. Integers wider than their wire
// field saturate (jump_age and jump_cooldown, magazine and spare rounds and
// spares_dropped at 0..255, incoming_magazine at -1..255, reload_stage at
// 0..7, spare_count at 0..8); an absent slot is all zero.
[[nodiscard]] WirePlayerState ToWire(const sim::PlayerState& state);

// Returns the simulation state of `wire`. An absent slot is
// weapon::LoadoutSlotState{} with the key 0; slot indices the loadout does
// not have become nullopt and fire modes it does not know the last one.
[[nodiscard]] sim::PlayerState FromWire(const WirePlayerState& wire);

}  // namespace blue::net

#endif  // BLUE_NET_STATE_MAPPING_H_
