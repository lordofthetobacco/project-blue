#include "editor/viewport/uv_view.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>

namespace blue::editor {
namespace {

constexpr float kZoomStep = 1.15f;
constexpr float kMinZoom = 0.05f;
constexpr float kMaxZoom = 200.0f;
// At most this many background tiles are drawn when tiling.
constexpr int kMaxTiles = 256;
constexpr ImU32 kCanvasColor = IM_COL32(40, 40, 44, 255);
constexpr ImU32 kSquareColor = IM_COL32(64, 64, 70, 255);
constexpr ImU32 kGridColor = IM_COL32(90, 90, 98, 255);
constexpr ImU32 kBorderColor = IM_COL32(200, 200, 210, 255);

// Maps UV space to the canvas and back for one frame.
struct UvMapping {
  ImVec2 canvas_center;
  glm::vec2 center;
  float scale;  // pixels per UV unit

  [[nodiscard]] ImVec2 ToScreen(glm::vec2 uv) const {
    return {canvas_center.x + (uv.x - center.x) * scale,
            canvas_center.y + (uv.y - center.y) * scale};
  }
  [[nodiscard]] glm::vec2 ToUv(ImVec2 screen) const {
    return {center.x + (screen.x - canvas_center.x) / scale,
            center.y + (screen.y - canvas_center.y) / scale};
  }
};

// Handles zoom (wheel at the cursor), pan (MMB / RMB drag) and reset
// (double-click) of the hovered canvas.
void HandleInput(const UvMapping& mapping, float base_scale,
                 UvViewState& state) {
  const ImGuiIO& io = ImGui::GetIO();
  if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
    state = UvViewState{};
    return;
  }
  if (io.MouseWheel != 0.0f) {
    const glm::vec2 cursor_uv = mapping.ToUv(io.MousePos);
    state.zoom = std::clamp(state.zoom * std::pow(kZoomStep, io.MouseWheel),
                            kMinZoom, kMaxZoom);
    const float scale = base_scale * state.zoom;
    state.center =
        cursor_uv - glm::vec2(io.MousePos.x - mapping.canvas_center.x,
                              io.MousePos.y - mapping.canvas_center.y) /
                        scale;
  }
  if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle) ||
      ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
    state.center -= glm::vec2(io.MouseDelta.x, io.MouseDelta.y) / mapping.scale;
  }
}

// Draws the background: the texture over the 0..1 square (repeated over
// the visible cells when tiling), else a plain square.
void DrawBackground(ImDrawList& draw, const UvMapping& mapping,
                    const UvViewOptions& options, ImVec2 canvas_min,
                    ImVec2 canvas_max) {
  if (options.background == ImTextureID{}) {
    draw.AddRectFilled(mapping.ToScreen({0.0f, 0.0f}),
                       mapping.ToScreen({1.0f, 1.0f}), kSquareColor);
    return;
  }
  int min_x = 0;
  int min_y = 0;
  int max_x = 0;
  int max_y = 0;
  if (options.tile_background) {
    const glm::vec2 uv_min = mapping.ToUv(canvas_min);
    const glm::vec2 uv_max = mapping.ToUv(canvas_max);
    min_x = static_cast<int>(std::floor(uv_min.x));
    min_y = static_cast<int>(std::floor(uv_min.y));
    max_x = static_cast<int>(std::floor(uv_max.x));
    max_y = static_cast<int>(std::floor(uv_max.y));
    if ((max_x - min_x + 1) * (max_y - min_y + 1) > kMaxTiles) {
      min_x = max_x = min_y = max_y = 0;
    }
  }
  for (int y = min_y; y <= max_y; ++y) {
    for (int x = min_x; x <= max_x; ++x) {
      const glm::vec2 cell(static_cast<float>(x), static_cast<float>(y));
      const ImU32 tint =
          x == 0 && y == 0 ? IM_COL32_WHITE : IM_COL32(150, 150, 150, 255);
      draw.AddImage(options.background, mapping.ToScreen(cell),
                    mapping.ToScreen(cell + glm::vec2(1.0f)), ImVec2(0, 0),
                    ImVec2(1, 1), tint);
    }
  }
}

