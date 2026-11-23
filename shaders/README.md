# Shaders

The renderer's shaders are written once in [Slang](https://shader-slang.org) and compiled by the
build to SPIR-V (Vulkan) and DXIL (Direct3D 12). The Metal backend still uses its hand-written
MSL in `metal/` (unchanged from the Metal branch) until it is moved to Slang too.

## Files

- `<name>.slang` with entry points: a program's source. Programs and their entry points,
  defines and order are listed in `programs.cmake`; that list generates
  `render/shader_manifest.h` (`render::ShaderProgram`), which the renderers use to ask for a
  program and the runtime compiler to recompile one.
- Shared files, pulled in with `#include` (all have include guards):
  `bindings.slang` (how resources are declared), `common.slang` (FrameUniforms, the frame's
  textures, `ClipPosition()`), `material.slang`, `pbr.slang`, `shadows.slang`,
  `environment.slang`, `sanitize.slang`, `motion_blur.slang`, `fullscreen.slang` (the vertex
  stage of every full-screen program).
- `gpu_bindings.h`: the set / binding numbers and array sizes, as plain `#define`s shared with
  C++ (`src/render/binding_layout.h`).

## Conventions

- **Entry points** are `VertexMain` and `FragmentMain` (other names when one file holds several
  programs, e.g. `depth.slang`), marked `[shader("vertex")]` / `[shader("fragment")]`.
- **Matrices** are column-major in memory (glm's layout; compiled with
  `-matrix-layout-column-major`). Write `mul(M, v)` for what GLSL writes `M * v`. Unlike GLSL,
  `M[i]` is **row** i and `float3x3(a, b, c)` takes rows.
- **Variants** are preprocessor defines compiled as separate programs on every target (DXIL has
  no specialization constants): `static const bool kAlphaMask = ALPHA_MASK != 0;`, then plain
  `if (kAlphaMask)`.
- **Resources** are declared with the macros of `bindings.slang`, never with raw attributes, so
  each has its Vulkan `(set, binding)` and its Direct3D register:
  - `BLUE_UNIFORMS(Type, name, binding, set)`: a uniform block, register `b<binding>`. Use
    16-byte members only (`float4`, `float4x4`, arrays of them), so std140 and the Direct3D
    packing agree; the offsets are compared with the C++ structs by a test.
  - `BLUE_TEXTURE(Type, name, binding, set)`: `Texture2D<float4>`, `TextureCube<float4>`,
    `Texture2DArray<float>`; register `t<binding>`.
  - `BLUE_SAMPLER(Type, name, binding, set)`: `SamplerState` / `SamplerComparisonState`;
    register `s<binding>`. Textures and samplers are separate (Direct3D 12 has no combined
    ones): each set has few samplers that its textures share, `tex.Sample(sampler, uv)`.
  - `BLUE_PUSH_CONSTANTS(Type, name)`: at most 128 bytes; Vulkan push constants, Direct3D 12
    root constants at `b0, space8`.
  - A number is used once per set whatever the kind, so both APIs share it.
- **Sets**: scene programs use set 0 for the frame (`common.slang`) and set 1 for what the pass
  adds (material, sky cube, scope). Full-screen programs use set 0 for their inputs (textures
  0..n-1, the shared `inputSampler` at 4). `src/render/binding_layout.h` is the same table as
  data; `tests/shader_bindings_test.cc` compares it with the reflection of every compiled
  program on every target.
- **Positions that must match**: passes that test depth `EQUAL` against the depth pre-pass
  compute the position with `ClipPosition()` (`common.slang`). Slang has no `invariant`
  qualifier for SPIR-V, so the Vulkan loader adds the `Invariant` decoration to every vertex
  shader's position (`shader::MarkPositionInvariant`); `precise` covers Direct3D.
- **Clip space** is the same on every backend: reversed Z, depth in [0, 1], the scene drawn
  with a Y-up viewport. `SV_Position.xy` in a fragment shader is the pixel centre, y down.
- **Warnings are errors**, and the same sources must compile for both targets (the build
  compiles DXIL on Windows even while only Vulkan renders: `BLUE_SHADERS_DXIL`).

## Build and hot reload

`cmake/Shaders.cmake` runs `slangc` (from the Vulkan SDK, at least 2026.17) once per stage and
target and writes `<build>/bin/shaders/spirv/<program>.vert.spv` etc., next to the executables,
plus reflection JSON for the tests. Blobs are always compiled with `-O2`, in every build type,
so every configuration renders the same pixels.

At run time the renderers only load those blobs. With `BLUE_SHADER_HOT_RELOAD=ON` (the default)
Debug builds of the game and the editor also watch this directory and recompile a program with
the Slang library when its source or any shared file changes; a shader that fails to compile
keeps its previous version and shows the error. `BLUE_SHADER_HOT_RELOAD=OFF` builds link and
ship no shader compiler.

## Adding a program

1. Write `shaders/<name>.slang` (for a full-screen effect: `#include "fullscreen.slang"` and a
   `FragmentMain(FullscreenVaryings input)`).
2. Add a `blue_shader_program(NAME <name> SOURCE <name>.slang VS VertexMain FS FragmentMain)`
   line to `programs.cmake`.
3. Use `render::ShaderProgram::k<Name>` in the pass (the comment at the top of
   `src/render/vulkan/vulkan_renderer.h` describes adding a pass). New bindings go into
   `gpu_bindings.h` and `src/render/binding_layout.h`.
