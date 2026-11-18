#include "editor/prefab_mode/prefab_viewport.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/matrix.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "animation/hierarchy.h"
#include "animation/transform.h"
#include "asset/mesh_data.h"
#include "cook/collision_cook.h"
#include "core/result.h"
#include "editor/document.h"
#include "editor/editor_context.h"
#include "editor/prefab_mode/prefab_edit.h"
#include "editor/prefab_mode/prefab_mode_state.h"
#include "editor/selection.h"
#include "editor/undo.h"
#include "editor/viewport/debug_draw.h"
#include "editor/viewport/editor_camera.h"
#include "editor/viewport/gizmo.h"
#include "editor/viewport/viewport_panel.h"
#include "prefab/prefab.h"
#include "render/scene_view.h"
#include "world/prefab_cache.h"
#include "world/prefab_pose.h"

namespace blue::editor {

namespace {

// Sockets and lights are picked when the ray passes within this fraction
// of their distance (about 2 degrees).
constexpr float kHandlePickCone = 0.035f;
// Opening a prefab frames it from the front-right and above (front = -Z),
// at this many bounding radii.
constexpr glm::vec3 kFrameDirection{0.55f, 0.35f, -0.76f};
constexpr float kFrameDistanceFactor = 2.6f;
constexpr float kMinFrameRadius = 0.05f;
// Half size of the box framed for a socket or light (F).
constexpr float kHandleFocusRadius = 0.1f;

// Returns the world-space box around `bounds` after `matrix`.
[[nodiscard]] asset::Bounds TransformBounds(const asset::Bounds& bounds,
                                            const glm::mat4& matrix) {
  glm::vec3 low(std::numeric_limits<float>::max());
  glm::vec3 high(std::numeric_limits<float>::lowest());
  for (int corner = 0; corner < 8; ++corner) {
    const glm::vec3 local{(corner & 1) ? bounds.max.x : bounds.min.x,
                          (corner & 2) ? bounds.max.y : bounds.min.y,
                          (corner & 4) ? bounds.max.z : bounds.min.z};
    const glm::vec3 point = glm::vec3(matrix * glm::vec4(local, 1.0f));
    low = glm::min(low, point);
    high = glm::max(high, point);
  }
  return {.min = low, .max = high};
}

void Merge(std::optional<asset::Bounds>& into, const asset::Bounds& bounds) {
  if (!into) {
    into = bounds;
    return;
  }
  into->min = glm::min(into->min, bounds.min);
  into->max = glm::max(into->max, bounds.max);
}

// Returns the ray parameter where `origin + t * direction` enters `bounds`,
// or nullopt when it misses.
[[nodiscard]] std::optional<float> RayBox(const glm::vec3& origin,
                                          const glm::vec3& direction,
                                          const asset::Bounds& bounds) {
  float near = 0.0f;
  float far = std::numeric_limits<float>::max();
  for (int axis = 0; axis < 3; ++axis) {
    if (std::abs(direction[axis]) < 1e-12f) {
      if (origin[axis] < bounds.min[axis] || origin[axis] > bounds.max[axis]) {
        return std::nullopt;
      }
      continue;
    }
    float t0 = (bounds.min[axis] - origin[axis]) / direction[axis];
    float t1 = (bounds.max[axis] - origin[axis]) / direction[axis];
    if (t0 > t1) {
      std::swap(t0, t1);
    }
    near = std::max(near, t0);
    far = std::min(far, t1);
    if (near > far) {
      return std::nullopt;
    }
  }
  return near;
}

// Returns the closest ray parameter at which the ray hits a triangle of
// `mesh` (both sides), or nullopt (Moller-Trumbore).
[[nodiscard]] std::optional<float> RayMesh(const glm::vec3& origin,
                                           const glm::vec3& direction,
                                           const prefab::Mesh& mesh) {
  if (!RayBox(origin, direction, mesh.bounds)) {
    return std::nullopt;
  }
  std::optional<float> best;
  const size_t vertex_count = mesh.vertices.size();
  for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
    const asset::Index a = mesh.indices[i];
    const asset::Index b = mesh.indices[i + 1];
    const asset::Index c = mesh.indices[i + 2];
    if (a >= vertex_count || b >= vertex_count || c >= vertex_count) {
      continue;
    }
    const glm::vec3 p0 = mesh.vertices[a].position;
    const glm::vec3 e1 = mesh.vertices[b].position - p0;
    const glm::vec3 e2 = mesh.vertices[c].position - p0;
    const glm::vec3 p = glm::cross(direction, e2);
    const float det = glm::dot(e1, p);
    if (std::abs(det) < 1e-14f) {
      continue;
    }
    const float inv = 1.0f / det;
    const glm::vec3 s = origin - p0;
    const float u = glm::dot(s, p) * inv;
    if (u < 0.0f || u > 1.0f) {
      continue;
    }
    const glm::vec3 q = glm::cross(s, e1);
    const float v = glm::dot(direction, q) * inv;
    if (v < 0.0f || u + v > 1.0f) {
      continue;
    }
    const float t = glm::dot(e2, q) * inv;
    if (t > 0.0f && (!best || t < *best)) {
      best = t;
    }
  }
  return best;
}

// Returns a gizmo id unique to `item`.
[[nodiscard]] int GizmoId(const SelectionItem& item) {
  return static_cast<int>(item.kind) * 1000003 + static_cast<int>(item.index);
}

}  // namespace

