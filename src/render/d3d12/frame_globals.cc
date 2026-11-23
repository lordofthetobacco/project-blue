#include "render/d3d12/frame_globals.h"

#include <d3d12.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "core/log.h"
#include "render/binding_layout.h"
#include "render/d3d12/d3d12_context.h"
#include "render/d3d12/descriptor_heaps.h"
#include "render/d3d12/format.h"
#include "render/d3d12/image.h"
#include "render/d3d12/root_layout.h"
#include "render/d3d12/samplers.h"
#include "render/d3d12/texture_cache.h"
#include "render/d3d12/upload_ring.h"
#include "render/frame_uniforms.h"

namespace blue::render::d3d12 {

namespace {

static_assert(BLUE_FRAME_ENVIRONMENT_BINDING == BLUE_FRAME_SHADOW_MAP_BINDING + 1 &&
                  BLUE_FRAME_BRDF_LUT_BINDING == BLUE_FRAME_SHADOW_MAP_BINDING + 2 &&
                  BLUE_FRAME_AO_BINDING == BLUE_FRAME_SHADOW_MAP_BINDING + 3,
              "The frame textures are one contiguous table");

uint32_t Require(std::optional<uint32_t> index, const char* what) {
  if (!index) {
    core::log::Fatal("Scene root signature without {}", what);
  }
  return *index;
}

// Returns a 1 x 1 sampled image with `layers` layers of `texel`.
Image MakeDefault(const D3d12Context& context, DXGI_FORMAT format,
                  std::span<const std::byte> texel, uint32_t layers, bool cube,
                  const wchar_t* name) {
  const std::vector<std::span<const std::byte>> subresources(layers, texel);
  return CreateSampledImage(
      context,
      {.extent = {1, 1}, .layers = layers, .cube = cube, .format = format,
       .name = name},
      {.extent = 1, .bytes = static_cast<uint32_t>(texel.size())},
      subresources);
}

}  // namespace

SceneRootParams SceneRootParams::Of(const RootLayout& layout,
                                    std::span<const BindingDesc> pass_set) {
  SceneRootParams params;
  params.frame_uniforms =
      Require(layout.Find(RootParamKind::kCbv, kFrameSet,
                          BLUE_FRAME_UNIFORMS_BINDING),
              "frame uniforms");
  params.frame_textures =
      Require(layout.Find(RootParamKind::kTable, kFrameSet,
                          BLUE_FRAME_SHADOW_MAP_BINDING),
              "frame textures");
  params.shadow_sampler =
      Require(layout.Find(RootParamKind::kSamplerTable, kFrameSet,
                          BLUE_FRAME_SHADOW_SAMPLER_BINDING),
              "shadow sampler");
  params.linear_sampler =
      Require(layout.Find(RootParamKind::kSamplerTable, kFrameSet,
                          BLUE_FRAME_LINEAR_SAMPLER_BINDING),
              "linear sampler");
  for (const BindingDesc& binding : pass_set) {
    if (binding.kind == BindingKind::kSampler) {
      params.pass_sampler = Require(
          layout.Find(RootParamKind::kSamplerTable, kPassSet, binding.binding),
          "pass sampler");
    } else if (params.pass_table == UINT32_MAX) {
      params.pass_table = Require(
          layout.Find(RootParamKind::kTable, kPassSet, binding.binding),
          "pass table");
    }
  }
  return params;
}

FrameGlobals::FrameGlobals(const D3d12Context& context,
                           const Samplers& samplers)
    : context_(context), samplers_(samplers) {
  constexpr float kLit = 1.0f;  // Shadow map depth: nothing occludes.
  constexpr std::array<std::byte, 8> kZeroHalf4{};
  constexpr std::array<std::byte, 4> kZeroHalf2{};
  constexpr std::array<std::byte, 1> kWhiteR8{std::byte{255}};
  default_shadow_map_ =
      MakeDefault(context, DXGI_FORMAT_R32_FLOAT,
                  std::as_bytes(std::span(&kLit, 1)), BLUE_CASCADE_COUNT,
                  false, L"blue default shadow map");
  default_cube_ = MakeDefault(context, DXGI_FORMAT_R16G16B16A16_FLOAT,
                              kZeroHalf4, 6, true, L"blue default cube");
  default_lut_ = MakeDefault(context, DXGI_FORMAT_R16G16_FLOAT, kZeroHalf2, 1,
                             false, L"blue default lut");
  default_ambient_occlusion_ =
      MakeDefault(context, DXGI_FORMAT_R8_UNORM, kWhiteR8, 1, false,
                  L"blue default ambient occlusion");
  textures_ = {default_shadow_map_.staging_srv(), default_cube_.staging_srv(),
               default_lut_.staging_srv(),
               default_ambient_occlusion_.staging_srv()};
}

void FrameGlobals::BindEnvironment(const Image& prefiltered_cube,
                                   const Image& brdf_lut) {
  textures_[1] = prefiltered_cube.staging_srv();
  textures_[2] = brdf_lut.staging_srv();
}

FrameBindings FrameGlobals::Prepare(const FrameUniforms& uniforms,
                                    UploadRing& ring) const {
  FrameBindings bindings;
  bindings.uniforms = ring.Push(std::as_bytes(std::span(&uniforms, 1)));
  DescriptorHeaps& heaps = context_.descriptors();
  const Descriptor table =
      heaps.AllocateTransient(static_cast<uint32_t>(textures_.size()));
  for (uint32_t i = 0; i < textures_.size(); ++i) {
    heaps.Copy(1, heaps.Offset(table.cpu, i), textures_[i]);
  }
  bindings.textures = table.gpu;
  return bindings;
}

void FrameGlobals::Bind(ID3D12GraphicsCommandList* cmd,
                        const SceneRootParams& params,
                        const FrameBindings& frame) const {
  cmd->SetGraphicsRootConstantBufferView(params.frame_uniforms, frame.uniforms);
  cmd->SetGraphicsRootDescriptorTable(params.frame_textures, frame.textures);
  cmd->SetGraphicsRootDescriptorTable(params.shadow_sampler,
                                      samplers_.shadow_compare());
  cmd->SetGraphicsRootDescriptorTable(params.linear_sampler,
                                      samplers_.linear_clamp());
}

}  // namespace blue::render::d3d12
