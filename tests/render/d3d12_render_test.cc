// Direct3D 12 backend tests that need a device (its internals): uploads through
// the backend's staging path read back exactly ("clear_and_upload"), the
// frame's output and viewport paths, readbacks, footprints against the device,
// shader hot reload to DXIL. Run on the hardware adapter and on WARP
// (--adapter=software); --gpu-validation adds the debug layer with GPU-based
// validation, whose errors fail the tests.

#include <d3d12.h>
#include <doctest/doctest.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/vec4.hpp>

#include "asset/mesh_data.h"
#include "core/extent.h"
#include "material/material.h"
#include "platform/file_system.h"
#include "render/backend.h"
#include "render/d3d12/buffer.h"
#include "render/d3d12/d3d12_context.h"
#include "render/d3d12/d3d12_renderer.h"
#include "render/d3d12/format.h"
#include "render/d3d12/fullscreen_pass.h"
#include "render/d3d12/image.h"
#include "render/d3d12/shader_programs.h"
#include "render/renderer.h"
#include "render/renderer_backend.h"
#include "render/scene_view.h"
#include "render/shader_manifest.h"
#include "render/texture_types.h"
#include "shader/compile_mode.h"
#include "tests/render/harness_options.h"
#include "tests/render/image_compare.h"
#include "tests/render/png_io.h"
#include "tests/render/scenes.h"
#include "tests/scratch_directory.h"