void PrefabViewportClient::Prepare(EditorContext& context,
                                   const PrefabDocument& document) {
  const int frame = ImGui::GetFrameCount();
  if (prepared_frame_ == frame) {
    return;
  }
  prepared_frame_ = frame;
  const prefab::Prefab& prefab = document.state();
  const size_t count = prefab.hierarchy.nodes.size();
  const world::PrefabPose* pose = context.prefab_pose();
  nodes_.resize(count);
  if (pose != nullptr && pose->clip() >= 0 &&
      pose->node_matrices().size() == count) {
    std::copy(pose->node_matrices().begin(), pose->node_matrices().end(),
              nodes_.begin());
  } else {
    // Rest pose from the document, so parameter edits show at once.
    rest_.resize(count);
    animation::ResetToRest(prefab.hierarchy, rest_);
    if (animation::IsValid(prefab.hierarchy)) {
      animation::ComputeWorldMatrices(prefab.hierarchy, rest_, nodes_);
    }
  }

  // Child previews follow the document's child list.
  if (children_.size() != prefab.children.size()) {
    children_.resize(prefab.children.size());
  }
  for (size_t i = 0; i < prefab.children.size(); ++i) {
    ChildPreview& preview = children_[i];
    if (preview.path == prefab.children[i].path) {
      continue;
    }
    preview = {.path = prefab.children[i].path};
    core::Result<world::PrefabId, prefab::PrefabError> loaded =
        context.prefab_cache().Get(preview.path);
    if (loaded.ok()) {
      preview.prefab = loaded.value();
      preview.pose.emplace(context.prefab_cache(), loaded.value());
    } else {
      context.Status("Cannot load child prefab " + preview.path + ": " +
                     prefab::ToString(loaded.error()));
    }
  }
}

glm::mat4 PrefabViewportClient::Node(int node) const {
  return node >= 0 && node < static_cast<int>(nodes_.size())
             ? nodes_[static_cast<size_t>(node)]
             : glm::mat4(1.0f);
}

glm::mat4 PrefabViewportClient::ChildMatrix(const prefab::Prefab& prefab,
                                            int child) const {
  const prefab::ChildPrefab& entry =
      prefab.children[static_cast<size_t>(child)];
  return Node(entry.node) * animation::ToMatrix(entry.local);
}

