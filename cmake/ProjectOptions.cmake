# Warnings, compile definitions and size-oriented code generation shared by every
# first-party target. Libraries expose the GLM/platform definitions publicly because they
# change glm types in headers; executables keep them private.
#
# blue_target_defaults(<target> [ALLOW_EXCEPTIONS_AND_RTTI])
#   ALLOW_EXCEPTIONS_AND_RTTI keeps exceptions and RTTI on; only the doctest executable
#   needs them (REQUIRE unwinds through an exception).
#
# Release and MinSizeRel builds optimize for size: -Oz/-Os (/O1), link-time optimization,
# per-function sections with dead-section garbage collection, stripped symbols. Debug and
# RelWithDebInfo keep CMake's defaults so stepping through code stays pleasant.
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

    set(size_configs "$<CONFIG:Release,MinSizeRel>")

    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /permissive- /utf-8
            $<${size_configs}:/O1 /Gy /Gw>)
        if(NOT ARG_ALLOW_EXCEPTIONS_AND_RTTI)
            # /EHs-c- alone makes the STL headers warn about try blocks; _HAS_EXCEPTIONS=0
            # switches them to their exception-free paths.
            target_compile_options(${target} PRIVATE /EHs-c- /GR-)
            target_compile_definitions(${target} PRIVATE _HAS_EXCEPTIONS=0)
        endif()
        if(type STREQUAL "EXECUTABLE")
            target_link_options(${target} PRIVATE $<${size_configs}:/OPT:REF /OPT:ICF>)
        endif()
    else()
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            set(size_opt -Oz)
        else()
            set(size_opt -Os)
        endif()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror
            $<${size_configs}:${size_opt} -ffunction-sections -fdata-sections>)
        if(NOT ARG_ALLOW_EXCEPTIONS_AND_RTTI)
            target_compile_options(${target} PRIVATE -fno-exceptions -fno-rtti)
        endif()
        if(type STREQUAL "EXECUTABLE")
            if(APPLE)
                # Apple's ld: drop unreferenced sections, debug info and local symbols.
                target_link_options(${target} PRIVATE
                    $<${size_configs}:LINKER:-dead_strip,-S,-x>)
            else()
                target_link_options(${target} PRIVATE
                    $<${size_configs}:LINKER:--gc-sections -s>)
            endif()
        endif()
    endif()

    # Link-time optimization across the first-party objects. Third-party libraries keep
    # their own settings (Jolt disables its IPO option in cmake/Dependencies.cmake).
    set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL ON)
endfunction()
