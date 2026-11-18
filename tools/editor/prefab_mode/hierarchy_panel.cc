#include <imgui.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cook/model_cook.h"
#include "cook/texture_cook.h"
#include "core/path.h"
#include "core/result.h"
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

// ImGui payload of an item dragged inside the hierarchy.
constexpr const char* kItemPayload = "BLUE_PREFAB_ITEM";

struct ItemPayload {
  AttachmentKind kind = AttachmentKind::kNode;
  int index = -1;
};

constexpr const char* kImportPopup = "Import model";
constexpr const char* kCookingPopup = "Cooking";
constexpr const char* kOrmPopup = "Pack ORM texture";
constexpr const char* kRenamePopup = "Rename";

[[nodiscard]] SelectionItem ItemOf(AttachmentKind kind, int index) {
  switch (kind) {
    case AttachmentKind::kNode:
      return {.kind = SelectionKind::kNode, .index = index};
    case AttachmentKind::kSocket:
      return {.kind = SelectionKind::kSocket, .index = index};
    case AttachmentKind::kLight:
      return {.kind = SelectionKind::kLight, .index = index};
    case AttachmentKind::kChild:
      return {.kind = SelectionKind::kChild, .index = index};
  }
  return {};
}

// Selects `item`: alone, or toggled with Ctrl held.
void ClickSelect(PrefabDocument& document, SelectionItem item) {
  if (ImGui::GetIO().KeyCtrl) {
    document.selection().Toggle(item);
  } else {
    document.selection().Set(item);
  }
}

// Adds a socket / light at `node` as one step and selects it.
void AddSocketEdit(PrefabDocument& document, int node) {
  int added = -1;
  document.Edit(
      "Add socket", ChangeKind::kParameters,
      [&](prefab::Prefab& prefab) { added = AddSocket(prefab, node); });
  document.selection().Set({.kind = SelectionKind::kSocket, .index = added});
}

void AddLightEdit(PrefabDocument& document, int node) {
  int added = -1;
  document.Edit(
      "Add light", ChangeKind::kParameters,
      [&](prefab::Prefab& prefab) { added = AddPointLight(prefab, node); });
  document.selection().Set({.kind = SelectionKind::kLight, .index = added});
}

void AddChildEdit(EditorContext& context, PrefabDocument& document,
                  const std::string& path, int node) {
  const bool added =
      TryEdit(context, document, "Add child prefab", ChangeKind::kStructure,
              [&](prefab::Prefab& prefab) {
                return AddChildPrefab(prefab, document.path(), path, node);
              });
  if (added) {
    document.selection().Set(
        {.kind = SelectionKind::kChild,
         .index = static_cast<int64_t>(document.state().children.size()) - 1});
  }
}

void AddChildFromDialog(EditorContext& context, int node) {
  context.dialogs().Open(
      FileKind::kPrefab, context.asset_root(),
      [&context, node](std::string path) {
        if (PrefabDocument* document = context.prefab_document()) {
          AddChildEdit(context, *document, path, node);
        }
      });
}

void DeleteEdit(EditorContext& context, PrefabDocument& document,
                SelectionItem item) {
  const bool structural = item.kind != SelectionKind::kSocket &&
                          item.kind != SelectionKind::kLight &&
                          item.kind != SelectionKind::kCollider;
  if (TryEdit(
          context, document, "Delete " + ItemLabel(document.state(), item),
          structural ? ChangeKind::kStructure : ChangeKind::kParameters,
          [&](prefab::Prefab& prefab) { return DeleteItem(prefab, item); })) {
    document.selection().Clear();
  }
}

// Shows the context menu entries for an attachment or node item.
void ItemMenu(EditorContext& context, PrefabDocument& document,
              PrefabModeShared& shared, SelectionItem item) {
  if (std::optional<std::string> name = ItemName(document.state(), item)) {
    if (ImGui::MenuItem("Rename...")) {
      shared.rename_dialog = {
          .open_requested = true, .item = item, .name = std::move(*name)};
    }
  }
  if (ImGui::MenuItem("Delete")) {
    DeleteEdit(context, document, item);
  }
}

// Shows a channel combo (R, G, B, A).
bool ChannelCombo(const char* label, int& channel) {
  static constexpr std::array<const char*, 4> kChannels{"R", "G", "B", "A"};
  bool changed = false;
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 3.0f);
  if (ImGui::BeginCombo(label, kChannels[static_cast<size_t>(channel & 3)])) {
    for (int i = 0; i < 4; ++i) {
      if (ImGui::Selectable(kChannels[static_cast<size_t>(i)], channel == i)) {
        channel = i;
        changed = true;
      }
    }
    ImGui::EndCombo();
  }
  return changed;
}

}  // namespace

