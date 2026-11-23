#include <doctest/doctest.h>

#include <cstdint>
#include <optional>
#include <span>

#include "render/binding_layout.h"
#include "render/d3d12/descriptor_heaps.h"
#include "render/d3d12/root_layout.h"

namespace blue::render::d3d12 {
namespace {

// Returns the parameter of `layout` of `kind` holding register
// `shader_register` of `space`; fails the test when there is none.
const RootParam& Param(const RootLayout& layout, RootParamKind kind,
                       uint32_t space, uint32_t shader_register) {
  const std::optional<uint32_t> index =
      layout.Find(kind, space, shader_register);
  REQUIRE(index.has_value());
  return layout.params[*index];
}

TEST_CASE("Every root layout starts with the push constants") {
  for (const RootLayout& layout :
       {SceneRootLayout(kMaterialBindings), SceneRootLayout(kSkyBindings),
        SceneRootLayout(kScopeBindings), SceneRootLayout({}),
        FullscreenRootLayout()}) {
    const RootParam& constants = layout.params[kPushConstantsParam];
    CHECK(constants.kind == RootParamKind::kConstants);
    CHECK(constants.space == kPushConstantSpace);
    CHECK(constants.shader_register == 0);
    CHECK(constants.constant_count * 4 == kMaxPushConstantBytes);
    CHECK(layout.cost_dwords() <= 64);
  }
}

TEST_CASE("The frame set: a root CBV, one texture table, one table per sampler") {
  const RootLayout layout = SceneRootLayout(kMaterialBindings);
  const RootParam& frame =
      Param(layout, RootParamKind::kCbv, kFrameSet, BLUE_FRAME_UNIFORMS_BINDING);
  CHECK(frame.stages == (kStageVertex | kStageFragment));
  const RootParam& textures = Param(layout, RootParamKind::kTable, kFrameSet,
                                    BLUE_FRAME_SHADOW_MAP_BINDING);
  REQUIRE(textures.range_count == 1);
  CHECK(textures.ranges[0].kind == BindingKind::kTexture);
  CHECK(textures.ranges[0].first_register == BLUE_FRAME_SHADOW_MAP_BINDING);
  CHECK(textures.ranges[0].count == 4);
  CHECK(layout.Find(RootParamKind::kTable, kFrameSet, BLUE_FRAME_AO_BINDING) ==
        layout.Find(RootParamKind::kTable, kFrameSet,
                    BLUE_FRAME_SHADOW_MAP_BINDING));
  CHECK(layout.Find(RootParamKind::kSamplerTable, kFrameSet,
                    BLUE_FRAME_SHADOW_SAMPLER_BINDING)
            .has_value());
  CHECK(layout.Find(RootParamKind::kSamplerTable, kFrameSet,
                    BLUE_FRAME_LINEAR_SAMPLER_BINDING)
            .has_value());
}

TEST_CASE("A material table is its CBV then its five textures") {
  const RootLayout layout = SceneRootLayout(kMaterialBindings);
  const RootParam& table = Param(layout, RootParamKind::kTable, kPassSet,
                                 BLUE_MATERIAL_PARAMS_BINDING);
  REQUIRE(table.range_count == 2);
  CHECK(table.ranges[0].kind == BindingKind::kUniformBuffer);
  CHECK(table.ranges[0].first_register == BLUE_MATERIAL_PARAMS_BINDING);
  CHECK(table.ranges[0].table_offset == 0);
  CHECK(table.ranges[1].kind == BindingKind::kTexture);
  CHECK(table.ranges[1].first_register == BLUE_MATERIAL_BASE_COLOR_BINDING);
  CHECK(table.ranges[1].count == kMaterialTextureCount);
  // MaterialSystem writes the texture of binding b at descriptor b.
  CHECK(table.ranges[1].table_offset == BLUE_MATERIAL_BASE_COLOR_BINDING);
  CHECK(table.table_size() <= DescriptorHeaps::kTableSize);
  CHECK(layout.Find(RootParamKind::kSamplerTable, kPassSet,
                    BLUE_MATERIAL_SAMPLER_BINDING)
            .has_value());
}

TEST_CASE("Sky and scope sets get their own tables") {
  const RootLayout sky = SceneRootLayout(kSkyBindings);
  const RootParam& cube =
      Param(sky, RootParamKind::kTable, kPassSet, BLUE_SKY_CUBE_BINDING);
  CHECK(cube.table_size() == 1);
  CHECK(sky.Find(RootParamKind::kSamplerTable, kPassSet,
                 BLUE_SKY_SAMPLER_BINDING)
            .has_value());

  const RootLayout scope = SceneRootLayout(kScopeBindings);
  const RootParam& lens =
      Param(scope, RootParamKind::kTable, kPassSet, BLUE_SCOPE_PICTURE_BINDING);
  CHECK(lens.table_size() == 2);
  CHECK(lens.stages == (kStageVertex | kStageFragment));
}

TEST_CASE("A depth-only layout has the frame set alone") {
  const RootLayout layout = SceneRootLayout({});
  for (const RootParam& param : layout.parameters()) {
    CHECK(param.space != kPassSet);
  }
}

TEST_CASE("Full-screen programs: inputs t0.., then the shared sampler") {
  const RootLayout layout = FullscreenRootLayout();
  REQUIRE(layout.param_count == 3);
  const RootParam& inputs = layout.params[1];
  CHECK(inputs.kind == RootParamKind::kTable);
  CHECK(inputs.space == kInputSet);
  REQUIRE(inputs.range_count == 1);
  CHECK(inputs.ranges[0].first_register == 0);
  CHECK(inputs.ranges[0].count == kMaxFullscreenInputs);
  CHECK(inputs.stages == kStageFragment);
  const RootParam& sampler = layout.params[2];
  CHECK(sampler.kind == RootParamKind::kSamplerTable);
  CHECK(sampler.shader_register == kFullscreenSampler.binding);
}

}  // namespace
}  // namespace blue::render::d3d12
