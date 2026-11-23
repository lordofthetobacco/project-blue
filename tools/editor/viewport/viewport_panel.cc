#include "editor/viewport/viewport_panel.h"

#include <ImGuizmo.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <string>
#include <utility>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/vec4.hpp>

#include "core/result.h"
#include "editor/commands.h"
#include "editor/editor_context.h"
#include "editor/viewport/debug_draw.h"

namespace blue::editor {
namespace {

// Radians of orbit / look per pixel of mouse movement.
constexpr float kOrbitSpeed = 0.006f;
// A press that moves further than this many pixels is a drag, not a click.
constexpr float kClickSlop = 4.0f;
// Fly speed in metres per second, scaled up with the orbit distance.
constexpr float kMinFlySpeed = 2.0f;
constexpr float kFastFlyFactor = 4.0f;
// The gizmo's own projection: a finite, OpenGL-style one (ImGuizmo's
// convention). Only its view of the scene must match the renderer's, not
// its depth range.
constexpr float kGizmoFarPlane = 10000.0f;

ImGuizmo::OPERATION ToImGuizmo(GizmoOperation operation) {
  switch (operation) {
    case GizmoOperation::kTranslate:
      return ImGuizmo::TRANSLATE;
    case GizmoOperation::kRotate:
      return ImGuizmo::ROTATE;
    case GizmoOperation::kScale:
      return ImGuizmo::SCALE;
  }
  return ImGuizmo::TRANSLATE;
}

// Shows a toolbar toggle button that is highlighted while `active`.
bool ToolbarButton(const char* label, bool active) {
  if (active) {
    ImGui::PushStyleColor(ImGuiCol_Button,
                          ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
  }
  const bool pressed = ImGui::SmallButton(label);
  if (active) {
    ImGui::PopStyleColor();
  }
  return pressed;
}

using CommandResult = core::Result<void, std::string>;

// Returns the toggle of `toggles` / `gizmo` named `name`, or null.
bool* FindToggle(std::string_view name, ViewportToggles& toggles,
                 GizmoSettings& gizmo) {
  struct Entry {
    std::string_view name;
    bool* value;
  };
  const Entry entries[] = {
      {"grid", &toggles.grid},           {"bounds", &toggles.bounds},
      {"colliders", &toggles.colliders}, {"sockets", &toggles.sockets},
      {"lights", &toggles.lights},       {"post", &toggles.post_effects},
      {"shadows", &toggles.shadows},     {"gizmo", &gizmo.visible},
  };
  for (const Entry& entry : entries) {
    if (entry.name == name) {
      return entry.value;
    }
  }
  return nullptr;
}

}  // namespace

ViewportPanel::ViewportPanel() = default;

PanelDesc ViewportPanel::Desc() {
  return {.name = kName,
          .modes = kAllModes,
          .slot = DockSlot::kCenter,
          .open_by_default = true,
          .window_flags =
              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse,
          .no_padding = true};
}

void ViewportPanel::SetClient(EditorMode mode, ViewportClient* client) {
  clients_[static_cast<size_t>(mode)] = client;
}

void ViewportPanel::Update(EditorContext& context, float dt) {
  if (!commands_registered_) {
    RegisterCommands(context.commands());
    commands_registered_ = true;
  }
  dt_ = dt;
  lines_.Clear();
  label_count_ = 0;
  // Prefab mode shows prefabs under the neutral default sky; Scene mode
  // sets the scene's environment itself when it becomes active.
  if (last_mode_ && *last_mode_ != context.mode() &&
      context.mode() == EditorMode::kPrefab) {
    context.renderer().ResetEnvironment();
  }
  last_mode_ = context.mode();
  mode_index_ = static_cast<size_t>(context.mode());
  static_cast<void>(context.renderer().PrepareViewport(size_));
}

void ViewportPanel::Draw(EditorContext& context) {
  DrawToolbar();

  const ImGuiIO& io = ImGui::GetIO();
  const ImVec2 available = ImGui::GetContentRegionAvail();
  image_size_ =
      ImVec2(std::max(available.x, 1.0f), std::max(available.y, 1.0f));
  image_min_ = ImGui::GetCursorScreenPos();
  size_ = {
      static_cast<uint32_t>(std::max(
          1.0f, std::round(image_size_.x * io.DisplayFramebufferScale.x))),
      static_cast<uint32_t>(std::max(
          1.0f, std::round(image_size_.y * io.DisplayFramebufferScale.y)))};

  // The image is drawn, not submitted as an item: ImGuizmo only starts a
  // drag while no ImGui item is hovered or active, so a button covering the
  // image would block it. Hover is the window plus the image rectangle; the
  // window must not move when dragged by its contents (the shell sets
  // ImGuiIO::ConfigWindowsMoveFromTitleBarOnly), and the panel is docked.
  ImDrawList& draw = *ImGui::GetWindowDrawList();
  const ImVec2 image_max(image_min_.x + image_size_.x,
                         image_min_.y + image_size_.y);
  draw.AddImage(context.imgui().ViewportTexture(), image_min_, image_max);
  ImGui::Dummy(image_size_);
  hovered_ = ImGui::IsWindowHovered() &&
             ImGui::IsMouseHoveringRect(image_min_, image_max) &&
             !ImGui::IsAnyItemHovered();

  HandleCameraInput(dt_);
  HandleShortcuts();

  ViewportClient* active = client(context.mode());
  gizmo_called_ = false;
  gizmo_over_ = false;
  draw.PushClipRect(image_min_, image_max, true);
  if (active != nullptr) {
    active->DrawOverlay(context, *this);
  }
  if (!gizmo_called_) {
    gizmo_using_ = false;
  }
  DrawLabels();
  draw.PopClipRect();

  // Picking: a left press on the image that the gizmo did not take, released
  // without dragging.
  const bool alt = io.KeyAlt;
  if (hovered_ && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !alt &&
      !gizmo_over_ && !gizmo_using_) {
    click_pending_ = true;
    click_start_ = io.MousePos;
  }
  if (click_pending_) {
    const float dx = io.MousePos.x - click_start_.x;
    const float dy = io.MousePos.y - click_start_.y;
    if (dx * dx + dy * dy > kClickSlop * kClickSlop || gizmo_using_) {
      click_pending_ = false;
    } else if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
      click_pending_ = false;
      if (active != nullptr) {
        active->Pick(context, *this, RayAt(io.MousePos),
                     io.KeyCtrl || io.KeyShift);
      }
    }
  }
  FrameNewDocument(context);
  if (frame_requested_) {
    frame_requested_ = false;
    std::optional<asset::Bounds> bounds;
    if (active != nullptr) {
      bounds = active->FocusBounds(context);
    }
    FrameBounds(bounds.value_or(
        asset::Bounds{.min = glm::vec3(-1.0f), .max = glm::vec3(1.0f)}));
  }

