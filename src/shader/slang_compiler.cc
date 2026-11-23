#include "shader/slang_compiler.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The Slang headers do not compile cleanly at our warning level.
#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include <slang/slang-com-ptr.h>
#include <slang/slang.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include "core/log.h"
#include "core/path.h"
#include "core/result.h"

namespace blue::shader {

namespace {

using CompileResult = core::Result<CompiledSlang, ShaderError>;

// Returns the text of `blob` (compiler diagnostics), or "" for null.
std::string BlobText(slang::IBlob* blob) {
  if (blob == nullptr) {
    return {};
  }
  return std::string(static_cast<const char*>(blob->getBufferPointer()),
                     blob->getBufferSize());
}

// Returns a failure for `path`: the diagnostics, or `fallback` without any.
CompileResult Failure(const std::string& path, slang::IBlob* diagnostics,
                      std::string_view fallback) {
  std::string message = BlobText(diagnostics);
  if (message.empty()) {
    message = path + ": error: " + std::string(fallback);
  }
  return CompileResult::Error(ShaderError{std::move(message)});
}

// One "NAME=VALUE" define split in two (value "1" without '=').
struct Define {
  std::string name;
  std::string value;
};

Define SplitDefine(std::string_view define) {
  const size_t equals = define.find('=');
  if (equals == std::string_view::npos) {
    return {std::string(define), "1"};
  }
  return {std::string(define.substr(0, equals)),
          std::string(define.substr(equals + 1))};
}

SlangStage ToSlangStage(ShaderStage stage) {
  switch (stage) {
    case ShaderStage::kVertex:
      return SLANG_STAGE_VERTEX;
    case ShaderStage::kFragment:
      return SLANG_STAGE_FRAGMENT;
    case ShaderStage::kCompute:
      return SLANG_STAGE_COMPUTE;
  }
  return SLANG_STAGE_NONE;
}

slang::CompilerOptionEntry IntOption(slang::CompilerOptionName name,
                                     int32_t value) {
  slang::CompilerOptionEntry entry;
  entry.name = name;
  entry.value.kind = slang::CompilerOptionValueKind::Int;
  entry.value.intValue0 = value;
  return entry;
}

slang::CompilerOptionEntry StringOption(slang::CompilerOptionName name,
                                        const char* value) {
  slang::CompilerOptionEntry entry;
  entry.name = name;
  entry.value.kind = slang::CompilerOptionValueKind::String;
  entry.value.stringValue0 = value;
  return entry;
}

}  // namespace

class SlangCompiler::Impl {
 public:
  explicit Impl(CompileMode mode) : mode_(mode) {
    if (SLANG_FAILED(slang::createGlobalSession(global_.writeRef()))) {
      core::log::Fatal("Could not start the Slang compiler");
    }
  }

