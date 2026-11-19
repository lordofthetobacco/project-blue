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
#
# MSVC: CMake's default CMAKE_CXX_FLAGS holds /EHsc and CMAKE_CXX_FLAGS_RELEASE /O2, which
# first-party targets switch to /EHs-c- and /O1; cl then prints a D9025 "overriding" note
# for every file. blue_remove_msvc_default_flags() takes both out of the directory flags
# (cmake/Dependencies.cmake calls it once all FetchContent sub-projects, which keep the
# defaults they copied, are added). Targets that still want them get them back from
# blue_restore_msvc_default_flags(): the root-scope third-party libraries and, through
# blue_target_defaults(... ALLOW_EXCEPTIONS_AND_RTTI), the doctest executable.

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

# Removes /EHsc and the Release /O2 from the calling directory's C++ flags and records
# which of them were there, for blue_restore_msvc_default_flags().
function(blue_remove_msvc_default_flags)
    if(NOT MSVC)
        return()
    endif()
    set(removed "")
    set(removed_release "")
    if(" ${CMAKE_CXX_FLAGS} " MATCHES " /EHsc ")
        set(removed /EHsc)
    endif()
    if(" ${CMAKE_CXX_FLAGS_RELEASE} " MATCHES " /O2 ")
        set(removed_release /O2)
    endif()
    blue_remove_flags(CMAKE_CXX_FLAGS ${removed})
    blue_remove_flags(CMAKE_CXX_FLAGS_RELEASE ${removed_release})
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}" PARENT_SCOPE)
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}" PARENT_SCOPE)
    set_property(GLOBAL PROPERTY blue_removed_cxx_flags "${removed}")
    set_property(GLOBAL PROPERTY blue_removed_cxx_flags_release "${removed_release}")
endfunction()

# Gives `target` back the C++ flags blue_remove_msvc_default_flags() removed, so it
# compiles exactly as with CMake's defaults (no-op elsewhere or before the removal).
function(blue_restore_msvc_default_flags target)
    get_property(removed GLOBAL PROPERTY blue_removed_cxx_flags)
    get_property(removed_release GLOBAL PROPERTY blue_removed_cxx_flags_release)
    if(removed)
        target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${removed}>)
    endif()
    if(removed_release)
        target_compile_options(${target} PRIVATE
            $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CONFIG:Release>>:${removed_release}>)
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

    set(size_configs "$<CONFIG:Release,MinSizeRel>")

    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /permissive- /utf-8
            $<${size_configs}:/O1 /Gy /Gw>)
        if(ARG_ALLOW_EXCEPTIONS_AND_RTTI)
            # The removed /EHsc; its Release /O2 is replaced by /O1 above anyway.
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
        # Objective-C++ (the Metal backend) is always ARC: Metal objects are plain strong
        # members, with no manual retain / release.
        target_compile_options(${target} PRIVATE
            $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
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