void PrefabViewportClient::DrawOverlay(EditorContext& context,
                                       ViewportPanel& viewport) {
  PrefabDocument* document = context.prefab_document();
  if (document == nullptr) {
    return;
  }
  Prepare(context, *document);
  FrameNewDocument(viewport, *document);
  const prefab::Prefab& prefab = document->state();
  const ViewportToggles& toggles = viewport.toggles();
  render::DebugLines& lines = viewport.lines();
  const Selection& selection = document->selection();
  const float socket_size =
      std::clamp(glm::length(prefab.bounds.size()) * 0.08f, 0.01f, 0.5f);

  if (toggles.sockets) {
    for (size_t i = 0; i < prefab.sockets.size(); ++i) {
      const prefab::Socket& socket = prefab.sockets[i];
      const glm::mat4 world =
          Node(socket.node) * animation::ToMatrix(socket.local);
      AddSocket(lines, world, socket_size);
      viewport.AddLabel(glm::vec3(world[3]), socket.name);
    }
  }
  if (toggles.lights) {
    for (size_t i = 0; i < prefab.lights.size(); ++i) {
      const prefab::PointLight& light = prefab.lights[i];
      const bool selected = selection.Contains(
          {.kind = SelectionKind::kLight, .index = static_cast<int64_t>(i)});
      AddPointLight(
          lines, glm::vec3(Node(light.node) * glm::vec4(light.position, 1.0f)),
          light.range, selected);
    }
  }
  if (toggles.colliders ||
      selection.Contains({.kind = SelectionKind::kCollider})) {
    AddCollider(lines, prefab.physics, glm::mat4(1.0f), prefab.bounds);
  }
  if (toggles.bounds) {
    for (const SelectionItem& item : selection.items()) {
      const int index = static_cast<int>(item.index);
      if (item.kind == SelectionKind::kMesh &&
          index < static_cast<int>(prefab.meshes.size())) {
        const prefab::Mesh& mesh = prefab.meshes[static_cast<size_t>(index)];
        AddBounds(lines, mesh.bounds, Node(mesh.node));
      } else if (item.kind == SelectionKind::kChild &&
                 index < static_cast<int>(children_.size()) &&
                 children_[static_cast<size_t>(index)].prefab) {
        const world::PrefabId id =
            *children_[static_cast<size_t>(index)].prefab;
        AddBounds(lines, context.prefab_cache().prefab(id).bounds,
                  ChildMatrix(prefab, index));
      }
    }
  }
  Manipulate(context, viewport, *document);
}

void PrefabViewportClient::FrameNewDocument(ViewportPanel& viewport,
                                            const PrefabDocument& document) {
  // A new document, or one loaded at a reused address (untouched since);
  // Save As on an edited document keeps the view.
  const bool fresh_load =
      framed_path_ != document.path() && document.revision().any == 0;
  if (framed_document_ == &document && !fresh_load) {
    framed_path_ = document.path();
    return;
  }
  framed_document_ = &document;
  framed_path_ = document.path();
  const asset::Bounds& bounds = document.state().bounds;
  const float radius =
      std::max(glm::length(bounds.size()) * 0.5f, kMinFrameRadius);
  const glm::vec3 center = bounds.center();
  viewport.camera().LookAt(
      center + kFrameDirection * (radius * kFrameDistanceFactor), center);
}

