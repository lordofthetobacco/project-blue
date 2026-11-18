#ifndef BLUE_EDITOR_VIEWPORT_DEBUG_DRAW_H_
#define BLUE_EDITOR_VIEWPORT_DEBUG_DRAW_H_

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "asset/mesh_data.h"
#include "prefab/prefab.h"
#include "render/debug_lines.h"

// Editor debug drawing shared by Prefab and Scene mode: what a prefab's
// collider, sockets and lights look like in the viewport, light icons and
// the player start. Everything goes into a render::DebugLines (usually
// ViewportPanel::lines()); labels are the caller's (ViewportPanel::
// AddLabel()). Pure CPU.

namespace blue::editor {

// Colours of the editor's debug draws (display space).
inline constexpr glm::vec4 kGridColor{0.32f, 0.33f, 0.35f, 1.0f};
inline constexpr glm::vec4 kGridMajorColor{0.45f, 0.46f, 0.50f, 1.0f};
inline constexpr glm::vec4 kBoundsColor{1.0f, 0.75f, 0.2f, 1.0f};
inline constexpr glm::vec4 kColliderColor{0.3f, 1.0f, 0.4f, 1.0f};
inline constexpr glm::vec4 kLightColor{1.0f, 0.9f, 0.4f, 1.0f};
inline constexpr glm::vec4 kPlayerStartColor{0.3f, 0.7f, 1.0f, 1.0f};

// Adds the ground grid around `focus` (1 m cells, 10 m major lines, reaching
// about `half_extent` metres), in the XZ plane at y = `height` (lift it a
// little above a ground surface at y = 0 to avoid z-fighting).
void AddGroundGrid(render::DebugLines& lines, const glm::vec3& focus,
                   float half_extent = 50.0f, float height = 0.0f);

// Adds the world-space box of `local_bounds` after `world`.
void AddBounds(render::DebugLines& lines, const asset::Bounds& local_bounds,
               const glm::mat4& world, const glm::vec4& color = kBoundsColor);

// Adds a wireframe of `physics`'s collider for a prefab placed at
// `prefab_world`: box, sphere and capsule from their parameters; hulls and
// triangle meshes as the bounds of `fallback_bounds` (prefab space) until
// cooked-shape triangles are available. Nothing for kNone.
void AddCollider(render::DebugLines& lines, const prefab::Physics& physics,
                 const glm::mat4& prefab_world,
                 const asset::Bounds& fallback_bounds,
                 const glm::vec4& color = kColliderColor);

// Adds the axes of a socket whose world matrix is `socket_world`, `size`
// metres long, always on top.
void AddSocket(render::DebugLines& lines, const glm::mat4& socket_world,
               float size = 0.05f);

// Adds a point light icon (small star) at `position`, plus its range sphere
// when `range` > 0 and `show_range`.
void AddPointLight(render::DebugLines& lines, const glm::vec3& position,
                   float range, bool show_range,
                   const glm::vec4& color = kLightColor);

// Adds the sun as an arrow of `length` metres ending at `anchor`, pointing
// along `direction` (the direction the light travels).
void AddSunArrow(render::DebugLines& lines, const glm::vec3& anchor,
                 const glm::vec3& direction, float length = 2.0f);

// Adds the player start: a 1.8 m capsule outline standing at `position`
// with an arrow for the view direction given by `yaw_radians` (0 = -Z,
// positive turns left, as scene::PlayerStart).
void AddPlayerStart(render::DebugLines& lines, const glm::vec3& position,
                    float yaw_radians,
                    const glm::vec4& color = kPlayerStartColor);

}  // namespace blue::editor

#endif  // BLUE_EDITOR_VIEWPORT_DEBUG_DRAW_H_
