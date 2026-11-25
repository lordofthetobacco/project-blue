#ifndef BLUE_SIM_COMMAND_BUILDER_H_
#define BLUE_SIM_COMMAND_BUILDER_H_

#include <cstdint>

#include "camera/look.h"
#include "input/input_frame.h"
#include "input/input_settings.h"
#include "sim/user_command.h"

namespace blue::sim {

// Collects render-frame input into one UserCommand per simulation tick, so a
// press is neither lost nor repeated however frames and ticks interleave:
//
//   - Accumulate once per render frame; Take once per tick run in that frame.
//   - Edges (buttons::pressed bits, the weapon selection) are kept until the
//     next Take, so a frame that runs no tick passes them on, and a frame that
//     runs several ticks gives them to the first one only.
//   - Held bits are those of the latest frame, plus any held input that went
//     down since the previous Take: a fire tap pressed and released between
//     two ticks still yields one tick with kFire held.
//   - The crouch and sprint toggle settings are resolved here into plain held
//     bits, with the same rules as player::PlayerController, so the
//     simulation never sees a toggle.
//
// The interaction target (UserCommand::interact_kind / interact_id) is not
// known here; the caller fills it in. Copyable. Not thread-safe.
class CommandBuilder {
 public:
  CommandBuilder() = default;
  CommandBuilder(const CommandBuilder&) = default;
  CommandBuilder& operator=(const CommandBuilder&) = default;

  // Adds one render frame: its actions under `settings`, and the absolute
  // view angles `look` after the frame's mouse look.
  void Accumulate(const input::InputFrame& frame,
                  const input::InputSettings& settings,
                  camera::LookAngles look);

  // Returns the command for `tick` and clears the collected edges.
  [[nodiscard]] UserCommand Take(uint32_t tick);

  // Forgets everything, toggle latches included (e.g. after a respawn).
  void Reset();

 private:
  uint32_t held_ = 0;        // Held bits of the latest frame.
  uint32_t tapped_ = 0;      // Held bits that went down since the last Take.
  uint32_t pressed_ = 0;     // Pressed bits since the last Take.
  int8_t select_slot_ = -1;  // Latest selection since the last Take.
  camera::LookAngles look_;
  bool crouch_latched_ = false;  // Toggle mode: crouch requested.
  bool sprint_latched_ = false;  // Toggle mode: sprint requested.
};

}  // namespace blue::sim

#endif  // BLUE_SIM_COMMAND_BUILDER_H_
