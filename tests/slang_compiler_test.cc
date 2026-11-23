// The runtime shader compiler (hot reload): what a reload does when a source
// is edited, breaks and is fixed again. No GPU.

#include "shader/slang_compiler.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/path.h"
#include "platform/file_system.h"
#include "render/shader_manifest.h"
#include "shader/spirv_blob.h"
#include "tests/scratch_directory.h"

namespace blue::shader {
namespace {

constexpr uint32_t kSpirvMagic = 0x07230203;

using ScratchDirectory = testing::ScratchDirectory;

constexpr std::string_view kProgram =
    "#include \"shared.slang\"\n"
    "struct Varyings {\n"
    "    float4 position : SV_Position;\n"
    "    float3 color : COLOR;\n"
    "};\n"
    "[shader(\"vertex\")]\n"
    "Varyings VertexMain([[vk::location(0)]] float3 position : POSITION)\n"
    "{\n"
    "    Varyings output;\n"
    "    output.position = float4(position, 1.0);\n"
    "    output.color = Tint() * float(SCALE);\n"
    "    return output;\n"
    "}\n"
    "[shader(\"fragment\")]\n"
    "float4 FragmentMain(Varyings input) : SV_Target\n"
    "{\n"
    "    return float4(input.color, 1.0);\n"
    "}\n";

constexpr std::string_view kShared =
    "float3 Tint() { return float3(1.0, 0.5, 0.25); }\n";

// Writes `text` to `path`, creating parent directories.
void WriteText(const std::string& path, std::string_view text) {
  std::vector<std::byte> bytes;
  for (const char c : text) {
    bytes.push_back(static_cast<std::byte>(c));
  }
  REQUIRE(platform::WriteFileAtomic(path, bytes).ok());
}

// A scratch directory with the program above; returns the program's path.
std::string WriteProgram(const ScratchDirectory& dir) {
  WriteText(dir.Child("shared.slang"), kShared);
  WriteText(dir.Child("program.slang"), kProgram);
  return dir.Child("program.slang");
}

constexpr std::array<std::string_view, 1> kScaleOne = {"SCALE=1"};
constexpr std::array<std::string_view, 1> kScaleTwo = {"SCALE=2"};

SlangRequest Vertex(const std::string& path) {
  return {.source_path = path,
          .entry_point = "VertexMain",
          .stage = ShaderStage::kVertex,
          .target = ShaderTarget::kSpirv,
          .defines = kScaleOne};
}

bool Contains(const std::vector<std::string>& files, const std::string& path) {
  return std::find(files.begin(), files.end(), core::NormalizePath(path)) !=
         files.end();
}

TEST_CASE("compiles a vertex and a fragment entry point to SPIR-V") {
  const ScratchDirectory dir("slang_basic");
  const std::string path = WriteProgram(dir);
  const SlangCompiler compiler(CompileMode::kRelease);

  const auto vertex = compiler.Compile(Vertex(path));
  REQUIRE_MESSAGE(vertex.ok(), vertex.error().message);
  std::vector<uint32_t> words = ToSpirvWords(vertex.value().code);
  REQUIRE_FALSE(words.empty());
  CHECK(words[0] == kSpirvMagic);
  // What the Vulkan backend does with every vertex shader.
  CHECK(MarkPositionInvariant(words));

  SlangRequest fragment = Vertex(path);
  fragment.entry_point = "FragmentMain";
  fragment.stage = ShaderStage::kFragment;
  const auto compiled = compiler.Compile(fragment);
  REQUIRE_MESSAGE(compiled.ok(), compiled.error().message);
  words = ToSpirvWords(compiled.value().code);
  REQUIRE_FALSE(words.empty());
  CHECK(words[0] == kSpirvMagic);
  CHECK_FALSE(MarkPositionInvariant(words));
}

TEST_CASE("the result lists the source and the files it includes") {
  const ScratchDirectory dir("slang_files");
  const std::string path = WriteProgram(dir);
  const SlangCompiler compiler(CompileMode::kRelease);
  const auto compiled = compiler.Compile(Vertex(path));
  REQUIRE_MESSAGE(compiled.ok(), compiled.error().message);
  const std::vector<std::string>& files = compiled.value().files;
  CHECK(Contains(files, path));
  CHECK(Contains(files, dir.Child("shared.slang")));
  CHECK(files.size() == 2);
}

TEST_CASE("defines select the variant") {
  const ScratchDirectory dir("slang_defines");
  const std::string path = WriteProgram(dir);
  const SlangCompiler compiler(CompileMode::kRelease);
  const auto one = compiler.Compile(Vertex(path));
  SlangRequest request = Vertex(path);
  request.defines = kScaleTwo;
  const auto two = compiler.Compile(request);
  REQUIRE(one.ok());
  REQUIRE(two.ok());
  CHECK(one.value().code != two.value().code);
  // Without the define the source does not compile.
  request.defines = {};
  CHECK_FALSE(compiler.Compile(request).ok());
}

TEST_CASE("a reload sees the edited file, an error and the fix") {
  const ScratchDirectory dir("slang_reload");
  const std::string path = WriteProgram(dir);
  const SlangCompiler compiler(CompileMode::kRelease);
  const auto first = compiler.Compile(Vertex(path));
  REQUIRE_MESSAGE(first.ok(), first.error().message);

  // Edit the included file: the same compiler object must read it again.
  WriteText(dir.Child("shared.slang"),
            "float3 Tint() { return float3(0.125, 0.75, 1.0); }\n");
  const auto edited = compiler.Compile(Vertex(path));
  REQUIRE_MESSAGE(edited.ok(), edited.error().message);
  CHECK(edited.value().code != first.value().code);

  // Break it: the error names the file and the line.
  WriteText(dir.Child("shared.slang"),
            "float3 Tint() {\n    return float3(1.0, 0.5;\n}\n");
  const auto broken = compiler.Compile(Vertex(path));
  REQUIRE_FALSE(broken.ok());
  CHECK(broken.error().message.find("shared.slang") != std::string::npos);
  CHECK(broken.error().message.find("shared.slang:2:") != std::string::npos);
  CHECK(broken.error().message.find("error") != std::string::npos);

  // Fix it: back to the first result.
  WriteText(dir.Child("shared.slang"), kShared);
  const auto fixed = compiler.Compile(Vertex(path));
  REQUIRE_MESSAGE(fixed.ok(), fixed.error().message);
  CHECK(fixed.value().code == first.value().code);
}

TEST_CASE("warnings are errors") {
  const ScratchDirectory dir("slang_warnings");
  WriteText(dir.Child("warn.slang"),
            "[shader(\"vertex\")]\n"
            "float4 VertexMain() : SV_Position\n"
            "{\n"
            "    bool flag = 1;\n"  // implicit int -> bool: a warning
            "    return float4(flag ? 1.0 : 0.0);\n"
            "}\n");
  const SlangCompiler compiler(CompileMode::kRelease);
  const auto compiled = compiler.Compile(Vertex(dir.Child("warn.slang")));
  REQUIRE_FALSE(compiled.ok());
  CHECK(compiled.error().message.find("warn.slang") != std::string::npos);
}

TEST_CASE("missing things are errors that name the file") {
  const ScratchDirectory dir("slang_missing");
  const std::string path = WriteProgram(dir);
  const SlangCompiler compiler(CompileMode::kRelease);

  SlangRequest request = Vertex(path);
  request.entry_point = "NoSuchEntry";
  auto compiled = compiler.Compile(request);
  REQUIRE_FALSE(compiled.ok());
  CHECK_FALSE(compiled.error().message.empty());

  compiled = compiler.Compile(Vertex(dir.Child("absent.slang")));
  REQUIRE_FALSE(compiled.ok());
  CHECK(compiled.error().message.find("absent") != std::string::npos);

  WriteText(dir.Child("include.slang"),
            "#include \"nowhere.slang\"\n"
            "[shader(\"vertex\")]\n"
            "float4 VertexMain() : SV_Position { return float4(0.0); }\n");
  compiled = compiler.Compile(Vertex(dir.Child("include.slang")));
  REQUIRE_FALSE(compiled.ok());
  CHECK(compiled.error().message.find("nowhere.slang") != std::string::npos);
}

TEST_CASE("debug mode keeps debug information") {
  const ScratchDirectory dir("slang_modes");
  const std::string path = WriteProgram(dir);
  const auto release = SlangCompiler(CompileMode::kRelease).Compile(Vertex(path));
  const auto debug = SlangCompiler(CompileMode::kDebug).Compile(Vertex(path));
  REQUIRE_MESSAGE(release.ok(), release.error().message);
  REQUIRE_MESSAGE(debug.ok(), debug.error().message);
  CHECK(debug.value().code.size() > release.value().code.size());
}

#if BLUE_SHADERS_HAVE_SPIRV
// The flags of the runtime compiler are the build's: a hot reload of an
// unchanged source must give the pipeline the shader it already has.
TEST_CASE("every program of the repository compiles to the build's SPIR-V") {
  const SlangCompiler compiler(CompileMode::kRelease);
  for (const render::ShaderProgramInfo& program : render::kShaderPrograms) {
    for (const ShaderStage stage :
         {ShaderStage::kVertex, ShaderStage::kFragment}) {
      const bool vertex = stage == ShaderStage::kVertex;
      const std::string_view entry =
          vertex ? program.vertex_entry : program.fragment_entry;
      if (entry.empty()) {
        continue;
      }
      CAPTURE(program.name);
      CAPTURE(entry);
      const auto compiled = compiler.Compile({
          .source_path = core::JoinPath(BLUE_SHADER_SOURCE_DIR, program.source),
          .entry_point = std::string(entry),
          .stage = stage,
          .target = ShaderTarget::kSpirv,
          .defines = program.defines,
      });
      REQUIRE_MESSAGE(compiled.ok(), compiled.error().message);
      const auto blob = platform::ReadFile(
          std::string(BLUE_SHADER_BLOB_DIR) + "/spirv/" +
              std::string(program.name) + (vertex ? ".vert.spv" : ".frag.spv"),
          size_t{4} << 20);
      REQUIRE(blob.ok());
      CHECK(compiled.value().code == blob.value());
    }
  }
}
#endif

}  // namespace
}  // namespace blue::shader
