#ifndef BLUE_SHADER_SLANG_COMPILER_H_
#define BLUE_SHADER_SLANG_COMPILER_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"
#include "shader/compile_mode.h"
#include "shader/shader_preprocessor.h"

// Compiles Slang shaders at run time (hot reload) with the Slang library of
// the Vulkan SDK, with the flags the build uses (cmake/Shaders.cmake).
// Nothing here needs a GPU device, so it works in unit tests and tools. Only
// built with BLUE_SHADER_HOT_RELOAD; shipped builds load the build's blobs.

namespace blue::shader {

// The code a shader is compiled to.
enum class ShaderTarget : uint8_t {
  kSpirv,  // SPIR-V 1.6 (Vulkan 1.3).
  kDxil,   // DXIL, Shader Model 6.0 (Direct3D 12).
};

// One entry point of one source file (ShaderStage and ShaderError are the
// ones of shader/shader_preprocessor.h).
struct SlangRequest {
  // The .slang file; its directory is also where #include looks.
  std::string source_path;
  std::string entry_point;
  ShaderStage stage = ShaderStage::kVertex;
  ShaderTarget target = ShaderTarget::kSpirv;
  // Preprocessor defines, each "NAME=VALUE" (or "NAME" for 1).
  std::span<const std::string_view> defines;
};

struct CompiledSlang {
  // SPIR-V (a multiple of 4 bytes) or a DXIL container.
  std::vector<std::byte> code;
  // Every file the result was made from: the source file and what it
  // includes, as normalized paths without duplicates.
  std::vector<std::string> files;
};

// Compiles single entry points of Slang files with column-major matrices and
// every warning an error; CompileMode picks -O2 or -O0 with debug info. The
// message of a failure is the compiler's text: "error[...]: ..." followed by
// " --> <file>:<line>:<column>" and the source line, possibly several times.
//
// Thread safety: not thread-safe. Use one compiler per thread. Neither
// copyable nor movable.
class SlangCompiler {
 public:
  explicit SlangCompiler(CompileMode mode);
  ~SlangCompiler();

  SlangCompiler(const SlangCompiler&) = delete;
  SlangCompiler& operator=(const SlangCompiler&) = delete;

  [[nodiscard]] core::Result<CompiledSlang, ShaderError> Compile(
      const SlangRequest& request) const;

 private:
  class Impl;

  std::unique_ptr<Impl> impl_;
};

}  // namespace blue::shader

#endif  // BLUE_SHADER_SLANG_COMPILER_H_
