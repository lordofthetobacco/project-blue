#ifndef BLUE_TOOLS_NET_PROBE_NET_PROBE_H_
#define BLUE_TOOLS_NET_PROBE_NET_PROBE_H_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <glm/vec3.hpp>

#include "camera/look.h"
#include "core/result.h"
#include "sim/player_state.h"
#include "sim/user_command.h"

// blue-net-probe: the game's networked client without a window. It builds
// the scene headless (sim::SimWorld for the content hash and weapon table, a
// collision world with kinematic dynamic bodies to predict in), connects
// net::ClientSession over a real net::Transport, plays a scripted command
// stream and prints what happened.

namespace blue::netprobe {

inline constexpr const char* kUsage =
    "usage: blue-net-probe --asset-dir DIR [--scene range.mscn] "
    "[--server 127.0.0.1:27015] [--name probe] [--duration 60] "
    "[--latency MS] [--jitter MS] [--loss PERCENT] [--seed N] "
    "[--aim-at-nearest] [--interact] [--max-replays-per-10s 1] [--trace]\n"
    "Exit 0 only if it played, stayed connected, decoded every payload and "
    "kept replays under the limit.";

struct ProbeOptions {
  std::string server = "127.0.0.1:27015";
  std::string asset_dir;
  std::string scene = "range.mscn";
  std::string name = "probe";
  double duration_seconds = 60.0;
  double latency_ms = 0.0;  // one way, each direction
  double jitter_ms = 0.0;
  double loss_percent = 0.0;
  uint64_t seed = 1;
  bool aim_at_nearest = false;
  bool trace = false;  // print the player once a second
  // Each cycle, drop the weapon in hand and pick it up again (the server
  // decides both: expect a replay for each).
  bool interact = false;
  double max_replays_per_10s = 1.0;
  bool show_help = false;
};

// Parses the command line; an error message for unknown flags, missing or
// malformed values, or a missing --asset-dir.
[[nodiscard]] core::Result<ProbeOptions, std::string> ParseOptions(
    int argc, const char* const argv[]);

// Length of one cycle of the command script, ticks (20 s).
inline constexpr uint32_t kScriptCycleTicks = 1200;

// Returns the scripted command for `script_tick` ticks after play began:
// per 20 s cycle a walked square with jumps and a sprinted side, a
// crouched walk (mirrored in odd cycles, so the path closes), a
// turn on the spot, a switch to the pistol and back, a reload, then fire
// bursts while aiming. `base_yaw` is the yaw play began with; `aim`, when
// set, replaces the angles (aiming at the nearest player).
[[nodiscard]] sim::UserCommand ScriptedCommand(
    uint32_t script_tick, float base_yaw,
    const std::optional<camera::LookAngles>& aim);

// Returns the fields in which `predicted` and `authoritative` differ
// (beyond sim::NeedsReplay's tolerances), e.g. "position 0.034 m, flags".
[[nodiscard]] std::string DescribeMismatch(
    const sim::PlayerState& predicted, const sim::PlayerState& authoritative);

// Connects, plays for options.duration_seconds and prints the summary.
// Returns the process exit code.
int RunProbe(const ProbeOptions& options);

}  // namespace blue::netprobe

#endif  // BLUE_TOOLS_NET_PROBE_NET_PROBE_H_