void PrefabViewportClient::Manipulate(EditorContext& context,
                                      ViewportPanel& viewport,
                                      PrefabDocument& document) {
  const std::optional<SelectionItem> primary = document.selection().primary();
  if (!primary) {
    return;
  }
  const prefab::Prefab& prefab = document.state();
  const size_t i = static_cast<size_t>(primary->index);
  glm::mat4 world(1.0f);
  glm::mat4 parent(1.0f);
  switch (primary->kind) {
    case SelectionKind::kSocket:
      if (i >= prefab.sockets.size()) {
        return;
      }
      parent = Node(prefab.sockets[i].node);
      world = parent * animation::ToMatrix(prefab.sockets[i].local);
      break;
    case SelectionKind::kLight:
      if (i >= prefab.lights.size()) {
        return;
      }
      parent = Node(prefab.lights[i].node);
      world = glm::translate(
          glm::mat4(1.0f),
          glm::vec3(parent * glm::vec4(prefab.lights[i].position, 1.0f)));
      break;
    case SelectionKind::kChild:
      if (i >= prefab.children.size()) {
        return;
      }
      parent = Node(prefab.children[i].node);
      world = ChildMatrix(prefab, static_cast<int>(i));
      break;
    case SelectionKind::kNode:
      if (i >= prefab.hierarchy.nodes.size()) {
        return;
      }
      parent = Node(prefab.hierarchy.nodes[i].parent);
      world = Node(static_cast<int>(i));
      break;
    case SelectionKind::kCollider: {
      const prefab::ColliderKind kind = prefab.physics.kind;
      if (kind != prefab::ColliderKind::kBox &&
          kind != prefab::ColliderKind::kSphere &&
          kind != prefab::ColliderKind::kCapsule) {
        return;
      }
      world = glm::translate(glm::mat4(1.0f), prefab.physics.center) *
              glm::mat4_cast(prefab.physics.rotation);
      break;
    }
    default:
      return;
  }
  const GizmoEvent event =
      viewport.Manipulate(world, GizmoId(*primary), &parent);
  if (event == GizmoEvent::kNone) {
    return;
  }
  shared_.gizmo_dragging = event != GizmoEvent::kEnd;
  const animation::Transform local = LocalFromWorld(parent, world);
  switch (primary->kind) {
    case SelectionKind::kSocket:
      document.BeginEdit("Move socket", ChangeKind::kParameters)
          .sockets[i]
          .local = local;
      break;
    case SelectionKind::kLight:
      document.BeginEdit("Move light", ChangeKind::kParameters)
          .lights[i]
          .position = local.translation;
      break;
    case SelectionKind::kChild:
      document.BeginEdit("Move child prefab", ChangeKind::kParameters)
          .children[i]
          .local = local;
      break;
    case SelectionKind::kNode: {
      prefab::Prefab& edited =
          document.BeginEdit("Move node", ChangeKind::kParameters);
      edited.hierarchy.nodes[i].rest = local;
      if (event == GizmoEvent::kEnd) {
        UpdatePrefabBounds(edited);
        // The GPU copy (clips, bounds) picks the new rest pose up once.
        (void)document.BeginEdit("Move node", ChangeKind::kStructure);
      }
      break;
    }
    case SelectionKind::kCollider: {
      prefab::Prefab& edited =
          document.BeginEdit("Move collider", ChangeKind::kParameters);
      edited.physics.center = local.translation;
      edited.physics.rotation = local.rotation;
      if (event == GizmoEvent::kEnd && !edited.physics.cooked_shape.empty()) {
        const prefab::Physics settings = edited.physics;
        const core::Result<void, cook::CookError> cooked =
            cook::CookCollision(edited, settings, cook::ShapeFit::kParameters);
        if (!cooked.ok()) {
          context.Status("Collider re-cook failed: " +
                         CookErrorText(cooked.error()));
        }
      }
      break;
    }
    default:
      break;
  }
  if (event == GizmoEvent::kEnd) {
    document.CommitEdit();
  }
}

