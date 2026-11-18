#ifndef BLUE_EDITOR_VIEWPORT_VIEWPORT_PANEL_H_
#define BLUE_EDITOR_VIEWPORT_VIEWPORT_PANEL_H_

#include <imgui.h>
#include <vulkan/vulkan.h>

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include "asset/mesh_data.h"
#include "editor/document.h"
#include "editor/panel.h"
#include "editor/viewport/editor_camera.h"
#include "editor/viewport/gizmo.h"
#include "render/debug_lines.h"
#include "render/scene_view.h"

namespace blue::editor {

class CommandRegistry;
class EditorContext;
class ViewportPanel;

// What the viewport toolbar switches on. Clients read these to decide which
// debug draws to add (editor/viewport/debug_draw.h).
struct ViewportToggles {
  bool grid = true;
  bool bounds = true;        // of the selection
  bool colliders = false;    // collider wireframes
  bool sockets = true;       // socket axes + labels
  bool lights = true;        // light icons + ranges
  bool post_effects = true;  // bloom, SSAO, FXAA; off = plain tonemap
  bool shadows = true;
};

// What a mode shows in the viewport and how it reacts to it. Prefab mode and
// Scene mode each register one (ViewportPanel::SetClient()). UI-rate code:
// a few virtual calls per frame.
class ViewportClient {
 public:
  virtual ~ViewportClient() = default;

  // Called while the viewport window is current, after camera input and
  // before picking: show the gizmo of the primary selection
  // (viewport.Manipulate()), labels (viewport.AddLabel()), ImGui overlays.
  virtual void DrawOverlay(EditorContext& /*context*/,
                           ViewportPanel& /*viewport*/) {}
  // A left click on the image that the gizmo did not take: select what
  // `ray` hits (or clear the selection). `additive`: Ctrl or Shift held.
  virtual void Pick(EditorContext& /*context*/, ViewportPanel& /*viewport*/,
                    const Ray& /*ray*/, bool /*additive*/) {}
  // World bounds to frame for F: the selection, or everything when nothing
  // is selected; nullopt frames the origin.
  [[nodiscard]] virtual std::optional<asset::Bounds> FocusBounds(
      EditorContext& /*context*/) {
    return std::nullopt;
  }
  // Fills the frame's draws, lights, sun, environment-related fields,
  // selected_draws and debug lines (viewport.lines()) into `view`, whose
  // camera and post settings the viewport has set already (the line spans
  // are set after this returns, so lines added here are included).
  // Everything `view` points to must stay valid until the end of the frame
  // (keep the storage in the client). Called after DrawOverlay() and Pick().
  virtual void BuildView(EditorContext& context, ViewportPanel& viewport,
                         render::SceneView& view) = 0;

 protected:
  ViewportClient() = default;
  ViewportClient(const ViewportClient&) = default;
  ViewportClient& operator=(const ViewportClient&) = default;
};

// The editor's 3D viewport: a Panel ("Viewport", DockSlot::kCenter,
// no_padding) showing the renderer's viewport image with a toolbar, the
// editor camera, the transform gizmo, debug lines and labels. One per
// editor, shared by both modes; the active mode's ViewportClient decides
// what is drawn.
//
// Update() calls renderer.PrepareViewport() with last frame's panel size;
// Draw() shows the image, handles input, runs the client and hands the
// finished SceneView to EditorContext::SetFrameView().
//
// Keys while the viewport is hovered (and no text field is active): W / E /
// R translate / rotate / scale gizmo, Q toggles local / world, Ctrl held
// snaps, F frames, G toggles the grid. Mouse: Alt+LMB orbit, MMB pan
// (Shift+Alt+LMB too), wheel dolly, RMB + WASD / Q E fly (Shift faster),
// LMB click picks. The toolbar holds the same switches. Text commands
// (--exec): "view frame | reset | orbit <yaw> <pitch> | dolly <steps> |
// target <x> <y> <z> | distance <m> | toggle <switch>" and "gizmo
// translate|rotate|scale|local|world|snap <step>".
//
// Not thread-safe. Neither copyable nor movable.
class ViewportPanel final : public Panel {
 public:
  static constexpr const char* kName = "Viewport";

  ViewportPanel();

  ViewportPanel(const ViewportPanel&) = delete;
  ViewportPanel& operator=(const ViewportPanel&) = delete;

  // How the shell registers this panel.
  [[nodiscard]] static PanelDesc Desc();

  // Sets the client of `mode` (null: show nothing but the grid). Borrowed;
  // must outlive the panel or be reset.
  void SetClient(EditorMode mode, ViewportClient* client);

  void Update(EditorContext& context, float dt) override;
  void Draw(EditorContext& context) override;

