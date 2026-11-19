#ifndef BLUE_SHADER_SHADER_COMPILER_H_
#define BLUE_SHADER_SHADER_COMPILER_H_

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"
#include "shader/shader_preprocessor.h"

// Compiles Metal Shading Language files at run time with the system's Metal
// compiler (-[MTLDevice newLibraryWithSource:options:error:]). The header is
// pure C++; Objective-C++ code gets at the Metal objects through
// shader/shader_library_metal.h.

namespace blue::shader {

enum class CompileMode {
  kDebug,    // Defines BLUE_SHADER_DEBUG for the shader source.
  kRelease,  // No extra macros.
};

// Owns one compiled Metal library (id<MTLLibrary>). Empty when default
// constructed or moved from. Move-only.
class ShaderLibrary {
 public:
  ShaderLibrary() = default;
  // Takes over a library retained with CFBridgingRetain (a +1 reference).
  // Used by shader_compiler.mm.
  explicit ShaderLibrary(void* retained_library)
      : library_(retained_library) {}
  ~ShaderLibrary();

  ShaderLibrary(ShaderLibrary&& other) noexcept;
  ShaderLibrary& operator=(ShaderLibrary&& other) noexcept;
  ShaderLibrary(const ShaderLibrary&) = delete;
  ShaderLibrary& operator=(const ShaderLibrary&) = delete;

  explicit operator bool() const { return library_ != nullptr; }

  // Returns true when the library defines a function called `name`. False
  // for an empty library.
  [[nodiscard]] bool HasFunction(std::string_view name) const;

  // The borrowed id<MTLLibrary>, or nullptr. Use MetalLibrary() from
  // shader/shader_library_metal.h instead of casting this.
  [[nodiscard]] void* handle() const { return library_; }

 private:
  void* library_ = nullptr;
};

struct CompiledShader {
  ShaderLibrary library;
  // Files pulled in through #include, as normalized paths without
  // duplicates. The compiled file itself is not listed.
  std::vector<std::string> dependencies;
  ShaderStage stage = ShaderStage::kVertex;
};

// Compiles single-stage MSL (language version 3.2, safe math, precise
// floating-point functions, invariance preserved) into a Metal library. The
// stage comes from the file name: "<name>.vert.metal", ".frag.metal" or
// ".comp.metal"; the file must define the stage's entry point (VertexMain,
// FragmentMain or ComputeMain, see EntryPointName()). Includes are flattened
// by PreprocessShader() first, so `#include "name"` resolves relative to the
// including file. A warning reported together with a library counts as an
// error, although the runtime compiler does not report most warnings.
//
// Thread safety: not thread-safe. Use one compiler per thread. Neither
// copyable nor movable.
class ShaderCompiler {
 public:
  // Uses the system default Metal device. Without one, every Compile() call
  // returns an error naming "no Metal device".
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
