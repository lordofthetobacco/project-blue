#include "shader/metal/shader_compiler.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "core/path.h"
#include "shader/shader_preprocessor.h"

namespace blue::shader::metal {

namespace {

// Returns the text of `error`, or `fallback` when there is none.
std::string ErrorText(NSError* error, std::string_view fallback) {
  if (error == nil) {
    return std::string(fallback);
  }
  const char* text = error.localizedDescription.UTF8String;
  return text != nullptr ? std::string(text) : std::string(fallback);
}

}  // namespace

ShaderLibrary::~ShaderLibrary() {
  if (library_ != nullptr) {
    CFBridgingRelease(library_);
  }
}

ShaderLibrary::ShaderLibrary(ShaderLibrary&& other) noexcept
    : library_(std::exchange(other.library_, nullptr)) {}

ShaderLibrary& ShaderLibrary::operator=(ShaderLibrary&& other) noexcept {
  if (this != &other) {
    if (library_ != nullptr) {
      CFBridgingRelease(library_);
    }
    library_ = std::exchange(other.library_, nullptr);
  }
  return *this;
}

bool ShaderLibrary::HasFunction(std::string_view name) const {
  if (library_ == nullptr) {
    return false;
  }
  @autoreleasepool {
    const id<MTLLibrary> library = (__bridge id<MTLLibrary>)library_;
    NSString* function_name =
        [[NSString alloc] initWithBytes:name.data()
                                 length:name.size()
                               encoding:NSUTF8StringEncoding];
    return function_name != nil &&
           [library.functionNames containsObject:function_name];
  }
}

// Holds the compiler and the options shared by every compilation.
class ShaderCompiler::Impl {
 public:
  explicit Impl(CompileMode mode)
      : options_([[MTLCompileOptions alloc] init]) {
    const id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (device != nil && [device supportsFamily:MTLGPUFamilyMetal4]) {
      MTL4CompilerDescriptor* descriptor =
          [[MTL4CompilerDescriptor alloc] init];
      descriptor.label = @"Shader compiler";
      compiler_ = [device newCompilerWithDescriptor:descriptor error:nil];
    }
    options_.languageVersion = MTLLanguageVersion3_2;
    // Safe math keeps isnan() / isinf() meaningful (shaders/sanitize.h).
    options_.mathMode = MTLMathModeSafe;
    options_.mathFloatingPointFunctions =
        MTLMathFloatingPointFunctionsPrecise;
    // The mesh pass depth-tests EQUAL against the depth pre-pass, so both
    // must compute bit-identical positions.
    options_.preserveInvariance = YES;
    if (mode == CompileMode::kDebug) {
      options_.preprocessorMacros = @{@"BLUE_SHADER_DEBUG" : @1};
    }
  }

  core::Result<CompiledShader, ShaderError> Compile(
      const std::string& path) const {
    using CompileResult = core::Result<CompiledShader, ShaderError>;
    auto preprocessed = PreprocessShader(path);
    if (!preprocessed.ok()) {
      return CompileResult::Error(preprocessed.error());
    }
    PreprocessedShader shader = std::move(preprocessed).value();
    const std::string root = core::NormalizePath(path);
    if (compiler_ == nil) {
      return CompileResult::Error(
          {"'" + root + "': cannot compile: no Metal 4 device"});
    }

    @autoreleasepool {
      NSString* source =
          [[NSString alloc] initWithBytes:shader.source.data()
                                   length:shader.source.size()
                                 encoding:NSUTF8StringEncoding];
      if (source == nil) {
        return CompileResult::Error(
            {"'" + root + "': the source is not valid UTF-8"});
      }
      MTL4LibraryDescriptor* descriptor = [[MTL4LibraryDescriptor alloc] init];
      descriptor.source = source;
      descriptor.options = options_;
      descriptor.name = @(core::FileName(root).c_str());
      NSError* error = nil;
      const id<MTLLibrary> library =
          [compiler_ newLibraryWithDescriptor:descriptor error:&error];
      if (library == nil) {
        return CompileResult::Error(
            {ErrorText(error, "'" + root + "': compilation failed")});
      }
      // A library together with an error means warnings: they are errors.
      if (error != nil) {
        return CompileResult::Error({ErrorText(error, root)});
      }

      CompiledShader compiled{
          .library =
              ShaderLibrary(const_cast<void*>(CFBridgingRetain(library))),
          .dependencies = std::move(shader.dependencies),
          .stage = shader.stage,
      };
      const std::string_view entry_point = EntryPointName(shader.stage);
      if (!compiled.library.HasFunction(entry_point)) {
        return CompileResult::Error({"'" + root + "': no entry point '" +
                                     std::string(entry_point) +
                                     "' for its stage"});
      }
      return CompileResult::Ok(std::move(compiled));
    }
  }

 private:
  // nil without a Metal 4 device.
  id<MTL4Compiler> compiler_ = nil;
  MTLCompileOptions* options_;
};

ShaderCompiler::ShaderCompiler(CompileMode mode)
    : impl_(std::make_unique<Impl>(mode)) {}

ShaderCompiler::~ShaderCompiler() = default;

core::Result<CompiledShader, ShaderError> ShaderCompiler::Compile(
    const std::string& path) const {
  return impl_->Compile(path);
}

}  // namespace blue::shader::metal
