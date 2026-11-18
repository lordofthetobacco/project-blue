#ifndef BLUE_EDITOR_MCP_MCP_ARGS_H_
#define BLUE_EDITOR_MCP_MCP_ARGS_H_

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "core/result.h"
#include "mcp/json.h"
#include "prefab/prefab.h"
#include "scene/scene.h"
#include "scene/scene_data.h"

// How MCP tool arguments (JSON objects, already checked against the tool's
// input schema for required members and basic types) map onto editor edits
// and command lines. Pure: no editor state, so it is unit-tested. Every
// Apply* function validates all of its arguments before changing anything:
// on error its target is unchanged.

namespace blue::editor {

using ArgsStatus = core::Result<void, std::string>;

// Reads optional member `key` of `args` as a vector of 2 / 3 / 4 numbers
// (a JSON array). Absent: nullopt. Present but malformed (wrong length, not
// numbers, not finite): an error naming `key`.
[[nodiscard]] core::Result<std::optional<glm::vec2>, std::string> GetVec2Arg(
    const mcp::Json& args, std::string_view key);
[[nodiscard]] core::Result<std::optional<glm::vec3>, std::string> GetVec3Arg(
    const mcp::Json& args, std::string_view key);
[[nodiscard]] core::Result<std::optional<glm::vec4>, std::string> GetVec4Arg(
    const mcp::Json& args, std::string_view key);
// Reads optional number `key`, which must lie in [min, max] when present.
[[nodiscard]] core::Result<std::optional<float>, std::string> GetFloatArg(
    const mcp::Json& args, std::string_view key, float min, float max);

// Reads member `key` as a list of instance ids: an integer or an array of
// integers (each > 0). Missing: an empty list.
[[nodiscard]] core::Result<std::vector<scene::InstanceId>, std::string>
GetInstanceIdsArg(const mcp::Json& args, std::string_view key);

// Reads member `key` as a list of strings: one string or an array of
// strings. Missing: an empty list.
[[nodiscard]] core::Result<std::vector<std::string>, std::string>
GetStringListArg(const mcp::Json& args, std::string_view key);

// Returns `word` as one word of an editor command line (commands.h):
// quoted when it is empty or holds spaces or ';'. Words containing '"' or a
// line break cannot be expressed: error.
[[nodiscard]] core::Result<std::string, std::string> QuoteCommandWord(
    std::string_view word);

// Joins `words` into one command (QuoteCommandWord() each).
[[nodiscard]] core::Result<std::string, std::string> BuildCommand(
    std::span<const std::string> words);

// scene_set_transform: "position" [x,y,z], "rotation_deg" [x,y,z] (Euler,
// degrees, XYZ like the inspector) or "rotation" [x,y,z,w] (quaternion,
// normalized here), "scale" [x,y,z] or a number (each > 0). At least one
// must be given.
[[nodiscard]] ArgsStatus ApplyTransformArgs(const mcp::Json& args,
                                            scene::Transform& transform);

// Material parameters shared by prefab_set_material and
// scene_set_material_override: "base_color" [r,g,b,a] in [0,1],
// "metallic", "roughness" in [0,1], "emissive" [r,g,b] >= 0,
// "emissive_strength" >= 0, "uv_scale" [u,v] > 0. Prefab materials also
// take "normal_scale" >= 0, "occlusion_strength" in [0,1], "alpha_mode"
// ("opaque"|"mask"), "alpha_cutoff" in [0,1], "double_sided".
// Returns how many parameters were given (0 is an error for the prefab
// version; overrides may only clear or replace).
[[nodiscard]] core::Result<int, std::string> ApplyMaterialArgs(
    const mcp::Json& args, prefab::Material& material);
[[nodiscard]] core::Result<int, std::string> ApplyOverrideArgs(
    const mcp::Json& args, scene::MaterialOverride& material);

// scene_set_sun: "elevation_deg" (-90..90) and "azimuth_deg" (either keeps
// the other's current value), "direction" [x,y,z] (light travel direction;
// not with the angles), "color" [r,g,b] >= 0, "intensity" >= 0,
// "casts_shadows".
[[nodiscard]] ArgsStatus ApplySunArgs(const mcp::Json& args,
                                      scene::SunLight& sun);

// scene_set_environment (except the HDRI path, which the caller resolves):
// "intensity" >= 0, "rotation_deg", "exposure_ev" (-20..20).
[[nodiscard]] ArgsStatus ApplyEnvironmentArgs(const mcp::Json& args,
                                              scene::Environment& environment);

// scene_add_point_light: "position" (required by the schema), "color",
// "intensity" >= 0, "range" > 0, "name".
[[nodiscard]] ArgsStatus ApplyPointLightArgs(const mcp::Json& args,
                                             scene::ScenePointLight& light);

// prefab_set_collider body values: "dynamic", "mass" > 0, "friction" >= 0,
// "restitution" in [0,1]. Returns how many were given.
[[nodiscard]] core::Result<int, std::string> ApplyBodyArgs(
    const mcp::Json& args, prefab::Physics& physics);

// Parses a collider kind name: none, box, sphere, capsule, hull, mesh.
[[nodiscard]] std::optional<prefab::ColliderKind> ParseColliderKindArg(
    std::string_view name);

// prefab_clip_edit: returns the editor command lines (prefab_commands.cc)
// for "action": "merge" (needs "clips" (>= 2 names) and "name"), "trim"
// ("clip", "start", "end"), "loop" ("clip", "loop"), "add_event" ("clip",
// "event", "time"), "remove_event" ("clip", "event_index"). Clips are named
// by name or index (string or integer), resolved against `prefab`.
[[nodiscard]] core::Result<std::string, std::string> ClipEditCommand(
    const mcp::Json& args, const prefab::Prefab& prefab);

// Returns `text` limited to `max_bytes`, cut at a line break when possible,
// with a note of how much was left out.
[[nodiscard]] std::string LimitText(std::string text, size_t max_bytes);

}  // namespace blue::editor

#endif  // BLUE_EDITOR_MCP_MCP_ARGS_H_
