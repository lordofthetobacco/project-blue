# Warnings, compile definitions and code generation shared by every first-party target, and
# the build-wide compiler, linker and cache settings that apply to first- and third-party
# code alike. Libraries expose the GLM/platform definitions publicly because they change glm
# types in headers; executables keep them private.
#
# blue_target_defaults(<target> [ALLOW_EXCEPTIONS_AND_RTTI])
#   ALLOW_EXCEPTIONS_AND_RTTI keeps exceptions and RTTI on; only the doctest executable
#   needs them (REQUIRE unwinds through an exception).
#
# Optimisation level per configuration (identical for first- and third-party code):
#   Debug           unoptimised (/Od, -O0); third-party cook libraries are the exception,
#                   see cmake/Dependencies.cmake.
#   Release         /O2 /Ob2 (MSVC, clang-cl), -O2 (GCC, Clang, AppleClang), NDEBUG, link-time
#                   optimisation (BLUE_ENABLE_LTO).
#   RelWithDebInfo  same code generation as Release plus debug info, no LTO (quick relinks).
#   MinSizeRel      CMake's size configuration (/O1, -Os), the one place that trades speed for
#                   size on purpose; also with LTO.
# CMake's own Release default is -O3 on GCC/Clang, replaced by -O2 below. Not used on purpose:
# fast-math (/fp:fast, -ffast-math: reassociation and finite-math assumptions change physics
# results and break cross-platform determinism), exceptions and RTTI stay off as before.
#
# Options (all cacheable, defaults chosen so the binary runs on any x64 / arm64 CPU):
#   BLUE_ENABLE_LTO            Release/MinSizeRel link-time optimisation (thin LTO on Clang).
#   BLUE_ENABLE_AVX2           Compile everything for x86-64-v3 (AVX2, FMA, BMI); x64 only.
#   BLUE_USE_COMPILER_CACHE    Use sccache or ccache when found.
#   BLUE_LINKER                auto | default | mold | lld (Linux; faster links when present).
#   BLUE_PRECOMPILED_HEADERS   Precompiled standard-library headers for first-party targets.
#   BLUE_UNITY_BUILD           Unity builds (batches of translation units) for first-party targets.
#
# MSVC: CMake's default CMAKE_CXX_FLAGS holds /EHsc, which first-party targets replace with
# /EHs-c-; cl prints a D9025 "overriding" note for every file when both are present.
# blue_remove_msvc_default_flags() takes /EHsc out of the directory flags (cmake/Dependencies.cmake
# calls it once all FetchContent sub-projects, which keep the defaults they copied, are added).
# Targets that still want it get it back from blue_restore_msvc_default_flags(): the root-scope
# third-party libraries and, through blue_target_defaults(... ALLOW_EXCEPTIONS_AND_RTTI), the
# doctest executable.

include(CheckIPOSupported)
include(CheckLinkerFlag)

option(BLUE_ENABLE_LTO "Link-time optimisation for Release and MinSizeRel" ON)
option(BLUE_ENABLE_AVX2 "Target x86-64-v3 (AVX2/FMA/BMI) instead of baseline x64" OFF)
option(BLUE_USE_COMPILER_CACHE "Use sccache/ccache as compiler launcher when found" ON)
option(BLUE_PRECOMPILED_HEADERS "Precompiled standard headers for first-party targets" OFF)
option(BLUE_UNITY_BUILD "Unity builds for first-party targets" OFF)
set(BLUE_LINKER "auto" CACHE STRING "Linker on Linux: auto, default, mold or lld")
set_property(CACHE BLUE_LINKER PROPERTY STRINGS auto default mold lld)

get_property(blue_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)

# --- Optimisation level 2 everywhere (first- and third-party) -----------------------------
if(NOT MSVC)
    # Release defaults to -O3 on GCC/Clang.
    foreach(lang C CXX)
        string(REPLACE "-O3" "-O2" CMAKE_${lang}_FLAGS_RELEASE "${CMAKE_${lang}_FLAGS_RELEASE}")
    endforeach()
