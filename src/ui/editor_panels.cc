#include "ui/editor_panels.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>
#include <glm/vec3.hpp>

#include "material/material.h"
#include "material/material_file.h"
#include "render/material_cache.h"
#include "render/renderer.h"
#include "render/scene_view.h"
#include "scene/scene.h"

namespace blue::ui {

namespace {

using material::MaterialDesc;
using material::TextureRef;

struct TextureSlotInfo {
  const char* label;
  TextureRef MaterialDesc::* ref;
};
constexpr std::array<TextureSlotInfo, 7> kTextureSlots{{
    {"Base colour", &MaterialDesc::base_color_texture},
    {"Metal-rough", &MaterialDesc::metallic_roughness_texture},
    {"Roughness", &MaterialDesc::roughness_texture},
    {"Metallic", &MaterialDesc::metallic_texture},
    {"Normal", &MaterialDesc::normal_texture},
    {"Occlusion", &MaterialDesc::occlusion_texture},
    {"Emissive", &MaterialDesc::emissive_texture},
}};

constexpr float kMaxEmissiveSlider = 50.0f;
constexpr float kMaxUvScaleSlider = 100.0f;
constexpr float kMaxEnvironmentIntensity = 8.0f;
constexpr float kMaxSunIntensity = 20.0f;
constexpr float kMaxPointLightIntensity = 100.0f;
constexpr float kMinExposureEv = -6.0f;
constexpr float kMaxExposureEv = 6.0f;
constexpr float kMaxBloomStrength = 0.3f;
constexpr float kMinSsaoRadius = 0.1f;
constexpr float kMaxSsaoRadius = 2.0f;
constexpr float kMaxSsaoIntensity = 4.0f;

// Copies `text` into `buffer`, truncated and null-terminated.
template <size_t N>
void CopyToBuffer(std::string_view text, std::array<char, N>& buffer) {
  const size_t length = std::min(text.size(), N - 1);
  std::memcpy(buffer.data(), text.data(), length);
  buffer[length] = '\0';
}

// Returns the contents of `buffer` up to its terminating null (added at the
// end if missing).
template <size_t N>
[[nodiscard]] std::string_view BufferView(std::array<char, N>& buffer) {
  buffer[N - 1] = '\0';
  return std::string_view(buffer.data());
}

// Shows `text` as a plain label (not a format string).
void TextView(std::string_view text) {
  ImGui::TextUnformatted(text.data(), text.data() + text.size());
}

// Returns the sun's direction of travel as elevation of the sun above the
// horizon and its azimuth (degrees; 0 = the sun stands towards -Z, 90 =
// towards +X).
struct SunAngles {
  float elevation = 45.0f;
  float azimuth = 0.0f;
};
[[nodiscard]] SunAngles AnglesOf(glm::vec3 direction) {
  const glm::vec3 to_sun = -glm::normalize(direction);
  return {
      .elevation = glm::degrees(std::asin(std::clamp(to_sun.y, -1.0f, 1.0f))),
      .azimuth = glm::degrees(std::atan2(to_sun.x, -to_sun.z))};
}
[[nodiscard]] glm::vec3 DirectionOf(const SunAngles& angles) {
  const float elevation = glm::radians(angles.elevation);
  const float azimuth = glm::radians(angles.azimuth);
  const glm::vec3 to_sun{std::cos(elevation) * std::sin(azimuth),
                         std::sin(elevation),
                         -std::cos(elevation) * std::cos(azimuth)};
  return -to_sun;
}

}  // namespace

EditorActions EditorPanels::Draw(const EditorContext& context) {
  const bool pick = DrawMaterials(context);
  EditorActions actions = DrawEnvironment(context);
  actions.pick_entity = pick;
  DrawPost(context.post);
  return actions;
}

void EditorPanels::Select(std::optional<size_t> entity) {
  if (entity != selected_entity_) {
    selected_entity_ = entity;
    selected_slot_ = 0;
    file_status_.clear();
  }
}

bool EditorPanels::DrawMaterials(const EditorContext& context) {
  const ImVec2 display = ImGui::GetIO().DisplaySize;
  ImGui::SetNextWindowPos({display.x * 0.62f, display.y * 0.30f},
                          ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowBgAlpha(0.85f);
  if (!ImGui::Begin("Materials", nullptr,
                    ImGuiWindowFlags_AlwaysAutoResize |
                        ImGuiWindowFlags_NoSavedSettings |
                        ImGuiWindowFlags_NoFocusOnAppearing)) {
    ImGui::End();
    return false;
  }
  const bool pick = DrawEntityPicker(context);

  // Entity slots first, then the weapons'.
  const size_t entity_count = context.entity_slots.size();
  const size_t total = entity_count + context.weapon_slots.size();
  if (selected_slot_ >= total) {
    selected_slot_ = 0;
  }
  ImGui::SeparatorText("Material");
  const float list_height = ImGui::GetTextLineHeightWithSpacing() *
                            static_cast<float>(std::min<size_t>(total, 8) + 1);
  if (ImGui::BeginListBox("##slots",
                          {ImGui::GetFontSize() * 24.0f, list_height})) {
    for (size_t i = 0; i < total; ++i) {
      const MaterialSlot& slot = i < entity_count
                                     ? context.entity_slots[i]
                                     : context.weapon_slots[i - entity_count];
      ImGui::PushID(static_cast<int>(i));
      if (ImGui::Selectable(slot.label.c_str(), i == selected_slot_)) {
        selected_slot_ = i;
      }
      ImGui::PopID();
    }
    ImGui::EndListBox();
  }
  if (total == 0) {
    ImGui::TextDisabled("No materials");
    ImGui::End();
    return pick;
  }
  const bool is_entity_slot = selected_slot_ < entity_count;
  const MaterialSlot& slot =
      is_entity_slot ? context.entity_slots[selected_slot_]
                     : context.weapon_slots[selected_slot_ - entity_count];
  DrawMaterialEditor(context, slot);
  DrawMaterialFiles(context, slot, is_entity_slot);
  ImGui::End();
  return pick;
}

bool EditorPanels::DrawEntityPicker(const EditorContext& context) {
  const std::vector<scene::Entity>& entities = context.scene.entities;
  if (selected_entity_ && *selected_entity_ >= entities.size()) {
    Select(std::nullopt);
  }
  const char* preview =
      selected_entity_ ? entities[*selected_entity_].name.c_str() : "(none)";
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
  if (ImGui::BeginCombo("Entity", preview)) {
    for (size_t i = 0; i < entities.size(); ++i) {
      ImGui::PushID(static_cast<int>(i));
      if (ImGui::Selectable(entities[i].name.c_str(), selected_entity_ == i)) {
        Select(i);
      }
      ImGui::PopID();
    }
    ImGui::EndCombo();
  }
  ImGui::SameLine();
  const bool pick = ImGui::Button("Pick in view");
  if (selected_entity_) {
    const scene::Entity& entity = entities[*selected_entity_];
    ImGui::TextDisabled("mesh %s", entity.mesh_path.c_str());
    if (!entity.material_path.empty()) {
      ImGui::TextDisabled("file %s", entity.material_path.c_str());
    }
  }
  return pick;
}

void EditorPanels::LoadBuffers(const MaterialDesc& desc,
                               const MaterialSlot& slot) {
  for (size_t i = 0; i < kTextureSlots.size(); ++i) {
    CopyToBuffer((desc.*kTextureSlots[i].ref).path, texture_paths_[i]);
  }
  CopyToBuffer(slot.save_path, save_path_);
  CopyToBuffer(slot.save_path, assign_path_);
}

void EditorPanels::DrawMaterialEditor(const EditorContext& context,
                                      const MaterialSlot& slot) {
  // A copy: the renderer keeps the current version until UpdateMaterial().
  MaterialDesc desc = context.renderer.material(slot.id);
  if (buffered_material_ != slot.id) {
    LoadBuffers(desc, slot);
    buffered_material_ = slot.id;
  }
  ImGui::Text("Name: %s", desc.name.c_str());

  ImGui::PushItemWidth(ImGui::GetFontSize() * 14.0f);
  bool changed = false;
  changed |= ImGui::ColorEdit4("Base colour", &desc.base_color.x,
                               ImGuiColorEditFlags_Float);
  changed |= ImGui::SliderFloat("Metallic", &desc.metallic, 0.0f, 1.0f);
  changed |= ImGui::SliderFloat("Roughness", &desc.roughness, 0.0f, 1.0f);
  changed |= ImGui::ColorEdit3("Emissive", &desc.emissive.x,
                               ImGuiColorEditFlags_Float);
  changed |= ImGui::SliderFloat("Emissive strength", &desc.emissive_strength,
                                0.0f, kMaxEmissiveSlider, "%.2f",
                                ImGuiSliderFlags_Logarithmic);
  changed |= ImGui::SliderFloat("Normal scale", &desc.normal_scale, 0.0f, 2.0f);
  changed |= ImGui::SliderFloat("Occlusion strength", &desc.occlusion_strength,
                                0.0f, 1.0f);
  changed |= ImGui::DragFloat2("UV scale", &desc.uv_scale.x, 0.01f, 1.0e-4f,
                               kMaxUvScaleSlider, "%.3f");
  bool masked = desc.alpha_mode == material::AlphaMode::kMask;
  if (ImGui::Checkbox("Alpha mask", &masked)) {
    desc.alpha_mode =
        masked ? material::AlphaMode::kMask : material::AlphaMode::kOpaque;
    changed = true;
  }
  if (masked) {
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
    changed |= ImGui::SliderFloat("Cutoff", &desc.alpha_cutoff, 0.0f, 1.0f);
  }
  changed |= ImGui::Checkbox("Double sided", &desc.double_sided);
  ImGui::PopItemWidth();

  changed |= DrawTextureSlots(desc);
  if (changed) {
    context.renderer.UpdateMaterial(slot.id, material::Sanitize(desc));
  }
}

bool EditorPanels::DrawTextureSlots(MaterialDesc& desc) {
  bool changed = false;
  if (!ImGui::TreeNode("Textures")) {
    return false;
  }
  for (size_t i = 0; i < kTextureSlots.size(); ++i) {
    TextureRef& ref = desc.*kTextureSlots[i].ref;
    ImGui::PushID(static_cast<int>(i));
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 22.0f);
    ImGui::InputText(kTextureSlots[i].label, texture_paths_[i].data(),
                     texture_paths_[i].size());
    ImGui::SameLine();
    // Applies the typed path; the same path keeps an embedded index.
    if (ImGui::SmallButton("Apply")) {
      const std::string_view typed = BufferView(texture_paths_[i]);
      if (typed != ref.path) {
        ref = TextureRef{.path = std::string(typed)};
      }
      changed = true;
    }
    if (ref.embedded()) {
      ImGui::SameLine();
      ImGui::TextDisabled("embedded #%d", ref.embedded_index);
    }
    ImGui::PopID();
  }
  ImGui::TextDisabled("Empty path = no texture (factor only)");
  ImGui::TreePop();
  return changed;
}

void EditorPanels::DrawMaterialFiles(const EditorContext& context,
                                     const MaterialSlot& slot,
                                     bool is_entity_slot) {
  ImGui::SeparatorText("Files");
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 22.0f);
  ImGui::InputText("##save", save_path_.data(), save_path_.size());
  ImGui::SameLine();
  if (ImGui::Button("Save as .bmat")) {
    const std::string path(BufferView(save_path_));
    const auto saved =
        material::SaveMaterialFile(context.renderer.material(slot.id), path);
    if (saved.ok()) {
      // Entities using that file pick up the new version.
      (void)context.material_files.Reload(path);
      file_status_ = "Saved " + path;
    } else {
      file_status_ =
          std::string("Save failed: ") + material::ToString(saved.error());
    }
  }

