// Checks the sample content the default scene uses: its lighting constants
// against the HDRI they were calibrated on, and the material files.

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <string_view>
#include <utility>

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include "asset/image_decoder.h"
#include "asset/model_importer.h"
#include "core/path.h"
#include "lighting/environment.h"
#include "material/material.h"
#include "material/material_file.h"
#include "scene/default_lighting.h"

namespace blue {
namespace {

constexpr float kPi = 3.14159265f;

std::string Sample(std::string_view relative) {
  return core::JoinPath(BLUE_SAMPLES_DIR, relative);
}

material::MaterialDesc LoadSampleMaterial(std::string_view relative) {
  auto loaded = material::LoadMaterialFile(Sample(relative));
  REQUIRE_MESSAGE(loaded.ok(), std::string(relative));
  return std::move(loaded).value();
}

}  // namespace

TEST_CASE("Default scene lighting matches its HDRI") {
  auto image = asset::LoadHdrFile(
      Sample("env/kloofendal_48d_partly_cloudy_puresky_2k.hdr"));
  REQUIRE(image.ok());
  const lighting::SunEstimate sun = lighting::EstimateSun(image.value());
  REQUIRE(sun.found);

  // The sun comes from the HDRI's sun, rotated with the panorama.
  const glm::vec3 aligned = glm::angleAxis(scene::kDefaultEnvironmentRotation,
                                           glm::vec3(0.0f, 1.0f, 0.0f)) *
                            sun.direction;
  CHECK(glm::dot(aligned, glm::normalize(scene::kDefaultSunDirection)) >
        std::cos(2.0f * kPi / 180.0f));
  // Over the shooter's right shoulder: it travels down range (-Z) and left.
  CHECK(scene::kDefaultSunDirection.z < 0.0f);
  CHECK(scene::kDefaultSunDirection.x < 0.0f);

  // Sun : sky on a horizontal white surface, and a sunlit 18% grey card
  // near 0.18 after exposure.
  const lighting::IrradianceSh9 sh =
      lighting::ComputeIrradianceSh9(image.value(), 256, sun.clamp_luminance);
  const float sky = scene::kDefaultEnvironmentIntensity *
                    lighting::EvaluateIrradiance(sh, {0.0f, 1.0f, 0.0f}).y /
                    kPi;
  const float sun_on_ground = scene::kDefaultSunIntensity *
                              -glm::normalize(scene::kDefaultSunDirection).y;
  CHECK(sun_on_ground / sky ==
        doctest::Approx(scene::kDefaultSunToSkyRatio).epsilon(0.05));
  const float grey_card =
      0.18f * (sky + sun_on_ground) * std::exp2(scene::kDefaultExposureEv);
  CHECK(grey_card == doctest::Approx(0.18f).epsilon(0.1));
}

TEST_CASE("Preview sphere materials sweep metallic x roughness") {
  constexpr std::array<std::string_view, 5> kTags{"005", "025", "050", "075",
                                                  "100"};
  constexpr std::array<float, 5> kRoughness{0.05f, 0.25f, 0.5f, 0.75f, 1.0f};
  for (size_t i = 0; i < kTags.size(); ++i) {
    const std::string tag(kTags[i]);
    const material::MaterialDesc dielectric =
        LoadSampleMaterial("materials/preview/dielectric_r" + tag + ".bmat");
    const material::MaterialDesc metal =
        LoadSampleMaterial("materials/preview/metal_r" + tag + ".bmat");
    CHECK(dielectric.metallic == 0.0f);
    CHECK(metal.metallic == 1.0f);
    CHECK(dielectric.roughness == doctest::Approx(kRoughness[i]));
    CHECK(metal.roughness == doctest::Approx(kRoughness[i]));
  }
}

TEST_CASE("Scene material files load with their textures") {
  const material::MaterialDesc ground =
      LoadSampleMaterial("materials/ground.bmat");
  CHECK_FALSE(ground.base_color_texture.empty());
  CHECK(ground.uv_scale.x == doctest::Approx(2.0f));
  CHECK(ground.uv_scale.y == doctest::Approx(30.1667f));
  const material::MaterialDesc steel =
      LoadSampleMaterial("materials/steel.bmat");
  CHECK(steel.metallic == 1.0f);
  CHECK_FALSE(steel.normal_texture.empty());
}

TEST_CASE("The M16 picks up its sidecar materials") {
  auto model = asset::ImportModel(Sample("m16.obj"));
  REQUIRE(model.ok());
  const auto& materials = model.value().materials;
  const auto named = [&](std::string_view name) {
    return std::ranges::find_if(
        materials,
        [&](const material::MaterialDesc& desc) { return desc.name == name; });
  };
  const auto steel = named("BaseMetal");
  REQUIRE(steel != materials.end());
  CHECK(steel->metallic == 1.0f);
  CHECK(steel->roughness == doctest::Approx(0.5f));
  const auto suppressor = named("Silencer");
  REQUIRE(suppressor != materials.end());
  CHECK(suppressor->roughness == doctest::Approx(0.75f));
  // Grip and stock are the same polymer: merged into one.
  const bool grip = named("Grip") != materials.end();
  const bool stock = named("Stock") != materials.end();
  CHECK(grip != stock);
  CHECK(materials.size() == 4);
}

}  // namespace blue
