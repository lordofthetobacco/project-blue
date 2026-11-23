#ifndef BLUE_TESTS_RENDER_SCENES_H_
#define BLUE_TESTS_RENDER_SCENES_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "render/debug_lines.h"
#include "render/renderer.h"
#include "render/scene_view.h"
#include "render/texture_types.h"

// The fixed scenes of the headless render tests (design §5.1). Everything is
// built from code (builtin meshes, procedural textures, constant materials):
// no asset files, nothing time-based.

namespace blue::render_test {

// The output size every scene is rendered at.
inline constexpr uint32_t kSceneWidth = 640;
inline constexpr uint32_t kSceneHeight = 360;

// One scene: the SceneView to draw and the storage its spans borrow. Not
// movable (the view points into it).
struct TestScene {
  TestScene() = default;
  TestScene(const TestScene&) = delete;
  TestScene& operator=(const TestScene&) = delete;

  // Rendered through PrepareViewport() + DrawViewportFrame() and read back
  // from the viewport image (the editor path) instead of DrawFrame() and
  // the output image.
  bool viewport_output = false;
  // Frames drawn; the last one is read back.
  int frames = 2;

  std::vector<render::DrawItem> draws;
  std::vector<render::PointLight> point_lights;
  render::DebugLines lines;
  std::vector<uint32_t> selected;
  render::SceneView view;
};

// One of the procedural textures the scenes upload: kTextureSize square,
// full mip chain, each level exactly MipByteSize() bytes.
struct TextureSource {
  render::TextureFormat format = render::TextureFormat::kRgba8Unorm;
  std::vector<std::vector<std::byte>> mips;
  std::string_view name;
};
inline constexpr uint32_t kTextureSize = 64;

// Returns the scenes' textures in a fixed order: checker (RGBA8 sRGB), bc7
// (BC7 sRGB, one solid colour per block), alpha_discs (RGBA8 sRGB),
// normal_bumps and orm (RGBA8 UNORM).
[[nodiscard]] std::vector<TextureSource> TestTextures();

// Returns the names of every scene, in a fixed order (the golden files are
// tests/render/golden/<name>.png).
[[nodiscard]] std::span<const std::string_view> SceneNames();

// Uploads what scene `name` needs into `renderer` (a fresh one: the scene
// expects the default environment and creates its materials first) and
// returns it. `name` must be one of SceneNames().
[[nodiscard]] std::unique_ptr<TestScene> BuildScene(std::string_view name,
                                                    render::Renderer& renderer);

}  // namespace blue::render_test

#endif  // BLUE_TESTS_RENDER_SCENES_H_
