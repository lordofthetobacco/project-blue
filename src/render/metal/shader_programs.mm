#include "render/metal/shader_programs.h"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/log.h"
#include "core/path.h"
#include "core/result.h"
#include "shader/metal/shader_compiler.h"

namespace blue::render::metal {

namespace {

bool Contains(const std::vector<std::string>& paths, const std::string& path) {
  return std::find(paths.begin(), paths.end(), path) != paths.end();
}

}  // namespace

ShaderPrograms::ShaderPrograms(const MetalContext& context,
                               shader::CompileMode mode)
    : context_(context), compiler_(mode) {}

core::Result<ShaderPrograms::CompiledProgram, shader::ShaderError>
ShaderPrograms::CompileProgram(const Program& program) const {
  using CompileResult = core::Result<CompiledProgram, shader::ShaderError>;
  auto vertex = compiler_.Compile(program.vertex_path);
  if (!vertex.ok()) {
    return CompileResult::Error(vertex.error());
  }
  CompiledProgram compiled{.vertex = std::move(vertex).value(), .fragment = {}};
  if (!program.fragment_path.empty()) {
    auto fragment = compiler_.Compile(program.fragment_path);
    if (!fragment.ok()) {
      return CompileResult::Error(fragment.error());
    }
    compiled.fragment = std::move(fragment).value();
  }
  return CompileResult::Ok(std::move(compiled));
}

std::optional<std::string> ShaderPrograms::Apply(Program& program,
                                                 CompiledProgram compiled) {
  const shader::metal::ShaderLibrary* fragment =
      compiled.fragment.library ? &compiled.fragment.library : nullptr;
  std::optional<std::string> error =
      program.create(compiled.vertex.library, fragment);
  if (error) {
    return error;
  }
  watcher_.Watch(program.vertex_path, compiled.vertex.dependencies);
  if (!program.fragment_path.empty()) {
    watcher_.Watch(program.fragment_path, compiled.fragment.dependencies);
  }
  program.current = std::move(compiled);
  program.error.clear();
  program.error_serial = 0;
  return std::nullopt;
}

void ShaderPrograms::SetError(Program& program, std::string message) {
  program.error = std::move(message);
  program.error_serial = next_error_serial_++;
  core::log::Error("Shader reload failed, keeping the previous one:\n{}",
                   program.error);
}

void ShaderPrograms::Add(const std::string& vertex_path,
                         const std::string& fragment_path,
                         PipelineFactory create) {
  Program program;
  program.vertex_path = core::NormalizePath(vertex_path);
  if (!fragment_path.empty()) {
    program.fragment_path = core::NormalizePath(fragment_path);
  }
  program.create = std::move(create);

  auto compiled = CompileProgram(program);
  if (!compiled.ok()) {
    core::log::Fatal("Shader compilation failed:\n{}",
                     compiled.error().message);
  }
  if (const std::optional<std::string> error =
          Apply(program, std::move(compiled).value())) {
    core::log::Fatal("Creating the pipeline of {} failed:\n{}",
                     core::FileName(program.fragment_path.empty()
                                        ? program.vertex_path
                                        : program.fragment_path),
                     *error);
  }
  programs_.push_back(std::move(program));
}

void ShaderPrograms::ReloadChanged() {
  const std::vector<std::string> changed = watcher_.Poll();
  if (changed.empty()) {
    return;
  }

  // Compile everything first: the GPU only has to go idle if at least one
  // pipeline is actually replaced.
  struct Rebuilt {
    Program* program;
    CompiledProgram compiled;
  };
  std::vector<Rebuilt> rebuilt;
  for (Program& program : programs_) {
    if (!Contains(changed, program.vertex_path) &&
        !Contains(changed, program.fragment_path)) {
      continue;
    }
    auto compiled = CompileProgram(program);
    if (compiled.ok()) {
      rebuilt.push_back({&program, std::move(compiled).value()});
    } else {
      SetError(program, compiled.error().message);
    }
  }
  if (rebuilt.empty()) {
    return;
  }

  context_.WaitIdle();
  for (Rebuilt& entry : rebuilt) {
    Program& program = *entry.program;
    if (std::optional<std::string> error =
            Apply(program, std::move(entry.compiled))) {
      SetError(program, std::move(*error));
      continue;
    }
    for (const std::string& path :
         {program.vertex_path, program.fragment_path}) {
      if (Contains(changed, path)) {
        core::log::Info("Reloaded {}", core::FileName(path));
      }
    }
  }
}

std::string_view ShaderPrograms::last_error() const {
  const Program* latest = nullptr;
  for (const Program& program : programs_) {
    if (program.error_serial != 0 &&
        (latest == nullptr || program.error_serial > latest->error_serial)) {
      latest = &program;
    }
  }
  return latest == nullptr ? std::string_view()
                           : std::string_view(latest->error);
}

}  // namespace blue::render::metal
