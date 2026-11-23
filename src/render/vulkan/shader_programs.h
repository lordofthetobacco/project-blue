#ifndef BLUE_RENDER_VULKAN_SHADER_PROGRAMS_H_
#define BLUE_RENDER_VULKAN_SHADER_PROGRAMS_H_

#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"
#include "shader/shader_compiler.h"
#include "shader/shader_watcher.h"

namespace blue::render::vulkan {

// Registry of the renderer's shader programs (a vertex shader plus an
// optional fragment shader, each compiled from a GLSL file) with hot reload.
//
// Each program is registered together with a factory that (re)creates the
// pipeline that uses it from SPIR-V. Add() compiles the program and creates
// the pipeline immediately. ReloadChanged() polls the watched source files,
// recompiles the programs whose sources (or includes) changed and recreates
// their pipelines. A program is always recompiled as a whole, so a failed
// edit in one stage is never masked by a later edit in the other.
//
// If a recompile fails, the old pipeline stays in use and the compiler
// message is kept for last_error() until a later reload of that same program
// succeeds. A pipeline factory that fails is fatal like any other Vulkan
// error: a shader that compiles but no longer matches the pipeline's vertex
// layout is not detected here.
//
// Borrows nothing but the device handle, which must outlive this object. Not
// thread-safe; call it from the thread that records frames, between frames.
class ShaderPrograms {
 public:
  // Creates (or replaces) a pipeline from SPIR-V. The spans are only valid for
  // the duration of the call; `fragment` is empty for programs without a
  // fragment stage. Replacing the pipeline destroys the old one; the GPU is
  // idle when that happens.
  using PipelineFactory = std::function<void(
      std::span<const uint32_t> vertex, std::span<const uint32_t> fragment)>;

  ShaderPrograms(VkDevice device, shader::CompileMode mode);

  ShaderPrograms(const ShaderPrograms&) = delete;
  ShaderPrograms& operator=(const ShaderPrograms&) = delete;

  // Compiles the program from `vertex_path` and `fragment_path` (empty for
  // none), calls `create` with the result and starts watching the sources.
  // A compile error is fatal: there is no previous pipeline to keep.
  void Add(const std::string& vertex_path, const std::string& fragment_path,
           PipelineFactory create);

  // Recompiles the programs whose sources changed since the last call (see
  // shader::ShaderWatcher for what counts as a change) and recreates their
  // pipelines, waiting for the GPU to go idle once if any pipeline is
  // replaced. Cheap when nothing changed.
  void ReloadChanged();

  // Returns the compiler message of the most recent failed reload that has
  // not been cleared by a later successful reload of the same program, or an
  // empty view. Valid until the next call to Add() or ReloadChanged().
  [[nodiscard]] std::string_view last_error() const;

 private:
  struct Program {
    std::string vertex_path;
    std::string fragment_path;
    PipelineFactory create;
    std::vector<uint32_t> vertex_spirv;
    std::vector<uint32_t> fragment_spirv;
    // The message of the last failed reload; empty while the program is fine.
    std::string error;
    // Orders failures: the highest one is the "most recent". Zero = no error.
    uint64_t error_serial = 0;
  };

  // The successful result of compiling every stage of a program.
  struct CompiledProgram {
    shader::CompiledShader vertex;
    shader::CompiledShader fragment;  // Empty spirv without a fragment stage.
  };

  [[nodiscard]] core::Result<CompiledProgram, shader::ShaderError>
  CompileProgram(const Program& program) const;
  // Makes `compiled` the program's current shaders: creates the pipeline,
  // starts watching the sources and clears the error.
  void Apply(Program& program, CompiledProgram compiled);

  VkDevice device_;
  shader::ShaderCompiler compiler_;
  shader::ShaderWatcher watcher_;
  std::vector<Program> programs_;
  uint64_t next_error_serial_ = 1;
};

}  // namespace blue::render::vulkan

#endif  // BLUE_RENDER_VULKAN_SHADER_PROGRAMS_H_
