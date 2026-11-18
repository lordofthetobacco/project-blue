#ifndef BLUE_RENDER_DEBUG_LINES_H_
#define BLUE_RENDER_DEBUG_LINES_H_

#include <cstdint>
#include <span>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "asset/mesh_data.h"

namespace blue::render {

// One end of a debug line: world-space position and an RGBA8 colour (R in
// the low byte) in display space, i.e. sRGB-encoded as it appears on screen.
// Must match the vertex input of render::OverlayPass (shaders/line.vert).
struct LineVertex {
  glm::vec3 position{0.0f};
  uint32_t color = 0;
};
static_assert(sizeof(LineVertex) == 16, "LineVertex is uploaded as is");

// Returns `color` (display-space RGBA in [0, 1], clamped) packed for
// LineVertex::color.
[[nodiscard]] uint32_t PackLineColor(const glm::vec4& color);

// Whether a debug line is hidden by the scene in front of it.
enum class LineDepth {
  kTested,       // Depth-tested against the scene (grid, bounds, ...).
  kAlwaysOnTop,  // Drawn over everything (gizmo-like markers).
};

// Collects coloured line segments for one frame; SceneView borrows its
// spans (see SceneView::lines). Lines are drawn one pixel wide after
// tonemapping, so their colours are exact on screen. Keeps its allocations
// across Clear(): refilling it every frame allocates nothing once warm.
// Copyable (a plain value). Not thread-safe.
class DebugLines {
 public:
  DebugLines() = default;
  DebugLines(const DebugLines&) = default;
  DebugLines& operator=(const DebugLines&) = default;

  // Removes every line, keeping the capacity.
  void Clear();

  // Adds the segment `a` -> `b`.
  void AddLine(const glm::vec3& a, const glm::vec3& b, const glm::vec4& color,
               LineDepth depth = LineDepth::kTested);
  // Adds the 12 edges of `bounds` (local space) after `transform`.
  void AddBox(const asset::Bounds& bounds, const glm::mat4& transform,
              const glm::vec4& color, LineDepth depth = LineDepth::kTested);
  // Adds the X (red), Y (green) and Z (blue) axes of `transform`, each
  // `length` long in the transform's units.
  void AddAxes(const glm::mat4& transform, float length,
               LineDepth depth = LineDepth::kTested);
  // Adds a circle of `radius` around `center` in the plane orthogonal to
  // `normal` (need not be unit, must be nonzero), with `segments` edges
  // (clamped to >= 3).
  void AddCircle(const glm::vec3& center, const glm::vec3& normal, float radius,
                 const glm::vec4& color, int segments = 32,
                 LineDepth depth = LineDepth::kTested);
  // Adds three orthogonal great circles of a sphere (a light's range, a
  // sphere collider).
  void AddSphere(const glm::vec3& center, float radius, const glm::vec4& color,
                 LineDepth depth = LineDepth::kTested);
  // Adds a small 3D cross of half-size `size` at `center` (sockets, light
  // positions).
  void AddMarker(const glm::vec3& center, float size, const glm::vec4& color,
                 LineDepth depth = LineDepth::kAlwaysOnTop);
  // Adds a square grid in the XZ plane at height `center.y`, centred on
  // (`center.x`, `center.z`) snapped to `spacing`, reaching `half_extent`
  // in each direction. Every `major_every`-th line (> 0, counted from the
  // world origin) uses `major_color`, the others `color`. Non-positive
  // spacing or extent adds nothing.
  void AddGrid(const glm::vec3& center, float half_extent, float spacing,
               const glm::vec4& color, int major_every,
               const glm::vec4& major_color);
  // Adds every edge of the triangles of `mesh` after `transform` (collision
  // and mesh wireframes). Shared edges are drawn twice.
  void AddWireframe(const asset::MeshData& mesh, const glm::mat4& transform,
                    const glm::vec4& color,
                    LineDepth depth = LineDepth::kTested);

  // Vertex pairs, one per segment, for SceneView::lines.
  [[nodiscard]] std::span<const LineVertex> tested() const { return tested_; }
  [[nodiscard]] std::span<const LineVertex> on_top() const { return on_top_; }
  [[nodiscard]] bool empty() const {
    return tested_.empty() && on_top_.empty();
  }

 private:
  // Returns the list `depth` selects.
  std::vector<LineVertex>& List(LineDepth depth) {
    return depth == LineDepth::kTested ? tested_ : on_top_;
  }

  std::vector<LineVertex> tested_;
  std::vector<LineVertex> on_top_;
};

}  // namespace blue::render

#endif  // BLUE_RENDER_DEBUG_LINES_H_
