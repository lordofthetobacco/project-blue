#include "editor/prefab_mode/prefab_widgets.h"

#include <imgui.h>

#include <array>
#include <charconv>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <glm/gtc/quaternion.hpp>
#include <glm/trigonometric.hpp>
#include <glm/vec3.hpp>

#include "animation/transform.h"
#include "cook/collision_cook.h"
#include "cook/cook_error.h"
#include "cook/texture_cook.h"
#include "core/path.h"
#include "core/result.h"
#include "editor/dialogs.h"
#include "editor/document.h"
#include "editor/drag_drop.h"
#include "editor/editor_context.h"
#include "editor/prefab_mode/prefab_edit.h"
#include "editor/prefab_mode/prefab_mode_state.h"
#include "editor/selection.h"
#include "editor/undo.h"
#include "material/material.h"
#include "prefab/prefab.h"

namespace blue::editor {

namespace {

constexpr float kMaxEmissiveStrength = 50.0f;
constexpr float kMaxUvScale = 100.0f;
constexpr std::array<int, 6> kMaxTextureSizes{0, 256, 512, 1024, 2048, 4096};

// Grows the std::string behind an InputText (as imgui_stdlib does).
int ResizeStringCallback(ImGuiInputTextCallbackData* data) {
  if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
    auto* text = static_cast<std::string*>(data->UserData);
    text->resize(static_cast<size_t>(data->BufTextLen));
    data->Buf = text->data();
  }
  return 0;
}

// Shows a combo of `labels`; returns true when `value` changed.
template <size_t N>
bool EnumCombo(const char* label, const std::array<const char*, N>& labels,
               int& value) {
  bool changed = false;
  const char* preview = value >= 0 && value < static_cast<int>(N)
                            ? labels[static_cast<size_t>(value)]
                            : "?";
  if (ImGui::BeginCombo(label, preview)) {
    for (size_t i = 0; i < N; ++i) {
      if (ImGui::Selectable(labels[i], value == static_cast<int>(i))) {
        value = static_cast<int>(i);
        changed = true;
      }
    }
    ImGui::EndCombo();
  }
  return changed;
}

// Returns the ORM dialog sources recorded for texture `texture` (as written
// by ImportOrmTexture: "path#channel" per channel), or defaults.
[[nodiscard]] OrmImportSources RecordedOrmSources(const prefab::Prefab& prefab,
                                                  int texture) {
  OrmImportSources sources;
  if (texture < 0 ||
      texture >= static_cast<int>(prefab.edit.texture_sources.size())) {
    return sources;
  }
  const std::vector<std::string>& recorded =
      prefab.edit.texture_sources[static_cast<size_t>(texture)];
  const std::array<OrmChannelSource*, 3> channels{
      &sources.occlusion, &sources.roughness, &sources.metallic};
  for (size_t i = 0; i < channels.size() && i < recorded.size(); ++i) {
    const std::string& entry = recorded[i];
    const size_t hash = entry.rfind('#');
    if (hash == std::string::npos || hash + 2 != entry.size()) {
      continue;
    }
    int channel = 0;
    std::from_chars(entry.data() + hash + 1, entry.data() + entry.size(),
                    channel);
    *channels[i] = {.path = entry.substr(0, hash), .channel = channel};
  }
  return sources;
}

// The directory dialogs for `prefab` start in: its source model's, or the
// asset root.
[[nodiscard]] std::string DialogDirectory(const EditorContext& context,
                                          const prefab::Prefab& prefab) {
  return prefab.edit.source_model.empty()
             ? context.asset_root()
             : core::ParentPath(prefab.edit.source_model);
}

void DrawTextureSlot(EditorContext& context, PrefabDocument& document,
                     PrefabModeShared& shared, int index,
                     prefab::TextureSlot slot) {
  const prefab::Prefab& prefab = document.state();
  const prefab::Material& material =
      prefab.materials[static_cast<size_t>(index)];
  const int texture = material.texture(slot);
  const size_t s = static_cast<size_t>(slot);
  ImGui::PushID(static_cast<int>(slot));

  const float size = ImGui::GetFrameHeight() * 2.6f;
  const prefab::Texture* cooked =
      texture >= 0 && texture < static_cast<int>(prefab.textures.size())
          ? &prefab.textures[static_cast<size_t>(texture)]
          : nullptr;
  const ImTextureID thumbnail = cooked != nullptr
                                    ? shared.thumbnails.Get(context, *cooked)
                                    : ImTextureID{};
  bool clicked = false;
  if (thumbnail != ImTextureID{}) {
    clicked = ImGui::ImageButton("##thumb", thumbnail, {size, size});
  } else {
    clicked = ImGui::Button(cooked != nullptr ? "?" : "none",
                            {size + ImGui::GetStyle().FramePadding.x * 2.0f,
                             size + ImGui::GetStyle().FramePadding.y * 2.0f});
  }
  if (clicked && cooked != nullptr) {
    document.selection().Set(
        {.kind = SelectionKind::kTexture, .index = texture});
  }
  if (std::optional<std::string> dropped = AcceptAssetDrop(FileKind::kImage)) {
    ImportSlotTextureEdit(context, document, shared, index, slot, *dropped);
  }
  if (cooked != nullptr && ImGui::BeginItemTooltip()) {
    ImGui::Text("%s\n%s %d x %d, %d mips, %.1f KiB", cooked->name.c_str(),
                FormatLabel(cooked->format), cooked->width, cooked->height,
                cooked->mip_count(),
                static_cast<double>(TextureByteSize(*cooked)) / 1024.0);
    ImGui::EndTooltip();
  }

  ImGui::SameLine();
  ImGui::BeginGroup();
  ImGui::TextUnformatted(SlotLabel(slot));
  if (cooked != nullptr) {
    ImGui::SameLine();
    ImGui::TextDisabled("%s  %s %dx%d", cooked->name.c_str(),
                        FormatLabel(cooked->format), cooked->width,
                        cooked->height);
  }
  if (ImGui::SmallButton("Replace...")) {
    ReplaceSlotTextureFromDialog(context, shared, index, slot);
  }
  if (slot == prefab::TextureSlot::kOrm) {
    ImGui::SameLine();
    if (ImGui::SmallButton("Pack...")) {
      shared.orm_dialog = {.open_requested = true,
                           .material = index,
                           .sources = RecordedOrmSources(prefab, texture)};
    }
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(texture < 0);
  if (ImGui::SmallButton("Remove")) {
    document.Edit("Remove texture from slot", ChangeKind::kParameters,
                  [&](prefab::Prefab& edited) {
                    edited.materials[static_cast<size_t>(index)].textures[s] =
                        -1;
                  });
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  int uv_set = material.uv_sets[s];
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4.0f);
  if (EnumCombo("UV set", std::array<const char*, 2>{"uv0", "uv1"}, uv_set)) {
    document.Edit(
        "UV set", ChangeKind::kParameters, [&](prefab::Prefab& edited) {
          edited.materials[static_cast<size_t>(index)].uv_sets[s] = uv_set;
        });
  }
  ImGui::EndGroup();
  ImGui::PopID();
}

void DrawTextureImportSettings(PrefabModeShared& shared) {
  if (!ImGui::TreeNode("Texture import settings")) {
    return;
  }
  TextureImportSettings& settings = shared.texture_settings;
  int quality = static_cast<int>(settings.quality);
  if (EnumCombo("Quality", std::array<const char*, 2>{"Fast", "High"},
                quality)) {
    settings.quality = static_cast<cook::CookQuality>(quality);
  }
  ImGui::Checkbox("Compress (BC7 / BC5)", &settings.compress);
  std::array<std::string, kMaxTextureSizes.size()> labels;
  std::array<const char*, kMaxTextureSizes.size()> pointers{};
  int current = 0;
  for (size_t i = 0; i < kMaxTextureSizes.size(); ++i) {
    labels[i] = kMaxTextureSizes[i] == 0 ? std::string("source size")
                                         : std::to_string(kMaxTextureSizes[i]);
    pointers[i] = labels[i].c_str();
    if (kMaxTextureSizes[i] == settings.max_size) {
      current = static_cast<int>(i);
    }
  }
  if (EnumCombo("Max size", pointers, current)) {
    settings.max_size = kMaxTextureSizes[static_cast<size_t>(current)];
  }
  ImGui::TreePop();
}

}  // namespace

bool InputTextCommit(PrefabModeShared& shared, const char* label,
                     const std::string& value, std::string& edited) {
  const ImGuiID id = ImGui::GetID(label);
  std::string local;
  std::string* text = &local;
  if (shared.text_buffer_id == id) {
    text = &shared.text_buffer;
  } else {
    local = value;
  }
  ImGui::InputText(label, text->data(), text->capacity() + 1,
                   ImGuiInputTextFlags_CallbackResize, ResizeStringCallback,
                   text);
  if (ImGui::IsItemActive() && shared.text_buffer_id != id) {
    shared.text_buffer = local;
    shared.text_buffer_id = id;
  }
  if (ImGui::IsItemDeactivated() && shared.text_buffer_id == id) {
    shared.text_buffer_id = 0;
    if (ImGui::IsItemDeactivatedAfterEdit() && shared.text_buffer != value) {
      edited = std::move(shared.text_buffer);
      return true;
    }
  }
  return false;
}

bool InputString(const char* label, std::string& text) {
  return ImGui::InputText(label, text.data(), text.capacity() + 1,
                          ImGuiInputTextFlags_CallbackResize,
                          ResizeStringCallback, &text);
}

FieldEdit TransformFields(const char* id, animation::Transform& transform,
                          EulerCache& euler, bool show_scale) {
  ImGui::PushID(id);
  FieldEdit edit;
  edit.changed = ImGui::DragFloat3("Position", &transform.translation.x, 0.005f,
                                   0.0f, 0.0f, "%.4f");
  edit.released = ImGui::IsItemDeactivatedAfterEdit();
  const ImGuiID rotation_id = ImGui::GetID("Rotation");
  glm::vec3 degrees = euler.id == rotation_id
                          ? euler.degrees
                          : glm::degrees(glm::eulerAngles(transform.rotation));
  if (ImGui::DragFloat3("Rotation", &degrees.x, 0.25f, 0.0f, 0.0f,
                        "%.2f deg")) {
    transform.rotation = glm::normalize(glm::quat(glm::radians(degrees)));
    edit.changed = true;
  }
  if (ImGui::IsItemActive()) {
    euler = {.id = rotation_id, .degrees = degrees};
  } else if (euler.id == rotation_id) {
    euler.id = 0;
  }
  edit.released |= ImGui::IsItemDeactivatedAfterEdit();
  if (show_scale) {
    edit.changed |= ImGui::DragFloat3("Scale", &transform.scale.x, 0.005f,
                                      0.001f, 1000.0f, "%.4f");
    edit.released |= ImGui::IsItemDeactivatedAfterEdit();
  }
  ImGui::PopID();
  return edit;
}

bool NodeCombo(const char* label, const prefab::Prefab& prefab, int& node) {
  const int count = static_cast<int>(prefab.hierarchy.nodes.size());
  const char* preview =
      node >= 0 && node < count
          ? prefab.hierarchy.nodes[static_cast<size_t>(node)].name.c_str()
          : "(prefab root)";
  bool changed = false;
  if (ImGui::BeginCombo(label, preview)) {
    if (ImGui::Selectable("(prefab root)", node < 0)) {
      node = -1;
      changed = true;
    }
    for (int i = 0; i < count; ++i) {
      ImGui::PushID(i);
      if (ImGui::Selectable(
              prefab.hierarchy.nodes[static_cast<size_t>(i)].name.c_str(),
              node == i)) {
        node = i;
        changed = true;
      }
      ImGui::PopID();
    }
    ImGui::EndCombo();
  }
  return changed;
}

void CommitOnRelease(PrefabDocument& document, bool released, bool structural) {
  if (!released) {
    return;
  }
  if (structural) {
    (void)document.BeginEdit("", ChangeKind::kStructure);
  }
  document.CommitEdit();
}

const char* SlotLabel(prefab::TextureSlot slot) {
  switch (slot) {
    case prefab::TextureSlot::kBaseColor:
      return "Base colour";
    case prefab::TextureSlot::kOrm:
      return "ORM (occlusion, roughness, metallic)";
    case prefab::TextureSlot::kNormal:
      return "Normal";
    case prefab::TextureSlot::kEmissive:
      return "Emissive";
    case prefab::TextureSlot::kCount:
      break;
  }
  return "?";
}

bool ImportSlotTextureEdit(EditorContext& context, PrefabDocument& document,
                           const PrefabModeShared& shared, int material,
                           prefab::TextureSlot slot, const std::string& path) {
  const bool done = TryEdit(
      context, document, "Replace texture", ChangeKind::kStructure,
      [&](prefab::Prefab& prefab) -> std::optional<std::string> {
        const core::Result<int, cook::CookError> imported = ImportSlotTexture(
            prefab, material, slot, path, shared.texture_settings);
        if (!imported.ok()) {
          return std::format("Cannot import {}: {}", path,
                             CookErrorText(imported.error()));
        }
        return std::nullopt;
      });
  if (done) {
    context.Status(std::format("Cooked {} into {}", core::FileName(path),
                               SlotLabel(slot)));
  }
  return done;
}

void ReplaceSlotTextureFromDialog(EditorContext& context,
                                  PrefabModeShared& shared, int material,
                                  prefab::TextureSlot slot) {
  const PrefabDocument* document = context.prefab_document();
  if (document == nullptr) {
    return;
  }
  context.dialogs().Open(
      FileKind::kImage, DialogDirectory(context, document->state()),
      [&context, &shared, material, slot](std::string path) {
        if (PrefabDocument* current = context.prefab_document()) {
          ImportSlotTextureEdit(context, *current, shared, material, slot,
                                path);
        }
      });
}

void DrawMaterialEditor(EditorContext& context, PrefabDocument& document,
                        PrefabModeShared& shared, int index) {
  if (index < 0 ||
      index >= static_cast<int>(document.state().materials.size())) {
    return;
  }
  const size_t i = static_cast<size_t>(index);
  ImGui::PushID(index);
  std::string name;
  if (InputTextCommit(shared, "Name", document.state().materials[i].name,
                      name)) {
    if (!name.empty()) {
      document.Edit("Rename material", ChangeKind::kParameters,
                    [&](prefab::Prefab& prefab) {
                      prefab.materials[i].name = std::move(name);
                    });
    }
  }

  // Continuous widgets edit a copy; the first change opens the undo step,
  // the release closes it. Discrete widgets are one step each.
  prefab::Material m = document.state().materials[i];
  bool changed = false;
  bool released = false;
  bool discrete = false;
  const auto track = [&](bool now) {
    changed |= now;
    released |= ImGui::IsItemDeactivatedAfterEdit();
  };
  track(ImGui::ColorEdit4("Base colour", &m.base_color.x,
                          ImGuiColorEditFlags_Float));
  track(ImGui::SliderFloat("Metallic", &m.metallic, 0.0f, 1.0f));
  track(ImGui::SliderFloat("Roughness", &m.roughness, 0.0f, 1.0f));
  track(
      ImGui::ColorEdit3("Emissive", &m.emissive.x, ImGuiColorEditFlags_Float));
  track(ImGui::SliderFloat("Emissive strength", &m.emissive_strength, 0.0f,
                           kMaxEmissiveStrength));
  track(ImGui::SliderFloat("Normal scale", &m.normal_scale, 0.0f, 2.0f));
  track(ImGui::SliderFloat("Occlusion strength", &m.occlusion_strength, 0.0f,
                           1.0f));
  track(
      ImGui::DragFloat2("UV scale", &m.uv_scale.x, 0.01f, 0.001f, kMaxUvScale));
  int alpha = static_cast<int>(m.alpha_mode);
  if (EnumCombo("Alpha", std::array<const char*, 2>{"Opaque", "Mask"}, alpha)) {
    m.alpha_mode = static_cast<material::AlphaMode>(alpha);
    discrete = true;
  }
  if (m.alpha_mode == material::AlphaMode::kMask) {
    track(ImGui::SliderFloat("Alpha cutoff", &m.alpha_cutoff, 0.0f, 1.0f));
  }
  discrete |= ImGui::Checkbox("Double sided", &m.double_sided);
  if (changed || discrete) {
    document.BeginEdit("Edit material", ChangeKind::kParameters).materials[i] =
        m;
  }
  if (released || discrete) {
    document.CommitEdit();
  }

  ImGui::SeparatorText("Textures");
  for (int slot = 0; slot < prefab::kTextureSlotCount; ++slot) {
    DrawTextureSlot(context, document, shared, index,
                    static_cast<prefab::TextureSlot>(slot));
  }
  DrawTextureImportSettings(shared);
  ImGui::PopID();
}

void DrawPhysicsEditor(EditorContext& context, PrefabDocument& document,
                       PrefabModeShared& shared) {
  static constexpr std::array<const char*, 6> kKinds{
      "None", "Box", "Sphere", "Capsule", "Convex hull", "Triangle mesh"};
  const prefab::Physics& current = document.state().physics;
  const auto recook = [&](std::string_view label, prefab::ColliderKind kind,
                          cook::ShapeFit fit) {
    TryEdit(context, document, label, ChangeKind::kParameters,
            [&](prefab::Prefab& prefab) -> std::optional<std::string> {
              prefab::Physics settings = prefab.physics;
              settings.kind = kind;
              if (kind == prefab::ColliderKind::kTriangleMesh) {
                settings.dynamic = false;
              }
              const core::Result<void, cook::CookError> cooked =
                  cook::CookCollision(prefab, settings, fit);
              if (!cooked.ok()) {
                return "Cannot cook the collider: " +
                       CookErrorText(cooked.error());
              }
              return std::nullopt;
            });
  };

  int kind = static_cast<int>(current.kind);
  if (EnumCombo("Collider", kKinds, kind)) {
    recook("Collider kind", static_cast<prefab::ColliderKind>(kind),
           cook::ShapeFit::kBounds);
  }
  if (current.kind == prefab::ColliderKind::kNone) {
    ImGui::TextDisabled("No physics body.");
    return;
  }
  ImGui::TextDisabled("%s", current.cooked_shape.empty()
                                ? "Shape not cooked"
                                : std::format("Cooked shape: {} bytes",
                                              current.cooked_shape.size())
                                      .c_str());

  prefab::Physics p = current;
  bool changed = false;
  bool released = false;
  const auto track = [&](bool now) {
    changed |= now;
    released |= ImGui::IsItemDeactivatedAfterEdit();
  };
  const bool primitive = p.kind == prefab::ColliderKind::kBox ||
                         p.kind == prefab::ColliderKind::kSphere ||
                         p.kind == prefab::ColliderKind::kCapsule;
  if (primitive) {
    animation::Transform placement{.translation = p.center,
                                   .rotation = p.rotation};
    const FieldEdit placed =
        TransformFields("collider", placement, shared.euler, false);
    changed |= placed.changed;
    released |= placed.released;
    p.center = placement.translation;
    p.rotation = placement.rotation;
    if (p.kind == prefab::ColliderKind::kBox) {
      track(ImGui::DragFloat3("Half extents", &p.half_extents.x, 0.005f, 0.001f,
                              10000.0f, "%.4f"));
    } else {
      track(ImGui::DragFloat("Radius", &p.radius, 0.005f, 0.001f, 10000.0f,
                             "%.4f"));
    }
    if (p.kind == prefab::ColliderKind::kCapsule) {
      track(ImGui::DragFloat("Half height", &p.half_height, 0.005f, 0.0f,
                             10000.0f, "%.4f"));
    }
  }
  bool discrete = false;
  ImGui::BeginDisabled(p.kind == prefab::ColliderKind::kTriangleMesh);
  discrete |= ImGui::Checkbox("Dynamic", &p.dynamic);
  ImGui::EndDisabled();
  if (p.dynamic) {
    track(ImGui::DragFloat("Mass (kg)", &p.mass, 0.05f, 0.001f, 100000.0f));
  }
  track(ImGui::SliderFloat("Friction", &p.friction, 0.0f, 2.0f));
  track(ImGui::SliderFloat("Restitution", &p.restitution, 0.0f, 1.0f));
  if (changed || discrete) {
    document.BeginEdit("Edit physics", ChangeKind::kParameters).physics = p;
  }
  if (released || discrete) {
    // Primitive parameters change the shape: re-cook it into the same step.
    if (primitive && !document.state().physics.cooked_shape.empty()) {
      prefab::Prefab& prefab =
          document.BeginEdit("Edit physics", ChangeKind::kParameters);
      const prefab::Physics settings = prefab.physics;
      const core::Result<void, cook::CookError> cooked =
          cook::CookCollision(prefab, settings, cook::ShapeFit::kParameters);
      if (!cooked.ok()) {
        context.Status("Collider re-cook failed: " +
                       CookErrorText(cooked.error()));
      }
    }
    document.CommitEdit();
  }

  if (ImGui::Button("Fit to bounds")) {
    recook("Fit collider", current.kind, cook::ShapeFit::kBounds);
  }
  ImGui::SameLine();
  if (ImGui::Button("Re-cook")) {
    recook("Re-cook collider", current.kind, cook::ShapeFit::kParameters);
  }
}

}  // namespace blue::editor
