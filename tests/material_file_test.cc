#include "material/material_file.h"

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>

#include "core/path.h"
#include "material/material.h"
#include "platform/file_system.h"
#include "tests/scratch_directory.h"

namespace blue::material {
namespace {

void WriteText(const std::string& path, std::string_view text) {
  REQUIRE(platform::WriteFileAtomic(path, std::as_bytes(std::span(text))).ok());
}

}  // namespace

TEST_CASE("Sanitize clamps every number into its range") {
  constexpr float kNan = std::numeric_limits<float>::quiet_NaN();
  MaterialDesc desc;
  desc.base_color = {2.0f, -1.0f, kNan, 0.5f};
  desc.metallic = 1.5f;
  desc.roughness = -0.5f;
  desc.emissive = {-1.0f, 5.0f, 0.0f};
  desc.emissive_strength = 1.0e9f;
  desc.normal_scale = kNan;
  desc.occlusion_strength = 2.0f;
  desc.uv_scale = {0.0f, 1.0e9f};
  desc.alpha_cutoff = -1.0f;

  const MaterialDesc clean = Sanitize(desc);

  CHECK(clean.base_color == glm::vec4(1.0f, 0.0f, 1.0f, 0.5f));
  CHECK(clean.metallic == 1.0f);
  CHECK(clean.roughness == 0.0f);
  CHECK(clean.emissive == glm::vec3(0.0f, 5.0f, 0.0f));
  CHECK(clean.emissive_strength == kMaxEmissiveStrength);
  CHECK(clean.normal_scale == 1.0f);
  CHECK(clean.occlusion_strength == 1.0f);
  CHECK(clean.uv_scale.x > 0.0f);
  CHECK(clean.uv_scale.y == kMaxUvScale);
  CHECK(clean.alpha_cutoff == 0.0f);
}

TEST_CASE("SameAppearance ignores only the name") {
  MaterialDesc a{.name = "a", .roughness = 0.3f};
  MaterialDesc b{.name = "b", .roughness = 0.3f};
  CHECK(SameAppearance(a, b));
  b.normal_texture.path = "n.png";
  CHECK_FALSE(SameAppearance(a, b));
}

TEST_CASE("RoughnessFromShininess maps Blinn-Phong exponents to GGX") {
  CHECK(RoughnessFromShininess(0.0f) == 1.0f);
  CHECK(RoughnessFromShininess(-3.0f) == 1.0f);
  CHECK(RoughnessFromShininess(2.0f) == doctest::Approx(std::sqrt(0.5f)));
  CHECK(RoughnessFromShininess(98.0f) == doctest::Approx(std::sqrt(0.02f)));
  // Mirror-like exponents stay above the floor.
  CHECK(RoughnessFromShininess(1.0e6f) == doctest::Approx(0.05f));
}

TEST_CASE("SidecarMaterialPath sanitizes the material name") {
  CHECK(SanitizeMaterialName("Base Metal/02") == "Base_Metal_02");
  CHECK(SanitizeMaterialName("") == "material");
  CHECK(SanitizeMaterialName("9m1.v-2_x") == "9m1.v-2_x");
  // UTF-8 bytes are replaced one by one.
  CHECK(SanitizeMaterialName("st\xC3\xA4hl") == "st__hl");
  CHECK(SidecarMaterialPath("samples/m16.obj", "BaseMetal") ==
        "samples/m16.obj.BaseMetal.bmat");
}

TEST_CASE("A material file round-trips every field") {
  const testing::ScratchDirectory dir("material_file");
  MaterialDesc desc{
      .name = "worn steel",
      .base_color = {0.9f, 0.8f, 0.7f, 0.6f},
      .metallic = 1.0f,
      .roughness = 0.35f,
      .emissive = {0.1f, 2.0f, 0.3f},
      .emissive_strength = 4.5f,
      .normal_scale = 0.75f,
      .occlusion_strength = 0.5f,
      .uv_scale = {2.0f, 3.0f},
      .alpha_mode = AlphaMode::kMask,
      .alpha_cutoff = 0.4f,
      .double_sided = true,
  };
  desc.base_color_texture.path = dir.Child("textures/color.jpg");
  desc.roughness_texture.path = dir.Child("textures/rough.jpg");
  desc.metallic_texture.path = dir.Child("textures/metal.jpg");
  desc.normal_texture = {.path = dir.Child("models/gun.glb"),
                         .embedded_index = 3};
  desc.occlusion_texture.path = dir.Child("ao.png");
  desc.emissive_texture.path = dir.Child("glow.png");
  desc.metallic_roughness_texture.path = dir.Child("mr.png");
  const std::string path = dir.Child("materials/steel.bmat");

  REQUIRE(SaveMaterialFile(desc, path).ok());
  const auto loaded = LoadMaterialFile(path);

  REQUIRE(loaded.ok());
  CHECK(loaded.value() == desc);

  // Texture paths are stored relative to the file.
  const auto text = platform::ReadFile(path, 4096);
  REQUIRE(text.ok());
  const std::string contents(reinterpret_cast<const char*>(text.value().data()),
                             text.value().size());
  CHECK(contents.find("base_color_texture = ../textures/color.jpg") !=
        std::string::npos);
  CHECK(contents.find("normal_texture.embedded = 3") != std::string::npos);
}

TEST_CASE("UV sets and normal encoding round-trip and default to absent") {
  const testing::ScratchDirectory dir("material_uv_sets");
  MaterialDesc desc{.name = "two_uv"};
  desc.uv_set = {0, 1, 0, 1, 1};
  desc.normal_encoding = NormalEncoding::kRg;
  desc.occlusion_in_metallic_roughness = true;
  const std::string path = dir.Child("two_uv.bmat");
  REQUIRE(SaveMaterialFile(desc, path).ok());
  const auto loaded = LoadMaterialFile(path);
  REQUIRE(loaded.ok());
  CHECK(loaded.value() == desc);

  // Defaults write none of the optional keys.
  const std::string plain_path = dir.Child("plain.bmat");
  REQUIRE(SaveMaterialFile(MaterialDesc{.name = "plain"}, plain_path).ok());
  const auto text = platform::ReadFile(plain_path, 4096);
  REQUIRE(text.ok());
  const std::string contents(reinterpret_cast<const char*>(text.value().data()),
                             text.value().size());
  CHECK(contents.find("uv_set") == std::string::npos);
  CHECK(contents.find("normal_encoding") == std::string::npos);
  CHECK(contents.find("occlusion_in_metallic_roughness") == std::string::npos);

  MaterialDesc out_of_range;
  out_of_range.uv_set = {7, 0, 1, 2, 0};
  CHECK(Sanitize(out_of_range).uv_set ==
        std::array<uint8_t, kTextureSlotCount>{1, 0, 1, 1, 0});
}

TEST_CASE("LoadMaterialFile keeps defaults for missing and bad values") {
  const testing::ScratchDirectory dir("material_defaults");
  const std::string path = dir.Child("plain.bmat");
  WriteText(path,
            "# comment\n"
            "\n"
            "roughness = 0.25\n"
            "metallic = lots\n"
            "base_color = 1 2\n"
            "no equals sign\n"
            "mystery_key = 3\n"
            "alpha_mode = blend\n"
            "uv_scale = 4, 5\n"
            "normal_texture.embedded = -2\n"
            "base_color_texture = \n");

  const auto loaded = LoadMaterialFile(path);

  REQUIRE(loaded.ok());
  const MaterialDesc& desc = loaded.value();
  CHECK(desc.name == "plain");  // the file stem
  CHECK(desc.roughness == 0.25f);
  CHECK(desc.metallic == MaterialDesc{}.metallic);
  CHECK(desc.base_color == MaterialDesc{}.base_color);
  CHECK((desc.alpha_mode == AlphaMode::kOpaque));
  CHECK(desc.uv_scale == glm::vec2(4.0f, 5.0f));
  CHECK(desc.normal_texture.empty());
  CHECK(desc.base_color_texture.empty());
}

TEST_CASE("LoadMaterialFile reports a missing file") {
  const auto loaded = LoadMaterialFile(
      core::JoinPath(platform::TempDirectory(), "blue_no_such_material.bmat"));
  REQUIRE_FALSE(loaded.ok());
  CHECK((loaded.error() == MaterialFileError::kCannotOpen));
}

TEST_CASE("The sample materials load with existing textures") {
  for (const char* name : {"ground.bmat", "steel.bmat"}) {
    CAPTURE(name);
    const auto loaded = LoadMaterialFile(
        core::JoinPath(BLUE_SAMPLES_DIR, "materials") + "/" + name);
    REQUIRE(loaded.ok());
    const MaterialDesc& desc = loaded.value();
    CHECK_FALSE(desc.base_color_texture.empty());
    CHECK_FALSE(desc.roughness_texture.empty());
    for (const TextureRef* ref :
         {&desc.base_color_texture, &desc.roughness_texture,
          &desc.metallic_texture, &desc.normal_texture,
          &desc.occlusion_texture}) {
      if (!ref->empty()) {
        CHECK(platform::IsRegularFile(ref->path));
      }
    }
  }
}

}  // namespace blue::material