  // The frame's view: camera and toggles here, content from the client,
  // then the debug lines (the client may still have added some). A fresh
  // view has PostSettings::upscaling off: the editor keeps FXAA, since the
  // temporal scaler would need the previous frame's transforms of
  // everything a tool moves (world::DrawHistory) and would soften the
  // one-pixel overlays' surroundings while the camera orbits.
  view_ = render::SceneView{};
  view_.camera = {.view = camera_.view(),
                  .projection = camera_.projection(aspect()),
                  .position = camera_.position(),
                  .vertical_fov_radians = camera_.vertical_fov_radians(),
                  .near_plane = camera_.near_plane()};
  view_.shadows_enabled = toggles().shadows;
  if (!toggles().post_effects) {
    view_.post.bloom_enabled = false;
    view_.post.ssao_enabled = false;
    view_.post.fxaa_enabled = false;
  }
  if (toggles().grid) {
    const float extent = std::clamp(camera_.distance() * 4.0f, 20.0f, 1000.0f);
    // Lifted with the distance so it does not z-fight a ground at y = 0.
    AddGroundGrid(lines_, camera_.target(), extent,
                  std::max(0.002f, 0.002f * camera_.distance()));
  }
  if (active != nullptr) {
    active->BuildView(context, *this, view_);
  }
  view_.lines = lines_.tested();
  view_.overlay_lines = lines_.on_top();
  context.SetFrameView(&view_);
}

GizmoEvent ViewportPanel::Manipulate(glm::mat4& world, int id,
                                     const glm::mat4* /*parent*/) {
  gizmo_called_ = true;
  if (!gizmo_.visible) {
    return GizmoEvent::kNone;
  }
  ImGuizmo::SetOrthographic(false);
  ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());
  ImGuizmo::SetRect(image_min_.x, image_min_.y, image_size_.x, image_size_.y);
  ImGuizmo::PushID(id);
  const glm::mat4 view = camera_.view();
  const glm::mat4 projection =
      glm::perspectiveRH_NO(camera_.vertical_fov_radians(), aspect(),
                            camera_.near_plane(), kGizmoFarPlane);
  // Scale always works on the object's own axes.
  const ImGuizmo::MODE mode = gizmo_.space == GizmoSpace::kLocal ||
                                      gizmo_.operation == GizmoOperation::kScale
                                  ? ImGuizmo::LOCAL
                                  : ImGuizmo::WORLD;
  float step = 0.0f;
  switch (gizmo_.operation) {
    case GizmoOperation::kTranslate:
      step = gizmo_.translate_snap;
      break;
    case GizmoOperation::kRotate:
      step = gizmo_.rotate_snap_degrees;
      break;
    case GizmoOperation::kScale:
      step = gizmo_.scale_snap;
      break;
  }
  const float snap[3] = {step, step, step};
  const bool snapping = ImGui::GetIO().KeyCtrl && step > 0.0f;
  const bool changed = ImGuizmo::Manipulate(
      glm::value_ptr(view), glm::value_ptr(projection),
      ToImGuizmo(gizmo_.operation), mode, glm::value_ptr(world), nullptr,
      snapping ? snap : nullptr);
  const bool using_now = ImGuizmo::IsUsing();
  gizmo_over_ = gizmo_over_ || ImGuizmo::IsOver();
  ImGuizmo::PopID();

