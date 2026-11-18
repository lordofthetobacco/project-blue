#ifndef BLUE_RENDER_SCENE_VIEW_H_
#define BLUE_RENDER_SCENE_VIEW_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "asset/mesh_data.h"
#include "render/debug_lines.h"
#include "scene/scene.h"

// What the application hands the renderer for one frame: camera, lights,
// draw items and post-processing settings, plus the statistics it gets back.

namespace blue::render {

struct Camera {
  glm::mat4 view{1.0f};
  glm::mat4 projection{1.0f};
  glm::vec3 position{0.0f};
  // Parameters of `projection`, needed to fit the shadow cascades.
  float vertical_fov_radians = 1.0f;
  float near_plane = 0.1f;
};

// Light intensities follow one convention throughout the renderer: color *
// intensity is the radiance a white Lambertian surface facing the light
// reflects (for a point light: at 1 m). So intensity 1 lights a white
// surface to exactly 1 before exposure, as in the pre-PBR shading.
//
// Point lights fall off with the inverse square of the distance d. A light
// with a positive `range` r is also faded by the window
// (1 - (d / r)^4)^2, clamped to [0, 1], so it reaches exactly zero at r and
// can be culled beyond it; range <= 0 means unlimited (pure inverse square,
// never culled). Point lights cast no shadows.
struct PointLight {
  glm::vec3 position{0.0f};
  glm::vec3 color{1.0f};
  float intensity = 1.0f;
  float range = 0.0f;  // m
};

// The most point lights a frame shades; SceneView::point_lights beyond it
// (after culling) are dropped, the dimmest at the camera first.
inline constexpr size_t kMaxPointLights = 32;

// Handle to a mesh owned by the renderer.
enum class MeshId : uint32_t {};

// Handle to a material owned by the renderer (Renderer::CreateMaterial()).
enum class MaterialId : uint32_t {};

// The material the renderer creates first: neutral grey dielectric. Used by
// draw items that name no material.
inline constexpr MaterialId kDefaultMaterial{0};

// One range of a mesh's index buffer drawn with one material: a submesh of
// asset::MeshData with its model-local material index resolved.
struct SubmeshDraw {
  uint32_t first_index = 0;
  uint32_t index_count = 0;
  MaterialId material = kDefaultMaterial;
};

struct DrawItem {
  MeshId mesh{};
  glm::mat4 transform{1.0f};
  // Local-space bounds of the mesh; culled against the camera and the shadow
  // cascades after `transform`.
  asset::Bounds bounds;
  // One draw per element; usually owned by the world::PrefabCache and
  // borrowed for the Renderer::DrawFrame call. Empty draws the whole mesh
  // with kDefaultMaterial (or `material_override`).
  std::span<const SubmeshDraw> submeshes;
  // Replaces the material of every submesh when set, e.g. a builtin cube
  // drawn as a tracer.
  std::optional<MaterialId> material_override;
};

// Draw counts of the most recent frame. Culled items were submitted in the
// SceneView but skipped because their bounds fell outside the view.
struct RenderStats {
  uint32_t draw_calls = 0;  // Main pass, one per submesh drawn.
  uint64_t triangles = 0;   // Main pass.
  uint32_t culled_draws = 0;
  uint32_t shadow_draw_calls = 0;  // Summed over the cascades.
  uint32_t shadow_culled_draws = 0;
};

// A collimated reflex sight, drawn over the opaque scene. The lens window is
// a disc in world space; its dot is an image at infinity along `axis`, so a
// pixel of the window shows the dot when the direction from the camera
// through it is within the dot's angular radius of `axis`. The window is
// depth tested (whatever is in front hides it) but writes no depth.
struct ReticleSight {
  // Maps the unit disc (x, y in [-1, 1], z = 0) onto the lens window in
  // world space.
  glm::mat4 lens{1.0f};
  glm::vec3 axis{0.0f, 0.0f, -1.0f};  // world, unit, pointing downrange
  // Nominal dot size: 3 MOA diameter.
  float dot_angular_radius = 4.36e-4f;  // radians
  // Linear HDR radiance, before exposure. Pure red just above 1: the ACES
  // tonemapper shifts brighter saturated reds towards orange, so this keeps
  // the dot a crisp red at the default exposure.
  glm::vec3 dot_color{1.1f, 0.0f, 0.0f};
  // The dot never gets smaller than this many pixels across, so it stays
  // visible when the nominal size is below a pixel.
  float min_dot_pixels = 2.0f;
  glm::vec3 tint{0.004f, 0.006f, 0.005f};  // light added over the window
};

// How the HDR image is mapped to the display.
enum class Tonemapper : int32_t {
  kAcesFitted = 0,  // Stephen Hill's fit of the ACES RRT + ODT (default).
  kPbrNeutral = 1,  // Khronos PBR Neutral: keeps base colours faithful.
  kClamp = 2,       // No curve, clipped at 1: for comparison only.
};

// Settings of the passes after the scene pass.
struct PostSettings {
  // Exposure in stops: the HDR image is scaled by 2^exposure_ev before
  // tonemapping. At 0 every tonemapper keeps mid grey (0.18) where it was
  // before the HDR pipeline (shaders/tonemap.frag).
  float exposure_ev = 0.0f;
  Tonemapper tonemapper = Tonemapper::kAcesFitted;

  // Disabled effects are skipped entirely (no GPU cost).
  // Bloom: a blurred copy of the HDR image mixed in before tonemapping,
  // hdr = mix(hdr, bloom, bloom_strength). No threshold: every pixel blooms
  // a little, very bright ones visibly.
  bool bloom_enabled = true;
  float bloom_strength = 0.04f;  // [0, 1]; useful up to about 0.3
  // Screen-space ambient occlusion, applied to ambient / image-based light
  // only (never to the sun or the point light).
  bool ssao_enabled = true;
  float ssao_radius = 0.5f;     // m, world-space sampling radius
  float ssao_intensity = 1.0f;  // occlusion exponent: ao = pow(ao, intensity)
  // FXAA on the tonemapped image; off = plain copy to the swapchain.
  bool fxaa_enabled = true;
};

// Everything the renderer needs to draw one frame. The spans are borrowed
// and only need to stay valid for the Renderer::DrawFrame call.
struct SceneView {
  Camera camera;
  // Culled against the camera frustum each frame (lights with a range);
  // at most kMaxPointLights of the visible ones are shaded.
  std::span<const PointLight> point_lights;
  // Flat ambient radiance, see AmbientLight() in shaders/pbr.glsl.
  glm::vec3 ambient{0.03f};
  scene::SunLight sun;
  // Shadows are skipped entirely when this is false or `sun` casts none.
  bool shadows_enabled = true;
  // Tints each pixel by the shadow cascade it samples.
  bool cascade_debug = false;
  std::span<const DrawItem> draws;
  // Drawn after the draw items when present.
  std::optional<ReticleSight> sight;
  PostSettings post;

  // Editor overlays, drawn over the tonemapped image; empty spans cost
  // nothing. Line vertex pairs (DebugLines::tested() / on_top()): `lines`
  // are hidden by the scene in front of them, `overlay_lines` are not.
  std::span<const LineVertex> lines;
  std::span<const LineVertex> overlay_lines;
  // Indices into `draws` of the selected items, outlined in
  // `selection_color` (display space; dimmed where the item is hidden).
  std::span<const uint32_t> selected_draws;
  glm::vec4 selection_color{1.0f, 0.6f, 0.1f, 1.0f};
};

}  // namespace blue::render

#endif  // BLUE_RENDER_SCENE_VIEW_H_
