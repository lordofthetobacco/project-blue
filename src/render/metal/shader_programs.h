#ifndef BLUE_RENDER_METAL_SHADER_PROGRAMS_H_
#define BLUE_RENDER_METAL_SHADER_PROGRAMS_H_

#include "render/metal/metal_types.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"
#include "render/metal/metal_context.h"
#include "shader/metal/shader_compiler.h"
#include "shader/shader_watcher.h"

namespace blue::render::metal {

// Registry of the renderer's shader programs (a vertex shader plus an
// optional fragment shader, each compiled from an MSL file) with hot reload.
//
// Each program is registered together with a factory that (re)creates the
// pipeline that uses it from the compiled libraries. Add() compiles the
// program and creates the pipeline immediately. ReloadChanged() polls the
// watched source files, recompiles the programs whose sources (or includes)
// changed and recreates their pipelines. A program is always recompiled as a
// whole, so a failed edit in one stage is never masked by a later edit in
// the other.
//
// If a recompile fails, or the factory reports an error for the new
// libraries (e.g. stages that no longer link), the old pipeline stays in use
// and the message is kept for last_error() until a later reload of that same
// program succeeds. The program keeps its current libraries alive.
//
// Borrows `context`, which must outlive this object. Not thread-safe; call
// it from the thread that records frames, between frames.
class ShaderPrograms {
 public:
  // Creates (or replaces) a pipeline from compiled shaders. The libraries
  // are borrowed for the call; `fragment` is null for programs without a
  // fragment stage. Returns an error text, and leaves the current pipeline
  // untouched, when the pipeline cannot be created; nullopt on success.
  // When a pipeline is replaced, the GPU is idle.
  using PipelineFactory = std::function<std::optional<std::string>(
      const shader::metal::ShaderLibrary& vertex,
      const shader::metal::ShaderLibrary* fragment)>;

  ShaderPrograms(const MetalContext& context, shader::CompileMode mode);

  ShaderPrograms(const ShaderPrograms&) = delete;
  ShaderPrograms& operator=(const ShaderPrograms&) = delete;

  // Compiles the program from `vertex_path` and `fragment_path` (empty for
  // none), calls `create` with the result and starts watching the sources.
  // A compile or factory error is fatal: there is no previous pipeline to
  // keep.
  void Add(const std::string& vertex_path, const std::string& fragment_path,
           PipelineFactory create);

  // Recompiles the programs whose sources changed since the last call (see
  // shader::ShaderWatcher for what counts as a change) and recreates their
  // pipelines, waiting for the GPU to go idle once if any pipeline is
  // replaced. Cheap when nothing changed.
  void ReloadChanged();

  // Returns the message of the most recent failed reload that has not been
  // cleared by a later successful reload of the same program, or an empty
  // view. Valid until the next call to Add() or ReloadChanged().
  [[nodiscard]] std::string_view last_error() const;

 private:
  // The successful result of compiling every stage of a program.
  struct CompiledProgram {
    shader::metal::CompiledShader vertex;
    // An empty library without a fragment stage.
    shader::metal::CompiledShader fragment;
  };

  struct Program {
    std::string vertex_path;
    std::string fragment_path;
    PipelineFactory create;
    // The libraries of the current pipeline.
    CompiledProgram current;
    // The message of the last failed reload; empty while the program is fine.
    std::string error;
    // Orders failures: the highest one is the "most recent". Zero = no error.
    uint64_t error_serial = 0;
  };

  [[nodiscard]] core::Result<CompiledProgram, shader::ShaderError>
  CompileProgram(const Program& program) const;
  // Creates the pipeline from `compiled`. On success makes it the program's
  // current shaders, starts watching the sources and clears the error;
  // otherwise returns the factory's message and changes nothing.
  [[nodiscard]] std::optional<std::string> Apply(Program& program,
                                                 CompiledProgram compiled);
  // Records `message` as the program's latest error and logs it.
  void SetError(Program& program, std::string message);

  const MetalContext& context_;
  shader::metal::ShaderCompiler compiler_;
  shader::ShaderWatcher watcher_;
  std::vector<Program> programs_;
  uint64_t next_error_serial_ = 1;
};

}  // namespace blue::render::metal

#endif  // BLUE_RENDER_METAL_SHADER_PROGRAMS_H_
