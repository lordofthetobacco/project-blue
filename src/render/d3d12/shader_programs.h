#ifndef BLUE_RENDER_D3D12_SHADER_PROGRAMS_H_
#define BLUE_RENDER_D3D12_SHADER_PROGRAMS_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "render/shader_manifest.h"
#include "shader/compile_mode.h"
#include "shader/shader_watcher.h"

namespace blue::shader {
class SlangCompiler;
}  // namespace blue::shader

namespace blue::render::d3d12 {

class D3d12Context;

// Registry of the renderer's shader programs (render::ShaderProgram: a
// vertex stage plus an optional fragment stage of one Slang file) with hot
// reload.
//
// Each program is registered together with a factory that (re)creates the
// pipeline state object that uses it from DXIL. Add() loads the DXIL the
// build compiled (cmake/Shaders.cmake) and creates the pipeline immediately.
// The same registry as the Vulkan backend's, for DXIL.
//
// With a source directory (and a build with BLUE_SHADER_HOT_RELOAD),
// ReloadChanged() polls the Slang sources, recompiles the programs whose
// source or any shared file changed with the runtime compiler and recreates
// their pipelines. A program is always recompiled as a whole, so a failed
// edit in one stage is never masked by a later edit in the other. Edits made
// while the application was not running need a build (which recompiles the
// blobs); only edits seen while it runs are reloaded.
//
// If a recompile fails, the old pipeline stays in use and the compiler
// message is kept for last_error() until a later reload of that same program
// succeeds. A pipeline factory that fails is fatal like any other Direct3D
// error: a shader that compiles but no longer matches the root signature or
// the input layout is not detected here.
//
// Borrows `context`, which must outlive this object. Not thread-safe; call
// it from the thread that records frames, between frames.
class ShaderPrograms {
 public:
  // Creates (or replaces) a pipeline from DXIL containers. The spans are only
  // valid for the duration of the call; `fragment` is empty for programs
  // without a fragment stage. Replacing the pipeline releases the old one;
  // the GPU is idle when that happens.
  using PipelineFactory = std::function<void(
      std::span<const std::byte> vertex, std::span<const std::byte> fragment)>;

  // `blob_dir` holds the build's DXIL (<name>.vert.dxil, <name>.frag.dxil).
  // `source_dir` holds the Slang sources to watch and recompile with `mode`;
  // empty (or a build without BLUE_SHADER_HOT_RELOAD): no hot reload.
  ShaderPrograms(const D3d12Context& context, std::string blob_dir,
                 std::string source_dir, shader::CompileMode mode);
  ~ShaderPrograms();

  ShaderPrograms(const ShaderPrograms&) = delete;
  ShaderPrograms& operator=(const ShaderPrograms&) = delete;

  // Loads the blobs of `program`, calls `create` with them and, with hot
  // reload, starts watching its sources. A missing or invalid blob is fatal:
  // there is no previous pipeline to keep.
  void Add(ShaderProgram program, PipelineFactory create);

  // Recompiles the programs whose sources changed since the last call (see
  // shader::ShaderWatcher for what counts as a change) and recreates their
  // pipelines, waiting for the GPU to go idle once if any pipeline is
  // replaced. Cheap when nothing changed; does nothing without hot reload.
  void ReloadChanged();

  // Returns the compiler message of the most recent failed reload that has
  // not been cleared by a later successful reload of the same program, or an
  // empty view. Valid until the next call to Add() or ReloadChanged().
  [[nodiscard]] std::string_view last_error() const;

 private:
  struct Program {
    ShaderProgram id{};
    // The program's Slang file; empty without hot reload.
    std::string source_path;
    PipelineFactory create;
    std::vector<std::byte> vertex_dxil;
    std::vector<std::byte> fragment_dxil;
    // The message of the last failed reload; empty while the program is fine.
    std::string error;
    // Orders failures: the highest one is the "most recent". Zero = no error.
    uint64_t error_serial = 0;
  };

  // The successful result of compiling every stage of a program.
  struct CompiledProgram {
    std::vector<std::byte> vertex;
    std::vector<std::byte> fragment;  // Empty without a fragment stage.
    // The files the stages were made from, the program's source excluded.
    std::vector<std::string> dependencies;
  };

  // Reads DXIL blob `file_name` of the blob directory; fatal on failure.
  [[nodiscard]] std::vector<std::byte> LoadBlob(
      const std::string& file_name) const;
  // Makes `compiled` the program's current shaders and creates the pipeline.
  static void Apply(Program& program, CompiledProgram compiled);

  const D3d12Context& context_;
  std::string blob_dir_;
  // Empty: no hot reload.
  std::string source_dir_;
  // Of the runtime compiler.
  [[maybe_unused]] shader::CompileMode mode_;
  // The files of source_dir_ that are no program's own source: what a
  // program is assumed to include until its first runtime compile reports
  // the real list.
  std::vector<std::string> shared_files_;
  // Created by the first reload (starting Slang takes a moment).
  std::unique_ptr<shader::SlangCompiler> compiler_;
  shader::ShaderWatcher watcher_;
  std::vector<Program> programs_;
  uint64_t next_error_serial_ = 1;
};

}  // namespace blue::render::d3d12

#endif  // BLUE_RENDER_D3D12_SHADER_PROGRAMS_H_
