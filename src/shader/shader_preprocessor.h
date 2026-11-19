#ifndef BLUE_SHADER_SHADER_PREPROCESSOR_H_
#define BLUE_SHADER_SHADER_PREPROCESSOR_H_

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"

// Flattens a Metal Shading Language source and the project headers it
// includes into one string, ready for the runtime Metal compiler (which has
// no file system access). Pure C++: needs no Metal device.

namespace blue::shader {

enum class ShaderStage {
  kVertex,    // "<name>.vert.metal", entry point VertexMain.
  kFragment,  // "<name>.frag.metal", entry point FragmentMain.
  kCompute,   // "<name>.comp.metal", entry point ComputeMain.
};

struct ShaderError {
  // The compiler's text, normally "<file>:<line>:<column>: error: ...",
  // possibly several lines. Also used for unreadable files, unsupported
  // stages and missing entry points; it always names the file.
  std::string message;
};

struct PreprocessedShader {
  // The flattened source, with #line directives that map every line back to
  // the file it came from.
  std::string source;
  // Files pulled in through #include, as normalized paths without
  // duplicates, in the order they were first included. The root file itself
  // is not listed.
  std::vector<std::string> dependencies;
  ShaderStage stage = ShaderStage::kVertex;
};

// Returns the stage named by the file name of `path` (".vert.metal",
// ".frag.metal" or ".comp.metal"), or nullopt for any other name.
[[nodiscard]] std::optional<ShaderStage> StageForPath(std::string_view path);

// Returns the name of the function a shader of `stage` must define.
[[nodiscard]] std::string_view EntryPointName(ShaderStage stage);

// Reads the shader at `path` and replaces each `#include "name"` line with
// the contents of `name`, resolved relative to the directory of the
// including file. `#include <name>` lines (e.g. <metal_stdlib>) are left for
// the Metal compiler. Every file is included at most once (later includes of
// it become blank lines) and includes nest at most 32 deep. Each file is
// read with a 1 MiB limit.
//
// The output starts every file's text with `#line 1 "<path>"` and continues
// the including file with `#line <next line> "<path>"`, so compiler messages
// name the original file and line.
//
// Include lines are recognized textually: an #include inside a block comment
// or a disabled #if branch is still expanded.
[[nodiscard]] core::Result<PreprocessedShader, ShaderError> PreprocessShader(
    const std::string& path);

}  // namespace blue::shader

#endif  // BLUE_SHADER_SHADER_PREPROCESSOR_H_