else()
    # RelWithDebInfo defaults to /Ob1 (inline only __inline); Release already has /Ob2.
    foreach(lang C CXX)
        string(REPLACE "/Ob1" "/Ob2" CMAKE_${lang}_FLAGS_RELWITHDEBINFO
            "${CMAKE_${lang}_FLAGS_RELWITHDEBINFO}")
    endforeach()
endif()

# --- Link-time optimisation -----------------------------------------------------------------
# The default is read when a target is created, so third-party libraries (Jolt, SDL3, ...)
# are included. The support check compiles a try-project, so it only runs when it matters.
if(BLUE_ENABLE_LTO AND (blue_multi_config OR CMAKE_BUILD_TYPE MATCHES "^(Release|MinSizeRel)$"))
    check_ipo_supported(RESULT blue_ipo_supported OUTPUT blue_ipo_output LANGUAGES C CXX)
    if(blue_ipo_supported)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL ON)
    else()
        message(WARNING "LTO not supported by this toolchain, continuing without: ${blue_ipo_output}")
    endif()
endif()

# --- Instruction set -------------------------------------------------------------------------
if(BLUE_ENABLE_AVX2)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|x86_64|x64|X86)$" OR
       CMAKE_OSX_ARCHITECTURES STREQUAL "x86_64")
        if(MSVC)
            add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:/arch:AVX2>)
        else()
            add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:-march=x86-64-v3>)
        endif()
    else()
        message(STATUS "BLUE_ENABLE_AVX2 ignored: target is not x86-64")
    endif()
endif()

# --- Compiler cache ----------------------------------------------------------------------------
if(BLUE_USE_COMPILER_CACHE AND NOT CMAKE_CXX_COMPILER_LAUNCHER)
    find_program(BLUE_COMPILER_CACHE NAMES sccache ccache)
    if(BLUE_COMPILER_CACHE)
        message(STATUS "Compiler cache: ${BLUE_COMPILER_CACHE}")
        set(CMAKE_C_COMPILER_LAUNCHER "${BLUE_COMPILER_CACHE}")
        set(CMAKE_CXX_COMPILER_LAUNCHER "${BLUE_COMPILER_CACHE}")
        if(MSVC)
            # /Zi writes a shared PDB the cache cannot reproduce; /Z7 embeds debug info in objects.
            set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>")
        endif()
    endif()
endif()

# --- Linker (Linux) ------------------------------------------------------------------------------
if(UNIX AND NOT APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND
   NOT BLUE_LINKER STREQUAL "default")
    if(BLUE_LINKER STREQUAL "auto")
        set(blue_linker_candidates mold lld)
    else()
        set(blue_linker_candidates ${BLUE_LINKER})
    endif()
    foreach(candidate IN LISTS blue_linker_candidates)
        find_program(blue_linker_${candidate} NAMES ld.${candidate} ${candidate} NO_CACHE)
        if(blue_linker_${candidate})
            check_linker_flag(CXX "-fuse-ld=${candidate}" blue_has_fuse_ld_${candidate})
            if(blue_has_fuse_ld_${candidate})
                message(STATUS "Linker: ${candidate}")
                add_link_options("-fuse-ld=${candidate}")
                break()
            endif()
        endif()
    endforeach()
endif()

# --- Generator specific ------------------------------------------------------------------------
if(MSVC AND CMAKE_GENERATOR MATCHES "Visual Studio")
    # Ninja already runs one cl per core; /MP there only oversubscribes the machine.
    add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:/MP>)
endif()

# Removes every whole-word `flag` from the space-separated flags in `variable`.
function(blue_remove_flags variable)
    set(flags " ${${variable}} ")
    foreach(flag IN LISTS ARGN)
        string(REPLACE " ${flag} " " " flags "${flags}")
    endforeach()
    string(REGEX REPLACE " +" " " flags "${flags}")
    string(STRIP "${flags}" flags)
    set(${variable} "${flags}" PARENT_SCOPE)
endfunction()