  GizmoEvent event = GizmoEvent::kNone;
  if (using_now && !gizmo_using_) {
    event = GizmoEvent::kBegin;
  } else if (using_now && changed) {
    event = GizmoEvent::kChanged;
  } else if (!using_now && gizmo_using_) {
    event = GizmoEvent::kEnd;
  }
  gizmo_using_ = using_now;
  return event;
}

void ViewportPanel::AddLabel(const glm::vec3& position, const std::string& text,
                             ImU32 color) {
  if (label_count_ == labels_.size()) {
    labels_.push_back({});
  }
  Label& label = labels_[label_count_++];
  label.position = position;
  label.text.assign(text);  // Reuses the string's capacity.
  label.color = color;
}

void ViewportPanel::FrameBounds(const asset::Bounds& bounds) {
  const float radius = glm::length(bounds.size()) * 0.5f;
  camera_.Frame(bounds.center(), std::max(radius, 0.05f));
}

Ray ViewportPanel::CenterRay() const {
  return camera_.ScreenRay({0.0f, 0.0f}, aspect());
}

Ray ViewportPanel::RayAt(ImVec2 screen) const {
  const glm::vec2 ndc((screen.x - image_min_.x) / image_size_.x * 2.0f - 1.0f,
                      1.0f - (screen.y - image_min_.y) / image_size_.y * 2.0f);
  return camera_.ScreenRay(ndc, aspect());
}

std::optional<ImVec2> ViewportPanel::WorldToScreen(const glm::vec3& p) const {
  const glm::vec4 clip =
      camera_.projection(aspect()) * camera_.view() * glm::vec4(p, 1.0f);
  if (!(clip.w > 1.0e-5f)) {
    return std::nullopt;
  }
  const float x = clip.x / clip.w;
  const float y = clip.y / clip.w;
  return ImVec2(image_min_.x + (x * 0.5f + 0.5f) * image_size_.x,
                image_min_.y + (0.5f - y * 0.5f) * image_size_.y);
}

