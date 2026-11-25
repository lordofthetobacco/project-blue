#ifndef BLUE_SIM_USER_COMMAND_H_
#define BLUE_SIM_USER_COMMAND_H_

#include <cstdint>

namespace blue::sim {

// Length of one simulation tick in seconds; equal to the physics fixed step.
inline constexpr float kTickSeconds = 1.0f / 60.0f;

// Bit masks of UserCommand::held and UserCommand::pressed. The bit positions
// are part of the wire protocol: never renumber, only append.
namespace buttons {

// Held: the input is down at the end of the tick, or was tapped during it.
namespace held {
inline constexpr uint32_t kForward = 1u << 0;
inline constexpr uint32_t kBack = 1u << 1;
inline constexpr uint32_t kLeft = 1u << 2;
inline constexpr uint32_t kRight = 1u << 3;
inline constexpr uint32_t kCrouch = 1u << 4;  // Toggle setting resolved.
inline constexpr uint32_t kSprint = 1u << 5;  // Toggle setting resolved.
inline constexpr uint32_t kFire = 1u << 6;
inline constexpr uint32_t kAim = 1u << 7;
}  // namespace held

// Pressed: the input went down at least once since the previous tick.
namespace pressed {
inline constexpr uint32_t kJump = 1u << 0;
inline constexpr uint32_t kReload = 1u << 1;
inline constexpr uint32_t kFireMode = 1u << 2;
inline constexpr uint32_t kDropWeapon = 1u << 3;
inline constexpr uint32_t kInteract = 1u << 4;
inline constexpr uint32_t kZeroUp = 1u << 5;
inline constexpr uint32_t kZeroDown = 1u << 6;
}  // namespace pressed

}  // namespace buttons

// Everything a player asks the simulation to do in one tick. Unsigned fields
// are bit masks (`held`, `pressed`) or modular counters (`tick`).
struct UserCommand {
  uint32_t tick = 0;     // The tick this command is executed in.
  uint32_t held = 0;     // buttons::held bits.
  uint32_t pressed = 0;  // buttons::pressed bits.
  float yaw = 0.0f;      // radians, absolute, camera::LookAngles convention
  float pitch = 0.0f;    // radians, absolute
  // weapon::WeaponSlot to take out (0 pistol, 1 rifle), or -1 for none.
  int8_t select_slot = -1;
  // What kInteract targets: a kind chosen by the interaction system (0 none)
  // and the target's id within that kind.
  uint8_t interact_kind = 0;
  uint32_t interact_id = 0;

  friend bool operator==(const UserCommand&, const UserCommand&) = default;
};

}  // namespace blue::sim

#endif  // BLUE_SIM_USER_COMMAND_H_
