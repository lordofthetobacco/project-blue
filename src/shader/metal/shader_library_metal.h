#ifndef BLUE_SHADER_METAL_SHADER_LIBRARY_METAL_H_
#define BLUE_SHADER_METAL_SHADER_LIBRARY_METAL_H_

// The Metal side of shader::metal::ShaderLibrary, for Objective-C++ code that builds
// pipelines from compiled shaders.

#ifndef __OBJC__
#error "shader_library_metal.h is Objective-C++; include it from .mm files only"
#endif

#import <Metal/Metal.h>

#include "shader/metal/shader_compiler.h"

namespace blue::shader::metal {

// Returns the library `library` owns (not retained beyond its lifetime), or
// nil for an empty library.
inline id<MTLLibrary> MetalLibrary(const ShaderLibrary& library) {
  return (__bridge id<MTLLibrary>)library.handle();
}

}  // namespace blue::shader::metal

#endif  // BLUE_SHADER_METAL_SHADER_LIBRARY_METAL_H_