void PrefabHierarchyPanel::Update(EditorContext& context, float /*dt*/) {
  FinishImportCook(context, shared_);
  if (PrefabDocument* document = context.prefab_document()) {
    // A widget that opened an undo step but never reported its release
    // (e.g. a colour picker popup) must not merge later edits into it.
    if (document->history().edit_open() && !ImGui::IsAnyItemActive() &&
        !ImGui::IsMouseDown(ImGuiMouseButton_Left) && !shared_.gizmo_dragging) {
      document->CommitEdit();
    }
    const prefab::Prefab& prefab = document->state();
    Selection& selection = document->selection();
    selection.PruneIndices(SelectionKind::kNode, prefab.hierarchy.nodes.size());
    selection.PruneIndices(SelectionKind::kMesh, prefab.meshes.size());
    selection.PruneIndices(SelectionKind::kMaterial, prefab.materials.size());
    selection.PruneIndices(SelectionKind::kTexture, prefab.textures.size());
    selection.PruneIndices(SelectionKind::kSocket, prefab.sockets.size());
    selection.PruneIndices(SelectionKind::kLight, prefab.lights.size());
    selection.PruneIndices(SelectionKind::kChild, prefab.children.size());
    selection.PruneIndices(SelectionKind::kClip, prefab.clips.size());
  }
  DrawImportModal(context);
  DrawOrmModal(context);
  DrawRenameModal(context);
}

void PrefabHierarchyPanel::Draw(EditorContext& context) {
  PrefabDocument* document = context.prefab_document();
  if (document == nullptr) {
    ImGui::TextDisabled("No prefab open.");
    ImGui::TextDisabled("File > Open a .pbo, or File > Import model...");
    return;
  }
  const prefab::Prefab& prefab = document->state();
  const std::optional<SelectionItem> primary = document->selection().primary();
  const int selected_node = primary && primary->kind == SelectionKind::kNode
                                ? static_cast<int>(primary->index)
                                : -1;
  if (ImGui::SmallButton("+ Socket")) {
    AddSocketEdit(*document, selected_node);
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("+ Light")) {
    AddLightEdit(*document, selected_node);
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("+ Child prefab...")) {
    AddChildFromDialog(context, selected_node);
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(!primary);
  if (ImGui::SmallButton("Delete") && primary) {
    DeleteEdit(context, *document, *primary);
  }
  ImGui::EndDisabled();
  ImGui::Separator();

  children_.assign(prefab.hierarchy.nodes.size(), {});
  std::vector<int> roots;
  for (size_t i = 0; i < prefab.hierarchy.nodes.size(); ++i) {
    const int parent = prefab.hierarchy.nodes[i].parent;
    (parent < 0 ? roots : children_[static_cast<size_t>(parent)])
        .push_back(static_cast<int>(i));
  }
  const bool open =
      ImGui::TreeNodeEx("(prefab root)", ImGuiTreeNodeFlags_DefaultOpen |
                                             ImGuiTreeNodeFlags_SpanAvailWidth |
                                             ImGuiTreeNodeFlags_OpenOnArrow);
  NodeTargets(context, *document, -1);
  if (open) {
    DrawAttachments(context, *document, -1);
    for (const int root : roots) {
      DrawNode(context, *document, root);
    }
    ImGui::TreePop();
  }
  if (!prefab.clips.empty()) {
    ImGui::SeparatorText("Clips");
    for (size_t i = 0; i < prefab.clips.size(); ++i) {
      const SelectionItem item{.kind = SelectionKind::kClip,
                               .index = static_cast<int64_t>(i)};
      ImGui::PushID(static_cast<int>(i));
      if (ImGui::Selectable(prefab.clips[i].name.c_str(),
                            document->selection().Contains(item))) {
        ClickSelect(*document, item);
        shared_.preview.clip = static_cast<int>(i);
      }
      if (ImGui::BeginPopupContextItem()) {
        ItemMenu(context, *document, shared_, item);
        ImGui::EndPopup();
      }
      ImGui::PopID();
    }
  }
  if (prefab.physics.kind != prefab::ColliderKind::kNone) {
    ImGui::SeparatorText("Physics");
    const SelectionItem item{.kind = SelectionKind::kCollider};
    if (ImGui::Selectable("collider", document->selection().Contains(item))) {
      ClickSelect(*document, item);
    }
  }
}

void PrefabHierarchyPanel::DrawNode(EditorContext& context,
                                    PrefabDocument& document, int node) {
  const prefab::Prefab& prefab = document.state();
  const SelectionItem item{.kind = SelectionKind::kNode, .index = node};
  bool has_attachments = false;
  for (const prefab::Mesh& mesh : prefab.meshes) {
    has_attachments |= mesh.node == node;
  }
  for (const prefab::Socket& socket : prefab.sockets) {
    has_attachments |= socket.node == node;
  }
  for (const prefab::PointLight& light : prefab.lights) {
    has_attachments |= light.node == node;
  }
  for (const prefab::ChildPrefab& child : prefab.children) {
    has_attachments |= child.node == node;
  }
  ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                             ImGuiTreeNodeFlags_SpanAvailWidth |
                             ImGuiTreeNodeFlags_DefaultOpen;
  if (children_[static_cast<size_t>(node)].empty() && !has_attachments) {
    flags |= ImGuiTreeNodeFlags_Leaf;
  }
  if (document.selection().Contains(item)) {
    flags |= ImGuiTreeNodeFlags_Selected;
  }
  ImGui::PushID(node);
  const bool open = ImGui::TreeNodeEx(
      "##node", flags, "%s",
      prefab.hierarchy.nodes[static_cast<size_t>(node)].name.c_str());
  if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
    ClickSelect(document, item);
  }
  NodeTargets(context, document, node);
  if (open) {
    // Copy: an edit in a child's menu may rebuild children_.
    const std::vector<int> kids = children_[static_cast<size_t>(node)];
    for (const int child : kids) {
      if (child < static_cast<int>(document.state().hierarchy.nodes.size())) {
        DrawNode(context, document, child);
      }
    }
    DrawAttachments(context, document, node);
    ImGui::TreePop();
  }
  ImGui::PopID();
}