# Removes /EHsc from the calling directory's C++ flags and records that it was there, for
# blue_restore_msvc_default_flags().
function(blue_remove_msvc_default_flags)
    if(NOT MSVC)
        return()
    endif()
    set(removed "")
    if(" ${CMAKE_CXX_FLAGS} " MATCHES " /EHsc ")
        set(removed /EHsc)
    endif()
    blue_remove_flags(CMAKE_CXX_FLAGS ${removed})
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}" PARENT_SCOPE)
    set_property(GLOBAL PROPERTY blue_removed_cxx_flags "${removed}")
endfunction()

# Gives `target` back the C++ flags blue_remove_msvc_default_flags() removed, so it
# compiles exactly as with CMake's defaults (no-op elsewhere or before the removal).
function(blue_restore_msvc_default_flags target)
    get_property(removed GLOBAL PROPERTY blue_removed_cxx_flags)
    if(removed)
        target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${removed}>)
    endif()
endfunction()

function(blue_target_defaults target)
    cmake_parse_arguments(ARG "ALLOW_EXCEPTIONS_AND_RTTI" "" "" ${ARGN})

    get_target_property(type ${target} TYPE)
    if(type STREQUAL "EXECUTABLE")
        set(visibility PRIVATE)
    else()
        set(visibility PUBLIC)
    endif()

    target_compile_definitions(${target} ${visibility}
        GLM_FORCE_DEPTH_ZERO_TO_ONE
        GLM_FORCE_RADIANS
        $<$<PLATFORM_ID:Windows>:NOMINMAX WIN32_LEAN_AND_MEAN>
    )

    set(optimized_configs "$<CONFIG:Release,RelWithDebInfo,MinSizeRel>")

    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /permissive- /utf-8 /external:W0
            $<${optimized_configs}:/Gy /Gw>)
        if(ARG_ALLOW_EXCEPTIONS_AND_RTTI)
            # The removed /EHsc.
            get_property(removed GLOBAL PROPERTY blue_removed_cxx_flags)
            if(removed)
                target_compile_options(${target} PRIVATE ${removed})
            endif()
        else()
            # /EHs-c- alone makes the STL headers warn about try blocks; _HAS_EXCEPTIONS=0
            # switches them to their exception-free paths.
            target_compile_options(${target} PRIVATE /EHs-c- /GR-)
            target_compile_definitions(${target} PRIVATE _HAS_EXCEPTIONS=0)
        endif()
        if(type STREQUAL "EXECUTABLE")
            # ICF needs a non-incremental link; RelWithDebInfo's /DEBUG would otherwise turn
            # REF and ICF off.
            target_link_options(${target} PRIVATE
                $<${optimized_configs}:/OPT:REF /OPT:ICF /INCREMENTAL:NO>)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror
            $<${optimized_configs}:-ffunction-sections -fdata-sections>)
        if(NOT ARG_ALLOW_EXCEPTIONS_AND_RTTI)
            target_compile_options(${target} PRIVATE -fno-exceptions -fno-rtti)
        endif()
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT CMAKE_CXX_COMPILER_LAUNCHER)
            # Debug info in .dwo files keeps Debug links short.
            target_compile_options(${target} PRIVATE $<$<CONFIG:Debug>:-gsplit-dwarf>)
        endif()
        if(type STREQUAL "EXECUTABLE")
            if(APPLE)
                # Apple's ld: drop unreferenced sections.
                target_link_options(${target} PRIVATE
                    $<${optimized_configs}:LINKER:-dead_strip>)
            else()
                target_link_options(${target} PRIVATE
                    $<${optimized_configs}:LINKER:--gc-sections>)
            endif()
        endif()
    endif()

    if(BLUE_PRECOMPILED_HEADERS)
        target_precompile_headers(${target} PRIVATE
            <algorithm> <array> <cstdint> <memory> <optional> <span> <string> <string_view>
            <unordered_map> <utility> <vector>)
    endif()
    if(BLUE_UNITY_BUILD)
        set_target_properties(${target} PROPERTIES UNITY_BUILD ON UNITY_BUILD_BATCH_SIZE 8)
    endif()
endfunction()