void ViewportPanel::RegisterCommands(CommandRegistry& commands) {
  commands.Register(
      "view",
      "view frame | reset | orbit <yaw deg> <pitch deg> | dolly <steps> | "
      "target <x> <y> <z> | distance <m> | toggle "
      "grid|bounds|colliders|sockets|lights|post|shadows|gizmo",
      [this](std::span<const std::string> args) -> CommandResult {
        if (args.empty()) {
          return CommandResult::Error("view: missing sub-command");
        }
        const std::string& verb = args[0];
        const auto number =
            [&](size_t i,
                std::string_view what) -> core::Result<float, std::string> {
          if (i >= args.size()) {
            return core::Result<float, std::string>::Error(
                "view " + verb + ": missing " + std::string(what));
          }
          return ParseFloatArg(args[i], what);
        };
        if (verb == "frame") {
          frame_requested_ = true;
        } else if (verb == "reset") {
          camera_ = EditorCamera();
        } else if (verb == "orbit") {
          const auto yaw = number(1, "yaw");
          const auto pitch = number(2, "pitch");
          if (!yaw.ok()) {
            return CommandResult::Error(yaw.error());
          }
          if (!pitch.ok()) {
            return CommandResult::Error(pitch.error());
          }
          constexpr float kRadiansPerDegree = 0.017453292f;
          camera_.Orbit(glm::vec2(yaw.value(), pitch.value()) *
                        kRadiansPerDegree);
        } else if (verb == "dolly") {
          const auto steps = number(1, "steps");
          if (!steps.ok()) {
            return CommandResult::Error(steps.error());
          }
          camera_.Dolly(steps.value());
        } else if (verb == "target") {
          glm::vec3 target(0.0f);
          for (int i = 0; i < 3; ++i) {
            const auto value = number(static_cast<size_t>(i) + 1, "coordinate");
            if (!value.ok()) {
              return CommandResult::Error(value.error());
            }
            target[i] = value.value();
          }
          camera_.LookAt(camera_.position() - camera_.target() + target,
                         target);
        } else if (verb == "distance") {
          const auto distance = number(1, "distance");
          if (!distance.ok()) {
            return CommandResult::Error(distance.error());
          }
          camera_.LookAt(
              camera_.target() - camera_.forward() * distance.value(),
              camera_.target());
        } else if (verb == "toggle") {
          bool* toggle = args.size() > 1
                             ? FindToggle(args[1], toggles(), gizmo_)
                             : nullptr;
          if (toggle == nullptr) {
            return CommandResult::Error("view toggle: unknown switch");
          }
          *toggle = !*toggle;
        } else {
          return CommandResult::Error("view: unknown sub-command '" + verb +
                                      "'");
        }
        return CommandResult::Ok();
      });
  commands.Register(
      "gizmo", "gizmo translate|rotate|scale|local|world|snap <step>",
      [this](std::span<const std::string> args) -> CommandResult {
        const std::string verb = args.empty() ? std::string() : args[0];
        if (verb == "translate") {
          gizmo_.operation = GizmoOperation::kTranslate;
        } else if (verb == "rotate") {
          gizmo_.operation = GizmoOperation::kRotate;
        } else if (verb == "scale") {
          gizmo_.operation = GizmoOperation::kScale;
        } else if (verb == "local") {
          gizmo_.space = GizmoSpace::kLocal;
        } else if (verb == "world") {
          gizmo_.space = GizmoSpace::kWorld;
        } else if (verb == "snap" && args.size() > 1) {
          const auto step = ParseFloatArg(args[1], "snap step");
          if (!step.ok()) {
            return CommandResult::Error(step.error());
          }
          switch (gizmo_.operation) {
            case GizmoOperation::kTranslate:
              gizmo_.translate_snap = step.value();
              break;
            case GizmoOperation::kRotate:
              gizmo_.rotate_snap_degrees = step.value();
              break;
            case GizmoOperation::kScale:
              gizmo_.scale_snap = step.value();
              break;
          }
        } else {
          return CommandResult::Error(
              "gizmo: expected translate, rotate, "
              "scale, local, world or snap <step>");
        }
        return CommandResult::Ok();
      });
}