// Draws a 10 x 10 grid over the 0..1 square and its border.
void DrawGrid(ImDrawList& draw, const UvMapping& mapping) {
  for (int i = 1; i < 10; ++i) {
    const float t = static_cast<float>(i) * 0.1f;
    draw.AddLine(mapping.ToScreen({t, 0.0f}), mapping.ToScreen({t, 1.0f}),
                 kGridColor);
    draw.AddLine(mapping.ToScreen({0.0f, t}), mapping.ToScreen({1.0f, t}),
                 kGridColor);
  }
  draw.AddRect(mapping.ToScreen({0.0f, 0.0f}), mapping.ToScreen({1.0f, 1.0f}),
               kBorderColor);
}

}  // namespace

bool DrawUvView(const char* id, std::span<const asset::Vertex> vertices,
                std::span<const asset::Index> indices,
                const UvViewOptions& options, UvViewState& state, ImVec2 size) {
  const ImVec2 available = ImGui::GetContentRegionAvail();
  const ImVec2 canvas_size(
      size.x > 0.0f ? size.x : std::max(available.x, 32.0f),
      size.y > 0.0f ? size.y : std::max(available.y, 32.0f));
  const ImVec2 canvas_min = ImGui::GetCursorScreenPos();
  const ImVec2 canvas_max(canvas_min.x + canvas_size.x,
                          canvas_min.y + canvas_size.y);
  ImGui::InvisibleButton(id, canvas_size,
                         ImGuiButtonFlags_MouseButtonLeft |
                             ImGuiButtonFlags_MouseButtonMiddle |
                             ImGuiButtonFlags_MouseButtonRight);
  const bool hovered = ImGui::IsItemHovered();

  const float base_scale = std::min(canvas_size.x, canvas_size.y) * 0.9f;
  UvMapping mapping{.canvas_center = {(canvas_min.x + canvas_max.x) * 0.5f,
                                      (canvas_min.y + canvas_max.y) * 0.5f},
                    .center = state.center,
                    .scale = base_scale * state.zoom};
  if (hovered || ImGui::IsItemActive()) {
    HandleInput(mapping, base_scale, state);
    mapping.center = state.center;
    mapping.scale = base_scale * state.zoom;
  }

  ImDrawList& draw = *ImGui::GetWindowDrawList();
  draw.PushClipRect(canvas_min, canvas_max, true);
  draw.AddRectFilled(canvas_min, canvas_max, kCanvasColor);
  DrawBackground(draw, mapping, options, canvas_min, canvas_max);
  DrawGrid(draw, mapping);

  const size_t triangle_count = indices.size() / 3;
  const size_t limit = std::min(
      triangle_count, static_cast<size_t>(std::max(options.max_triangles, 0)));
  for (size_t t = 0; t < limit; ++t) {
    std::array<ImVec2, 3> corners;
    bool valid = true;
    for (size_t k = 0; k < 3; ++k) {
      const asset::Index index = indices[t * 3 + k];
      if (index >= vertices.size()) {
        valid = false;
        break;
      }
      const asset::Vertex& vertex = vertices[index];
      corners[k] =
          mapping.ToScreen(options.uv_set == 1 ? vertex.uv1 : vertex.uv0);
    }
    if (!valid) {
      continue;
    }
    // Skip triangles entirely outside the canvas.
    const float min_x = std::min({corners[0].x, corners[1].x, corners[2].x});
    const float max_x = std::max({corners[0].x, corners[1].x, corners[2].x});
    const float min_y = std::min({corners[0].y, corners[1].y, corners[2].y});
    const float max_y = std::max({corners[0].y, corners[1].y, corners[2].y});
    if (max_x < canvas_min.x || min_x > canvas_max.x || max_y < canvas_min.y ||
        min_y > canvas_max.y) {
      continue;
    }
    draw.AddPolyline(corners.data(), 3, options.wire_color, 1.0f,
                     ImDrawFlags_Closed);
  }
  draw.PopClipRect();

  char caption[96];
  std::snprintf(caption, sizeof(caption), "uv%d  %zu triangles%s  zoom %.2f",
                options.uv_set, triangle_count,
                limit < triangle_count ? " (capped)" : "", state.zoom);
  draw.AddText(ImVec2(canvas_min.x + 6.0f, canvas_min.y + 4.0f),
               IM_COL32(230, 230, 230, 230), caption);
  return hovered;
}

}  // namespace blue::editor