  // Shows the gizmo for `world` (in/out). Call from
  // ViewportClient::DrawOverlay() for the one selected object. `parent`
  // (when non-null) is the world matrix of the object's parent, used for
  // the local space. `id` distinguishes objects so a drag does not jump to
  // another one. kBegin -> BeginEdit, kChanged -> apply, kEnd -> commit.
  GizmoEvent Manipulate(glm::mat4& world, int id,
                        const glm::mat4* parent = nullptr);

  // Debug lines of this frame; cleared in Update(). Add any time before the
  // client's BuildView() returns.
  [[nodiscard]] render::DebugLines& lines() { return lines_; }
  // Draws `text` centred at world position `position` (not when behind the
  // camera). From DrawOverlay() (or earlier in the frame).
  void AddLabel(const glm::vec3& position, const std::string& text,
                ImU32 color = IM_COL32(255, 255, 255, 230));

  // Frames `bounds` (world space).
  void FrameBounds(const asset::Bounds& bounds);

  [[nodiscard]] EditorCamera& camera() { return camera_; }
  [[nodiscard]] const EditorCamera& camera() const { return camera_; }
  // The toggles of the active mode (each mode keeps its own; Scene mode
  // starts without the grid, its scenes have a ground).
  [[nodiscard]] ViewportToggles& toggles() { return toggles_[mode_index_]; }
  [[nodiscard]] const ViewportToggles& toggles() const {
    return toggles_[mode_index_];
  }
  [[nodiscard]] GizmoSettings& gizmo() { return gizmo_; }
  // The ray through the centre of the image (e.g. "place in front of the
  // camera").
  [[nodiscard]] Ray CenterRay() const;
  // The ray through ImGui screen position `screen` (e.g. a drag-and-drop
  // target position).
  [[nodiscard]] Ray RayAt(ImVec2 screen) const;
  // True while the image is hovered.
  [[nodiscard]] bool hovered() const { return hovered_; }
  // Returns the ImGui screen position of world point `p`, or nullopt when
  // it is behind the camera.
  [[nodiscard]] std::optional<ImVec2> WorldToScreen(const glm::vec3& p) const;
  // The image's top-left corner and size in ImGui screen coordinates (valid
  // from Draw() on), e.g. for drop targets.
  [[nodiscard]] ImVec2 image_min() const { return image_min_; }
  [[nodiscard]] ImVec2 image_size() const { return image_size_; }

 private:
  // The camera drag in progress (it lasts while its button is held).
  enum class CameraDrag { kNone, kOrbit, kPan, kFly };

  struct Label {
    glm::vec3 position;
    std::string text;
    ImU32 color;
  };

  // Registers the "view" and "gizmo" text commands (once, from Update()).
  void RegisterCommands(CommandRegistry& commands);
  void DrawToolbar();
  // Points the camera along the player start of a scene document shown for
  // the first time (Prefab mode frames its own documents).
  void FrameNewDocument(EditorContext& context);
  // Camera mouse drags, fly keys and wheel.
  void HandleCameraInput(float dt);
  void HandleShortcuts();
  void DrawLabels() const;
  [[nodiscard]] float aspect() const;
  [[nodiscard]] ViewportClient* client(EditorMode mode) const {
    return clients_[static_cast<size_t>(mode)];
  }

  std::array<ViewportClient*, 2> clients_{};
  EditorCamera camera_;
  std::array<ViewportToggles, 2> toggles_{ViewportToggles{},
                                          ViewportToggles{.grid = false}};
  size_t mode_index_ = 0;  // EditorMode of the current frame
  // The scene document shown last frame; a new one is framed once.
  const SceneDocument* shown_scene_ = nullptr;
  GizmoSettings gizmo_;
  render::DebugLines lines_;
  std::vector<Label> labels_;
  size_t label_count_ = 0;  // labels_[0, label_count_) are this frame's
  // The view handed to the context; its spans point into lines_ and the
  // client's storage.
  render::SceneView view_;

  VkExtent2D size_{1, 1};
  ImVec2 image_min_{0.0f, 0.0f};
  ImVec2 image_size_{1.0f, 1.0f};
  float dt_ = 0.0f;
  bool commands_registered_ = false;
  std::optional<EditorMode> last_mode_;
  CameraDrag camera_drag_ = CameraDrag::kNone;
  bool hovered_ = false;
  bool frame_requested_ = false;
  // Gizmo state this frame: Manipulate() was called, it is being dragged,
  // it is under the mouse.
  bool gizmo_called_ = false;
  bool gizmo_using_ = false;
  bool gizmo_over_ = false;
  // Left button pressed on the image and not dragged since.
  bool click_pending_ = false;
  ImVec2 click_start_{0.0f, 0.0f};
};

}  // namespace blue::editor

#endif  // BLUE_EDITOR_VIEWPORT_VIEWPORT_PANEL_H_