  [[nodiscard]] CompileResult Compile(const SlangRequest& request) const {
    const std::string path = core::NormalizePath(request.source_path);
    const std::string directory = core::ParentPath(path);
    const std::string module_name = core::FileStem(path);

    // A session per compile: its defines differ per program, and a session
    // caches the modules it loaded (a reload must read the files again).
    const bool spirv = request.target == ShaderTarget::kSpirv;
    slang::TargetDesc target;
    target.format = spirv ? SLANG_SPIRV : SLANG_DXIL;
    target.profile = global_->findProfile(spirv ? "spirv_1_6" : "sm_6_0");

    std::vector<Define> defines;
    defines.reserve(request.defines.size());
    for (const std::string_view define : request.defines) {
      defines.push_back(SplitDefine(define));
    }
    std::vector<slang::PreprocessorMacroDesc> macros;
    macros.reserve(defines.size());
    for (const Define& define : defines) {
      macros.push_back({define.name.c_str(), define.value.c_str()});
    }
    // As cmake/Shaders.cmake: DXIL sources see BLUE_DXIL (shaders/README.md).
    if (!spirv) {
      macros.push_back({"BLUE_DXIL", "1"});
    }

    // The flags of cmake/Shaders.cmake.
    const bool debug = mode_ == CompileMode::kDebug;
    const std::array options = {
        StringOption(slang::CompilerOptionName::WarningsAsErrors, "all"),
        IntOption(slang::CompilerOptionName::Optimization,
                  debug ? SLANG_OPTIMIZATION_LEVEL_NONE
                        : SLANG_OPTIMIZATION_LEVEL_HIGH),
        IntOption(slang::CompilerOptionName::DebugInformation,
                  debug ? SLANG_DEBUG_INFO_LEVEL_STANDARD
                        : SLANG_DEBUG_INFO_LEVEL_NONE),
    };
    const char* search_path = directory.c_str();
    slang::SessionDesc session_desc;
    session_desc.targets = &target;
    session_desc.targetCount = 1;
    session_desc.defaultMatrixLayoutMode = SLANG_MATRIX_LAYOUT_COLUMN_MAJOR;
    session_desc.searchPaths = &search_path;
    session_desc.searchPathCount = 1;
    session_desc.preprocessorMacros = macros.data();
    session_desc.preprocessorMacroCount = static_cast<SlangInt>(macros.size());
    session_desc.compilerOptionEntries = options.data();
    session_desc.compilerOptionEntryCount =
        static_cast<uint32_t>(options.size());
    Slang::ComPtr<slang::ISession> session;
    if (SLANG_FAILED(
            global_->createSession(session_desc, session.writeRef()))) {
      return Failure(path, nullptr, "could not create a Slang session");
    }

    Slang::ComPtr<slang::IBlob> diagnostics;
    // Owned by the session.
    slang::IModule* module =
        session->loadModule(module_name.c_str(), diagnostics.writeRef());
    if (module == nullptr) {
      return Failure(path, diagnostics.get(), "could not be read or compiled");
    }

    Slang::ComPtr<slang::IEntryPoint> entry_point;
    if (SLANG_FAILED(module->findAndCheckEntryPoint(
            request.entry_point.c_str(),
            ToSlangStage(request.stage), entry_point.writeRef(), diagnostics.writeRef()))) {
      return Failure(path, diagnostics.get(),
                     "no entry point '" + request.entry_point + "'");
    }

    const std::array<slang::IComponentType*, 2> components = {
        module, entry_point.get()};
    Slang::ComPtr<slang::IComponentType> composite;
    if (SLANG_FAILED(session->createCompositeComponentType(
            components.data(), static_cast<SlangInt>(components.size()),
            composite.writeRef(), diagnostics.writeRef()))) {
      return Failure(path, diagnostics.get(), "could not be composed");
    }
    Slang::ComPtr<slang::IComponentType> linked;
    if (SLANG_FAILED(
            composite->link(linked.writeRef(), diagnostics.writeRef()))) {
      return Failure(path, diagnostics.get(), "could not be linked");
    }
    Slang::ComPtr<slang::IBlob> code;
    if (SLANG_FAILED(linked->getEntryPointCode(0, 0, code.writeRef(),
                                               diagnostics.writeRef())) ||
        code.get() == nullptr || code->getBufferSize() == 0) {
      return Failure(path, diagnostics.get(), "produced no code");
    }
    // Warnings are errors: a clean compile leaves no diagnostics.

    CompiledSlang compiled;
    const auto* bytes = static_cast<const std::byte*>(code->getBufferPointer());
    compiled.code.assign(bytes, bytes + code->getBufferSize());
    compiled.files.push_back(path);
    const SlangInt32 file_count = module->getDependencyFileCount();
    for (SlangInt32 i = 0; i < file_count; ++i) {
      const char* file = module->getDependencyFilePath(i);
      if (file == nullptr) {
        continue;
      }
      std::string normalized = core::NormalizePath(file);
      bool known = false;
      for (const std::string& existing : compiled.files) {
        known = known || existing == normalized;
      }
      if (!known) {
        compiled.files.push_back(std::move(normalized));
      }
    }
    return CompileResult::Ok(std::move(compiled));
  }

 private:
  CompileMode mode_;
  Slang::ComPtr<slang::IGlobalSession> global_;
};

SlangCompiler::SlangCompiler(CompileMode mode)
    : impl_(std::make_unique<Impl>(mode)) {}

SlangCompiler::~SlangCompiler() = default;

core::Result<CompiledSlang, ShaderError> SlangCompiler::Compile(
    const SlangRequest& request) const {
  return impl_->Compile(request);
}

}  // namespace blue::shader