  if (is_entity_slot && selected_entity_) {
    scene::Entity& entity = context.scene.entities[*selected_entity_];
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 22.0f);
    ImGui::InputText("##assign", assign_path_.data(), assign_path_.size());
    ImGui::SameLine();
    if (ImGui::Button("Assign to entity")) {
      const std::string path(BufferView(assign_path_));
      if (context.material_files.Reload(path)) {
        entity.material_path = path;
        buffered_material_.reset();  // The entity's slots change.
        file_status_ = "Assigned " + path + " (F5 saves the scene)";
      } else {
        file_status_ = "Cannot load " + path;
      }
    }
    if (!entity.material_path.empty()) {
      if (ImGui::Button("Reload file")) {
        file_status_ = context.material_files.Reload(entity.material_path)
                           ? "Reloaded " + entity.material_path
                           : "Cannot load " + entity.material_path;
        buffered_material_.reset();
      }
      ImGui::SameLine();
      if (ImGui::Button("Use model materials")) {
        entity.material_path.clear();
        buffered_material_.reset();
        file_status_ = "Cleared the material file (F5 saves the scene)";
      }
    }
  }
  if (!file_status_.empty()) {
    ImGui::TextDisabled("%s", file_status_.c_str());
  }
}

EditorActions EditorPanels::DrawEnvironment(const EditorContext& context) {
  EditorActions actions;
  scene::Scene& scene = context.scene;
  scene::Environment& environment = scene.environment;
  if (buffered_hdri_ != environment.hdri_path) {
    buffered_hdri_ = environment.hdri_path;
    CopyToBuffer(environment.hdri_path, hdri_path_);
  }

  const ImVec2 display = ImGui::GetIO().DisplaySize;
  ImGui::SetNextWindowPos({display.x * 0.30f, display.y * 0.55f},
                          ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowBgAlpha(0.85f);
  if (!ImGui::Begin("Environment", nullptr,
                    ImGuiWindowFlags_AlwaysAutoResize |
                        ImGuiWindowFlags_NoSavedSettings |
                        ImGuiWindowFlags_NoFocusOnAppearing)) {
    ImGui::End();
    return actions;
  }
  ImGui::PushItemWidth(ImGui::GetFontSize() * 14.0f);
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 22.0f);
  ImGui::InputText("##hdri", hdri_path_.data(), hdri_path_.size());
  ImGui::SameLine();
  if (ImGui::Button("Load HDRI")) {
    environment.hdri_path = std::string(BufferView(hdri_path_));
    buffered_hdri_ = environment.hdri_path;
    actions.load_environment = true;
  }
  ImGui::TextDisabled("Empty path = generated default sky");
  actions.environment_params_changed |=
      ImGui::SliderFloat("Sky intensity", &environment.intensity, 0.0f,
                         kMaxEnvironmentIntensity, "%.2f");
  float rotation_degrees = glm::degrees(environment.rotation_radians);
  if (ImGui::SliderFloat("Sky rotation", &rotation_degrees, -180.0f, 180.0f,
                         "%.1f deg")) {
    environment.rotation_radians = glm::radians(rotation_degrees);
    actions.environment_params_changed = true;
  }
  ImGui::SliderFloat("Exposure (EV)", &environment.exposure_ev, kMinExposureEv,
                     kMaxExposureEv, "%+.1f");
  constexpr std::array<const char*, 3> kTonemappers{"ACES (fitted)",
                                                    "PBR Neutral", "Clamp"};
  int tonemapper = static_cast<int>(context.post.tonemapper);
  if (ImGui::Combo("Tonemapper", &tonemapper, kTonemappers.data(),
                   static_cast<int>(kTonemappers.size()))) {
    context.post.tonemapper = static_cast<render::Tonemapper>(tonemapper);
  }

  ImGui::SeparatorText("Sun");
  scene::SunLight& sun = scene.sun;
  SunAngles angles = AnglesOf(sun.direction);
  bool sun_moved = ImGui::SliderFloat("Elevation", &angles.elevation, 1.0f,
                                      90.0f, "%.1f deg");
  sun_moved |= ImGui::SliderFloat("Azimuth", &angles.azimuth, -180.0f, 180.0f,
                                  "%.1f deg");
  if (sun_moved) {
    sun.direction = DirectionOf(angles);
  }
  ImGui::ColorEdit3("Colour", &sun.color.x, ImGuiColorEditFlags_Float);
  ImGui::SliderFloat("Intensity", &sun.intensity, 0.0f, kMaxSunIntensity,
                     "%.2f");
  ImGui::Checkbox("Casts shadows", &sun.casts_shadows);
  ImGui::BeginDisabled(!context.environment_has_sun);
  actions.align_sun = ImGui::Button("Align sun to HDRI");
  ImGui::SameLine();
  actions.sun_intensity_from_environment = ImGui::Button("Intensity from HDRI");
  ImGui::EndDisabled();
  if (!context.environment_has_sun) {
    ImGui::TextDisabled("No sun found in this environment");
  }

  ImGui::SeparatorText("Point light");
  ImGui::DragFloat3("Position", &scene.light.position.x, 0.05f);
  ImGui::ColorEdit3("Light colour", &scene.light.color.x,
                    ImGuiColorEditFlags_Float);
  ImGui::SliderFloat("Light intensity", &scene.light.intensity, 0.0f,
                     kMaxPointLightIntensity, "%.1f",
                     ImGuiSliderFlags_Logarithmic);
  ImGui::PopItemWidth();
  ImGui::TextDisabled("Saved with the scene (F5)");
  ImGui::End();
  return actions;
}

