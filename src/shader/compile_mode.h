#ifndef BLUE_SHADER_COMPILE_MODE_H_
#define BLUE_SHADER_COMPILE_MODE_H_

namespace blue::shader {

// How shaders are compiled, by every backend's compiler.
enum class CompileMode {
  kDebug,    // No optimization, debug info in the output.
  kRelease,  // Full performance optimization, no debug info.
};

}  // namespace blue::shader

#endif  // BLUE_SHADER_COMPILE_MODE_H_
