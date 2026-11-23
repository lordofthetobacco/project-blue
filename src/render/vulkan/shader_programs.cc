#include "render/vulkan/shader_programs.h"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/log.h"
#include "core/path.h"
#include "core/result.h"
#include "platform/file_system.h"
#include "render/shader_manifest.h"
#include "render/vulkan/vk_check.h"
#include "shader/spirv_blob.h"

#if BLUE_SHADER_HOT_RELOAD
#include "shader/slang_compiler.h"
#endif

namespace blue::render::vulkan {

namespace {

// No shader of the renderer is near this.
constexpr size_t kMaxBlobBytes = size_t{4} << 20;

#if BLUE_SHADER_HOT_RELOAD
bool Contains(const std::vector<std::string>& paths, const std::string& path) {
  return std::find(paths.begin(), paths.end(), path) != paths.end();
}

// Returns the files of `directory` that programs may include (*.slang and
// *.h that are not a program's own source), as normalized paths.
std::vector<std::string> ListSharedSources(const std::string& directory) {
  std::vector<std::string> files;
  const std::optional<std::vector<std::string>> names =
      platform::ListDirectory(directory);
  if (!names) {
    return files;
  }
  for (const std::string& name : *names) {
    if (!name.ends_with(".slang") && !name.ends_with(".h")) {
      continue;
    }
    bool program_source = false;
    for (const ShaderProgramInfo& program : kShaderPrograms) {
      program_source = program_source || program.source == name;
    }
    if (!program_source) {
      files.push_back(core::NormalizePath(core::JoinPath(directory, name)));
    }
  }
  return files;
}

// One stage of `info` compiled from `source_path`; `out` gets the SPIR-V and
// `dependencies` every new file it was made from.
[[nodiscard]] std::optional<shader::ShaderError> CompileStage(
    const shader::SlangCompiler& compiler, const ShaderProgramInfo& info,
    const std::string& source_path, shader::ShaderStage stage,
    std::vector<uint32_t>& out, std::vector<std::string>& dependencies) {
  auto compiled = compiler.Compile({
      .source_path = source_path,
      .entry_point = std::string(stage == shader::ShaderStage::kVertex
                                     ? info.vertex_entry
                                     : info.fragment_entry),
      .stage = stage,
      .target = shader::ShaderTarget::kSpirv,
      .defines = info.defines,
  });
  if (!compiled.ok()) {
    return compiled.error();
  }
  out = shader::ToSpirvWords(compiled.value().code);
  if (out.empty()) {
    return shader::ShaderError{source_path + ": error: empty SPIR-V"};
  }
  for (const std::string& file : compiled.value().files) {
    if (file != source_path && !Contains(dependencies, file)) {
      dependencies.push_back(file);
    }
  }
  return std::nullopt;
}
#endif  // BLUE_SHADER_HOT_RELOAD

}  // namespace

ShaderPrograms::ShaderPrograms(VkDevice device, std::string blob_dir,
                               std::string source_dir, shader::CompileMode mode)
    : device_(device), blob_dir_(std::move(blob_dir)), mode_(mode) {
#if BLUE_SHADER_HOT_RELOAD
  if (!source_dir.empty()) {
    source_dir_ = core::NormalizePath(source_dir);
    shared_files_ = ListSharedSources(source_dir_);
    if (shared_files_.empty()) {
      core::log::Warn("No shader sources in {}: hot reload is off",
                      source_dir_);
      source_dir_.clear();
    } else {
      core::log::Info("Shader hot reload from {}", source_dir_);
    }
  }
#else
  static_cast<void>(source_dir);
#endif
}

ShaderPrograms::~ShaderPrograms() = default;

std::vector<uint32_t> ShaderPrograms::LoadBlob(
    const std::string& file_name) const {
  const std::string path = core::JoinPath(blob_dir_, file_name);
  const auto bytes = platform::ReadFile(path, kMaxBlobBytes);
  if (!bytes.ok()) {
    core::log::Fatal("Cannot read the shader blob {} ({}); build the "
                     "blue_shaders target",
                     path, platform::ToString(bytes.error()));
  }
  std::vector<uint32_t> spirv = shader::ToSpirvWords(bytes.value());
  if (spirv.empty()) {
    core::log::Fatal("{} is not SPIR-V", path);
  }
  return spirv;
}

void ShaderPrograms::Apply(Program& program, CompiledProgram compiled) {
  // Passes that test depth EQUAL against each other need bit-identical
  // positions from different vertex shaders (see shader/spirv_blob.h).
  if (!shader::MarkPositionInvariant(compiled.vertex)) {
    core::log::Fatal("Vertex shader of program {} writes no position",
                     ShaderProgramInfoOf(program.id).name);
  }
  program.vertex_spirv = std::move(compiled.vertex);
  program.fragment_spirv = std::move(compiled.fragment);
  program.create(program.vertex_spirv, program.fragment_spirv);
  program.error.clear();
  program.error_serial = 0;
}

void ShaderPrograms::Add(ShaderProgram id, PipelineFactory create) {
  const ShaderProgramInfo& info = ShaderProgramInfoOf(id);
  Program program;
  program.id = id;
  program.create = std::move(create);

  CompiledProgram loaded;
  loaded.vertex = LoadBlob(std::string(info.name) + ".vert.spv");
  if (!info.fragment_entry.empty()) {
    loaded.fragment = LoadBlob(std::string(info.name) + ".frag.spv");
  }
  Apply(program, std::move(loaded));

  if (!source_dir_.empty()) {
    program.source_path =
        core::NormalizePath(core::JoinPath(source_dir_, info.source));
    // Programs of one file share a watched root; the real include list
    // replaces this superset after the first runtime compile.
    watcher_.Watch(program.source_path, shared_files_);
  }
  programs_.push_back(std::move(program));
}

void ShaderPrograms::ReloadChanged() {
#if BLUE_SHADER_HOT_RELOAD
  if (source_dir_.empty()) {
    return;
  }
  const std::vector<std::string> changed = watcher_.Poll();
  if (changed.empty()) {
    return;
  }
  if (!compiler_) {
    compiler_ = std::make_unique<shader::SlangCompiler>(mode_);
  }

  // Compile everything first: the GPU only has to go idle if at least one
  // pipeline is actually replaced.
  struct Rebuilt {
    Program* program;
    CompiledProgram compiled;
  };
  std::vector<Rebuilt> rebuilt;
  for (Program& program : programs_) {
    if (!Contains(changed, program.source_path)) {
      continue;
    }
    const ShaderProgramInfo& info = ShaderProgramInfoOf(program.id);
    CompiledProgram compiled;
    std::optional<shader::ShaderError> error =
        CompileStage(*compiler_, info, program.source_path,
                     shader::ShaderStage::kVertex, compiled.vertex,
                     compiled.dependencies);
    if (!error && !info.fragment_entry.empty()) {
      error = CompileStage(*compiler_, info, program.source_path,
                           shader::ShaderStage::kFragment, compiled.fragment,
                           compiled.dependencies);
    }
    if (error) {
      program.error = std::move(error->message);
      program.error_serial = next_error_serial_++;
      core::log::Error("Shader reload failed, keeping the previous one:\n{}",
                       program.error);
    } else {
      rebuilt.push_back({&program, std::move(compiled)});
    }
  }
  if (rebuilt.empty()) {
    return;
  }

  CheckVk(vkDeviceWaitIdle(device_));
  for (Rebuilt& entry : rebuilt) {
    Program& program = *entry.program;
    std::vector<std::string> dependencies =
        std::move(entry.compiled.dependencies);
    Apply(program, std::move(entry.compiled));
    watcher_.Watch(program.source_path, dependencies);
    core::log::Info("Reloaded {}", ShaderProgramInfoOf(program.id).name);
  }
#endif  // BLUE_SHADER_HOT_RELOAD
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