void PrefabViewportClient::Pick(EditorContext& context,
                                ViewportPanel& /*viewport*/, const Ray& ray,
                                bool additive) {
  PrefabDocument* document = context.prefab_document();
  if (document == nullptr) {
    return;
  }
  Prepare(context, *document);
  const prefab::Prefab& prefab = document->state();
  const glm::vec3 direction = glm::normalize(ray.direction);
  std::optional<SelectionItem> hit;
  float best = std::numeric_limits<float>::max();

  // Handles first: the point closest to the ray within the cone.
  const auto handle = [&](const glm::vec3& point, SelectionItem item) {
    const float along = glm::dot(point - ray.origin, direction);
    if (along <= 0.0f) {
      return;
    }
    const float off = glm::length(point - (ray.origin + direction * along));
    if (off <= along * kHandlePickCone && along < best) {
      best = along;
      hit = item;
    }
  };
  for (size_t i = 0; i < prefab.sockets.size(); ++i) {
    handle(glm::vec3(Node(prefab.sockets[i].node) *
                     animation::ToMatrix(prefab.sockets[i].local)[3]),
           {.kind = SelectionKind::kSocket, .index = static_cast<int64_t>(i)});
  }
  for (size_t i = 0; i < prefab.lights.size(); ++i) {
    handle(glm::vec3(Node(prefab.lights[i].node) *
                     glm::vec4(prefab.lights[i].position, 1.0f)),
           {.kind = SelectionKind::kLight, .index = static_cast<int64_t>(i)});
  }
  if (!hit) {
    for (size_t m = 0; m < prefab.meshes.size(); ++m) {
      const prefab::Mesh& mesh = prefab.meshes[m];
      const glm::mat4 inverse = glm::inverse(Node(mesh.node));
      const glm::vec3 origin = glm::vec3(inverse * glm::vec4(ray.origin, 1.0f));
      const glm::vec3 local_direction =
          glm::vec3(inverse * glm::vec4(direction, 0.0f));
      // t is the same in both spaces: the map is affine.
      if (const std::optional<float> t = RayMesh(origin, local_direction, mesh);
          t && *t < best) {
        best = *t;
        hit = {.kind = SelectionKind::kMesh, .index = static_cast<int64_t>(m)};
      }
    }
    for (size_t c = 0; c < prefab.children.size() && c < children_.size();
         ++c) {
      if (!children_[c].prefab) {
        continue;
      }
      const glm::mat4 inverse =
          glm::inverse(ChildMatrix(prefab, static_cast<int>(c)));
      const asset::Bounds& bounds =
          context.prefab_cache().prefab(*children_[c].prefab).bounds;
      if (const std::optional<float> t =
              RayBox(glm::vec3(inverse * glm::vec4(ray.origin, 1.0f)),
                     glm::vec3(inverse * glm::vec4(direction, 0.0f)), bounds);
          t && *t < best) {
        best = *t;
        hit = {.kind = SelectionKind::kChild, .index = static_cast<int64_t>(c)};
      }
    }
  }
  Selection& selection = document->selection();
  if (!hit) {
    if (!additive) {
      selection.Clear();
    }
    return;
  }
  if (additive) {
    selection.Toggle(*hit);
  } else {
    selection.Set(*hit);
  }
}

std::optional<asset::Bounds> PrefabViewportClient::FocusBounds(
    EditorContext& context) {
  PrefabDocument* document = context.prefab_document();
  if (document == nullptr) {
    return std::nullopt;
  }
  Prepare(context, *document);
  const prefab::Prefab& prefab = document->state();
  std::optional<asset::Bounds> bounds;
  const auto around = [&](const glm::vec3& point) {
    Merge(bounds, {.min = point - glm::vec3(kHandleFocusRadius),
                   .max = point + glm::vec3(kHandleFocusRadius)});
  };
  for (const SelectionItem& item : document->selection().items()) {
    const size_t i = static_cast<size_t>(item.index);
    switch (item.kind) {
      case SelectionKind::kMesh:
        if (i < prefab.meshes.size()) {
          Merge(bounds, TransformBounds(prefab.meshes[i].bounds,
                                        Node(prefab.meshes[i].node)));
        }
        break;
      case SelectionKind::kNode:
        for (const prefab::Mesh& mesh : prefab.meshes) {
          if (mesh.node >= 0 &&
              IsInSubtree(prefab, mesh.node, static_cast<int>(i))) {
            Merge(bounds, TransformBounds(mesh.bounds, Node(mesh.node)));
          }
        }
        if (!bounds && i < prefab.hierarchy.nodes.size()) {
          around(glm::vec3(Node(static_cast<int>(i))[3]));
        }
        break;
      case SelectionKind::kSocket:
        if (i < prefab.sockets.size()) {
          around(glm::vec3(Node(prefab.sockets[i].node) *
                           animation::ToMatrix(prefab.sockets[i].local)[3]));
        }
        break;
      case SelectionKind::kLight:
        if (i < prefab.lights.size()) {
          around(glm::vec3(Node(prefab.lights[i].node) *
                           glm::vec4(prefab.lights[i].position, 1.0f)));
        }
        break;
      case SelectionKind::kChild:
        if (i < children_.size() && children_[i].prefab) {
          Merge(bounds,
                TransformBounds(
                    context.prefab_cache().prefab(*children_[i].prefab).bounds,
                    ChildMatrix(prefab, static_cast<int>(i))));
        }
        break;
      default:
        break;
    }
  }
  if (!bounds) {
    bounds = prefab.bounds;
  }
  return bounds;
}

