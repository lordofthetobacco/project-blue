#ifndef BLUE_SHADER_SPIRV_BLOB_H_
#define BLUE_SHADER_SPIRV_BLOB_H_

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// Helpers for SPIR-V blobs as the build and the runtime compiler produce
// them. Pure; no Vulkan needed.

namespace blue::shader {

// Returns the bytes of a SPIR-V file as words; empty if the size is zero or
// not a multiple of 4.
[[nodiscard]] std::vector<uint32_t> ToSpirvWords(
    std::span<const std::byte> bytes);

// Adds the Invariant decoration to the Position built-in output of the
// vertex shader module `spirv`, so that two vertex shaders computing the
// position with the same expression get bit-identical results (the mesh
// pass tests depth EQUAL against the depth pre-pass). Slang has no
// `invariant` qualifier for SPIR-V; this is what GLSL's
// `invariant gl_Position;` compiled to.
//
// Returns true if the module now carries the decoration (also when it
// already did), false, leaving `spirv` untouched, if it is not a SPIR-V
// module or has no Position output.
[[nodiscard]] bool MarkPositionInvariant(std::vector<uint32_t>& spirv);

}  // namespace blue::shader

#endif  // BLUE_SHADER_SPIRV_BLOB_H_
