#ifndef BLUE_EDITOR_VIEWPORT_UV_VIEW_H_
#define BLUE_EDITOR_VIEWPORT_UV_VIEW_H_

#include <imgui.h>

#include <span>

#include <glm/vec2.hpp>

#include "asset/mesh_data.h"

namespace blue::editor {

// Zoom and pan of one UV view; keep one per widget (e.g. in the panel).
struct UvViewState {
  float zoom = 1.0f;       // 1 = the 0..1 square fits the canvas
  glm::vec2 center{0.5f};  // UV at the canvas centre
};

// Options of DrawUvView().
struct UvViewOptions {
  int uv_set = 0;  // 0 = asset::Vertex::uv0, 1 = uv1
  // Shown under the 0..1 square (e.g. a texture from
  // EditorContext::CreateImageTexture()); 0 = none (checker-free grey).
  ImTextureID background = ImTextureID{};
  // Repeats the background outside 0..1 (tiling textures).
  bool tile_background = true;
  ImU32 wire_color = IM_COL32(255, 200, 60, 200);
  // Draws at most this many triangles (big meshes); the rest are skipped
  // and the count shown.
  int max_triangles = 200000;
};

// Draws the triangles of `indices` (into `vertices`) in UV space on an
// ImGui canvas filling `size` (<= 0 components: available region), over
// the 0..1 square with a grid and the optional background. Mouse wheel
// zooms at the cursor, MMB / RMB drag pans, double-click resets. `id` keys
// the ImGui widget. Returns true while the canvas is hovered. Costs one
// ImGui line per triangle edge: fine for editor meshes, capped by
// max_triangles.
bool DrawUvView(const char* id, std::span<const asset::Vertex> vertices,
                std::span<const asset::Index> indices,
                const UvViewOptions& options, UvViewState& state,
                ImVec2 size = ImVec2(0.0f, 0.0f));

}  // namespace blue::editor

#endif  // BLUE_EDITOR_VIEWPORT_UV_VIEW_H_