void PrefabViewportClient::BuildView(EditorContext& context,
                                     ViewportPanel& /*viewport*/,
                                     render::SceneView& view) {
  draws_.clear();
  selected_.clear();
  lights_.clear();
  child_draws_.clear();
  PrefabDocument* document = context.prefab_document();
  world::PrefabPose* pose = context.prefab_pose();
  if (document == nullptr) {
    view.draws = draws_;
    return;
  }
  Prepare(context, *document);
  const prefab::Prefab& prefab = document->state();
  if (pose != nullptr) {
    pose->AppendDraws(glm::mat4(1.0f), draws_);
    // At rest the document's node matrices win (live node edits).
    if (pose->clip() < 0 && draws_.size() == prefab.meshes.size()) {
      for (size_t m = 0; m < prefab.meshes.size(); ++m) {
        draws_[m].transform = Node(prefab.meshes[m].node);
      }
    }
  }
  const uint32_t own_draws = static_cast<uint32_t>(draws_.size());
  for (size_t c = 0; c < prefab.children.size() && c < children_.size(); ++c) {
    const auto first = static_cast<uint32_t>(draws_.size());
    if (children_[c].pose) {
      children_[c].pose->AppendDraws(ChildMatrix(prefab, static_cast<int>(c)),
                                     draws_);
    }
    child_draws_.emplace_back(first,
                              static_cast<uint32_t>(draws_.size()) - first);
  }

  // Selection outline.
  const auto select_mesh = [&](size_t m) {
    if (m < own_draws) {
      selected_.push_back(static_cast<uint32_t>(m));
    }
  };
  for (const SelectionItem& item : document->selection().items()) {
    const size_t i = static_cast<size_t>(item.index);
    switch (item.kind) {
      case SelectionKind::kMesh:
        select_mesh(i);
        break;
      case SelectionKind::kNode:
        for (size_t m = 0; m < prefab.meshes.size(); ++m) {
          if (prefab.meshes[m].node >= 0 &&
              IsInSubtree(prefab, prefab.meshes[m].node, static_cast<int>(i))) {
            select_mesh(m);
          }
        }
        break;
      case SelectionKind::kMaterial:
        for (size_t m = 0; m < prefab.meshes.size(); ++m) {
          for (const asset::Submesh& submesh : prefab.meshes[m].submeshes) {
            if (submesh.material == static_cast<int>(i)) {
              select_mesh(m);
              break;
            }
          }
        }
        break;
      case SelectionKind::kChild:
        if (i < child_draws_.size()) {
          for (uint32_t d = 0; d < child_draws_[i].second; ++d) {
            selected_.push_back(child_draws_[i].first + d);
          }
        }
        break;
      default:
        break;
    }
  }

  for (const prefab::PointLight& light : prefab.lights) {
    lights_.push_back({.position = glm::vec3(Node(light.node) *
                                             glm::vec4(light.position, 1.0f)),
                       .color = light.color,
                       .intensity = light.intensity,
                       .range = light.range});
  }
  view.draws = draws_;
  view.selected_draws = selected_;
  view.point_lights = lights_;
}

}  // namespace blue::editor
