#include "shader/metal/shader_compiler.h"

#include <SDL3/SDL_filesystem.h>
#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/path.h"
#include "platform/file_system.h"
#include "tests/scratch_directory.h"

// These tests compile with the system's Metal compiler, so they need a Metal
// device (every supported Mac has one, also without a display).

namespace blue::shader::metal {
namespace {

constexpr std::string_view kValidVertex =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "vertex float4 VertexMain(uint id [[vertex_id]]) {\n"
    "  return float4(float(id), 0.0, 0.0, 1.0);\n"
    "}\n";

constexpr std::string_view kValidFragment =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "fragment float4 FragmentMain() { return float4(1.0); }\n";

using ScratchDirectory = testing::ScratchDirectory;

// Writes `text` to `path`, creating parent directories.
void WriteText(const std::string& path, std::string_view text) {
  std::vector<std::byte> bytes;
  for (const char c : text) {
    bytes.push_back(static_cast<std::byte>(c));
  }
  REQUIRE(platform::WriteFileAtomic(path, bytes).ok());
}

// Returns the compiler message of a failed result, or "" for a success, for
// use as an assertion message.
std::string ErrorOf(const core::Result<CompiledShader, ShaderError>& result) {
  return result.ok() ? std::string() : result.error().message;
}

// Compiles `text` as the file `name` in a scratch directory.
core::Result<CompiledShader, ShaderError> CompileText(
    const ScratchDirectory& dir, const std::string& name, std::string_view text,
    CompileMode mode = CompileMode::kDebug) {
  const std::string path = dir.Child(name);
  WriteText(path, text);
  return ShaderCompiler(mode).Compile(path);
}

TEST_CASE("compiles a vertex and a fragment shader to a Metal library") {
  ScratchDirectory dir("shader_valid");
  for (const CompileMode mode : {CompileMode::kDebug, CompileMode::kRelease}) {
    const auto vertex = CompileText(dir, "a.vert.metal", kValidVertex, mode);
    REQUIRE_MESSAGE(vertex.ok(), ErrorOf(vertex));
    CHECK(static_cast<bool>(vertex.value().library));
    CHECK(vertex.value().library.HasFunction("VertexMain"));
    CHECK_FALSE(vertex.value().library.HasFunction("FragmentMain"));
    CHECK(vertex.value().stage == ShaderStage::kVertex);
    CHECK(vertex.value().dependencies.empty());

    const auto fragment =
        CompileText(dir, "a.frag.metal", kValidFragment, mode);
    REQUIRE_MESSAGE(fragment.ok(), ErrorOf(fragment));
    CHECK(fragment.value().library.HasFunction("FragmentMain"));
    CHECK(fragment.value().stage == ShaderStage::kFragment);
  }
}

TEST_CASE("compiles a compute shader") {
  ScratchDirectory dir("shader_comp");
  const auto result =
      CompileText(dir, "a.comp.metal",
                  "#include <metal_stdlib>\n"
                  "kernel void ComputeMain(\n"
                  "    uint id [[thread_position_in_grid]]) { (void)id; }\n");
  REQUIRE_MESSAGE(result.ok(), ErrorOf(result));
  CHECK(result.value().library.HasFunction("ComputeMain"));
  CHECK(result.value().stage == ShaderStage::kCompute);
}

TEST_CASE("a library is move-only and empties its source") {
  ScratchDirectory dir("shader_move");
  auto result = CompileText(dir, "a.frag.metal", kValidFragment);
  REQUIRE_MESSAGE(result.ok(), ErrorOf(result));
  ShaderLibrary moved = std::move(result.value().library);
  CHECK(static_cast<bool>(moved));
  CHECK_FALSE(static_cast<bool>(result.value().library));
  CHECK_FALSE(result.value().library.HasFunction("FragmentMain"));
  ShaderLibrary assigned;
  assigned = std::move(moved);
  CHECK(assigned.HasFunction("FragmentMain"));
  CHECK_FALSE(static_cast<bool>(moved));
}

TEST_CASE("a syntax error names the file and the line") {
  ScratchDirectory dir("shader_syntax");
  const auto result = CompileText(dir, "broken.frag.metal",
                                  "#include <metal_stdlib>\n"
                                  "using namespace metal;\n"
                                  "fragment float4 FragmentMain() {\n"
                                  "  float4 color = float4(1.0);\n"
                                  "  return color + undefinedName;\n"
                                  "}\n");
  REQUIRE_FALSE(result.ok());
  const std::string& message = result.error().message;
  CHECK(message.find(dir.Child("broken.frag.metal") + ":5:") !=
        std::string::npos);
  CHECK(message.find("error") != std::string::npos);
}

TEST_CASE("an error in an included file names that file and its line") {
  ScratchDirectory dir("shader_include_error");
  WriteText(dir.Child("lib/bad.h"),
            "// Line 1.\n"
            "// Line 2.\n"
            "static float Bad() { return undefinedName; }\n");
  const auto result = CompileText(dir, "main.frag.metal",
                                  "#include <metal_stdlib>\n"
                                  "#include \"lib/bad.h\"\n"
                                  "fragment float4 FragmentMain() {\n"
                                  "  return float4(1.0);\n"
                                  "}\n");
  REQUIRE_FALSE(result.ok());
  CHECK(result.error().message.find(dir.Child("lib/bad.h") + ":3:") !=
        std::string::npos);
}

TEST_CASE("resolves includes relative to the including file") {
  ScratchDirectory dir("shader_include");
  WriteText(dir.Child("common.h"),
            "#ifndef COMMON_H_\n"
            "#define COMMON_H_\n"
            "constant float kScale = 2.0;\n"
            "#endif\n");
  WriteText(dir.Child("lib/helper.h"),
            "#include \"../common.h\"\n"
            "static float Scaled(float x) { return x * kScale; }\n");
  const auto result =
      CompileText(dir, "shaders/main.frag.metal",
                  "#include <metal_stdlib>\n"
                  "#include \"../lib/helper.h\"\n"
                  "#include \"../common.h\"\n"
                  "fragment float4 FragmentMain() {\n"
                  "  return float4(Scaled(0.5));\n"
                  "}\n");
  REQUIRE_MESSAGE(result.ok(), ErrorOf(result));

  std::vector<std::string> dependencies = result.value().dependencies;
  std::sort(dependencies.begin(), dependencies.end());
  const std::vector<std::string> expected = {dir.Child("common.h"),
                                             dir.Child("lib/helper.h")};
  CHECK(dependencies == expected);
  for (const std::string& dependency : dependencies) {
    CHECK(dependency == core::NormalizePath(dependency));
  }
}

TEST_CASE("a missing include is an error naming the file") {
  ScratchDirectory dir("shader_missing_include");
  const auto result = CompileText(dir, "main.vert.metal",
                                  "#include <metal_stdlib>\n"
                                  "#include \"does_not_exist.h\"\n"
                                  "vertex float4 VertexMain() {\n"
                                  "  return float4(0.0);\n"
                                  "}\n");
  REQUIRE_FALSE(result.ok());
  CHECK(result.error().message.find("does_not_exist.h") != std::string::npos);
}

TEST_CASE("an unknown extension is an error") {
  ScratchDirectory dir("shader_extension");
  const auto result = CompileText(dir, "shader.txt", kValidVertex);
  REQUIRE_FALSE(result.ok());
  CHECK(result.error().message.find("shader.txt") != std::string::npos);
}

TEST_CASE("a missing file is an error") {
  ScratchDirectory dir("shader_missing_file");
  const auto result = ShaderCompiler(CompileMode::kDebug)
                          .Compile(dir.Child("nope.vert.metal"));
  REQUIRE_FALSE(result.ok());
  CHECK(result.error().message.find("nope.vert.metal") != std::string::npos);
}

TEST_CASE("a shader without its stage's entry point is an error") {
  ScratchDirectory dir("shader_entry_point");
  const auto result = CompileText(dir, "other.frag.metal",
                                  "#include <metal_stdlib>\n"
                                  "fragment float4 Shade() {\n"
                                  "  return float4(1.0);\n"
                                  "}\n");
  REQUIRE_FALSE(result.ok());
  const std::string& message = result.error().message;
  CHECK(message.find("other.frag.metal") != std::string::npos);
  CHECK(message.find("FragmentMain") != std::string::npos);
}

// kDebug defines BLUE_SHADER_DEBUG for the shader; kRelease does not.
TEST_CASE("debug mode defines BLUE_SHADER_DEBUG") {
  ScratchDirectory dir("shader_modes");
  constexpr std::string_view kDebugOnly =
      "#ifndef BLUE_SHADER_DEBUG\n"
      "#error \"compiled without BLUE_SHADER_DEBUG\"\n"
      "#endif\n"
      "#include <metal_stdlib>\n"
      "fragment float4 FragmentMain() { return float4(1.0); }\n";
  const auto debug =
      CompileText(dir, "debug.frag.metal", kDebugOnly, CompileMode::kDebug);
  const auto release =
      CompileText(dir, "debug.frag.metal", kDebugOnly, CompileMode::kRelease);
  CHECK_MESSAGE(debug.ok(), ErrorOf(debug));
  REQUIRE_FALSE(release.ok());
  CHECK(release.error().message.find("BLUE_SHADER_DEBUG") !=
        std::string::npos);
}

SDL_EnumerationResult CollectName(void* userdata, const char* /*dirname*/,
                                  const char* name) {
  static_cast<std::vector<std::string>*>(userdata)->emplace_back(name);
  return SDL_ENUM_CONTINUE;
}

// Guards the repository: every shipped shader must compile. Files ending in
// .h are include-only and are compiled through their includers.
TEST_CASE("every shader in the repository compiles in both modes") {
  std::vector<std::string> names;
  REQUIRE(SDL_EnumerateDirectory(BLUE_SHADER_SOURCE_DIR, CollectName, &names));
  size_t compiled_count = 0;
  for (const std::string& name : names) {
    if (!StageForPath(name).has_value()) {
      continue;
    }
    const std::string path = core::JoinPath(BLUE_SHADER_SOURCE_DIR, name);
    for (const CompileMode mode :
         {CompileMode::kDebug, CompileMode::kRelease}) {
      const auto result = ShaderCompiler(mode).Compile(path);
      CHECK_MESSAGE(result.ok(), name, ": ", ErrorOf(result));
    }
    ++compiled_count;
  }
  CHECK(compiled_count >= 2);
}

}  // namespace
}  // namespace blue::shader::metal
