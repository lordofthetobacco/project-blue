#ifndef BLUE_SHADER_SHADER_COMPILER_H_
#define BLUE_SHADER_SHADER_COMPILER_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/result.h"

// Compiles GLSL files to SPIR-V at run time with shaderc. Nothing here needs
// a Vulkan device, so it works in unit tests and tools.

namespace blue::shader {

enum class CompileMode {
  kDebug,    // No optimization, debug info (OpLine/OpSource) in the output.
  kRelease,  // Full performance optimization, no debug info.
};

struct CompiledShader {
  std::vector<uint32_t> spirv;
  // Files pulled in through #include, as normalized paths without
  // duplicates. The compiled file itself is not listed.
  std::vector<std::string> dependencies;
};

struct ShaderError {
  // The compiler's text, normally "<file>:<line>: error: ...", possibly
  // several lines. Also used for unreadable files and unsupported stages.
  std::string message;
};

// Compiles single-stage GLSL for Vulkan 1.3 / SPIR-V 1.6, with warnings
// treated as errors. The stage comes from the file extension: ".vert",
// ".frag" or ".comp".
//
// `#include "name"` is resolved relative to the directory of the including
// file; `#include <name>` is resolved the same way. Sources do not need the
// GL_GOOGLE_include_directive extension line.
//
// Thread safety: not thread-safe. Use one compiler per thread. Neither
// copyable nor movable.
class ShaderCompiler {
 public:
  explicit ShaderCompiler(CompileMode mode);
  ~ShaderCompiler();

  ShaderCompiler(const ShaderCompiler&) = delete;
  ShaderCompiler& operator=(const ShaderCompiler&) = delete;

  // Reads and compiles the file at `path` (at most 1 MiB, like every
  // included file). Returns the compiler's message on any failure.
  [[nodiscard]] core::Result<CompiledShader, ShaderError> Compile(
      const std::string& path) const;

 private:
  class Impl;

  std::unique_ptr<Impl> impl_;
};

}  // namespace blue::shader

#endif  // BLUE_SHADER_SHADER_COMPILER_H_
