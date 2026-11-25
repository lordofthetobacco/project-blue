#ifndef BLUE_COOK_POSE_FILE_H_
#define BLUE_COOK_POSE_FILE_H_

#include <string>
#include <string_view>
#include <vector>

#include "animation/transform.h"
#include "core/result.h"
#include "prefab/prefab.h"

// Pose files (.pose): where a baked, posed figure holds its weapon. Written
// by tools/assets/kyle/bake_kyle_poses.py next to the figure's mesh; read by
// `blue-pack samples` to nest the weapon prefab in the figure prefab.
//
// Format: "key = value" lines, '#' starts a comment line:
//   weapon = m16.pbo                 the held weapon's prefab file name
//   <name>.translation = x y z       metres, figure space
//   <name>.rotation = w x y z        unit quaternion, figure space
// The transform named "weapon" places the weapon prefab's space in the
// figure; every other named transform (e.g. hand_r, hand_l) is a socket.
// Figure space: +Y up, the figure faces -Z, origin between the feet on the
// ground.

namespace blue::cook {

struct PoseFile {
  std::string weapon;                 // prefab file name, as written
  animation::Transform weapon_local;  // weapon prefab space -> figure space
  // The other named transforms in file order, all on the root (node -1).
  std::vector<prefab::Socket> sockets;
};

// Parses the text of a pose file. Fails with a message naming the line for
// a line without '=', an unknown key, a malformed or non-finite number, a
// rotation that is not a unit quaternion (|q| outside 0.999 .. 1.001), a
// name given twice, a name missing its translation or rotation, or a
// missing "weapon" (key or transform). Rotations are returned normalised.
[[nodiscard]] core::Result<PoseFile, std::string> ParsePoseFile(
    std::string_view text);

// Reads and parses the pose file at `path`.
[[nodiscard]] core::Result<PoseFile, std::string> LoadPoseFile(
    const std::string& path);

}  // namespace blue::cook

#endif  // BLUE_COOK_POSE_FILE_H_