void PrefabHierarchyPanel::DrawAttachments(EditorContext& context,
                                           PrefabDocument& document, int node) {
  const auto leaf = [&](SelectionItem item, const std::string& label,
                        std::optional<AttachmentKind> drag) {
    ImGui::PushID(static_cast<int>(item.kind));
    ImGui::PushID(static_cast<int>(item.index));
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf |
                               ImGuiTreeNodeFlags_NoTreePushOnOpen |
                               ImGuiTreeNodeFlags_SpanAvailWidth;
    if (document.selection().Contains(item)) {
      flags |= ImGuiTreeNodeFlags_Selected;
    }
    ImGui::TreeNodeEx("##item", flags, "%s", label.c_str());
    if (ImGui::IsItemClicked()) {
      ClickSelect(document, item);
    }
    if (drag && ImGui::BeginDragDropSource()) {
      const ItemPayload payload{.kind = *drag,
                                .index = static_cast<int>(item.index)};
      ImGui::SetDragDropPayload(kItemPayload, &payload, sizeof(payload));
      ImGui::TextUnformatted(label.c_str());
      ImGui::EndDragDropSource();
    }
    if (ImGui::BeginPopupContextItem()) {
      ItemMenu(context, document, shared_, item);
      ImGui::EndPopup();
    }
    ImGui::PopID();
    ImGui::PopID();
  };
  // Iterate by index on the current state: an edit from a menu changes it.
  for (size_t i = 0; i < document.state().meshes.size(); ++i) {
    const prefab::Mesh& mesh = document.state().meshes[i];
    if (mesh.node == node) {
      leaf({.kind = SelectionKind::kMesh, .index = static_cast<int64_t>(i)},
           "[mesh] " + mesh.name, std::nullopt);
    }
  }
  for (size_t i = 0; i < document.state().sockets.size(); ++i) {
    const prefab::Socket& socket = document.state().sockets[i];
    if (socket.node == node) {
      leaf({.kind = SelectionKind::kSocket, .index = static_cast<int64_t>(i)},
           "[socket] " + socket.name, AttachmentKind::kSocket);
    }
  }
  for (size_t i = 0; i < document.state().lights.size(); ++i) {
    const prefab::PointLight& light = document.state().lights[i];
    if (light.node == node) {
      leaf({.kind = SelectionKind::kLight, .index = static_cast<int64_t>(i)},
           "[light] " + light.name, AttachmentKind::kLight);
    }
  }
  for (size_t i = 0; i < document.state().children.size(); ++i) {
    const prefab::ChildPrefab& child = document.state().children[i];
    if (child.node == node) {
      leaf({.kind = SelectionKind::kChild, .index = static_cast<int64_t>(i)},
           "[prefab] " + core::FileName(child.path), AttachmentKind::kChild);
    }
  }
}

