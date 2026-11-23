# Build-time shader compilation: Slang sources in shaders/ -> SPIR-V (Vulkan) and DXIL
# (Direct3D 12) blobs, plus the generated table of programs the renderers and the runtime
# compiler share. See shaders/README.md.
#
#   blue_shader_program(NAME <name> SOURCE <file.slang> VS <entry> [FS <entry>]
#                       [DEFINES <NAME=VALUE>...])
#       Declares one program (a vertex stage and an optional fragment stage of one source
#       file, with preprocessor defines). <name> is snake_case and unique.
#
#   blue_finalize_shaders()
#       Call once after every blue_shader_program(). Creates
#         - target blue_shaders: compiles every program for every enabled target into
#           ${CMAKE_BINARY_DIR}/bin/shaders/<spirv|dxil>/<name>.<vert|frag>.<spv|dxil>
#           (next to the executables; DXIL that no renderer loads goes to
#           ${CMAKE_BINARY_DIR}/shader_check/dxil instead) and its reflection into
#           ${CMAKE_BINARY_DIR}/shader_reflection/<target>/<name>.<stage>.json;
#         - ${CMAKE_BINARY_DIR}/generated/render/shader_manifest.h
#           (render::ShaderProgram, render::ShaderProgramInfoOf());
#         - target blue_shader_manifest (INTERFACE): the include path of that header.
#       Sets BLUE_SHADER_BLOB_DIR and BLUE_SHADER_REFLECTION_DIR in the caller's scope.
#
# Targets: spirv when BLUE_RENDERER_VULKAN; dxil when BLUE_RENDERER_D3D12 or
# BLUE_SHADERS_DXIL (default ON on Windows: the DXIL of every program is compiled and
# checked by the tests even while only Vulkan renders, so a shader that Direct3D rejects
# fails the build at once).
#
# Flags are the same here and in the runtime compiler (src/shader/slang_compiler.cc):
# column-major matrices (glm's layout; shaders write mul(M, v)), every warning an error,
# -O2. Build-time blobs are always optimized, whatever the build type, so every
# configuration renders the same pixels; shader::CompileMode only affects hot reloads.

# The oldest Slang the shaders were validated with (Vulkan SDK 1.4.363).
set(BLUE_SLANG_MIN_VERSION "2026.17")

if(WIN32)
    set(blue_shaders_dxil_default ON)
else()
    set(blue_shaders_dxil_default OFF)
endif()
option(BLUE_SHADERS_DXIL
    "Also compile every shader to DXIL (checked by the tests) when Direct3D 12 is off"
    ${blue_shaders_dxil_default})

set_property(GLOBAL PROPERTY BLUE_SHADER_PROGRAMS "")

function(blue_shader_program)
    cmake_parse_arguments(PARSE_ARGV 0 arg "" "NAME;SOURCE;VS;FS" "DEFINES")
    if(NOT arg_NAME OR NOT arg_SOURCE OR NOT arg_VS)
        message(FATAL_ERROR "blue_shader_program: NAME, SOURCE and VS are required")
    endif()
    if(NOT arg_NAME MATCHES "^[a-z][a-z0-9_]*$")
        message(FATAL_ERROR "blue_shader_program: NAME '${arg_NAME}' must be snake_case")
    endif()
    get_property(programs GLOBAL PROPERTY BLUE_SHADER_PROGRAMS)
    if(arg_NAME IN_LIST programs)
        message(FATAL_ERROR "blue_shader_program: duplicate program '${arg_NAME}'")
    endif()
    set_property(GLOBAL APPEND PROPERTY BLUE_SHADER_PROGRAMS "${arg_NAME}")
    set_property(GLOBAL PROPERTY BLUE_SHADER_${arg_NAME}_SOURCE "${arg_SOURCE}")
    set_property(GLOBAL PROPERTY BLUE_SHADER_${arg_NAME}_VS "${arg_VS}")
    set_property(GLOBAL PROPERTY BLUE_SHADER_${arg_NAME}_FS "${arg_FS}")
    set_property(GLOBAL PROPERTY BLUE_SHADER_${arg_NAME}_DEFINES "${arg_DEFINES}")
endfunction()

