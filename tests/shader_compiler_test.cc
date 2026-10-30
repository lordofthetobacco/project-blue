#include "shader/shader_compiler.h"

#include <SDL3/SDL_filesystem.h>
#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/path.h"
#include "platform/file_system.h"
#include "tests/scratch_directory.h"

namespace blue::shader {
namespace {

constexpr uint32_t kSpirvMagic = 0x07230203;
constexpr uint32_t kOpLine = 8;
constexpr size_t kSpirvHeaderWords = 5;

constexpr std::string_view kValidVertex =
    "#version 450\n"
    "layout(location = 0) in vec3 position;\n"
    "void main() { gl_Position = vec4(position, 1.0); }\n";

constexpr std::string_view kValidFragment =
    "#version 450\n"
    "layout(location = 0) out vec4 color;\n"
    "void main() { color = vec4(1.0); }\n";

using ScratchDirectory = testing::ScratchDirectory;

// Writes `text` to `path`, creating parent directories.
void WriteText(const std::string& path, std::string_view text) {
  std::vector<std::byte> bytes;
  for (const char c : text) {
    bytes.push_back(static_cast<std::byte>(c));
  }
  REQUIRE(platform::WriteFileAtomic(path, bytes).ok());
}

// Returns true if the SPIR-V module contains an instruction with `opcode`.
// Walks the instruction stream (first word: word count << 16 | opcode)
// instead of searching raw words, so operands cannot produce false hits.
bool HasOpcode(const std::vector<uint32_t>& spirv, uint32_t opcode) {
  size_t i = kSpirvHeaderWords;
  while (i < spirv.size()) {
    const uint32_t word_count = spirv[i] >> 16;
    if ((spirv[i] & 0xFFFFu) == opcode) {
      return true;
    }
    if (word_count == 0) {
      return false;  // Malformed; avoid looping forever.
    }
    i += word_count;
  }
  return false;
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

TEST_CASE("compiles a vertex and a fragment shader to SPIR-V") {
  ScratchDirectory dir("shader_valid");
  for (const CompileMode mode : {CompileMode::kDebug, CompileMode::kRelease}) {
    const auto vertex = CompileText(dir, "a.vert", kValidVertex, mode);
    REQUIRE_MESSAGE(vertex.ok(), ErrorOf(vertex));
    REQUIRE_FALSE(vertex.value().spirv.empty());
    CHECK(vertex.value().spirv[0] == kSpirvMagic);
    CHECK(vertex.value().dependencies.empty());

    const auto fragment = CompileText(dir, "a.frag", kValidFragment, mode);
    REQUIRE_MESSAGE(fragment.ok(), ErrorOf(fragment));
    CHECK(fragment.value().spirv[0] == kSpirvMagic);
  }
}

TEST_CASE("compiles a compute shader") {
  ScratchDirectory dir("shader_comp");
  const auto result = CompileText(dir, "a.comp",
                                  "#version 450\n"
                                  "layout(local_size_x = 8) in;\n"
                                  "void main() {}\n");
  REQUIRE_MESSAGE(result.ok(), ErrorOf(result));
  CHECK(result.value().spirv[0] == kSpirvMagic);
}

TEST_CASE("a syntax error names the file and the line") {
  ScratchDirectory dir("shader_syntax");
  const auto result = CompileText(dir, "broken.frag",
                                  "#version 450\n"
                                  "layout(location = 0) out vec4 color;\n"
                                  "void main() {\n"
                                  "  color = vec4(1.0)\n"  // Missing ';'.
                                  "}\n");
  REQUIRE_FALSE(result.ok());
  const std::string& message = result.error().message;
  CHECK(message.find("broken.frag") != std::string::npos);
  CHECK(message.find(":5:") != std::string::npos);
  CHECK(message.find("error") != std::string::npos);
}

TEST_CASE("warnings are errors") {
  ScratchDirectory dir("shader_warning");
  // Enabling an unknown extension is a warning in glslang.
  const auto result = CompileText(dir, "warn.frag",
                                  "#version 450\n"
                                  "#extension GL_BLUE_unknown : enable\n"
                                  "layout(location = 0) out vec4 color;\n"
                                  "void main() { color = vec4(1.0); }\n");
  CHECK_FALSE(result.ok());
}

TEST_CASE("resolves includes relative to the including file") {
  ScratchDirectory dir("shader_include");
  WriteText(dir.Child("common.glsl"),
            "#ifndef COMMON_GLSL\n"
            "#define COMMON_GLSL\n"
            "const float kScale = 2.0;\n"
            "#endif\n");
  WriteText(dir.Child("lib/helper.glsl"),
            "#include \"../common.glsl\"\n"
            "float Scaled(float x) { return x * kScale; }\n");
  const auto result =
      CompileText(dir, "shaders/main.frag",
                  "#version 450\n"
                  "#include \"../lib/helper.glsl\"\n"
                  "#include \"../common.glsl\"\n"
                  "layout(location = 0) out vec4 color;\n"
                  "void main() { color = vec4(Scaled(0.5)); }\n");
  REQUIRE_MESSAGE(result.ok(), ErrorOf(result));

  std::vector<std::string> dependencies = result.value().dependencies;
  std::sort(dependencies.begin(), dependencies.end());
  const std::vector<std::string> expected = {dir.Child("common.glsl"),
                                             dir.Child("lib/helper.glsl")};
  CHECK(dependencies == expected);
  for (const std::string& dependency : dependencies) {
    CHECK(dependency == core::NormalizePath(dependency));
  }
}

TEST_CASE("a missing include is an error naming the file") {
  ScratchDirectory dir("shader_missing_include");
  const auto result = CompileText(dir, "main.vert",
                                  "#version 450\n"
                                  "#include \"does_not_exist.glsl\"\n"
                                  "void main() {}\n");
  REQUIRE_FALSE(result.ok());
  CHECK(result.error().message.find("does_not_exist.glsl") !=
        std::string::npos);
}

TEST_CASE("an unknown extension is an error") {
  ScratchDirectory dir("shader_extension");
  const auto result = CompileText(dir, "shader.txt", kValidVertex);
  REQUIRE_FALSE(result.ok());
  CHECK(result.error().message.find("shader.txt") != std::string::npos);
}

TEST_CASE("a missing file is an error") {
  ScratchDirectory dir("shader_missing_file");
  const auto result =
      ShaderCompiler(CompileMode::kDebug).Compile(dir.Child("nope.vert"));
  REQUIRE_FALSE(result.ok());
  CHECK(result.error().message.find("nope.vert") != std::string::npos);
}

// kDebug keeps line information (OpLine) and does not optimize; kRelease
// strips the line information and removes dead code, so its module is
// smaller for a shader that computes values it never uses.
TEST_CASE("debug and release modes differ in debug info and size") {
  ScratchDirectory dir("shader_modes");
  constexpr std::string_view kWithDeadCode =
      "#version 450\n"
      "layout(location = 0) in vec3 position;\n"
      "void main() {\n"
      "  float unused = dot(position, position) * 3.0;\n"
      "  unused += sin(unused);\n"
      "  gl_Position = vec4(position, 1.0);\n"
      "}\n";
  const auto debug =
      CompileText(dir, "dead.vert", kWithDeadCode, CompileMode::kDebug);
  const auto release =
      CompileText(dir, "dead.vert", kWithDeadCode, CompileMode::kRelease);
  REQUIRE_MESSAGE(debug.ok(), ErrorOf(debug));
  REQUIRE_MESSAGE(release.ok(), ErrorOf(release));

  CHECK(HasOpcode(debug.value().spirv, kOpLine));
  CHECK_FALSE(HasOpcode(release.value().spirv, kOpLine));
  CHECK(release.value().spirv.size() < debug.value().spirv.size());
}

SDL_EnumerationResult CollectName(void* userdata, const char* /*dirname*/,
                                  const char* name) {
  static_cast<std::vector<std::string>*>(userdata)->emplace_back(name);
  return SDL_ENUM_CONTINUE;
}

// Guards the repository: every shipped shader must compile. Files ending in
// .glsl are include-only and are compiled through their includers.
TEST_CASE("every shader in the repository compiles in both modes") {
  std::vector<std::string> names;
  REQUIRE(SDL_EnumerateDirectory(BLUE_SHADER_SOURCE_DIR, CollectName, &names));
  size_t compiled_count = 0;
  for (const std::string& name : names) {
    const std::string extension = name.substr(name.rfind('.') + 1);
    if (extension != "vert" && extension != "frag" && extension != "comp") {
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
}  // namespace blue::shader