void EditorPanels::DrawPost(render::PostSettings& post) const {
  const ImVec2 display = ImGui::GetIO().DisplaySize;
  ImGui::SetNextWindowPos({display.x * 0.30f, display.y * 0.30f},
                          ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowBgAlpha(0.85f);
  if (ImGui::Begin("Post", nullptr,
                   ImGuiWindowFlags_AlwaysAutoResize |
                       ImGuiWindowFlags_NoSavedSettings |
                       ImGuiWindowFlags_NoFocusOnAppearing)) {
    ImGui::PushItemWidth(ImGui::GetFontSize() * 10.0f);
    ImGui::Checkbox("Bloom", &post.bloom_enabled);
    ImGui::BeginDisabled(!post.bloom_enabled);
    ImGui::SliderFloat("Bloom strength", &post.bloom_strength, 0.0f,
                       kMaxBloomStrength, "%.3f");
    ImGui::EndDisabled();
    ImGui::Checkbox("SSAO", &post.ssao_enabled);
    ImGui::BeginDisabled(!post.ssao_enabled);
    ImGui::SliderFloat("SSAO radius", &post.ssao_radius, kMinSsaoRadius,
                       kMaxSsaoRadius, "%.2f m");
    ImGui::SliderFloat("SSAO intensity", &post.ssao_intensity, 0.0f,
                       kMaxSsaoIntensity, "%.2f");
    ImGui::EndDisabled();
    ImGui::Checkbox("FXAA", &post.fxaa_enabled);
    ImGui::PopItemWidth();
    ImGui::TextDisabled("Not saved");
  }
  ImGui::End();
}

}  // namespace blue::ui