void PrefabHierarchyPanel::NodeTargets(EditorContext& context,
                                       PrefabDocument& document, int node) {
  if (node >= 0 && ImGui::BeginDragDropSource()) {
    const ItemPayload payload{.kind = AttachmentKind::kNode, .index = node};
    ImGui::SetDragDropPayload(kItemPayload, &payload, sizeof(payload));
    ImGui::TextUnformatted(document.state()
                               .hierarchy.nodes[static_cast<size_t>(node)]
                               .name.c_str());
    ImGui::EndDragDropSource();
  }
  if (ImGui::BeginDragDropTarget()) {
    if (const ImGuiPayload* payload =
            ImGui::AcceptDragDropPayload(kItemPayload)) {
      ItemPayload moved;
      std::memcpy(&moved, payload->Data, sizeof(moved));
      const bool done = TryEdit(
          context, document, "Move to node",
          moved.kind == AttachmentKind::kNode ? ChangeKind::kStructure
                                              : ChangeKind::kParameters,
          [&](prefab::Prefab& prefab) -> std::optional<std::string> {
            if (!MoveToNode(prefab, moved.kind, moved.index, node)) {
              return std::string("a node cannot move into its own subtree");
            }
            return std::nullopt;
          });
      if (done && moved.kind == AttachmentKind::kNode) {
        // Reparenting reorders the nodes: select the moved node again.
        document.selection().Clear();
      } else if (done) {
        document.selection().Set(ItemOf(moved.kind, moved.index));
      }
    }
    ImGui::EndDragDropTarget();
  }
  if (std::optional<std::string> dropped = AcceptAssetDrop(FileKind::kPrefab)) {
    AddChildEdit(context, document, *dropped, node);
  }
  if (ImGui::BeginPopupContextItem("##node_menu")) {
    if (ImGui::MenuItem("Add socket here")) {
      AddSocketEdit(document, node);
    }
    if (ImGui::MenuItem("Add light here")) {
      AddLightEdit(document, node);
    }
    if (ImGui::MenuItem("Add child prefab here...")) {
      AddChildFromDialog(context, node);
    }
    if (node >= 0) {
      ImGui::Separator();
      ItemMenu(context, document, shared_,
               {.kind = SelectionKind::kNode, .index = node});
    }
    ImGui::EndPopup();
  }
}

