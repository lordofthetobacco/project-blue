#include "shader/shader_preprocessor.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "core/path.h"
#include "platform/file_system.h"
#include "tests/scratch_directory.h"

namespace blue::shader {
namespace {

using ScratchDirectory = testing::ScratchDirectory;

// Writes `text` to `path`, creating parent directories.
void WriteText(const std::string& path, std::string_view text) {
  std::vector<std::byte> bytes;
  for (const char c : text) {
    bytes.push_back(static_cast<std::byte>(c));
  }
  REQUIRE(platform::WriteFileAtomic(path, bytes).ok());
}

// Returns the message of a failed result, or "" for a success, for use as an
// assertion message.
std::string ErrorOf(
    const core::Result<PreprocessedShader, ShaderError>& result) {
  return result.ok() ? std::string() : result.error().message;
}

// Returns how often `needle` occurs in `text`.
size_t CountOf(std::string_view text, std::string_view needle) {
  size_t count = 0;
  for (size_t at = text.find(needle); at != std::string_view::npos;
       at = text.find(needle, at + needle.size())) {
    ++count;
  }
  return count;
}

TEST_CASE("the stage comes from the file name") {
  CHECK(StageForPath("/a/b/fullscreen.vert.metal") == ShaderStage::kVertex);
  CHECK(StageForPath("present.frag.metal") == ShaderStage::kFragment);
  CHECK(StageForPath("c/cull.comp.metal") == ShaderStage::kCompute);
  CHECK_FALSE(StageForPath("present.frag").has_value());
  CHECK_FALSE(StageForPath("present.metal").has_value());
  CHECK_FALSE(StageForPath(".frag.metal").has_value());
  CHECK_FALSE(StageForPath("sanitize.h").has_value());

  CHECK(EntryPointName(ShaderStage::kVertex) == "VertexMain");
  CHECK(EntryPointName(ShaderStage::kFragment) == "FragmentMain");
  CHECK(EntryPointName(ShaderStage::kCompute) == "ComputeMain");
}

TEST_CASE("emits #line at the start of every file and after each include") {
  ScratchDirectory dir("preprocess_line");
  const std::string root = dir.Child("main.frag.metal");
  const std::string header = dir.Child("a.h");
  WriteText(header, "float a;\n");
  WriteText(root,
            "#include <metal_stdlib>\n"
            "#include \"a.h\"\n"
            "float x;\n");
  const auto result = PreprocessShader(root);
  REQUIRE_MESSAGE(result.ok(), ErrorOf(result));
  CHECK(result.value().source == "#line 1 \"" + root +
                                     "\"\n"
                                     "#include <metal_stdlib>\n"
                                     "#line 1 \"" +
                                     header +
                                     "\"\n"
                                     "float a;\n"
                                     "#line 3 \"" +
                                     root +
                                     "\"\n"
                                     "float x;\n");
  CHECK(result.value().stage == ShaderStage::kFragment);
}

TEST_CASE("angle-bracket includes pass through untouched") {
  ScratchDirectory dir("preprocess_angle");
  const std::string root = dir.Child("main.vert.metal");
  WriteText(root,
            "#include <metal_stdlib>\n"
            "  #  include <simd/simd.h>\n");
  const auto result = PreprocessShader(root);
  REQUIRE_MESSAGE(result.ok(), ErrorOf(result));
  CHECK(result.value().source.find("#include <metal_stdlib>\n") !=
        std::string::npos);
  CHECK(result.value().source.find("  #  include <simd/simd.h>\n") !=
        std::string::npos);
  CHECK(result.value().dependencies.empty());
}

TEST_CASE("resolves includes relative to the including file") {
  ScratchDirectory dir("preprocess_relative");
  WriteText(dir.Child("common.h"), "constant float kScale = 2.0;\n");
  WriteText(dir.Child("lib/helper.h"),
            "#include \"../common.h\"\n"
            "float Scaled(float x) { return x * kScale; }\n");
  const std::string root = dir.Child("shaders/main.frag.metal");
  WriteText(root,
            "#include \"../lib/helper.h\"\n"
            "  #  include   \"../common.h\"\n");
  const auto result = PreprocessShader(root);
  REQUIRE_MESSAGE(result.ok(), ErrorOf(result));

  const std::string& source = result.value().source;
  CHECK(source.find("kScale = 2.0") != std::string::npos);
  CHECK(source.find("Scaled(float x)") != std::string::npos);

  std::vector<std::string> dependencies = result.value().dependencies;
  std::sort(dependencies.begin(), dependencies.end());
  const std::vector<std::string> expected = {dir.Child("common.h"),
                                             dir.Child("lib/helper.h")};
  CHECK(dependencies == expected);
  for (const std::string& dependency : dependencies) {
    CHECK(dependency == core::NormalizePath(dependency));
  }
}

TEST_CASE("includes every file at most once") {
  ScratchDirectory dir("preprocess_once");
  WriteText(dir.Child("a.h"), "float OnlyOnce();\n");
  WriteText(dir.Child("b.h"),
            "#include \"a.h\"\n"
            "#include \"main.frag.metal\"\n");
  const std::string root = dir.Child("main.frag.metal");
  WriteText(root,
            "#include \"a.h\"\n"
            "#include \"b.h\"\n"
            "#include \"./a.h\"\n"
            "float y;\n");
  const auto result = PreprocessShader(root);
  REQUIRE_MESSAGE(result.ok(), ErrorOf(result));

  const std::string& source = result.value().source;
  CHECK(CountOf(source, "float OnlyOnce();") == 1);
  CHECK(CountOf(source, "float y;") == 1);
  // The skipped include on line 3 becomes a blank line, so "float y;" is
  // still line 4.
  CHECK(source.find("#line 3 \"" + root + "\"\n\nfloat y;\n") !=
        std::string::npos);
  const std::vector<std::string> expected = {dir.Child("a.h"),
                                             dir.Child("b.h")};
  CHECK(result.value().dependencies == expected);
}

TEST_CASE("a missing include is an error naming both files") {
  ScratchDirectory dir("preprocess_missing_include");
  const std::string root = dir.Child("main.vert.metal");
  WriteText(root,
            "#include <metal_stdlib>\n"
            "#include \"does_not_exist.h\"\n");
  const auto result = PreprocessShader(root);
  REQUIRE_FALSE(result.ok());
  const std::string& message = result.error().message;
  CHECK(message.find("does_not_exist.h") != std::string::npos);
  CHECK(message.find(root + ":2:") != std::string::npos);
}

TEST_CASE("includes nest at most 32 deep") {
  // root -> h1.h -> ... -> h<levels>.h
  const auto build_chain = [](const ScratchDirectory& dir, int levels) {
    for (int i = 1; i <= levels; ++i) {
      const std::string next = i < levels ? "#include \"h" +
                                                std::to_string(i + 1) +
                                                ".h\"\n"
                                          : std::string("float deepest;\n");
      WriteText(dir.Child("h" + std::to_string(i) + ".h"), next);
    }
    const std::string root = dir.Child("main.frag.metal");
    WriteText(root, "#include \"h1.h\"\n");
    return root;
  };

  ScratchDirectory deep_enough("preprocess_depth_ok");
  const auto ok = PreprocessShader(build_chain(deep_enough, 32));
  REQUIRE_MESSAGE(ok.ok(), ErrorOf(ok));
  CHECK(ok.value().dependencies.size() == 32);
  CHECK(ok.value().source.find("float deepest;") != std::string::npos);

  ScratchDirectory too_deep("preprocess_depth_fail");
  const auto failed = PreprocessShader(build_chain(too_deep, 33));
  REQUIRE_FALSE(failed.ok());
  CHECK(failed.error().message.find("h33.h") != std::string::npos);
  CHECK(failed.error().message.find("32") != std::string::npos);
}

TEST_CASE("an unknown extension or a missing file is an error") {
  ScratchDirectory dir("preprocess_errors");
  WriteText(dir.Child("shader.txt"), "float x;\n");
  const auto extension = PreprocessShader(dir.Child("shader.txt"));
  REQUIRE_FALSE(extension.ok());
  CHECK(extension.error().message.find("shader.txt") != std::string::npos);

  const auto missing = PreprocessShader(dir.Child("nope.vert.metal"));
  REQUIRE_FALSE(missing.ok());
  CHECK(missing.error().message.find("nope.vert.metal") != std::string::npos);
}

TEST_CASE("a last line without a newline still ends its file") {
  ScratchDirectory dir("preprocess_newline");
  const std::string header = dir.Child("a.h");
  WriteText(header, "float a;");  // No trailing newline.
  const std::string root = dir.Child("main.frag.metal");
  WriteText(root, "#include \"a.h\"\nfloat b;");
  const auto result = PreprocessShader(root);
  REQUIRE_MESSAGE(result.ok(), ErrorOf(result));
  CHECK(result.value().source.find("float a;\n#line 2 \"" + root + "\"\n" +
                                   "float b;\n") != std::string::npos);
}

}  // namespace
}  // namespace blue::shader