void ViewportPanel::FrameNewDocument(EditorContext& context) {
  // Behind and above the player start, looking where the player looks.
  constexpr float kPitch = -0.3f;
  constexpr float kLookAhead = 6.0f;  // m in front of the player
  constexpr float kRadius = 6.0f;     // m framed around that point
  constexpr float kEyeHeight = 1.6f;
  // Prefab mode frames its documents itself (PrefabViewportClient).
  if (context.mode() != EditorMode::kScene) {
    return;
  }
  const SceneDocument* document = context.scene_document();
  if (document == shown_scene_) {
    return;
  }
  shown_scene_ = document;
  if (document == nullptr) {
    return;
  }
  const scene::PlayerStart& start = document->state().player_start;
  const glm::vec3 forward(-std::sin(start.yaw_radians), 0.0f,
                          -std::cos(start.yaw_radians));
  camera_.SetOrientation(start.yaw_radians, kPitch);
  camera_.Frame(
      start.position + glm::vec3(0.0f, kEyeHeight, 0.0f) + forward * kLookAhead,
      kRadius);
}

void ViewportPanel::DrawToolbar() {
  ImGui::SetCursorPos(
      ImVec2(ImGui::GetCursorPosX() + 4.0f, ImGui::GetCursorPosY() + 2.0f));
  if (ToolbarButton("Move (W)",
                    gizmo_.operation == GizmoOperation::kTranslate)) {
    gizmo_.operation = GizmoOperation::kTranslate;
  }
  ImGui::SameLine();
  if (ToolbarButton("Rotate (E)",
                    gizmo_.operation == GizmoOperation::kRotate)) {
    gizmo_.operation = GizmoOperation::kRotate;
  }
  ImGui::SameLine();
  if (ToolbarButton("Scale (R)", gizmo_.operation == GizmoOperation::kScale)) {
    gizmo_.operation = GizmoOperation::kScale;
  }
  ImGui::SameLine();
  if (ImGui::SmallButton(gizmo_.space == GizmoSpace::kLocal ? "Local (Q)"
                                                            : "World (Q)")) {
    gizmo_.space = gizmo_.space == GizmoSpace::kLocal ? GizmoSpace::kWorld
                                                      : GizmoSpace::kLocal;
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("Snap...")) {
    ImGui::OpenPopup("##viewport_snap");
  }
  if (ImGui::BeginPopup("##viewport_snap")) {
    ImGui::TextUnformatted("Snap steps (hold Ctrl while dragging)");
    ImGui::SetNextItemWidth(120.0f);
    ImGui::DragFloat("Move (m)", &gizmo_.translate_snap, 0.01f, 0.001f, 100.0f,
                     "%.3f");
    ImGui::SetNextItemWidth(120.0f);
    ImGui::DragFloat("Rotate (deg)", &gizmo_.rotate_snap_degrees, 0.5f, 0.5f,
                     180.0f, "%.1f");
    ImGui::SetNextItemWidth(120.0f);
    ImGui::DragFloat("Scale", &gizmo_.scale_snap, 0.01f, 0.001f, 10.0f, "%.3f");
    ImGui::EndPopup();
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("Show...")) {
    ImGui::OpenPopup("##viewport_show");
  }
  if (ImGui::BeginPopup("##viewport_show")) {
    ImGui::Checkbox("Grid (G)", &toggles().grid);
    ImGui::Checkbox("Selection bounds", &toggles().bounds);
    ImGui::Checkbox("Colliders", &toggles().colliders);
    ImGui::Checkbox("Sockets", &toggles().sockets);
    ImGui::Checkbox("Lights", &toggles().lights);
    ImGui::Checkbox("Gizmo", &gizmo_.visible);
    ImGui::Separator();
    ImGui::Checkbox("Post effects", &toggles().post_effects);
    ImGui::Checkbox("Shadows", &toggles().shadows);
    ImGui::EndPopup();
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("Frame (F)")) {
    frame_requested_ = true;
  }
}