void PrefabHierarchyPanel::DrawImportModal(EditorContext& context) {
  ImportDialog& dialog = shared_.import_dialog;
  if (dialog.open_requested) {
    dialog.open_requested = false;
    ImGui::OpenPopup(kImportPopup);
  }
  if (ImGui::BeginPopupModal(kImportPopup, nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    cook::ModelCookOptions& options = dialog.options;
    ImGui::Text(
        "%s: %s",
        dialog.target == ImportTarget::kReimport ? "Re-import" : "Import",
        dialog.path.c_str());
    ImGui::DragFloat("Scale", &options.scale, 0.001f, 0.0001f, 1000.0f, "%.4f");
    bool z_up = options.up_axis == cook::UpAxis::kZ;
    if (ImGui::Checkbox("Source is Z-up", &z_up)) {
      options.up_axis = z_up ? cook::UpAxis::kZ : cook::UpAxis::kY;
    }
    ImGui::Checkbox("Merge static nodes (drops hierarchy and clips)",
                    &options.merge_static_nodes);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 20.0f);
    InputString("Drop nodes (comma-separated)", dialog.drop_nodes);
    ImGui::Checkbox("Use .bmat sidecar materials",
                    &options.use_material_overrides);
    ImGui::Checkbox("Optimise meshes", &options.optimize_meshes);
    ImGui::Checkbox("Compress textures", &options.compress_textures);
    bool high = options.texture_quality == cook::CookQuality::kHigh;
    if (ImGui::Checkbox("High texture quality (slow)", &high)) {
      options.texture_quality =
          high ? cook::CookQuality::kHigh : cook::CookQuality::kFast;
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
    ImGui::InputInt("Max texture size (0 = source)", &options.max_texture_size,
                    256);
    if (options.max_texture_size < 0) {
      options.max_texture_size = 0;
    }
    if (dialog.target == ImportTarget::kReimport) {
      ImGui::TextWrapped(
          "Kept by name: material parameters and UV sets, clip loop flags and "
          "events, sockets, lights, child prefabs and physics settings.");
    }
    if (ImGui::Button(dialog.target == ImportTarget::kReimport ? "Re-import"
                                                               : "Import")) {
      StartImportCook(context, shared_);
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }

  if (shared_.cook.busy() && !ImGui::IsPopupOpen(kCookingPopup)) {
    ImGui::OpenPopup(kCookingPopup);
  }
  if (ImGui::BeginPopupModal(
          kCookingPopup, nullptr,
          ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
    ImGui::Text("Cooking %s ... %.1f s",
                core::FileName(shared_.cook.path()).c_str(),
                static_cast<double>(shared_.cook.elapsed_seconds()));
    if (!shared_.cook.busy()) {
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
}

void PrefabHierarchyPanel::DrawOrmModal(EditorContext& context) {
  OrmDialog& dialog = shared_.orm_dialog;
  if (dialog.open_requested) {
    dialog.open_requested = false;
    ImGui::OpenPopup(kOrmPopup);
  }
  if (!ImGui::BeginPopupModal(kOrmPopup, nullptr,
                              ImGuiWindowFlags_AlwaysAutoResize)) {
    return;
  }
  ImGui::TextUnformatted(
      "Packs occlusion (R), roughness (G) and metallic (B) into one linear "
      "texture.\nEach comes from one channel of an image; empty = none "
      "(the material factor applies).");
  const std::array<std::pair<const char*, OrmChannelSource*>, 3> rows{{
      {"Occlusion", &dialog.sources.occlusion},
      {"Roughness", &dialog.sources.roughness},
      {"Metallic", &dialog.sources.metallic},
  }};
  for (size_t i = 0; i < rows.size(); ++i) {
    OrmChannelSource& source = *rows[i].second;
    ImGui::PushID(static_cast<int>(i));
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 22.0f);
    InputString(rows[i].first, source.path);
    if (std::optional<std::string> dropped =
            AcceptAssetDrop(FileKind::kImage)) {
      source.path = *dropped;
    }
    ImGui::SameLine();
    if (ImGui::Button("...")) {
      context.dialogs().Open(FileKind::kImage, context.asset_root(),
                             [this, i](std::string path) {
                               const std::array<OrmChannelSource*, 3> targets{
                                   &shared_.orm_dialog.sources.occlusion,
                                   &shared_.orm_dialog.sources.roughness,
                                   &shared_.orm_dialog.sources.metallic};
                               targets[i]->path = std::move(path);
                             });
    }
    ImGui::SameLine();
    ChannelCombo("channel", source.channel);
    ImGui::PopID();
  }
  PrefabDocument* document = context.prefab_document();
  ImGui::BeginDisabled(document == nullptr || shared_.cook.busy());
  if (ImGui::Button("Pack and cook") && document != nullptr) {
    const bool done =
        TryEdit(context, *document, "Pack ORM texture", ChangeKind::kStructure,
                [&](prefab::Prefab& prefab) -> std::optional<std::string> {
                  const core::Result<int, cook::CookError> packed =
                      ImportOrmTexture(prefab, dialog.material, dialog.sources,
                                       shared_.texture_settings);
                  if (!packed.ok()) {
                    return "Cannot pack the ORM texture: " +
                           CookErrorText(packed.error());
                  }
                  return std::nullopt;
                });
    if (done) {
      ImGui::CloseCurrentPopup();
    }
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button("Cancel")) {
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

void PrefabHierarchyPanel::DrawRenameModal(EditorContext& context) {
  RenameDialog& dialog = shared_.rename_dialog;
  if (dialog.open_requested) {
    dialog.open_requested = false;
    ImGui::OpenPopup(kRenamePopup);
  }
  if (!ImGui::BeginPopupModal(kRenamePopup, nullptr,
                              ImGuiWindowFlags_AlwaysAutoResize)) {
    return;
  }
  if (ImGui::IsWindowAppearing()) {
    ImGui::SetKeyboardFocusHere();
  }
  const bool enter = [&] {
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);
    InputString("Name", dialog.name);
    return ImGui::IsItemDeactivated() &&
           ImGui::IsKeyPressed(ImGuiKey_Enter, false);
  }();
  PrefabDocument* document = context.prefab_document();
  if ((ImGui::Button("OK") || enter) && document != nullptr) {
    const bool done = TryEdit(
        context, *document, "Rename",
        dialog.item.kind == SelectionKind::kClip ? ChangeKind::kStructure
                                                 : ChangeKind::kParameters,
        [&](prefab::Prefab& prefab) {
          return RenameItem(prefab, dialog.item, dialog.name);
        });
    if (done) {
      ImGui::CloseCurrentPopup();
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("Cancel")) {
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

}  // namespace blue::editor