namespace blue::render_test {
namespace {

using render::d3d12::Buffer;
using render::d3d12::D3d12Context;
using render::d3d12::D3d12Renderer;
using render::d3d12::FullscreenPass;
using render::d3d12::Image;

bool Testing() {
  return std::ranges::find(Options().backends, render::Backend::kD3d12) !=
         Options().backends.end();
}

render::RendererOptions HeadlessOptions() {
  return {
      .window = nullptr,
      .backend = render::Backend::kD3d12,
      .shader_dir = BLUE_SHADER_BLOB_DIR,
      .adapter = Options().adapter,
      .headless_extent = {kSceneWidth, kSceneHeight},
      .gpu_validation = Options().gpu_validation,
      .max_anisotropy = 1.0f,
  };
}

// A Direct3D 12 renderer, for its internals.
struct TestRenderer {
  std::unique_ptr<render::RendererBackend> backend;
  D3d12Renderer* d3d12 = nullptr;
};

TestRenderer MakeD3d12Renderer(const render::RendererOptions& options) {
  TestRenderer renderer;
  renderer.backend = render::CreateD3d12Renderer(options);
  REQUIRE_MESSAGE(renderer.backend != nullptr,
                  "no Direct3D 12 adapter for this --adapter");
  REQUIRE(renderer.backend->backend() == render::Backend::kD3d12);
  renderer.d3d12 = static_cast<D3d12Renderer*>(renderer.backend.get());
  INFO("adapter ", renderer.d3d12->device_name());
  return renderer;
}

// Requires that the debug layer reported no error (a no-op without it).
void CheckValidation(const render::RendererBackend& renderer) {
  const render::ValidationStats stats = renderer.validation_stats();
  CHECK_MESSAGE(stats.errors == 0, "D3D12 debug layer errors: ", stats.errors);
  std::printf("  d3d12 debug layer %s: %u errors, %u warnings\n",
              stats.enabled ? "on" : "off", stats.errors, stats.warnings);
}

// The 8-bit texels of mip `level` of `source`, as RGBA rows top to bottom,
// as a sampler returns them (BC7: decoded).
std::vector<uint8_t> DecodedTexels(const TextureSource& source,
                                   uint32_t level) {
  const uint32_t size = std::max(kTextureSize >> level, 1u);
  const std::vector<std::byte>& bytes = source.mips[level];
  std::vector<uint8_t> texels(size_t{size} * size * 4);
  if (source.format != render::TextureFormat::kBc7Srgb) {
    std::memcpy(texels.data(), bytes.data(), texels.size());
    return texels;
  }
  // The scenes' BC7 blocks are mode 6 with every index 0: each texel of a
  // block is endpoint 0, (7 bits << 1) | p-bit 0 per channel.
  const uint32_t blocks = std::max((size + 3) / 4, 1u);
  for (uint32_t by = 0; by < blocks; ++by) {
    for (uint32_t bx = 0; bx < blocks; ++bx) {
      const auto* block = reinterpret_cast<const uint8_t*>(
          bytes.data() + (size_t{by} * blocks + bx) * 16);
      uint32_t position = 0;
      const auto bits = [&](uint32_t count) {
        uint32_t value = 0;
        for (uint32_t i = 0; i < count; ++i, ++position) {
          value |= ((block[position / 8] >> (position % 8)) & 1u) << i;
        }
        return value;
      };
      REQUIRE(bits(7) == 1u << 6);  // Mode 6.
      std::array<uint32_t, 4> endpoint0{};
      for (uint32_t channel = 0; channel < 4; ++channel) {
        endpoint0[channel] = bits(7);
        bits(7);  // Endpoint 1.
      }
      const uint32_t p_bit = bits(1);
      for (uint32_t y = by * 4; y < std::min(by * 4 + 4, size); ++y) {
        for (uint32_t x = bx * 4; x < std::min(bx * 4 + 4, size); ++x) {
          for (uint32_t channel = 0; channel < 4; ++channel) {
            texels[(size_t{y} * size + x) * 4 + channel] =
                static_cast<uint8_t>((endpoint0[channel] << 1) | p_bit);
          }
        }
      }
    }
  }
  return texels;
}

bool IsSrgb(render::TextureFormat format) {
  return format == render::TextureFormat::kBc7Srgb ||
         format == render::TextureFormat::kRgba8Srgb;
}

// Must match GridConstants in shaders/test_texture_grid.slang.
struct GridConstants {
  int32_t origin[2] = {0, 0};
  int32_t mip = 0;
  int32_t padding = 0;
};

constexpr uint32_t kTileGap = 4;
constexpr std::array<uint8_t, 4> kGridClear = {64, 128, 192, 255};

// The pixel of the grid where mip `level` of texture `index` starts: one row
// of mips per texture.
std::array<uint32_t, 2> TileOrigin(size_t index, uint32_t level) {
  uint32_t x = 0;
  for (uint32_t l = 0; l < level; ++l) {
    x += std::max(kTextureSize >> l, 1u) + kTileGap;
  }
  return {x, static_cast<uint32_t>(index) * (kTextureSize + kTileGap)};
}

// Reads `resource` (in COPY_SOURCE, R8G8B8A8, `extent`) back to the CPU.
RgbaImage ReadBack(const D3d12Context& context, ID3D12Resource* resource,
                   core::Extent2D extent) {
  const uint32_t row_pitch = static_cast<uint32_t>(render::d3d12::AlignUp(
      uint64_t{extent.width} * 4, render::d3d12::kRowPitchAlignment));
  const Buffer buffer(context, uint64_t{row_pitch} * extent.height,
                      Buffer::Access::kReadback, L"test readback");
  context.SubmitImmediate([&](ID3D12GraphicsCommandList* cmd) {
    const D3D12_TEXTURE_COPY_LOCATION destination{
        .pResource = buffer.resource(),
        .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
        .PlacedFootprint = {.Offset = 0,
                            .Footprint = {.Format = DXGI_FORMAT_R8G8B8A8_UNORM,
                                          .Width = extent.width,
                                          .Height = extent.height,
                                          .Depth = 1,
                                          .RowPitch = row_pitch}},
    };
    const D3D12_TEXTURE_COPY_LOCATION source{
        .pResource = resource,
        .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
        .SubresourceIndex = 0,
    };
    cmd->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
  });
  RgbaImage image;
  image.width = extent.width;
  image.height = extent.height;
  image.rgba.resize(size_t{extent.width} * extent.height * 4);
  const auto* mapped = static_cast<const uint8_t*>(buffer.mapped());
  for (uint32_t row = 0; row < extent.height; ++row) {
    std::memcpy(image.rgba.data() + size_t{row} * extent.width * 4,
                mapped + size_t{row} * row_pitch, size_t{extent.width} * 4);
  }
  return image;
}

std::string AdapterName() {
  return Options().adapter == render::AdapterPreference::kSoftware ? "warp"
                                                                   : "hw";
}

TEST_CASE("clear_and_upload: textures uploaded by the d3d12 backend read back "
          "exactly") {
  if (!Testing()) {
    return;
  }
  TestRenderer renderer = MakeD3d12Renderer(HeadlessOptions());
  const D3d12Context& context = renderer.d3d12->context();

  // The scenes' textures through the backend's real upload path.
  const std::vector<TextureSource> sources = TestTextures();
  std::vector<std::vector<std::span<const std::byte>>> spans(sources.size());
  std::vector<render::TextureUpload> uploads;
  for (size_t i = 0; i < sources.size(); ++i) {
    for (const std::vector<std::byte>& mip : sources[i].mips) {
      spans[i].emplace_back(mip);
    }
    uploads.push_back({.format = sources[i].format,
                       .width = kTextureSize,
                       .height = kTextureSize,
                       .mips = spans[i],
                       .name = sources[i].name});
  }
  const std::vector<render::TextureId> ids =
      renderer.backend->UploadTextures(uploads);
  REQUIRE(ids.size() == sources.size());
  CHECK(renderer.backend->texture_stats().count == sources.size());

  // Every mip of every texture, texel for texel, into one target through
  // the test program: _SRGB views for sRGB textures (decode + encode is
  // exact for 8-bit values), UNORM ones for the others.
  const auto levels = static_cast<uint32_t>(sources.front().mips.size());
  const std::array<uint32_t, 2> far_corner =
      TileOrigin(sources.size() - 1, levels);
  const core::Extent2D extent{far_corner[0], far_corner[1] + kTextureSize};
  const std::array<float, 4> clear = {
      kGridClear[0] / 255.0f, kGridClear[1] / 255.0f, kGridClear[2] / 255.0f,
      kGridClear[3] / 255.0f};
  D3D12_CLEAR_VALUE clear_value{.Format = DXGI_FORMAT_R8G8B8A8_UNORM};
  std::copy(clear.begin(), clear.end(), clear_value.Color);
  Image target(context,
               {.extent = extent,
                .format = DXGI_FORMAT_R8G8B8A8_TYPELESS,
                .flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                .initial_state = D3D12_RESOURCE_STATE_RENDER_TARGET,
                .clear_value = clear_value,
                .name = L"test grid"});
  const size_t unorm_rtv = target.AddRtv(DXGI_FORMAT_R8G8B8A8_UNORM);
  const size_t srgb_rtv = target.AddRtv(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
  render::d3d12::ShaderPrograms programs(
      context, std::string(BLUE_SHADER_BLOB_DIR) + "/dxil", "",
      shader::CompileMode::kRelease);
  const FullscreenPass unorm_pass(context, programs,
                                  renderer.d3d12->fullscreen_root_signature(),
                                  render::ShaderProgram::kTestTextureGrid,
                                  DXGI_FORMAT_R8G8B8A8_UNORM);
  const FullscreenPass srgb_pass(context, programs,
                                 renderer.d3d12->fullscreen_root_signature(),
                                 render::ShaderProgram::kTestTextureGrid,
                                 DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);

  context.SubmitImmediate([&](ID3D12GraphicsCommandList* cmd) {
    cmd->ClearRenderTargetView(target.rtv(unorm_rtv), clear.data(), 0, nullptr);
    for (size_t i = 0; i < sources.size(); ++i) {
      const bool srgb = IsSrgb(sources[i].format);
      const FullscreenPass& pass = srgb ? srgb_pass : unorm_pass;
      const std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 1> inputs = {
          renderer.d3d12->textures().image(ids[i]).staging_srv()};
      for (uint32_t level = 0; level < levels; ++level) {
        const std::array<uint32_t, 2> origin = TileOrigin(i, level);
        const uint32_t size = std::max(kTextureSize >> level, 1u);
        const GridConstants constants{
            .origin = {static_cast<int32_t>(origin[0]),
                       static_cast<int32_t>(origin[1])},
            .mip = static_cast<int32_t>(level)};
        // The triangle covers the whole target; the scissor limits it to
        // the tile.
        const D3D12_RECT tile{static_cast<LONG>(origin[0]),
                              static_cast<LONG>(origin[1]),
                              static_cast<LONG>(origin[0] + size),
                              static_cast<LONG>(origin[1] + size)};
        pass.Record(cmd, target.rtv(srgb ? srgb_rtv : unorm_rtv), extent,
                    tile, inputs, renderer.d3d12->samplers().nearest_clamp(),
                    std::as_bytes(std::span(&constants, 1)));
      }
    }
    render::d3d12::BarrierBatch barriers(cmd);
    target.Transition(barriers, D3D12_RESOURCE_STATE_COPY_SOURCE);
  });
  const RgbaImage result = ReadBack(context, target.resource(), extent);

  RgbaImage expected;
  expected.width = extent.width;
  expected.height = extent.height;
  expected.rgba.resize(result.rgba.size());
  for (size_t i = 0; i < expected.rgba.size(); i += 4) {
    std::copy(kGridClear.begin(), kGridClear.end(), expected.rgba.begin() + i);
  }
  for (size_t i = 0; i < sources.size(); ++i) {
    for (uint32_t level = 0; level < levels; ++level) {
      const std::vector<uint8_t> texels = DecodedTexels(sources[i], level);
      const std::array<uint32_t, 2> origin = TileOrigin(i, level);
      const uint32_t size = std::max(kTextureSize >> level, 1u);
      for (uint32_t y = 0; y < size; ++y) {
        std::memcpy(
            expected.rgba.data() +
                ((size_t{origin[1]} + y) * extent.width + origin[0]) * 4,
            texels.data() + size_t{y} * size * 4, size_t{size} * 4);
      }
    }
  }
  const CompareMetrics metrics = Compare(result, expected, 0);
  std::printf("clear_and_upload   d3d12 (%s)  %s  %s\n", AdapterName().c_str(),
              Passes(metrics, kExact) ? "ok  " : "FAIL",
              Describe(metrics).c_str());
  CHECK(WritePng(Options().out_dir + "/clear_and_upload.d3d12." +
                     AdapterName() + ".png",
                 result));
  if (!Passes(metrics, kExact)) {
    CHECK(WritePng(Options().out_dir + "/clear_and_upload.d3d12.expected.png",
                   expected));
    CHECK(WritePng(Options().out_dir + "/clear_and_upload.d3d12.diff.png",
                   AmplifiedDiff(result, expected, 16)));
  }
  CHECK_MESSAGE(Passes(metrics, kExact), Describe(metrics));

  // Released textures leave the statistics at once.
  renderer.backend->ReleaseTextures(ids);
  CHECK(renderer.backend->texture_stats().count == 0);
  renderer.backend->FlushReleases();
  CheckValidation(*renderer.backend);
}

TEST_CASE("d3d12 footprints match the device's GetCopyableFootprints") {
  if (!Testing()) {
    return;
  }
  const D3d12Context context(Options().adapter, Options().gpu_validation);
  REQUIRE(context.has_device());
  constexpr std::array kFormats = {
      render::TextureFormat::kBc7Srgb, render::TextureFormat::kBc5Unorm,
      render::TextureFormat::kRgba8Unorm};
  constexpr std::array<core::Extent2D, 4> kExtents = {
      {{64, 64}, {100, 37}, {640, 360}, {3, 1}}};
  for (const render::TextureFormat format : kFormats) {
    for (const core::Extent2D extent : kExtents) {
      const bool block = render::d3d12::CopyBlockOf(format).extent > 1;
      // BC textures need dimensions that are multiples of 4 at mip 0.
      if (block && (extent.width % 4 != 0 || extent.height % 4 != 0)) {
        continue;
      }
      const uint32_t levels = render::MipLevelCount(extent);
      const D3D12_RESOURCE_DESC desc{
          .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
          .Width = extent.width,
          .Height = extent.height,
          .DepthOrArraySize = 1,
          .MipLevels = static_cast<UINT16>(levels),
          .Format = render::d3d12::ToDxgiFormat(format),
          .SampleDesc = {1, 0},
          .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
      };
      for (uint32_t level = 0; level < levels; ++level) {
        CAPTURE(static_cast<int>(format));
        CAPTURE(extent.width);
        CAPTURE(extent.height);
        CAPTURE(level);
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT device{};
        UINT rows = 0;
        UINT64 row_bytes = 0;
        UINT64 total = 0;
        context.device()->GetCopyableFootprints(&desc, level, 1, 0, &device,
                                                &rows, &row_bytes, &total);
        const render::d3d12::MipFootprint ours =
            render::d3d12::MipFootprintOf(format, extent, level);
        CHECK(ours.row_count == rows);
        CHECK(ours.row_bytes == row_bytes);
        CHECK(ours.row_pitch == device.Footprint.RowPitch);
        CHECK(ours.upload_bytes == total);
      }
    }
  }
}

// Draws three empty frames through `renderer` into the window output (or,
// with `viewport`, the editor viewport) and returns the last one read back.
RgbaImage DrawAndRead(render::RendererBackend& renderer, bool viewport,
                      bool fxaa) {
  render::SceneView view;
  view.post.fxaa_enabled = fxaa;
  uint64_t ticket = 0;
  for (int frame = 0; frame < 3; ++frame) {
    if (viewport) {
      static_cast<void>(renderer.PrepareViewport({kSceneWidth, kSceneHeight}));
      if (frame == 2) {
        ticket = renderer.RequestViewportReadback();
      }
      renderer.DrawViewportFrame(view, nullptr);
    } else {
      if (frame == 2) {
        ticket = renderer.RequestOutputReadback();
      }
      renderer.DrawFrame(view, nullptr);
    }
  }
  renderer.WaitIdle();
  render::ViewportPixels pixels;
  REQUIRE(renderer.TakeViewportReadback(ticket, pixels) ==
          render::ReadbackStatus::kReady);
  CHECK(renderer.TakeViewportReadback(ticket, pixels) ==
        render::ReadbackStatus::kUnknown);
  RgbaImage image;
  image.width = pixels.width;
  image.height = pixels.height;
  image.rgba.resize(pixels.rgba.size());
  std::memcpy(image.rgba.data(), pixels.rgba.data(), pixels.rgba.size());
  return image;
}

// Whether every pixel of `image` is `rgba`.
bool Uniform(const RgbaImage& image, std::array<uint8_t, 4> rgba) {
  for (size_t i = 0; i < image.rgba.size(); i += 4) {
    if (!std::equal(rgba.begin(), rgba.end(), image.rgba.begin() + i)) {
      return false;
    }
  }
  return !image.rgba.empty();
}

// Returns `image` with red and blue swapped.
RgbaImage SwapRedBlue(RgbaImage image) {
  for (size_t i = 0; i + 3 < image.rgba.size(); i += 4) {
    std::swap(image.rgba[i], image.rgba[i + 2]);
  }
  return image;
}

bool Same(const RgbaImage& a, const RgbaImage& b) {
  return Passes(Compare(a, b, 0), kExact);
}

TEST_CASE("The d3d12 frame presents to the output and the viewport alike") {
  if (!Testing()) {
    return;
  }
  TestRenderer renderer = MakeD3d12Renderer(HeadlessOptions());
  // An empty scene: the default sky, tonemapped.
  const RgbaImage output = DrawAndRead(*renderer.backend, false, false);
  CHECK(output.width == kSceneWidth);
  CHECK(output.height == kSceneHeight);
  CHECK_FALSE(Uniform(output, {output.rgba[0], output.rgba[1], output.rgba[2],
                               255}));  // The sky has a gradient.
  CHECK(renderer.backend->output_extent().width == kSceneWidth);
  CHECK(renderer.backend->gpu_frame_ms().has_value());
  CHECK(renderer.backend->gpu_scope_ms(render::GpuScope::kPresent).has_value());

  // The editor path: the scene into the viewport image, the UI (none) over a
  // cleared window output.
  const RgbaImage viewport = DrawAndRead(*renderer.backend, true, false);
  CHECK(viewport.width == kSceneWidth);
  CHECK(Same(viewport, output));
  CHECK(renderer.backend->viewport().generation == 1);
  CHECK(renderer.backend->viewport().texture != 0);
  // The window output of a viewport frame is the UI's black background.
  const uint64_t ticket = renderer.backend->RequestOutputReadback();
  renderer.backend->DrawViewportFrame(render::SceneView{}, nullptr);
  renderer.backend->WaitIdle();
  render::ViewportPixels window;
  REQUIRE(renderer.backend->TakeViewportReadback(ticket, window) ==
          render::ReadbackStatus::kReady);
  RgbaImage window_image{.width = window.width, .height = window.height};
  window_image.rgba.resize(window.rgba.size());
  std::memcpy(window_image.rgba.data(), window.rgba.data(), window.rgba.size());
  CHECK(Uniform(window_image, {0, 0, 0, 255}));

  const render::MemoryStats memory = renderer.backend->memory_stats();
  CHECK(memory.heap_count == 2);
  CHECK(memory.driver_budget);
  CHECK(memory.render_target_bytes > 0);
  CheckValidation(*renderer.backend);
}

TEST_CASE("d3d12 meshes and materials are created, updated and released") {
  if (!Testing()) {
    return;
  }
  TestRenderer renderer = MakeD3d12Renderer(HeadlessOptions());
  render::RendererBackend& backend = *renderer.backend;
  const asset::MeshData cube = asset::MakeCube();
  const render::MeshId mesh = backend.UploadMesh(cube.vertices, cube.indices);
  CHECK(backend.memory_stats().mesh_count == 1);
  material::MaterialDesc desc;
  desc.base_color = glm::vec4(0.2f, 0.4f, 0.6f, 1.0f);
  const render::MaterialId material = backend.CreateMaterial(desc);
  CHECK(material != render::kDefaultMaterial);
  CHECK(backend.material_count() == 2);
  for (int frame = 0; frame < 6; ++frame) {
    desc.roughness = 0.1f * static_cast<float>(frame);
    backend.UpdateMaterial(material, desc);
    backend.DrawFrame(render::SceneView{}, nullptr);
  }
  CHECK(backend.material(material).roughness == doctest::Approx(0.5f));
  backend.ReleaseMaterial(material);
  backend.ReleaseMesh(mesh);
  CHECK(backend.memory_stats().mesh_count == 0);
  CHECK(backend.memory_stats().retired_bytes > 0);
  backend.FlushReleases();
  CHECK(backend.memory_stats().retired_bytes == 0);
  backend.DrawFrame(render::SceneView{}, nullptr);
  backend.WaitIdle();
  CheckValidation(backend);
}

TEST_CASE("The d3d12 backend draws an ImGui frame over the output") {
  if (!Testing()) {
    return;
  }
  TestRenderer renderer = MakeD3d12Renderer(HeadlessOptions());
  render::RendererBackend& backend = *renderer.backend;
  const RgbaImage without_ui = DrawAndRead(backend, false, false);
  ImGuiContext* imgui = ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(static_cast<float>(kSceneWidth),
                          static_cast<float>(kSceneHeight));
  io.DeltaTime = 1.0f / 60.0f;
  io.IniFilename = nullptr;
  backend.InitImGuiRenderer();

  uint64_t ticket = 0;
  for (int frame = 0; frame < 3; ++frame) {
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f));
    ImGui::SetNextWindowSize(ImVec2(200.0f, 100.0f));
    ImGui::Begin("d3d12");
    ImGui::TextUnformatted("Direct3D 12");
    ImGui::Image(backend.ui_texture(render::TextureId{0}), ImVec2(32, 32));
    ImGui::End();
    ImGui::Render();
    if (frame == 2) {
      ticket = backend.RequestOutputReadback();
    }
    backend.DrawFrame(render::SceneView{}, ImGui::GetDrawData());
  }
  backend.WaitIdle();
  render::ViewportPixels pixels;
  REQUIRE(backend.TakeViewportReadback(ticket, pixels) ==
          render::ReadbackStatus::kReady);
  // Inside the window: ImGui's colours; far from it: the scene as drawn
  // without UI.
  const auto pixel = [&pixels](uint32_t x, uint32_t y) {
    const size_t at = (size_t{y} * pixels.width + x) * 4;
    return std::array<uint8_t, 4>{static_cast<uint8_t>(pixels.rgba[at]),
                                  static_cast<uint8_t>(pixels.rgba[at + 1]),
                                  static_cast<uint8_t>(pixels.rgba[at + 2]),
                                  static_cast<uint8_t>(pixels.rgba[at + 3])};
  };
  const auto scene_pixel = [&without_ui](uint32_t x, uint32_t y) {
    const size_t at = (size_t{y} * without_ui.width + x) * 4;
    return std::array<uint8_t, 4>{without_ui.rgba[at], without_ui.rgba[at + 1],
                                  without_ui.rgba[at + 2],
                                  without_ui.rgba[at + 3]};
  };
  CHECK(pixel(100, 60) != scene_pixel(100, 60));
  CHECK(pixel(600, 300) == scene_pixel(600, 300));

