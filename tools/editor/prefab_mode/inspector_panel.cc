#include <imgui.h>

#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "animation/transform.h"
#include "asset/mesh_data.h"
#include "core/path.h"
#include "editor/dialogs.h"
#include "editor/document.h"
#include "editor/drag_drop.h"
#include "editor/editor_context.h"
#include "editor/prefab_mode/prefab_edit.h"
#include "editor/prefab_mode/prefab_mode_state.h"
#include "editor/prefab_mode/prefab_panels.h"
#include "editor/prefab_mode/prefab_widgets.h"
#include "editor/selection.h"
#include "editor/undo.h"
#include "prefab/prefab.h"

namespace blue::editor {

namespace {

// Shows the name field of `item`, renaming it on commit.
void NameField(EditorContext& context, PrefabDocument& document,
               PrefabModeShared& shared, SelectionItem item) {
  const std::optional<std::string> name = ItemName(document.state(), item);
  if (!name) {
    return;
  }
  std::string edited;
  if (InputTextCommit(shared, "Name", *name, edited)) {
    TryEdit(context, document, "Rename",
            item.kind == SelectionKind::kClip ? ChangeKind::kStructure
                                              : ChangeKind::kParameters,
            [&](prefab::Prefab& prefab) {
              return RenameItem(prefab, item, std::move(edited));
            });
  }
}

// Shows a node combo that re-attaches the item, keeping its placement.
void ParentField(EditorContext& context, PrefabDocument& document,
                 AttachmentKind kind, int index, int node) {
  int chosen = node;
  if (NodeCombo("Parent node", document.state(), chosen)) {
    TryEdit(context, document, "Change parent node",
            kind == AttachmentKind::kNode ? ChangeKind::kStructure
                                          : ChangeKind::kParameters,
            [&](prefab::Prefab& prefab) -> std::optional<std::string> {
              if (!MoveToNode(prefab, kind, index, chosen)) {
                return std::string("a node cannot move into its own subtree");
              }
              return std::nullopt;
            });
  }
}

void BoundsText(const asset::Bounds& bounds) {
  const glm::vec3 size = bounds.size();
  ImGui::Text("Bounds %.3f x %.3f x %.3f m", static_cast<double>(size.x),
              static_cast<double>(size.y), static_cast<double>(size.z));
}

}  // namespace

void PrefabInspectorPanel::Draw(EditorContext& context) {
  PrefabDocument* document = context.prefab_document();
  if (document == nullptr) {
    ImGui::TextDisabled("No prefab open.");
    return;
  }
  const std::optional<SelectionItem> primary = document->selection().primary();
  if (!primary) {
    const PrefabSummary summary = Summarize(document->state());
    ImGui::TextDisabled("Nothing selected.");
    ImGui::Text("%d nodes, %d meshes, %d materials, %d textures",
                summary.node_count, summary.mesh_count, summary.material_count,
                summary.texture_count);
    ImGui::Text("%d clips, %d sockets, %d lights, %d child prefabs",
                summary.clip_count, summary.socket_count, summary.light_count,
                summary.child_count);
    BoundsText(document->state().bounds);
    DrawTags(context, *document);
    return;
  }
  ImGui::SeparatorText(ItemLabel(document->state(), *primary).c_str());
  const int index = static_cast<int>(primary->index);
  switch (primary->kind) {
    case SelectionKind::kNode:
      DrawNode(context, *document, index);
      break;
    case SelectionKind::kMesh:
      DrawMesh(context, *document, index);
      break;
    case SelectionKind::kMaterial:
      DrawMaterialEditor(context, *document, shared_, index);
      break;
    case SelectionKind::kTexture:
      DrawTexture(context, *document, index);
      break;
    case SelectionKind::kSocket:
      NameField(context, *document, shared_, *primary);
      DrawSocket(*document, index);
      ParentField(context, *document, AttachmentKind::kSocket, index,
                  document->state().sockets[static_cast<size_t>(index)].node);
      break;
    case SelectionKind::kLight:
      NameField(context, *document, shared_, *primary);
      DrawLight(*document, index);
      ParentField(context, *document, AttachmentKind::kLight, index,
                  document->state().lights[static_cast<size_t>(index)].node);
      break;
    case SelectionKind::kChild:
      DrawChild(context, *document, index);
      break;
    case SelectionKind::kClip: {
      NameField(context, *document, shared_, *primary);
      const animation::Clip& clip =
          document->state().clips[static_cast<size_t>(index)];
      ImGui::Text("%.3f s, %zu channels, %zu events, %s",
                  static_cast<double>(clip.duration), clip.channels.size(),
                  clip.events.size(), clip.loop ? "loops" : "plays once");
      ImGui::TextDisabled("Edit it in the Animation panel.");
      break;
    }
    case SelectionKind::kCollider:
      DrawPhysicsEditor(context, *document, shared_);
      break;
    default:
      break;
  }
  ImGui::Spacing();
  if (ImGui::Button("Delete")) {
    if (TryEdit(context, *document, "Delete", ChangeKind::kStructure,
                [&](prefab::Prefab& prefab) {
                  return DeleteItem(prefab, *primary);
                })) {
      document->selection().Clear();
    }
  }
}

void PrefabInspectorPanel::DrawNode(EditorContext& context,
                                    PrefabDocument& document, int node) {
  NameField(context, document, shared_,
            {.kind = SelectionKind::kNode, .index = node});
  const size_t i = static_cast<size_t>(node);
  animation::Transform rest = document.state().hierarchy.nodes[i].rest;
  // The preview shows rest-pose edits live from the document; the release
  // makes it a structural step so the GPU copy (and clips) pick it up.
  const FieldEdit edit = TransformFields("rest", rest, shared_.euler);
  if (edit.changed) {
    prefab::Prefab& prefab =
        document.BeginEdit("Move node", ChangeKind::kParameters);
    prefab.hierarchy.nodes[i].rest = rest;
    UpdatePrefabBounds(prefab);
  }
  CommitOnRelease(document, edit.released, /*structural=*/true);
  ParentField(context, document, AttachmentKind::kNode, node,
              document.state().hierarchy.nodes[i].parent);
}

void PrefabInspectorPanel::DrawMesh(EditorContext& context,
                                    PrefabDocument& document, int mesh_index) {
  NameField(context, document, shared_,
            {.kind = SelectionKind::kMesh, .index = mesh_index});
  const prefab::Prefab& prefab = document.state();
  const prefab::Mesh& mesh = prefab.meshes[static_cast<size_t>(mesh_index)];
  ImGui::Text("%zu vertices, %zu triangles, %zu submeshes",
              mesh.vertices.size(), mesh.indices.size() / 3,
              mesh.submeshes.size());
  BoundsText(mesh.bounds);
  ImGui::Text("Node: %s",
              mesh.node < 0
                  ? "(prefab root)"
                  : prefab.hierarchy.nodes[static_cast<size_t>(mesh.node)]
                        .name.c_str());
  if (ImGui::Button("Show UVs")) {
    shared_.uv_mesh = mesh_index;
  }
  ImGui::SeparatorText("Submesh materials");
  for (size_t s = 0; s < mesh.submeshes.size(); ++s) {
    const asset::Submesh& submesh = mesh.submeshes[s];
    ImGui::PushID(static_cast<int>(s));
    const char* current =
        submesh.material >= 0 &&
                submesh.material < static_cast<int>(prefab.materials.size())
            ? prefab.materials[static_cast<size_t>(submesh.material)]
                  .name.c_str()
            : "?";
    const std::string label =
        std::format("#{} ({} tris)", s, submesh.index_count / 3);
    int chosen = -1;
    if (ImGui::BeginCombo(label.c_str(), current)) {
      for (size_t m = 0; m < prefab.materials.size(); ++m) {
        ImGui::PushID(static_cast<int>(m));
        if (ImGui::Selectable(prefab.materials[m].name.c_str(),
                              static_cast<int>(m) == submesh.material)) {
          chosen = static_cast<int>(m);
        }
        ImGui::PopID();
      }
      ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Edit")) {
      document.selection().Set(
          {.kind = SelectionKind::kMaterial, .index = submesh.material});
    }
    ImGui::PopID();
    if (chosen >= 0) {
      document.Edit("Assign material", ChangeKind::kStructure,
                    [&](prefab::Prefab& edited) {
                      (void)SetSubmeshMaterial(edited, mesh_index,
                                               static_cast<int>(s), chosen);
                    });
      break;  // `mesh` may be stale now.
    }
  }
}

void PrefabInspectorPanel::DrawTags(EditorContext& context,
                                    PrefabDocument& document) {
  ImGui::SeparatorText("Tags");
  ImGui::TextDisabled("key = value; e.g. weapon = glock17 makes a weapon.");
  // Copied: an edit replaces the document's state mid-loop.
  const std::vector<prefab::Tag> tags = document.state().tags;
  for (const prefab::Tag& tag : tags) {
    ImGui::PushID(tag.key.c_str());
    std::string edited;
    if (InputTextCommit(shared_, tag.key.c_str(), tag.value, edited)) {
      TryEdit(context, document, "Set tag", ChangeKind::kParameters,
              [&](prefab::Prefab& prefab) {
                return SetPrefabTag(prefab, tag.key, edited);
              });
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Remove")) {
      TryEdit(context, document, "Remove tag", ChangeKind::kParameters,
              [&](prefab::Prefab& prefab) {
                return RemovePrefabTag(prefab, tag.key);
              });
    }
    ImGui::PopID();
  }
  ImGui::PushID("new tag");
  (void)InputString("Key", new_tag_key_);
  (void)InputString("Value", new_tag_value_);
  if (ImGui::Button("Add tag") && !new_tag_key_.empty()) {
    if (TryEdit(context, document, "Add tag", ChangeKind::kParameters,
                [&](prefab::Prefab& prefab) -> std::optional<std::string> {
                  if (prefab::FindTag(prefab, new_tag_key_)) {
                    return std::format("tag '{}' exists; edit its value",
                                       new_tag_key_);
                  }
                  return SetPrefabTag(prefab, new_tag_key_, new_tag_value_);
                })) {
      new_tag_key_.clear();
      new_tag_value_.clear();
    }
  }
  ImGui::PopID();
}

void PrefabInspectorPanel::DrawSocket(PrefabDocument& document, int socket) {
  const size_t i = static_cast<size_t>(socket);
  animation::Transform local = document.state().sockets[i].local;
  const FieldEdit edit = TransformFields("socket", local, shared_.euler);
  if (edit.changed) {
    document.BeginEdit("Move socket", ChangeKind::kParameters)
        .sockets[i]
        .local = local;
  }
  CommitOnRelease(document, edit.released);
}

void PrefabInspectorPanel::DrawLight(PrefabDocument& document, int light) {
  const size_t i = static_cast<size_t>(light);
  prefab::PointLight edited = document.state().lights[i];
  bool changed = false;
  bool released = false;
  const auto track = [&](bool now) {
    changed |= now;
    released |= ImGui::IsItemDeactivatedAfterEdit();
  };
  track(ImGui::DragFloat3("Position", &edited.position.x, 0.005f, 0.0f, 0.0f,
                          "%.3f"));
  track(
      ImGui::ColorEdit3("Colour", &edited.color.x, ImGuiColorEditFlags_Float));
  track(
      ImGui::DragFloat("Intensity", &edited.intensity, 0.05f, 0.0f, 10000.0f));
  track(ImGui::DragFloat("Range (m)", &edited.range, 0.05f, 0.01f, 10000.0f));
  if (changed) {
    document.BeginEdit("Edit light", ChangeKind::kParameters).lights[i] =
        edited;
  }
  CommitOnRelease(document, released);
}

void PrefabInspectorPanel::DrawChild(EditorContext& context,
                                     PrefabDocument& document, int child) {
  const size_t i = static_cast<size_t>(child);
  const prefab::ChildPrefab& current = document.state().children[i];
  ImGui::TextWrapped("%s", current.path.c_str());
  if (std::optional<std::string> dropped = AcceptAssetDrop(FileKind::kPrefab)) {
    TryEdit(
        context, document, "Replace child prefab", ChangeKind::kStructure,
        [&](prefab::Prefab& prefab) -> std::optional<std::string> {
          prefab::ChildPrefab& target = prefab.children[i];
          if (std::optional<std::string> cycle = FindChildCycle(
                  document.path(), *dropped, ReadChildPathsFromFile, nullptr)) {
            return "Cannot use " + core::FileName(*dropped) + ": " + *cycle;
          }
          target.path = *dropped;
          return std::nullopt;
        });
  }
  if (ImGui::SmallButton("Open in Prefab mode")) {
    const std::string path = current.path;
    (void)context.OpenFile(path);
    return;
  }
  animation::Transform local = current.local;
  const FieldEdit edit = TransformFields("child", local, shared_.euler);
  if (edit.changed) {
    document.BeginEdit("Move child prefab", ChangeKind::kParameters)
        .children[i]
        .local = local;
  }
  CommitOnRelease(document, edit.released);
  ParentField(context, document, AttachmentKind::kChild, child,
              document.state().children[i].node);
}

void PrefabInspectorPanel::DrawTexture(EditorContext& context,
                                       PrefabDocument& document, int texture) {
  NameField(context, document, shared_,
            {.kind = SelectionKind::kTexture, .index = texture});
  const prefab::Prefab& prefab = document.state();
  const prefab::Texture& cooked = prefab.textures[static_cast<size_t>(texture)];
  ImGui::Text("%s, %d x %d, %d mips, %.1f KiB", FormatLabel(cooked.format),
              cooked.width, cooked.height, cooked.mip_count(),
              static_cast<double>(TextureByteSize(cooked)) / 1024.0);
  ImGui::Text("Used by %d material slot(s)", CountTextureUses(prefab, texture));
  if (texture < static_cast<int>(prefab.edit.texture_sources.size())) {
    for (const std::string& source :
         prefab.edit.texture_sources[static_cast<size_t>(texture)]) {
      ImGui::TextDisabled("source: %s", source.c_str());
    }
  }
  const ImTextureID thumbnail = shared_.thumbnails.Get(context, cooked);
  if (thumbnail != ImTextureID{}) {
    const float side = ImGui::GetContentRegionAvail().x;
    const float aspect = static_cast<float>(cooked.height) /
                         static_cast<float>(std::max(1, cooked.width));
    ImGui::Image(thumbnail, {side, side * aspect});
  }
}

}  // namespace blue::editor
