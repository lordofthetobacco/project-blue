#include "tests/render/scenes.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/common.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/ext/vector_uint3_sized.hpp>
#include <glm/ext/vector_uint4_sized.hpp>
#include <glm/geometric.hpp>
#include <glm/mat3x3.hpp>
#include <glm/mat4x4.hpp>
#include <glm/matrix.hpp>
#include <glm/trigonometric.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "asset/image_decoder.h"
#include "asset/mesh_data.h"
#include "camera/projection.h"
#include "core/log.h"
#include "material/material.h"
#include "render/texture_types.h"

namespace blue::render_test {
namespace {

using render::DrawItem;
using render::MaterialId;
using render::MeshId;
using render::Renderer;
using render::TextureFormat;
using render::TextureId;
using render::TextureUpload;

constexpr std::array<std::string_view, 9> kSceneNames = {
    "clear_and_upload", "lit_basic",     "lit_full_aces",
    "lit_full_neutral", "lit_full_clamp", "materials",
    "weapon_motion",    "scope",          "editor_overlay",
};

constexpr float kPi = std::numbers::pi_v<float>;

// ---------------------------------------------------------------------------
// Procedural textures.

// One mip level of an uploaded texture.
using MipBytes = std::vector<std::byte>;

// An RGBA8 image, rows top to bottom.
struct Rgba8 {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> texels;
};

// Returns `image` halved by a 2 x 2 box filter (sizes are powers of two).
Rgba8 Downsample(const Rgba8& image) {
  Rgba8 half{.width = std::max(image.width / 2, 1u),
             .height = std::max(image.height / 2, 1u)};
  half.texels.resize(size_t{half.width} * half.height * 4);
  for (uint32_t y = 0; y < half.height; ++y) {
    for (uint32_t x = 0; x < half.width; ++x) {
      for (uint32_t c = 0; c < 4; ++c) {
        uint32_t sum = 0;
        for (uint32_t dy = 0; dy < 2; ++dy) {
          for (uint32_t dx = 0; dx < 2; ++dx) {
            const uint32_t sx = std::min(x * 2 + dx, image.width - 1);
            const uint32_t sy = std::min(y * 2 + dy, image.height - 1);
            sum += image.texels[(size_t{sy} * image.width + sx) * 4 + c];
          }
        }
        half.texels[(size_t{y} * half.width + x) * 4 + c] =
            static_cast<uint8_t>((sum + 2) / 4);
      }
    }
  }
  return half;
}

// Returns the full mip chain of `level0` as upload bytes.
std::vector<MipBytes> MipChain(Rgba8 level0) {
  std::vector<MipBytes> mips;
  Rgba8 level = std::move(level0);
  while (true) {
    const auto* bytes = reinterpret_cast<const std::byte*>(level.texels.data());
    mips.emplace_back(bytes, bytes + level.texels.size());
    if (level.width == 1 && level.height == 1) {
      break;
    }
    level = Downsample(level);
  }
  return mips;
}

// Returns a kTextureSize square image whose texel (x, y) is `texel(x, y)`.
template <typename TexelFn>
Rgba8 Generate(const TexelFn& texel) {
  Rgba8 image{.width = kTextureSize, .height = kTextureSize};
  image.texels.resize(size_t{kTextureSize} * kTextureSize * 4);
  for (uint32_t y = 0; y < kTextureSize; ++y) {
    for (uint32_t x = 0; x < kTextureSize; ++x) {
      const glm::u8vec4 value = texel(x, y);
      uint8_t* out = &image.texels[(size_t{y} * kTextureSize + x) * 4];
      out[0] = value.r;
      out[1] = value.g;
      out[2] = value.b;
      out[3] = value.a;
    }
  }
  return image;
}

// Writes the low `count` bits of `value` at bit `position` of a BC7 block.
void PutBits(std::array<uint8_t, 16>& block, uint32_t& position,
             uint32_t value, uint32_t count) {
  for (uint32_t i = 0; i < count; ++i, ++position) {
    if ((value >> i) & 1) {
      block[position / 8] |= static_cast<uint8_t>(1u << (position % 8));
    }
  }
}

// Returns a BC7 mode 6 block of one colour: endpoint 0 is (rgb7 << 1) | 1
// per channel (alpha 255) and every index 0, so it decodes exactly to
// endpoint 0.
std::array<uint8_t, 16> SolidBc7Block(glm::u8vec3 color) {
  std::array<uint8_t, 16> block{};
  uint32_t position = 0;
  PutBits(block, position, 1u << 6, 7);  // Mode 6: six zeros, then a one.
  for (int channel = 0; channel < 3; ++channel) {
    PutBits(block, position, color[channel] >> 1, 7);  // Endpoint 0.
    PutBits(block, position, color[channel] >> 1, 7);  // Endpoint 1.
  }
  PutBits(block, position, 127, 7);  // Alpha endpoint 0.
  PutBits(block, position, 127, 7);  // Alpha endpoint 1.
  PutBits(block, position, 1, 1);    // P-bit 0.
  PutBits(block, position, 1, 1);    // P-bit 1.
  // The 63 index bits stay 0.
  return block;
}

// A BC7 texture: per 4 x 4 block one colour of a hue/brightness pattern,
// every level computed from the block's centre so the mips agree.
std::vector<MipBytes> Bc7Pattern() {
  std::vector<MipBytes> mips;
  for (uint32_t size = kTextureSize; size >= 1; size /= 2) {
    const uint32_t blocks = std::max((size + 3) / 4, 1u);
    MipBytes level;
    level.reserve(size_t{blocks} * blocks * 16);
    for (uint32_t by = 0; by < blocks; ++by) {
      for (uint32_t bx = 0; bx < blocks; ++bx) {
        const float u = (static_cast<float>(bx) + 0.5f) / blocks;
        const float v = (static_cast<float>(by) + 0.5f) / blocks;
        const glm::u8vec3 color{
            static_cast<uint8_t>(40 + 200 * u),
            static_cast<uint8_t>(40 + 200 * v),
            static_cast<uint8_t>(((bx + by) % 2 == 0) ? 220 : 60)};
        for (const uint8_t byte : SolidBc7Block(color)) {
          level.push_back(static_cast<std::byte>(byte));
        }
      }
    }
    mips.push_back(std::move(level));
    if (size == 1) {
      break;
    }
  }
  return mips;
}

// Orange and cream checker, 8 texel squares (sRGB colour).
Rgba8 Checker() {
  return Generate([](uint32_t x, uint32_t y) {
    const bool odd = ((x / 8) + (y / 8)) % 2 == 1;
    return odd ? glm::u8vec4{230, 120, 30, 255} : glm::u8vec4{235, 225, 200, 255};
  });
}

// White discs on a fully transparent background (alpha-masked materials).
Rgba8 AlphaDiscs() {
  return Generate([](uint32_t x, uint32_t y) {
    const float cx = static_cast<float>(x % 16) - 7.5f;
    const float cy = static_cast<float>(y % 16) - 7.5f;
    const bool inside = cx * cx + cy * cy < 36.0f;
    return glm::u8vec4{250, 250, 250, inside ? uint8_t{255} : uint8_t{0}};
  });
}

// A tangent-space normal map (RGB, +Y up) of sine bumps, 4 per side.
Rgba8 NormalBumps() {
  return Generate([](uint32_t x, uint32_t y) {
    constexpr float kFrequency = 2.0f * kPi / 16.0f;
    constexpr float kAmplitude = 0.6f;
    const float fx = static_cast<float>(x) + 0.5f;
    const float fy = static_cast<float>(y) + 0.5f;
    const float dhdx = kAmplitude * std::cos(fx * kFrequency) *
                       std::sin(fy * kFrequency);
    const float dhdy = kAmplitude * std::sin(fx * kFrequency) *
                       std::cos(fy * kFrequency);
    // Image rows run down while +Y (green) is up.
    const glm::vec3 n = glm::normalize(glm::vec3(-dhdx, dhdy, 1.0f));
    const auto encode = [](float value) {
      return static_cast<uint8_t>(std::lround((value * 0.5f + 0.5f) * 255.0f));
    };
    return glm::u8vec4{encode(n.x), encode(n.y), encode(n.z), 255};
  });
}

// Packed ORM: R occlusion (darker grooves), G roughness (stripes), B
// metallic (left half metal).
Rgba8 OrmPattern() {
  return Generate([](uint32_t x, uint32_t y) {
    const uint8_t occlusion = (y % 16) < 2 ? uint8_t{80} : uint8_t{255};
    const uint8_t roughness = (x / 8) % 2 == 0 ? uint8_t{50} : uint8_t{210};
    const uint8_t metallic = x < kTextureSize / 2 ? uint8_t{255} : uint8_t{0};
    return glm::u8vec4{occlusion, roughness, metallic, 255};
  });
}

// ---------------------------------------------------------------------------
// Shared GPU content.

struct Mesh {
  MeshId id{};
  asset::Bounds bounds;
};

struct Assets {
  Mesh cube;
  Mesh sphere;
  Mesh ground;  // 20 m plane
  Mesh quad;    // 1 m plane
  TextureId checker{};
  TextureId bc7{};
  TextureId alpha_discs{};
  TextureId normal_bumps{};
  TextureId orm{};
};

Mesh Upload(Renderer& renderer, const asset::MeshData& data) {
  return {.id = renderer.UploadMesh(data), .bounds = data.bounds};
}

Assets UploadAssets(Renderer& renderer) {
  Assets assets;
  assets.cube = Upload(renderer, asset::MakeCube());
  assets.sphere = Upload(renderer, asset::MakeSphere(48));
  assets.ground = Upload(renderer, asset::MakePlane(20.0f));
  assets.quad = Upload(renderer, asset::MakePlane(1.0f));

  const std::vector<TextureSource> sources = TestTextures();

  std::vector<std::vector<std::span<const std::byte>>> spans(sources.size());
  std::vector<TextureUpload> uploads;
  for (size_t i = 0; i < sources.size(); ++i) {
    for (const MipBytes& mip : sources[i].mips) {
      spans[i].emplace_back(mip);
    }
    uploads.push_back({.format = sources[i].format,
                       .width = kTextureSize,
                       .height = kTextureSize,
                       .mips = spans[i],
                       .name = sources[i].name});
  }
  const std::vector<TextureId> ids = renderer.UploadTextures(uploads);
  if (ids.size() != uploads.size()) {
    core::log::Fatal("Texture upload returned {} ids for {} textures",
                     ids.size(), uploads.size());
  }
  assets.checker = ids[0];
  assets.bc7 = ids[1];
  assets.alpha_discs = ids[2];
  assets.normal_bumps = ids[3];
  assets.orm = ids[4];
  return assets;
}

// An untextured material.
MaterialId Plain(Renderer& renderer, glm::vec3 color, float roughness,
                 float metallic = 0.0f) {
  material::MaterialDesc desc;
  desc.base_color = glm::vec4(color, 1.0f);
  desc.roughness = roughness;
  desc.metallic = metallic;
  return renderer.CreateMaterial(desc);
}

// A material that shows `texture` unlit: black base, the texture as
// emissive; with `masked`, its alpha also cuts the surface out.
MaterialId ShowTexture(Renderer& renderer, TextureId texture, bool masked) {
  material::MaterialDesc desc;
  desc.base_color = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
  desc.roughness = 1.0f;
  desc.emissive = glm::vec3(1.0f);
  if (masked) {
    desc.alpha_mode = material::AlphaMode::kMask;
    return renderer.CreateMaterial(
        desc, {.base_color = texture, .emissive = texture});
  }
  return renderer.CreateMaterial(desc, {.emissive = texture});
}

// The exposure of the lit scenes: the default sun (intensity 3) on light
// surfaces would clip at 0 EV.
constexpr float kLitExposureEv = -1.0f;

DrawItem Item(const Mesh& mesh, const glm::mat4& transform,
              MaterialId material) {
  return {.mesh = mesh.id,
          .transform = transform,
          .bounds = mesh.bounds,
          .material_override = material};
}

glm::mat4 At(glm::vec3 position, float scale = 1.0f) {
  return glm::scale(glm::translate(glm::mat4(1.0f), position), glm::vec3(scale));
}

// The camera at `eye` looking at `target`, vertical field `fov_degrees`.
render::Camera LookAt(glm::vec3 eye, glm::vec3 target,
                      float fov_degrees = 60.0f) {
  render::Camera camera;
  camera.position = eye;
  camera.view = glm::lookAt(eye, target, glm::vec3(0.0f, 1.0f, 0.0f));
  camera.vertical_fov_radians = glm::radians(fov_degrees);
  camera.near_plane = 0.05f;
  camera.projection = camera::ReversedInfinitePerspective(
      camera.vertical_fov_radians,
      static_cast<float>(kSceneWidth) / static_cast<float>(kSceneHeight),
      camera.near_plane);
  camera.previous_view_projection = camera.projection * camera.view;
  return camera;
}

// Post settings with every optional effect off (exact image of the scene).
render::PostSettings PlainPost() {
  render::PostSettings post;
  post.bloom_enabled = false;
  post.ssao_enabled = false;
  post.fxaa_enabled = false;
  post.weapon_motion_blur_enabled = false;
  return post;
}

// A 64 x 32 equirectangular sky: blue gradient above the horizon, brown
// ground below, a small bright sun.
asset::ImageDataF GeneratedSky() {
  asset::ImageDataF sky{.width = 64, .height = 32};
  sky.pixels.resize(size_t{64} * 32 * 3);
  for (int y = 0; y < sky.height; ++y) {
    const float elevation = 0.5f - (static_cast<float>(y) + 0.5f) / 32.0f;
    for (int x = 0; x < sky.width; ++x) {
      glm::vec3 color = elevation > 0.0f
                            ? glm::mix(glm::vec3(0.6f, 0.7f, 0.85f),
                                       glm::vec3(0.15f, 0.3f, 0.75f),
                                       elevation * 2.0f)
                            : glm::vec3(0.18f, 0.14f, 0.1f);
      if ((x == 20 || x == 21) && (y == 9 || y == 10)) {
        color = glm::vec3(40.0f, 36.0f, 30.0f);
      }
      float* out = &sky.pixels[(size_t{static_cast<uint32_t>(y)} * 64 +
                                static_cast<uint32_t>(x)) * 3];
      out[0] = color.r;
      out[1] = color.g;
      out[2] = color.b;
    }
  }
  return sky;
}

// Points the scene's view at its storage.
void BindStorage(TestScene& scene) {
  scene.view.draws = scene.draws;
  scene.view.point_lights = scene.point_lights;
  scene.view.lines = scene.lines.tested();
  scene.view.overlay_lines = scene.lines.on_top();
  scene.view.selected_draws = scene.selected;
  scene.view.frame_seconds = 1.0f / 60.0f;
}

// ---------------------------------------------------------------------------
// Scenes.

// Every uploaded texture shown unlit on a quad facing the camera, over the
// default sky. No post effects, clamp tonemapper: checks uploads (RGBA8
// sRGB/UNORM with mips, BC7) and the clear.
void ClearAndUpload(TestScene& scene, Renderer& renderer) {
  const Assets assets = UploadAssets(renderer);
  const std::array textures = {assets.checker, assets.bc7, assets.alpha_discs,
                               assets.normal_bumps, assets.orm};
  const glm::mat4 face_camera =
      glm::rotate(glm::mat4(1.0f), kPi / 2.0f, glm::vec3(1.0f, 0.0f, 0.0f));
  for (size_t i = 0; i < textures.size(); ++i) {
    const float x = (static_cast<float>(i) - 2.0f) * 1.2f;
    scene.draws.push_back(
        Item(assets.quad, At(glm::vec3(x, 0.0f, 0.0f)) * face_camera,
             ShowTexture(renderer, textures[i],
                         /*masked=*/textures[i] == assets.alpha_discs)));
  }
  scene.view.camera = LookAt({0.0f, 0.0f, 4.0f}, {0.0f, 0.0f, 0.0f});
  scene.view.shadows_enabled = false;
  scene.view.post = PlainPost();
  scene.view.post.tonemapper = render::Tonemapper::kClamp;
}

// Ground, a red cube and a metal sphere under the sun; no shadows, SSAO or
// bloom; FXAA, ACES.
void LitBasic(TestScene& scene, Renderer& renderer) {
  const Assets assets = UploadAssets(renderer);
  scene.draws.push_back(Item(assets.ground, At({0.0f, 0.0f, 0.0f}),
                             Plain(renderer, glm::vec3(0.5f), 0.8f)));
  scene.draws.push_back(Item(assets.cube, At({-1.0f, 0.5f, 0.0f}),
                             Plain(renderer, {0.8f, 0.1f, 0.1f}, 0.5f)));
  scene.draws.push_back(Item(assets.sphere, At({1.0f, 0.5f, 0.0f}, 1.0f),
                             Plain(renderer, {0.9f, 0.9f, 0.9f}, 0.3f, 1.0f)));
  scene.view.camera = LookAt({0.0f, 1.6f, 4.0f}, {0.0f, 0.5f, 0.0f});
  scene.view.shadows_enabled = false;
  scene.view.post = PlainPost();
  scene.view.post.fxaa_enabled = true;
  scene.view.post.exposure_ev = kLitExposureEv;
}

// Everything lit: shadows, SSAO, bloom, two point lights, a generated
// environment, textured and normal-mapped objects.
void LitFull(TestScene& scene, Renderer& renderer, render::Tonemapper mapper) {
  const Assets assets = UploadAssets(renderer);
  renderer.SetEnvironment(GeneratedSky(), 1.0f, 0.5f);

  material::MaterialDesc checker;
  checker.roughness = 0.7f;
  checker.uv_scale = glm::vec2(0.5f);
  material::MaterialDesc bumpy;
  bumpy.base_color = glm::vec4(0.3f, 0.55f, 0.8f, 1.0f);
  bumpy.roughness = 0.35f;
  material::MaterialDesc orm;
  orm.base_color = glm::vec4(0.9f, 0.75f, 0.5f, 1.0f);
  orm.occlusion_in_metallic_roughness = true;
  orm.roughness = 1.0f;
  orm.metallic = 1.0f;

  scene.draws.push_back(Item(assets.ground, At({0.0f, 0.0f, 0.0f}),
                             renderer.CreateMaterial(
                                 checker, {.base_color = assets.checker})));
  scene.draws.push_back(
      Item(assets.cube,
           At({-1.6f, 0.5f, 0.2f}) *
               glm::rotate(glm::mat4(1.0f), 0.6f, glm::vec3(0.0f, 1.0f, 0.0f)),
           renderer.CreateMaterial(checker, {.base_color = assets.bc7})));
  scene.draws.push_back(Item(
      assets.sphere, At({0.0f, 0.6f, 0.0f}, 1.2f),
      renderer.CreateMaterial(bumpy, {.normal = assets.normal_bumps})));
  scene.draws.push_back(
      Item(assets.sphere, At({1.6f, 0.5f, 0.3f}),
           renderer.CreateMaterial(orm, {.metallic_roughness = assets.orm})));
  scene.draws.push_back(Item(assets.cube, At({0.6f, 0.25f, 1.3f}, 0.5f),
                             Plain(renderer, {0.9f, 0.9f, 0.9f}, 0.9f)));
  scene.point_lights.push_back({.position = {-0.8f, 1.2f, 1.2f},
                                .color = {1.0f, 0.6f, 0.3f},
                                .intensity = 2.0f,
                                .range = 6.0f});
  scene.point_lights.push_back({.position = {1.2f, 0.8f, 1.5f},
                                .color = {0.3f, 0.6f, 1.0f},
                                .intensity = 1.5f,
                                .range = 5.0f});
  scene.view.camera = LookAt({0.5f, 2.0f, 4.5f}, {0.0f, 0.4f, 0.0f});
  scene.view.post.bloom_strength = 0.08f;
  scene.view.post.exposure_ev = kLitExposureEv;
  scene.view.post.tonemapper = mapper;
  scene.view.post.weapon_motion_blur_enabled = false;
}

// Material features side by side: alpha mask, double-sided back face,
// emissive, normal map, ORM, BC7, a metal/roughness ramp.
void Materials(TestScene& scene, Renderer& renderer) {
  const Assets assets = UploadAssets(renderer);
  scene.draws.push_back(Item(assets.ground, At({0.0f, 0.0f, 0.0f}),
                             Plain(renderer, glm::vec3(0.35f), 0.9f)));

  material::MaterialDesc masked;
  masked.alpha_mode = material::AlphaMode::kMask;
  masked.alpha_cutoff = 0.5f;
  masked.double_sided = true;
  masked.base_color = glm::vec4(0.2f, 0.8f, 0.3f, 1.0f);
  scene.draws.push_back(Item(
      assets.quad,
      At({-2.2f, 0.7f, 0.0f}, 1.2f) *
          glm::rotate(glm::mat4(1.0f), kPi / 2.0f, glm::vec3(1.0f, 0.0f, 0.0f)),
      renderer.CreateMaterial(masked, {.base_color = assets.alpha_discs})));

  // Seen from behind (its normal faces away from the camera).
  material::MaterialDesc two_sided;
  two_sided.double_sided = true;
  two_sided.base_color = glm::vec4(0.9f, 0.85f, 0.2f, 1.0f);
  scene.draws.push_back(Item(
      assets.quad,
      At({-0.9f, 0.6f, 0.0f}) *
          glm::rotate(glm::mat4(1.0f), -kPi / 2.0f, glm::vec3(1.0f, 0.0f, 0.0f)),
      renderer.CreateMaterial(two_sided)));

  material::MaterialDesc emissive;
  emissive.base_color = glm::vec4(0.05f, 0.05f, 0.05f, 1.0f);
  emissive.emissive = glm::vec3(1.0f, 0.4f, 0.1f);
  emissive.emissive_strength = 4.0f;
  scene.draws.push_back(Item(assets.sphere, At({0.3f, 0.5f, 0.0f}, 0.8f),
                             renderer.CreateMaterial(emissive)));

  material::MaterialDesc bumpy;
  bumpy.base_color = glm::vec4(0.7f, 0.7f, 0.75f, 1.0f);
  bumpy.roughness = 0.4f;
  scene.draws.push_back(Item(
      assets.cube,
      At({1.4f, 0.5f, 0.0f}, 0.9f) *
          glm::rotate(glm::mat4(1.0f), 0.5f, glm::vec3(0.0f, 1.0f, 0.0f)),
      renderer.CreateMaterial(bumpy, {.normal = assets.normal_bumps})));

  material::MaterialDesc orm;
  orm.occlusion_in_metallic_roughness = true;
  orm.roughness = 1.0f;
  orm.metallic = 1.0f;
  orm.base_color = glm::vec4(0.95f, 0.6f, 0.4f, 1.0f);
  scene.draws.push_back(
      Item(assets.sphere, At({2.6f, 0.5f, 0.0f}, 0.9f),
           renderer.CreateMaterial(orm, {.metallic_roughness = assets.orm})));

  material::MaterialDesc bc7;
  scene.draws.push_back(
      Item(assets.cube, At({-2.6f, 0.3f, 1.4f}, 0.6f),
           renderer.CreateMaterial(bc7, {.base_color = assets.bc7})));

  for (int i = 0; i < 5; ++i) {
    const float t = static_cast<float>(i) / 4.0f;
    scene.draws.push_back(Item(
        assets.sphere, At({-1.2f + 0.6f * static_cast<float>(i), 0.2f, 1.6f},
                          0.4f),
        Plain(renderer, {0.95f, 0.8f, 0.5f}, 0.1f + 0.85f * t, 1.0f - t)));
  }
  scene.view.camera = LookAt({0.0f, 1.8f, 5.0f}, {0.0f, 0.5f, 0.0f});
  scene.view.post.bloom_strength = 0.06f;
  scene.view.post.exposure_ev = kLitExposureEv;
  scene.view.post.weapon_motion_blur_enabled = false;
}

// A box "weapon" held in front of the camera, moved since the previous frame
// (previous_transform): weapon motion blur over a lit scene.
void WeaponMotion(TestScene& scene, Renderer& renderer) {
  const Assets assets = UploadAssets(renderer);
  scene.draws.push_back(Item(assets.ground, At({0.0f, 0.0f, 0.0f}),
                             Plain(renderer, glm::vec3(0.45f), 0.8f)));
  scene.draws.push_back(Item(assets.cube, At({0.0f, 0.5f, -3.0f}),
                             Plain(renderer, {0.2f, 0.4f, 0.8f}, 0.5f)));
  scene.view.camera = LookAt({0.0f, 1.6f, 2.0f}, {0.0f, 1.2f, -3.0f});

  const glm::mat4 camera_to_world = glm::inverse(scene.view.camera.view);
  const MaterialId body = Plain(renderer, {0.12f, 0.12f, 0.13f}, 0.4f, 0.8f);
  const MaterialId accent = Plain(renderer, {0.7f, 0.5f, 0.2f}, 0.3f, 1.0f);
  // Camera space: right, down, forward (-z). The weapon swung to the left
  // and down since the previous frame.
  const glm::mat4 swing =
      glm::translate(glm::mat4(1.0f), glm::vec3(-0.04f, -0.015f, 0.0f)) *
      glm::rotate(glm::mat4(1.0f), 0.08f, glm::vec3(0.0f, 1.0f, 0.0f));
  const std::array<std::pair<glm::mat4, MaterialId>, 3> parts = {{
      {glm::translate(glm::mat4(1.0f), {0.18f, -0.16f, -0.45f}) *
           glm::scale(glm::mat4(1.0f), {0.06f, 0.08f, 0.40f}),
       body},
      {glm::translate(glm::mat4(1.0f), {0.18f, -0.25f, -0.33f}) *
           glm::scale(glm::mat4(1.0f), {0.05f, 0.14f, 0.06f}),
       body},
      {glm::translate(glm::mat4(1.0f), {0.18f, -0.11f, -0.40f}) *
           glm::scale(glm::mat4(1.0f), {0.03f, 0.03f, 0.20f}),
       accent},
  }};
  for (const auto& [local, material] : parts) {
    DrawItem item = Item(assets.cube, camera_to_world * local, material);
    item.previous_transform = camera_to_world * swing * local;
    item.hidden_in_scope = true;
    scene.draws.push_back(item);
  }
  scene.view.post.weapon_motion_blur_enabled = true;
  scene.view.post.weapon_motion_blur_strength = 1.0f;
  scene.view.post.exposure_ev = kLitExposureEv;
}

// A magnified scope picture (with reticle) and a reflex sight over a
// downrange scene.
void Scope(TestScene& scene, Renderer& renderer) {
  const Assets assets = UploadAssets(renderer);
  scene.draws.push_back(Item(assets.ground, At({0.0f, 0.0f, -8.0f}),
                             Plain(renderer, glm::vec3(0.4f), 0.85f)));
  for (int i = 0; i < 5; ++i) {
    const float z = -12.0f - 6.0f * static_cast<float>(i);
    scene.draws.push_back(Item(
        assets.cube,
        At({0.8f * static_cast<float>(i % 3 - 1), 0.75f, z}, 1.5f),
        Plain(renderer,
              {0.2f + 0.15f * static_cast<float>(i), 0.3f,
               0.8f - 0.12f * static_cast<float>(i)},
              0.5f)));
  }
  const glm::vec3 eye{0.0f, 1.2f, 2.0f};
  scene.view.camera = LookAt(eye, {0.0f, 1.0f, -20.0f});
  const glm::mat4 camera_to_world = glm::inverse(scene.view.camera.view);
  const glm::vec3 right = glm::vec3(camera_to_world[0]);
  const glm::vec3 up = glm::vec3(camera_to_world[1]);
  const glm::vec3 axis = -glm::vec3(camera_to_world[2]);

  render::ScopeSight scope;
  scope.position = eye + axis * 0.3f;
  scope.view = glm::lookAt(scope.position, scope.position + axis, up);
  scope.camera_tangent = 0.04f;
  scope.resolution = 256;
  scope.lens = camera_to_world *
               glm::translate(glm::mat4(1.0f), {0.0f, 0.0f, -0.12f}) *
               glm::scale(glm::mat4(1.0f), {0.02f, 0.02f, 1.0f});
  scope.right = right;
  scope.up = up;
  scope.axis = axis;
  scope.magnification = 4.0f;
  scope.apparent_field = 0.16f;
  scope.vignetting = 0.3f;
  scope.reticle_lines[0] = {-0.2f, 0.0f, 0.2f, 0.0f};
  scope.reticle_lines[1] = {0.0f, -0.2f, 0.0f, 0.2f};
  scope.reticle_half_widths[0] = 0.0006f;
  scope.reticle_half_widths[1] = 0.0006f;
  scope.reticle_line_count = 2;
  scope.reticle_color = glm::vec3(0.0f);
  scene.view.scope = scope;

  // Off to the lower left, its axis through the lens centre so the dot shows
  // in the middle of the window.
  const glm::vec3 sight_center{-0.09f, -0.04f, -0.2f};
  render::ReticleSight sight;
  sight.lens = camera_to_world * glm::translate(glm::mat4(1.0f), sight_center) *
               glm::scale(glm::mat4(1.0f), {0.018f, 0.018f, 1.0f});
  sight.axis = glm::normalize(glm::mat3(camera_to_world) * sight_center);
  sight.min_dot_pixels = 3.0f;
  scene.view.sight = sight;
  scene.view.post.weapon_motion_blur_enabled = false;
  scene.view.post.exposure_ev = kLitExposureEv;
}

// The editor path: the scene into the viewport image with a grid, boxes and
// axes (depth-tested and on top) and a selection outline.
void EditorOverlay(TestScene& scene, Renderer& renderer) {
  LitBasic(scene, renderer);
  scene.viewport_output = true;
  scene.lines.AddGrid({0.0f, 0.001f, 0.0f}, 5.0f, 0.5f,
                      {0.6f, 0.6f, 0.6f, 0.5f}, 4, {0.9f, 0.9f, 0.9f, 0.8f});
  scene.lines.AddBox(scene.draws[1].bounds, scene.draws[1].transform,
                     {1.0f, 1.0f, 0.0f, 1.0f}, render::LineDepth::kAlwaysOnTop);
  scene.lines.AddAxes(At({0.0f, 0.01f, 1.0f}), 0.8f);
  scene.lines.AddSphere({1.0f, 0.5f, 0.0f}, 0.7f, {0.3f, 0.8f, 1.0f, 1.0f});
  scene.selected.push_back(2);
}

}  // namespace

std::vector<TextureSource> TestTextures() {
  std::vector<TextureSource> sources;
  sources.push_back({TextureFormat::kRgba8Srgb, MipChain(Checker()), "checker"});
  sources.push_back({TextureFormat::kBc7Srgb, Bc7Pattern(), "bc7"});
  sources.push_back(
      {TextureFormat::kRgba8Srgb, MipChain(AlphaDiscs()), "alpha_discs"});
  sources.push_back(
      {TextureFormat::kRgba8Unorm, MipChain(NormalBumps()), "normal_bumps"});
  sources.push_back({TextureFormat::kRgba8Unorm, MipChain(OrmPattern()), "orm"});
  return sources;
}

std::span<const std::string_view> SceneNames() { return kSceneNames; }

std::unique_ptr<TestScene> BuildScene(std::string_view name,
                                      Renderer& renderer) {
  auto scene = std::make_unique<TestScene>();
  if (name == "clear_and_upload") {
    ClearAndUpload(*scene, renderer);
  } else if (name == "lit_basic") {
    LitBasic(*scene, renderer);
  } else if (name == "lit_full_aces") {
    LitFull(*scene, renderer, render::Tonemapper::kAcesFitted);
  } else if (name == "lit_full_neutral") {
    LitFull(*scene, renderer, render::Tonemapper::kPbrNeutral);
  } else if (name == "lit_full_clamp") {
    LitFull(*scene, renderer, render::Tonemapper::kClamp);
  } else if (name == "materials") {
    Materials(*scene, renderer);
  } else if (name == "weapon_motion") {
    WeaponMotion(*scene, renderer);
  } else if (name == "scope") {
    Scope(*scene, renderer);
  } else if (name == "editor_overlay") {
    EditorOverlay(*scene, renderer);
  } else {
    core::log::Fatal("Unknown render test scene {}", name);
  }
  BindStorage(*scene);
  return scene;
}

}  // namespace blue::render_test
