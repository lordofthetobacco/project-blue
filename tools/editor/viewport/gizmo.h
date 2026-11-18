#ifndef BLUE_EDITOR_VIEWPORT_GIZMO_H_
#define BLUE_EDITOR_VIEWPORT_GIZMO_H_

#include <glm/mat4x4.hpp>

#include "animation/transform.h"

// Transform gizmo settings and the helpers that convert its world matrices
// back into the transforms the documents store. The drawing itself is
// ViewportPanel::Manipulate() (ImGuizmo).

namespace blue::editor {

enum class GizmoOperation { kTranslate, kRotate, kScale };
enum class GizmoSpace { kWorld, kLocal };

// Snap steps apply while Ctrl is held.
struct GizmoSettings {
  GizmoOperation operation = GizmoOperation::kTranslate;
  GizmoSpace space = GizmoSpace::kWorld;
  float translate_snap = 0.25f;  // m
  float rotate_snap_degrees = 15.0f;
  float scale_snap = 0.1f;
  bool visible = true;
};

// What a Manipulate() call did. One drag is kBegin, any number of kChanged
// (including on the kBegin frame's movement, reported as kBegin), kEnd:
// wrap it in one undo step (BeginEdit on kBegin, CommitEdit on kEnd).
enum class GizmoEvent {
  kNone,     // Not touched.
  kBegin,    // Drag started this frame; the matrix may already be changed.
  kChanged,  // Dragging; the matrix changed.
  kEnd,      // Released this frame; the matrix is final.
};

// Returns `world` expressed relative to `parent_world` (inverse(parent) *
// world) decomposed into translation, rotation and scale. Shear (from
// non-uniform parent scale plus rotation) is dropped.
// The inverse is animation::ToMatrix().
[[nodiscard]] animation::Transform LocalFromWorld(const glm::mat4& parent_world,
                                                  const glm::mat4& world);

}  // namespace blue::editor

#endif  // BLUE_EDITOR_VIEWPORT_GIZMO_H_