void ViewportPanel::HandleCameraInput(float dt) {
  const ImGuiIO& io = ImGui::GetIO();
  const glm::vec2 delta(io.MouseDelta.x, io.MouseDelta.y);

  // A drag starts on the image and lasts while its button is held, even
  // when the cursor leaves the image.
  if (camera_drag_ == CameraDrag::kNone && hovered_ && !gizmo_using_) {
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && io.KeyAlt) {
      camera_drag_ = io.KeyShift ? CameraDrag::kPan : CameraDrag::kOrbit;
    } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
      camera_drag_ = CameraDrag::kPan;
    } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
      camera_drag_ = CameraDrag::kFly;
    }
  }
  switch (camera_drag_) {
    case CameraDrag::kNone:
      break;
    case CameraDrag::kOrbit:
      camera_.Orbit(-delta * kOrbitSpeed);
      if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        camera_drag_ = CameraDrag::kNone;
      }
      break;
    case CameraDrag::kPan:
      camera_.Pan(delta, image_size_.y);
      if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle) &&
          !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        camera_drag_ = CameraDrag::kNone;
      }
      break;
    case CameraDrag::kFly: {
      camera_.Look(-delta * kOrbitSpeed);
      glm::vec3 move(0.0f);
      move.z += ImGui::IsKeyDown(ImGuiKey_W) ? 1.0f : 0.0f;
      move.z -= ImGui::IsKeyDown(ImGuiKey_S) ? 1.0f : 0.0f;
      move.x += ImGui::IsKeyDown(ImGuiKey_D) ? 1.0f : 0.0f;
      move.x -= ImGui::IsKeyDown(ImGuiKey_A) ? 1.0f : 0.0f;
      move.y += ImGui::IsKeyDown(ImGuiKey_E) ? 1.0f : 0.0f;
      move.y -= ImGui::IsKeyDown(ImGuiKey_Q) ? 1.0f : 0.0f;
      if (glm::dot(move, move) > 0.0f) {
        const float speed = std::max(kMinFlySpeed, camera_.distance()) *
                            (io.KeyShift ? kFastFlyFactor : 1.0f);
        camera_.Move(glm::normalize(move) * speed * dt);
      }
      if (!ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
        camera_drag_ = CameraDrag::kNone;
      }
      break;
    }
  }
  if (hovered_ && io.MouseWheel != 0.0f) {
    camera_.Dolly(io.MouseWheel);
  }
}

void ViewportPanel::HandleShortcuts() {
  const ImGuiIO& io = ImGui::GetIO();
  const bool focused =
      hovered_ || ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
  // Keys belong to text fields while typing, and to flying while RMB is held.
  if (!focused || io.WantTextInput || camera_drag_ == CameraDrag::kFly ||
      io.KeyCtrl) {
    return;
  }
  if (ImGui::IsKeyPressed(ImGuiKey_W, false)) {
    gizmo_.operation = GizmoOperation::kTranslate;
  }
  if (ImGui::IsKeyPressed(ImGuiKey_E, false)) {
    gizmo_.operation = GizmoOperation::kRotate;
  }
  if (ImGui::IsKeyPressed(ImGuiKey_R, false)) {
    gizmo_.operation = GizmoOperation::kScale;
  }
  if (ImGui::IsKeyPressed(ImGuiKey_Q, false)) {
    gizmo_.space = gizmo_.space == GizmoSpace::kLocal ? GizmoSpace::kWorld
                                                      : GizmoSpace::kLocal;
  }
  if (ImGui::IsKeyPressed(ImGuiKey_F, false)) {
    frame_requested_ = true;
  }
  if (ImGui::IsKeyPressed(ImGuiKey_G, false)) {
    toggles().grid = !toggles().grid;
  }
}

void ViewportPanel::DrawLabels() const {
  ImDrawList& draw = *ImGui::GetWindowDrawList();
  for (size_t i = 0; i < label_count_; ++i) {
    const Label& label = labels_[i];
    const std::optional<ImVec2> screen = WorldToScreen(label.position);
    if (!screen) {
      continue;
    }
    const ImVec2 text_size = ImGui::CalcTextSize(label.text.c_str());
    const ImVec2 corner(screen->x - text_size.x * 0.5f,
                        screen->y - text_size.y - 4.0f);
    draw.AddRectFilled(
        ImVec2(corner.x - 3.0f, corner.y - 1.0f),
        ImVec2(corner.x + text_size.x + 3.0f, corner.y + text_size.y + 1.0f),
        IM_COL32(0, 0, 0, 140), 3.0f);
    draw.AddText(corner, label.color, label.text.c_str());
  }
}

float ViewportPanel::aspect() const {
  return image_size_.x / std::max(image_size_.y, 1.0f);
}

}  // namespace blue::editor
