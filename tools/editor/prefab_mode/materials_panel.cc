#include <imgui.h>

#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <vector>

#include "editor/document.h"
#include "editor/editor_context.h"
#include "editor/prefab_mode/prefab_edit.h"
#include "editor/prefab_mode/prefab_mode_state.h"
#include "editor/prefab_mode/prefab_panels.h"
#include "editor/prefab_mode/prefab_widgets.h"
#include "editor/selection.h"
#include "editor/undo.h"
#include "editor/viewport/uv_view.h"
#include "prefab/prefab.h"

namespace blue::editor {

void PrefabMaterialsPanel::Draw(EditorContext& context) {
  PrefabDocument* document = context.prefab_document();
  if (document == nullptr) {
    ImGui::TextDisabled("No prefab open.");
    return;
  }
  const int selected = static_cast<int>(
      document->selection().PrimaryIndex(SelectionKind::kMaterial));
  if (selected >= 0) {
    material_ = selected;
  }
  const int count = static_cast<int>(document->state().materials.size());
  if (material_ >= count) {
    material_ = count - 1;
  }

  if (ImGui::SmallButton("+ Material")) {
    document->Edit("Add material", ChangeKind::kStructure,
                   [&](prefab::Prefab& prefab) {
                     std::vector<std::string> taken;
                     for (const prefab::Material& m : prefab.materials) {
                       taken.push_back(m.name);
                     }
                     prefab.materials.push_back(
                         {.name = MakeUniqueName("material", taken)});
                   });
    material_ = count;
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(material_ < 0);
  if (ImGui::SmallButton("Duplicate") && material_ >= 0) {
    document->Edit("Duplicate material", ChangeKind::kStructure,
                   [&](prefab::Prefab& prefab) {
                     std::vector<std::string> taken;
                     for (const prefab::Material& m : prefab.materials) {
                       taken.push_back(m.name);
                     }
                     prefab::Material copy =
                         prefab.materials[static_cast<size_t>(material_)];
                     copy.name = MakeUniqueName(copy.name, taken);
                     prefab.materials.push_back(std::move(copy));
                   });
    material_ = count;
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("Delete") && material_ >= 0) {
    if (TryEdit(context, *document, "Delete material", ChangeKind::kStructure,
                [&](prefab::Prefab& prefab) {
                  return DeleteItem(prefab, {.kind = SelectionKind::kMaterial,
                                             .index = material_});
                })) {
      document->selection().Clear();
      material_ = 0;
    }
  }
  ImGui::EndDisabled();

  const prefab::Prefab& prefab = document->state();
  const float list_height =
      ImGui::GetTextLineHeightWithSpacing() *
      static_cast<float>(std::min<size_t>(prefab.materials.size(), 6) + 1);
  if (ImGui::BeginListBox("##materials", {-1.0f, list_height})) {
    for (size_t i = 0; i < prefab.materials.size(); ++i) {
      ImGui::PushID(static_cast<int>(i));
      if (ImGui::Selectable(prefab.materials[i].name.c_str(),
                            static_cast<int>(i) == material_)) {
        material_ = static_cast<int>(i);
        document->selection().Set({.kind = SelectionKind::kMaterial,
                                   .index = static_cast<int64_t>(i)});
      }
      ImGui::PopID();
    }
    ImGui::EndListBox();
  }
  if (material_ >= 0 &&
      material_ < static_cast<int>(document->state().materials.size())) {
    DrawMaterialEditor(context, *document, shared_, material_);
  }
  if (ImGui::CollapsingHeader("Textures")) {
    DrawTextureList(context, *document);
  }
}

void PrefabMaterialsPanel::DrawTextureList(EditorContext& context,
                                           PrefabDocument& document) {
  const prefab::Prefab& prefab = document.state();
  size_t total = 0;
  for (const prefab::Texture& texture : prefab.textures) {
    total += TextureByteSize(texture);
  }
  ImGui::Text("%zu textures, %.2f MiB", prefab.textures.size(),
              static_cast<double>(total) / (1024.0 * 1024.0));
  ImGui::SameLine();
  if (ImGui::SmallButton("Remove unused")) {
    int removed = 0;
    TryEdit(context, document, "Remove unused textures", ChangeKind::kStructure,
            [&](prefab::Prefab& edited) -> std::optional<std::string> {
              removed = RemoveUnusedTextures(edited);
              return std::nullopt;
            });
    context.Status(std::format("Removed {} unused texture(s)", removed));
  }
  if (!ImGui::BeginTable("##textures", 6,
                         ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                             ImGuiTableFlags_SizingFixedFit)) {
    return;
  }
  ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
  ImGui::TableSetupColumn("Format");
  ImGui::TableSetupColumn("Size");
  ImGui::TableSetupColumn("KiB");
  ImGui::TableSetupColumn("Uses");
  ImGui::TableSetupColumn("");
  ImGui::TableHeadersRow();
  std::optional<int> remove;
  for (size_t i = 0; i < prefab.textures.size(); ++i) {
    const prefab::Texture& texture = prefab.textures[i];
    const SelectionItem item{.kind = SelectionKind::kTexture,
                             .index = static_cast<int64_t>(i)};
    ImGui::PushID(static_cast<int>(i));
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    if (ImGui::Selectable(texture.name.c_str(),
                          document.selection().Contains(item))) {
      document.selection().Set(item);
    }
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(FormatLabel(texture.format));
    ImGui::TableNextColumn();
    ImGui::Text("%dx%d/%d", texture.width, texture.height, texture.mip_count());
    ImGui::TableNextColumn();
    ImGui::Text("%.0f", static_cast<double>(TextureByteSize(texture)) / 1024.0);
    ImGui::TableNextColumn();
    ImGui::Text("%d", CountTextureUses(prefab, static_cast<int>(i)));
    ImGui::TableNextColumn();
    if (ImGui::SmallButton("x")) {
      remove = static_cast<int>(i);
    }
    ImGui::PopID();
  }
  ImGui::EndTable();
  if (remove) {
    document.Edit(
        "Remove texture", ChangeKind::kStructure,
        [&](prefab::Prefab& edited) { (void)RemoveTexture(edited, *remove); });
    document.selection().Clear();
  }
}

void PrefabUvPanel::Draw(EditorContext& context) {
  PrefabDocument* document = context.prefab_document();
  if (document == nullptr || document->state().meshes.empty()) {
    ImGui::TextDisabled("No meshes.");
    return;
  }
  const prefab::Prefab& prefab = document->state();
  const int selected_mesh = static_cast<int>(
      document->selection().PrimaryIndex(SelectionKind::kMesh));
  if (selected_mesh >= 0) {
    shared_.uv_mesh = selected_mesh;
  }
  if (shared_.uv_mesh < 0 ||
      shared_.uv_mesh >= static_cast<int>(prefab.meshes.size())) {
    shared_.uv_mesh = 0;
  }
  const prefab::Mesh& mesh =
      prefab.meshes[static_cast<size_t>(shared_.uv_mesh)];
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
  if (ImGui::BeginCombo("Mesh", mesh.name.c_str())) {
    for (size_t i = 0; i < prefab.meshes.size(); ++i) {
      ImGui::PushID(static_cast<int>(i));
      if (ImGui::Selectable(prefab.meshes[i].name.c_str(),
                            static_cast<int>(i) == shared_.uv_mesh)) {
        shared_.uv_mesh = static_cast<int>(i);
      }
      ImGui::PopID();
    }
    ImGui::EndCombo();
  }
  ImGui::SameLine();
  ImGui::RadioButton("uv0", &shared_.uv_set, 0);
  ImGui::SameLine();
  ImGui::RadioButton("uv1", &shared_.uv_set, 1);
  ImGui::SameLine();
  if (background_ >= static_cast<int>(prefab.textures.size())) {
    background_ = -1;
  }
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
  if (ImGui::BeginCombo("Background",
                        background_ < 0
                            ? "(none)"
                            : prefab.textures[static_cast<size_t>(background_)]
                                  .name.c_str())) {
    if (ImGui::Selectable("(none)", background_ < 0)) {
      background_ = -1;
    }
    for (size_t i = 0; i < prefab.textures.size(); ++i) {
      ImGui::PushID(static_cast<int>(i));
      if (ImGui::Selectable(prefab.textures[i].name.c_str(),
                            static_cast<int>(i) == background_)) {
        background_ = static_cast<int>(i);
      }
      ImGui::PopID();
    }
    ImGui::EndCombo();
  }
  UvViewOptions options;
  options.uv_set = shared_.uv_set;
  if (background_ >= 0) {
    options.background = shared_.thumbnails.Get(
        context, prefab.textures[static_cast<size_t>(background_)]);
  }
  DrawUvView("##uv", mesh.vertices, mesh.indices, options, view_);
}

}  // namespace blue::editor