# Sets <out> to the CamelCase of snake_case <name>.
function(blue_shader_camel_case name out)
    string(REPLACE "_" ";" words "${name}")
    set(result "")
    foreach(word IN LISTS words)
        string(SUBSTRING "${word}" 0 1 first)
        string(SUBSTRING "${word}" 1 -1 rest)
        string(TOUPPER "${first}" first)
        string(APPEND result "${first}${rest}")
    endforeach()
    set(${out} "${result}" PARENT_SCOPE)
endfunction()

# Finds slangc (and checks its version) into the cache variable BLUE_SLANGC.
function(blue_find_slangc)
    set(hints "")
    if(DEFINED ENV{VULKAN_SDK})
        list(APPEND hints "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
    endif()
    if(Vulkan_INCLUDE_DIR)
        cmake_path(GET Vulkan_INCLUDE_DIR PARENT_PATH sdk_dir)
        list(APPEND hints "${sdk_dir}/Bin" "${sdk_dir}/bin")
    endif()
    find_program(BLUE_SLANGC slangc HINTS ${hints}
        DOC "The Slang compiler (ships with the Vulkan SDK)")
    if(NOT BLUE_SLANGC)
        message(FATAL_ERROR "slangc not found. It ships with the Vulkan SDK "
            "(>= 1.3.296); set VULKAN_SDK or BLUE_SLANGC.")
    endif()
    execute_process(COMMAND "${BLUE_SLANGC}" -v
        OUTPUT_VARIABLE version_out ERROR_VARIABLE version_err
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
    # slangc prints e.g. "2026.17-25-gc7954ebd1" (to stderr in some versions).
    string(REGEX MATCH "[0-9]+\\.[0-9]+(\\.[0-9]+)?" version "${version_out}${version_err}")
    if(NOT version)
        message(FATAL_ERROR "Could not read the version of ${BLUE_SLANGC}")
    endif()
    if(version VERSION_LESS BLUE_SLANG_MIN_VERSION)
        message(FATAL_ERROR "slangc ${version} is too old: the shaders need "
            ">= ${BLUE_SLANG_MIN_VERSION} (${BLUE_SLANGC})")
    endif()
    message(STATUS "Slang: ${BLUE_SLANGC} (${version})")
endfunction()

function(blue_finalize_shaders)
    blue_find_slangc()

    set(source_dir "${CMAKE_SOURCE_DIR}/shaders")
    set(blob_dir "${CMAKE_BINARY_DIR}/bin/shaders")
    set(reflection_dir "${CMAKE_BINARY_DIR}/shader_reflection")
    set(BLUE_SHADER_BLOB_DIR "${blob_dir}" PARENT_SCOPE)
    set(BLUE_SHADER_REFLECTION_DIR "${reflection_dir}" PARENT_SCOPE)

    set(targets "")
    if(BLUE_RENDERER_VULKAN)
        list(APPEND targets spirv)
    endif()
    if(BLUE_RENDERER_D3D12 OR BLUE_SHADERS_DXIL)
        list(APPEND targets dxil)
    endif()
    set(spirv_flags -target spirv -profile spirv_1_6)
    set(spirv_extension spv)
    set(spirv_dir "${blob_dir}/spirv")
    # Next to the executables only when a renderer loads them; DXIL compiled just to be
    # checked stays out of what ships.
    if(BLUE_RENDERER_D3D12)
        set(dxil_dir "${blob_dir}/dxil")
    else()
        set(dxil_dir "${CMAKE_BINARY_DIR}/shader_check/dxil")
    endif()
    # Shader Model 6.0: the widest hardware; no later feature is used.
    set(dxil_flags -target dxil -profile sm_6_0)
    set(dxil_extension dxil)

    # Every program depends on every source: an edit to a shared file recompiles all of
    # them (seconds, in parallel), and a new include needs no CMake edit.
    file(GLOB shader_sources CONFIGURE_DEPENDS "${source_dir}/*.slang" "${source_dir}/*.h")

    get_property(programs GLOBAL PROPERTY BLUE_SHADER_PROGRAMS)
    set(outputs "")
    set(enumerators "")
    set(define_arrays "")
    set(entries "")
    foreach(name IN LISTS programs)
        get_property(source GLOBAL PROPERTY BLUE_SHADER_${name}_SOURCE)
        get_property(vs GLOBAL PROPERTY BLUE_SHADER_${name}_VS)
        get_property(fs GLOBAL PROPERTY BLUE_SHADER_${name}_FS)
        get_property(defines GLOBAL PROPERTY BLUE_SHADER_${name}_DEFINES)
        if(NOT EXISTS "${source_dir}/${source}")
            message(FATAL_ERROR "Shader program ${name}: ${source_dir}/${source} not found")
        endif()

        set(define_flags "")
        set(define_strings "")
        foreach(define IN LISTS defines)
            list(APPEND define_flags "-D${define}")
            string(APPEND define_strings "\"${define}\", ")
        endforeach()

        foreach(target IN LISTS targets)
            foreach(stage IN ITEMS vertex fragment)
                if(stage STREQUAL "vertex")
                    set(entry "${vs}")
                    set(short vert)
                else()
                    set(entry "${fs}")
                    set(short frag)
                endif()
                if(NOT entry)
                    continue()
                endif()
                set(output "${${target}_dir}/${name}.${short}.${${target}_extension}")
                set(reflection "${reflection_dir}/${target}/${name}.${short}.json")
                add_custom_command(
                    OUTPUT "${output}" "${reflection}"
                    COMMAND "${CMAKE_COMMAND}" -E make_directory
                            "${${target}_dir}" "${reflection_dir}/${target}"
                    COMMAND "${BLUE_SLANGC}" "${source_dir}/${source}"
                            -entry "${entry}" -stage ${stage} ${${target}_flags}
                            -matrix-layout-column-major -warnings-as-errors all -O2
                            ${define_flags}
                            -reflection-json "${reflection}" -o "${output}"
                    DEPENDS ${shader_sources}
                    COMMENT "Slang ${target} ${name}.${short}"
                    VERBATIM)
                list(APPEND outputs "${output}")
            endforeach()
        endforeach()

        blue_shader_camel_case("${name}" camel)
        string(APPEND enumerators "  k${camel},\n")
        if(define_strings)
            list(LENGTH defines define_count)
            string(APPEND define_arrays
                "inline constexpr std::array<std::string_view, ${define_count}> "
                "kDefines${camel} = {${define_strings}};\n")
            set(define_span "kDefines${camel}")
        else()
            set(define_span "{}")
        endif()
        string(APPEND entries
            "    ShaderProgramInfo{\"${name}\", \"${source}\", \"${vs}\", \"${fs}\", "
            "${define_span}},\n")
    endforeach()

    add_custom_target(blue_shaders ALL DEPENDS ${outputs})

    list(LENGTH programs program_count)
    set(manifest "// Generated by cmake/Shaders.cmake from the blue_shader_program() list. Do not edit.
#ifndef BLUE_RENDER_SHADER_MANIFEST_H_
#define BLUE_RENDER_SHADER_MANIFEST_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace blue::render {

// Every shader program of the renderer, in the order of the build's list.
enum class ShaderProgram : uint8_t {
${enumerators}};

inline constexpr size_t kShaderProgramCount = ${program_count};

// How a program is compiled: by the build (cmake/Shaders.cmake) and, with the
// same values, by the runtime compiler on a hot reload.
struct ShaderProgramInfo {
  // Blob files are <name>.vert.<ext> and <name>.frag.<ext>.
  std::string_view name;
  // File in shaders/.
  std::string_view source;
  std::string_view vertex_entry;
  // Empty: no fragment stage (depth only).
  std::string_view fragment_entry;
  // Preprocessor defines, each \"NAME=VALUE\".
  std::span<const std::string_view> defines;
};

namespace shader_manifest_internal {

${define_arrays}
inline constexpr std::array<ShaderProgramInfo, kShaderProgramCount> kPrograms = {
${entries}};

}  // namespace shader_manifest_internal

// Every program, indexed by ShaderProgram.
inline constexpr std::span<const ShaderProgramInfo> kShaderPrograms =
    shader_manifest_internal::kPrograms;

[[nodiscard]] constexpr const ShaderProgramInfo& ShaderProgramInfoOf(
    ShaderProgram program) {
  return kShaderPrograms[static_cast<size_t>(program)];
}

}  // namespace blue::render

#endif  // BLUE_RENDER_SHADER_MANIFEST_H_
")
    # Written only when the content changes, so reconfiguring does not rebuild its users.
    file(CONFIGURE OUTPUT "${CMAKE_BINARY_DIR}/generated/render/shader_manifest.h"
        CONTENT "${manifest}" @ONLY)

    add_library(blue_shader_manifest INTERFACE)
    target_include_directories(blue_shader_manifest INTERFACE "${CMAKE_BINARY_DIR}/generated")
endfunction()
