#ifndef BLUE_EDITOR_MCP_MCP_EXPORT_H_
#define BLUE_EDITOR_MCP_MCP_EXPORT_H_

#include <string>

#include <glm/gtc/quaternion.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "animation/transform.h"
#include "mcp/json.h"
#include "prefab/prefab.h"
#include "scene/scene.h"
#include "scene/scene_data.h"

// JSON views of editor documents for the MCP tools and resources
// (scene_get, prefab_get, blue://scene, blue://prefab). Pure: no editor
// state, no GPU. The shapes are documented in the tool descriptions
// (mcp_tools.cc); keep both in sync.
//
// Conventions: vectors are arrays ([x, y, z]); rotations are given twice,
// as a quaternion [x, y, z, w] and as Euler angles in degrees
// ("rotation_deg", XYZ as glm::eulerAngles); paths are shown relative to the
// asset root when inside it.

namespace blue::editor {

[[nodiscard]] mcp::Json Vec2Json(const glm::vec2& v);
[[nodiscard]] mcp::Json Vec3Json(const glm::vec3& v);
[[nodiscard]] mcp::Json Vec4Json(const glm::vec4& v);
// {"position", "rotation" (quaternion xyzw), "rotation_deg", "scale"}.
[[nodiscard]] mcp::Json TransformJson(const scene::Transform& transform);
[[nodiscard]] mcp::Json TransformJson(const animation::Transform& transform);

// The whole scene: prefabs, instances (id, name, prefab path, parent id,
// local transform, world position, visible, material and physics
// overrides), sun (with elevation / azimuth in degrees), environment, point
// lights, player start (yaw / pitch in degrees), gravity. Paths are shown
// relative to `asset_root`.
[[nodiscard]] mcp::Json SceneToJson(const scene::SceneData& scene,
                                    const std::string& asset_root);

// The whole prefab without bulk data: nodes (index, name, parent, rest
// transform), meshes (node, vertex / triangle counts, submeshes with their
// material), materials (every parameter, texture per slot with uv set),
// textures (name, format, size, mips, bytes), clips (duration, loop,
// events, channel count), sockets, lights, children, physics, bounds, the
// import source, and a summary of counts and sizes.
[[nodiscard]] mcp::Json PrefabToJson(const prefab::Prefab& prefab,
                                     const std::string& asset_root);

}  // namespace blue::editor

#endif  // BLUE_EDITOR_MCP_MCP_EXPORT_H_
