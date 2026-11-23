// Compares render/binding_layout.h and the C++ uniform structs with the
// reflection slangc wrote for every compiled shader program
// (cmake/Shaders.cmake), on every compiled target: a shader and the C++ side
// that disagree about a binding or an offset fail here, before any GPU runs.

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "mcp/json.h"
#include "platform/file_system.h"
#include "render/binding_layout.h"
#include "render/frame_uniforms.h"
#include "render/shader_manifest.h"

namespace blue::render {
namespace {

using mcp::Json;

struct Target {
  std::string_view name;       // Directory of the reflection files.
  bool descriptor_tables = false;  // SPIR-V: one binding kind for everything.
};

std::vector<Target> Targets() {
  std::vector<Target> targets;
#if BLUE_SHADERS_HAVE_SPIRV
  targets.push_back({"spirv", true});
#endif
#if BLUE_SHADERS_HAVE_DXIL
  targets.push_back({"dxil", false});
#endif
  return targets;
}

// Returns the reflection of one stage of `program`.
Json LoadReflection(const Target& target, const ShaderProgramInfo& program,
                    std::string_view stage) {
  const std::string path = std::string(BLUE_SHADER_REFLECTION_DIR) + "/" +
                           std::string(target.name) + "/" +
                           std::string(program.name) + "." +
                           std::string(stage) + ".json";
  const auto bytes = platform::ReadFile(path, size_t{16} << 20);
  REQUIRE_MESSAGE(bytes.ok(), "cannot read ", path);
  const std::string_view text(
      reinterpret_cast<const char*>(bytes.value().data()),
      bytes.value().size());
  auto json = mcp::ParseJson(text);
  REQUIRE_MESSAGE(json.ok(), "invalid JSON in ", path);
  return std::move(json).value();
}

// Where a shader parameter is bound, in the terms of binding_layout.h.
struct Bound {
  bool push_constants = false;
  uint32_t set = 0;
  uint32_t binding = 0;
  BindingKind kind = BindingKind::kUniformBuffer;
};

std::optional<BindingKind> KindOfType(const Json& type) {
  const std::string* kind = type.GetString("kind");
  if (kind == nullptr) {
    return std::nullopt;
  }
  if (*kind == "constantBuffer") {
    return BindingKind::kUniformBuffer;
  }
  if (*kind == "resource") {
    return BindingKind::kTexture;
  }
  if (*kind == "samplerState") {
    return BindingKind::kSampler;
  }
  return std::nullopt;
}

// Reads the binding of reflection parameter `parameter`.
Bound ReadBinding(const Json& parameter, const Target& target) {
  Bound bound;
  const Json* type = parameter.GetObject("type");
  REQUIRE(type != nullptr);
  const std::optional<BindingKind> kind = KindOfType(*type);
  REQUIRE_MESSAGE(kind.has_value(), "unknown parameter kind");
  bound.kind = *kind;

  // Vulkan push constants: a list with a pushConstantBuffer entry.
  if (const Json* bindings = parameter.GetArray("bindings")) {
    for (const Json& entry : bindings->array_items()) {
      const std::string* entry_kind = entry.GetString("kind");
      if (entry_kind != nullptr && *entry_kind == "pushConstantBuffer") {
        bound.push_constants = true;
      }
    }
    REQUIRE_MESSAGE(bound.push_constants, "unexpected binding list");
    return bound;
  }
  const Json* binding = parameter.GetObject("binding");
  REQUIRE(binding != nullptr);
  bound.set = static_cast<uint32_t>(binding->GetInteger("space").value_or(0));
  bound.binding =
      static_cast<uint32_t>(binding->GetInteger("index").value_or(0));
  // Direct3D 12: root constants are a constant buffer in their own space.
  if (!target.descriptor_tables && bound.set == kPushConstantSpace) {
    bound.push_constants = true;
    CHECK(bound.binding == 0);
    CHECK(bound.kind == BindingKind::kUniformBuffer);
  }
  if (!target.descriptor_tables) {
    // The register class must agree with the type.
    const std::string* register_kind = binding->GetString("kind");
    REQUIRE(register_kind != nullptr);
    const std::string_view expected =
        bound.kind == BindingKind::kUniformBuffer
            ? "constantBuffer"
            : bound.kind == BindingKind::kTexture ? "shaderResource"
                                                  : "samplerState";
    CHECK(*register_kind == expected);
  }
  return bound;
}

const BindingDesc* FindByName(std::span<const BindingDesc> table,
                              std::string_view name) {
  for (const BindingDesc& desc : table) {
    if (desc.name == name) {
      return &desc;
    }
  }
  return nullptr;
}

// Returns the byte size of the struct in uniform block type `type`.
int64_t UniformBlockSize(const Json& type) {
  const Json* element = type.GetObject("elementType");
  REQUIRE(element != nullptr);
  const Json* sizes = element->GetArray("sizes");
  REQUIRE(sizes != nullptr);
  for (const Json& size : sizes->array_items()) {
    const std::string* kind = size.GetString("kind");
    if (kind != nullptr && *kind == "uniform") {
      return size.GetInteger("value").value_or(-1);
    }
  }
  return -1;
}

// Returns the byte offset of field `name` of uniform block type `type`.
int64_t FieldOffset(const Json& type, std::string_view name) {
  const Json* element = type.GetObject("elementType");
  REQUIRE(element != nullptr);
  const Json* fields = element->GetArray("fields");
  REQUIRE(fields != nullptr);
  for (const Json& field : fields->array_items()) {
    const std::string* field_name = field.GetString("name");
    if (field_name != nullptr && *field_name == name) {
      const Json* binding = field.GetObject("binding");
      REQUIRE(binding != nullptr);
      return binding->GetInteger("offset").value_or(-1);
    }
  }
  return -1;
}

struct FieldOffsetOf {
  std::string_view shader_name;
  size_t offset;
};

// FrameUniforms as C++ lays it out (render/frame_uniforms.h).
constexpr std::array kFrameFields = {
    FieldOffsetOf{"viewProj", offsetof(FrameUniforms, view_proj)},
    FieldOffsetOf{"view", offsetof(FrameUniforms, view)},
    FieldOffsetOf{"projection", offsetof(FrameUniforms, projection)},
    FieldOffsetOf{"inverseProjection",
                  offsetof(FrameUniforms, inverse_projection)},
    FieldOffsetOf{"inverseViewProj", offsetof(FrameUniforms, inverse_view_proj)},
    FieldOffsetOf{"cameraPosition", offsetof(FrameUniforms, camera_position)},
    FieldOffsetOf{"ambientColor", offsetof(FrameUniforms, ambient_color)},
    FieldOffsetOf{"sunDirection", offsetof(FrameUniforms, sun_direction)},
    FieldOffsetOf{"sunColor", offsetof(FrameUniforms, sun_color)},
    FieldOffsetOf{"cascadeViewProjection",
                  offsetof(FrameUniforms, cascade_view_projection)},
    FieldOffsetOf{"cascadeSplitFar", offsetof(FrameUniforms, cascade_split_far)},
    FieldOffsetOf{"cascadeTexelWorldSize",
                  offsetof(FrameUniforms, cascade_texel_world_size)},
    FieldOffsetOf{"shadowParams", offsetof(FrameUniforms, shadow_params)},
    FieldOffsetOf{"screen", offsetof(FrameUniforms, screen)},
    FieldOffsetOf{"irradianceSh", offsetof(FrameUniforms, irradiance_sh)},
    FieldOffsetOf{"environmentParams",
                  offsetof(FrameUniforms, environment_params)},
    FieldOffsetOf{"pointLightCount", offsetof(FrameUniforms, point_light_count)},
    FieldOffsetOf{"pointLights", offsetof(FrameUniforms, point_lights)},
    FieldOffsetOf{"previousViewProj",
                  offsetof(FrameUniforms, previous_view_proj)},
    FieldOffsetOf{"motionBlur", offsetof(FrameUniforms, motion_blur)},
    FieldOffsetOf{"unjitteredViewProj",
                  offsetof(FrameUniforms, unjittered_view_proj)},
    FieldOffsetOf{"jitter", offsetof(FrameUniforms, jitter)},
};

// MaterialParams (render/material_params.cc asserts the same numbers).
constexpr std::array kMaterialFields = {
    FieldOffsetOf{"baseColor", 0}, FieldOffsetOf{"emissive", 16},
    FieldOffsetOf{"surface", 32},  FieldOffsetOf{"uvAlpha", 48},
    FieldOffsetOf{"options", 64},
};
constexpr int64_t kMaterialParamsSize = 80;

void CheckFields(const Json& type, std::span<const FieldOffsetOf> fields,
                 int64_t size) {
  CHECK(UniformBlockSize(type) == size);
  for (const FieldOffsetOf& field : fields) {
    CAPTURE(field.shader_name);
    CHECK(FieldOffset(type, field.shader_name) ==
          static_cast<int64_t>(field.offset));
  }
}

// Checks every global parameter of one stage's reflection.
void CheckStage(const Target& target, const ShaderProgramInfo& program,
                std::string_view stage) {
  const Json reflection = LoadReflection(target, program, stage);
  const Json* parameters = reflection.GetArray("parameters");
  REQUIRE(parameters != nullptr);

  // A full-screen program declares the inputs' sampler.
  bool fullscreen = false;
  for (const Json& parameter : parameters->array_items()) {
    const std::string* name = parameter.GetString("name");
    fullscreen = fullscreen || (name != nullptr && *name == "inputSampler");
  }

  for (const Json& parameter : parameters->array_items()) {
    const std::string* name = parameter.GetString("name");
    REQUIRE(name != nullptr);
    CAPTURE(*name);
    const Bound bound = ReadBinding(parameter, target);
    const Json* type = parameter.GetObject("type");
    REQUIRE(type != nullptr);

    if (bound.push_constants) {
      const int64_t size = UniformBlockSize(*type);
      CHECK(size > 0);
      CHECK(size <= static_cast<int64_t>(kMaxPushConstantBytes));
      continue;
    }
    if (fullscreen) {
      CHECK(bound.set == kInputSet);
      if (*name == kFullscreenSampler.name) {
        CHECK(bound.binding == kFullscreenSampler.binding);
        CHECK(bound.kind == BindingKind::kSampler);
      } else {
        CHECK(bound.kind == BindingKind::kTexture);
        CHECK(bound.binding < kMaxFullscreenInputs);
      }
      continue;
    }

    // A scene program: the frame set, or one of the pass sets.
    const BindingDesc* desc = nullptr;
    if (bound.set == kFrameSet) {
      desc = FindByName(kFrameBindings, *name);
    } else if (bound.set == kPassSet) {
      for (const std::span<const BindingDesc> table : kPassSets) {
        if (desc == nullptr) {
          desc = FindByName(table, *name);
        }
      }
    }
    REQUIRE_MESSAGE(desc != nullptr,
                    "not in render/binding_layout.h (set ", bound.set, ")");
    CHECK(bound.binding == desc->binding);
    CHECK(bound.kind == desc->kind);

    if (*name == "frame") {
      CheckFields(*type, kFrameFields, sizeof(FrameUniforms));
    } else if (*name == "material") {
      CheckFields(*type, kMaterialFields, kMaterialParamsSize);
    }
  }
}

TEST_CASE("Every shader program binds what render/binding_layout.h says") {
  const std::vector<Target> targets = Targets();
  REQUIRE_FALSE(targets.empty());
  for (const Target& target : targets) {
    for (const ShaderProgramInfo& program : kShaderPrograms) {
      CAPTURE(target.name);
      CAPTURE(program.name);
      CheckStage(target, program, "vert");
      if (!program.fragment_entry.empty()) {
        CheckStage(target, program, "frag");
      }
    }
  }
}

TEST_CASE("The binding tables use every number once per set") {
  const auto check = [](std::span<const BindingDesc> table) {
    for (size_t i = 0; i < table.size(); ++i) {
      for (size_t j = i + 1; j < table.size(); ++j) {
        CHECK(table[i].binding != table[j].binding);
        CHECK(table[i].name != table[j].name);
      }
    }
  };
  check(kFrameBindings);
  for (const std::span<const BindingDesc> table : kPassSets) {
    check(table);
  }
  CHECK(kFullscreenSampler.binding >= kMaxFullscreenInputs);
}

TEST_CASE("The manifest names every program once") {
  CHECK(kShaderPrograms.size() == kShaderProgramCount);
  for (size_t i = 0; i < kShaderPrograms.size(); ++i) {
    CHECK_FALSE(kShaderPrograms[i].name.empty());
    CHECK_FALSE(kShaderPrograms[i].vertex_entry.empty());
    CHECK(ShaderProgramInfoOf(static_cast<ShaderProgram>(i)).name ==
          kShaderPrograms[i].name);
    for (size_t j = i + 1; j < kShaderPrograms.size(); ++j) {
      CHECK(kShaderPrograms[i].name != kShaderPrograms[j].name);
    }
  }
}

}  // namespace
}  // namespace blue::render