  backend.WaitIdle();
  backend.ShutdownImGuiRenderer();
  ImGui::DestroyContext(imgui);
  CheckValidation(backend);
}

#if BLUE_SHADER_HOT_RELOAD
TEST_CASE("An edited d3d12 shader is reloaded; a broken edit keeps the old one") {
  if (!Testing()) {
    return;
  }
  constexpr std::string_view kOutput =
      "return float4(params.decodeSrgb != 0 ? SrgbToLinear(encoded) : encoded, "
      "1.0);";
  const testing::ScratchDirectory dir("d3d12_hot_reload");
  const auto names = platform::ListDirectory(BLUE_SHADER_SOURCE_DIR);
  REQUIRE(names.has_value());
  for (const std::string& name : *names) {
    if (name.ends_with(".slang") || name.ends_with(".h")) {
      const auto bytes = platform::ReadFile(
          std::string(BLUE_SHADER_SOURCE_DIR) + "/" + name, size_t{1} << 20);
      REQUIRE(bytes.ok());
      REQUIRE(platform::WriteFileAtomic(dir.Child(name), bytes.value()).ok());
    }
  }
  const auto read = platform::ReadFile(dir.Child("present.slang"), 1 << 20);
  REQUIRE(read.ok());
  const std::string original(reinterpret_cast<const char*>(read.value().data()),
                             read.value().size());
  const size_t at = original.find(kOutput);
  REQUIRE(at != std::string::npos);
  const auto write = [&dir](const std::string& text) {
    REQUIRE(platform::WriteFileAtomic(
                dir.Child("present.slang"),
                std::as_bytes(std::span(text.data(), text.size())))
                .ok());
  };
  const auto poll = [](render::RendererBackend& renderer) {
    for (int i = 0; i < 3; ++i) {
      renderer.ReloadChangedShaders();
    }
  };

  render::RendererOptions options = HeadlessOptions();
  options.shader_source_dir = dir.path();
  TestRenderer renderer = MakeD3d12Renderer(options);
  render::RendererBackend& backend = *renderer.backend;
  const RgbaImage before = DrawAndRead(backend, false, false);

  // Red and blue swapped in the present pass's output.
  std::string swapped = original;
  swapped.replace(at, kOutput.size(),
                  "return float4((params.decodeSrgb != 0 ? "
                  "SrgbToLinear(encoded) : encoded).bgr, 1.0);");
  write(swapped);
  poll(backend);
  CHECK_MESSAGE(backend.last_shader_error().empty(),
                backend.last_shader_error());
  const RgbaImage swapped_image = DrawAndRead(backend, false, false);
  CHECK(Same(swapped_image, SwapRedBlue(before)));

  // A broken edit: the error names the file, the old pipeline stays.
  std::string broken = original;
  broken.replace(at, kOutput.size(), "return float4(encoded, 1.0;");
  write(broken);
  poll(backend);
  CHECK(std::string(backend.last_shader_error()).find("present.slang") !=
        std::string::npos);
  CHECK(Same(DrawAndRead(backend, false, false), swapped_image));

  write(original);
  poll(backend);
  CHECK(backend.last_shader_error().empty());
  CHECK(Same(DrawAndRead(backend, false, false), before));
  CheckValidation(backend);
}
#endif  // BLUE_SHADER_HOT_RELOAD

}  // namespace
}  // namespace blue::render_test
