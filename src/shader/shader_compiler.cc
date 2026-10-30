#include "shader/shader_compiler.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <shaderc/shaderc.hpp>

#include "core/path.h"
#include "platform/file_system.h"

namespace blue::shader {

namespace {

constexpr size_t kMaxSourceBytes = 1024 * 1024;

// Returns the shader stage for the extension of `path`, or nullopt when it is
// not a supported stage.
std::optional<shaderc_shader_kind> StageForPath(const std::string& path) {
  const std::string name = core::FileName(path);
  const size_t dot = name.rfind('.');
  const std::string extension =
      dot == std::string::npos ? std::string() : name.substr(dot);
  if (extension == ".vert") {
    return shaderc_vertex_shader;
  }
  if (extension == ".frag") {
    return shaderc_fragment_shader;
  }
  if (extension == ".comp") {
    return shaderc_compute_shader;
  }
  return std::nullopt;
}

// Reads a source file as text. Returns the error text on failure.
core::Result<std::string, std::string> ReadSource(const std::string& path) {
  using ReadResult = core::Result<std::string, std::string>;
  const auto file = platform::ReadFile(path, kMaxSourceBytes);
  if (!file.ok()) {
    return ReadResult::Error("cannot read '" + path +
                             "': " + platform::ToString(file.error()));
  }
  const std::vector<std::byte>& bytes = file.value();
  return ReadResult::Ok(
      std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

// Resolves includes against the including file's directory and records every
// file it hands to the compiler. Borrows `dependencies`, which must outlive
// the compilation.
class FileIncluder : public shaderc::CompileOptions::IncluderInterface {
 public:
  explicit FileIncluder(std::vector<std::string>* dependencies)
      : dependencies_(dependencies) {}

  shaderc_include_result* GetInclude(const char* requested_source,
                                     shaderc_include_type /*type*/,
                                     const char* requesting_source,
                                     size_t /*include_depth*/) override {
    const std::string path =
        core::JoinPath(core::ParentPath(requesting_source), requested_source);
    auto source = ReadSource(path);

    auto* result = new IncludeData;
    if (source.ok()) {
      if (std::find(dependencies_->begin(), dependencies_->end(), path) ==
          dependencies_->end()) {
        dependencies_->push_back(path);
      }
      result->name = path;
      result->content = std::move(source).value();
    } else {
      // A failed include has an empty name and the message as content.
      result->content = source.error();
    }
    result->data = {result->name.data(), result->name.size(),
                    result->content.data(), result->content.size(), result};
    return &result->data;
  }

  void ReleaseInclude(shaderc_include_result* data) override {
    delete static_cast<IncludeData*>(data->user_data);
  }

 private:
  // Keeps the strings that the shaderc result points into alive.
  struct IncludeData {
    std::string name;
    std::string content;
    shaderc_include_result data{};
  };

  std::vector<std::string>* dependencies_;
};

}  // namespace

// Holds the shaderc compiler and the options shared by every compilation.
class ShaderCompiler::Impl {
 public:
  explicit Impl(CompileMode mode) : mode_(mode) {}

  core::Result<CompiledShader, ShaderError> Compile(
      const std::string& path) const {
    using CompileResult = core::Result<CompiledShader, ShaderError>;
    const std::string root = core::NormalizePath(path);
    const std::optional<shaderc_shader_kind> stage = StageForPath(root);
    if (!stage) {
      return CompileResult::Error(
          {"'" + root +
           "': unsupported shader extension (expected .vert, "
           ".frag or .comp)"});
    }
    auto source = ReadSource(root);
    if (!source.ok()) {
      return CompileResult::Error({source.error()});
    }

    CompiledShader compiled;
    shaderc::CompileOptions options;
    options.SetTargetEnvironment(shaderc_target_env_vulkan,
                                 shaderc_env_version_vulkan_1_3);
    options.SetTargetSpirv(shaderc_spirv_version_1_6);
    options.SetWarningsAsErrors();
    if (mode_ == CompileMode::kDebug) {
      options.SetOptimizationLevel(shaderc_optimization_level_zero);
      options.SetGenerateDebugInfo();
    } else {
      options.SetOptimizationLevel(shaderc_optimization_level_performance);
    }
    options.SetIncluder(std::make_unique<FileIncluder>(&compiled.dependencies));

    const shaderc::SpvCompilationResult result =
        compiler_.CompileGlslToSpv(source.value().data(), source.value().size(),
                                   *stage, root.c_str(), options);
    if (result.GetCompilationStatus() != shaderc_compilation_status_success) {
      return CompileResult::Error({result.GetErrorMessage()});
    }
    compiled.spirv.assign(result.cbegin(), result.cend());
    return CompileResult::Ok(std::move(compiled));
  }

 private:
  CompileMode mode_;
  shaderc::Compiler compiler_;
};

ShaderCompiler::ShaderCompiler(CompileMode mode)
    : impl_(std::make_unique<Impl>(mode)) {}

ShaderCompiler::~ShaderCompiler() = default;

core::Result<CompiledShader, ShaderError> ShaderCompiler::Compile(
    const std::string& path) const {
  return impl_->Compile(path);
}

}  // namespace blue::shader
