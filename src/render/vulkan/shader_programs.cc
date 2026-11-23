#include "render/vulkan/shader_programs.h"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/log.h"
#include "core/path.h"
#include "core/result.h"
#include "render/vulkan/vk_check.h"
#include "shader/shader_compiler.h"

namespace blue::render::vulkan {

namespace {

bool Contains(const std::vector<std::string>& paths, const std::string& path) {
  return std::find(paths.begin(), paths.end(), path) != paths.end();
}

}  // namespace

ShaderPrograms::ShaderPrograms(VkDevice device, shader::CompileMode mode)
    : device_(device), compiler_(mode) {}

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

void ShaderPrograms::Apply(Program& program, CompiledProgram compiled) {
  program.vertex_spirv = std::move(compiled.vertex.spirv);
  program.fragment_spirv = std::move(compiled.fragment.spirv);
  program.create(program.vertex_spirv, program.fragment_spirv);

  watcher_.Watch(program.vertex_path, compiled.vertex.dependencies);
  if (!program.fragment_path.empty()) {
    watcher_.Watch(program.fragment_path, compiled.fragment.dependencies);
  }
  program.error.clear();
  program.error_serial = 0;
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
  Apply(program, std::move(compiled).value());
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
      program.error = compiled.error().message;
      program.error_serial = next_error_serial_++;
      core::log::Error("Shader reload failed, keeping the previous one:\n{}",
                       program.error);
    }
  }
  if (rebuilt.empty()) {
    return;
  }

  CheckVk(vkDeviceWaitIdle(device_));
  for (Rebuilt& entry : rebuilt) {
    const Program& program = *entry.program;
    Apply(*entry.program, std::move(entry.compiled));
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

}  // namespace blue::render::vulkan
