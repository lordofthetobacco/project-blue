# Renderer backend options, their validation and the default backend; finds the Vulkan SDK
# when the Vulkan backend is on. Included by the root CMakeLists.txt except in sim-only builds
# (BLUE_SIM_ONLY), which build no renderer.

# --- Renderer backends --------------------------------------------------------------------
# Each backend is a native renderer behind render::Renderer (src/render/renderer_backend.h),
# in its own directory with its own CMakeLists.txt. Vulkan builds everywhere (macOS: MoltenVK
# from the Vulkan SDK); D3D12 only on Windows, Metal only on Apple platforms. A backend whose
# directory does not exist yet (D3D12 before package D1, Metal before the Metal branch is
# merged) defaults to OFF and cannot be switched on.
set(blue_d3d12_available OFF)
if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/render/d3d12/CMakeLists.txt")
    set(blue_d3d12_available ON)
endif()
set(blue_metal_available OFF)
if(APPLE AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/render/metal/CMakeLists.txt")
    set(blue_metal_available ON)
endif()

# Settings whose default is computed ("automatic") follow a changed automatic value in an
# existing cache, while a value set by hand (or by -D) is kept: <var>_AUTO (internal) records
# the automatic value of the last configure, and a cache entry equal to it is moved to the new
# one. Caches written before these records existed are recognised by the STRINGS property of
# BLUE_DEFAULT_RENDERER (a fresh -D value has none); their automatic value was <legacy_auto>.
# Call before the option() / set(CACHE) that defines <var>. BOOL values compare as ON/OFF.
get_property(blue_cache_predates_auto_records CACHE BLUE_DEFAULT_RENDERER PROPERTY STRINGS)
function(blue_follow_automatic_default var type auto legacy_auto)
    if(DEFINED ${var}_AUTO)
        set(previous "${${var}_AUTO}")
    elseif(blue_cache_predates_auto_records)
        set(previous "${legacy_auto}")
    else()
        set(previous "")
    endif()
    set(current "${${var}}")
    if(type STREQUAL "BOOL")
        foreach(value IN ITEMS auto previous current)
            if(NOT "${${value}}" STREQUAL "")
                if(${${value}})
                    set(${value} ON)
                else()
                    set(${value} OFF)
                endif()
            endif()
        endforeach()
    endif()
    if(DEFINED ${var} AND NOT previous STREQUAL ""
       AND current STREQUAL previous AND NOT current STREQUAL auto)
        message(STATUS "${var} follows the new automatic default: ${previous} -> ${auto}")
        set_property(CACHE ${var} PROPERTY VALUE "${auto}")
    endif()
    set(${var}_AUTO "${auto}" CACHE INTERNAL "The automatic ${var} of the last configure")
endfunction()

option(BLUE_RENDERER_VULKAN "Build the Vulkan renderer backend" ON)
# Caches from before src/render/d3d12 (or src/render/metal) existed hold an automatic OFF.
blue_follow_automatic_default(BLUE_RENDERER_D3D12 BOOL ${blue_d3d12_available} OFF)
option(BLUE_RENDERER_D3D12 "Build the Direct3D 12 renderer backend (Windows)" ${blue_d3d12_available})
blue_follow_automatic_default(BLUE_RENDERER_METAL BOOL ${blue_metal_available} OFF)
option(BLUE_RENDERER_METAL "Build the Metal renderer backend (Apple)" ${blue_metal_available})
option(BLUE_SHADER_HOT_RELOAD
    "Link the runtime shader compiler and reload shaders when their sources change" ON)

if(BLUE_RENDERER_D3D12 AND NOT WIN32)
    message(FATAL_ERROR "BLUE_RENDERER_D3D12 is only available on Windows")
endif()
if(BLUE_RENDERER_METAL AND NOT APPLE)
    message(FATAL_ERROR "BLUE_RENDERER_METAL is only available on Apple platforms")
endif()
if(BLUE_RENDERER_D3D12 AND NOT blue_d3d12_available)
    message(FATAL_ERROR "BLUE_RENDERER_D3D12: src/render/d3d12 does not exist yet")
endif()
if(BLUE_RENDERER_METAL AND NOT blue_metal_available)
    message(FATAL_ERROR "BLUE_RENDERER_METAL: src/render/metal does not exist yet")
endif()
if(NOT BLUE_RENDERER_VULKAN AND NOT BLUE_RENDERER_D3D12 AND NOT BLUE_RENDERER_METAL)
    message(FATAL_ERROR "Enable at least one renderer backend (BLUE_RENDERER_VULKAN, "
        "BLUE_RENDERER_D3D12, BLUE_RENDERER_METAL)")
endif()
if(NOT BLUE_SHADER_HOT_RELOAD AND BLUE_RENDERER_METAL)
    # Vulkan and Direct3D 12 load the blobs the build compiles (cmake/Shaders.cmake); the
    # Metal backend still compiles its MSL sources at run time.
    message(FATAL_ERROR "BLUE_SHADER_HOT_RELOAD=OFF is not supported with "
        "BLUE_RENDERER_METAL yet")
endif()

# The backend used unless --renderer= or BLUE_RENDERER names another one:
# Metal on Apple, Direct3D 12 on Windows, Vulkan elsewhere (design §1.4).
if(BLUE_RENDERER_METAL)
    set(blue_default_renderer metal)
elseif(BLUE_RENDERER_D3D12)
    set(blue_default_renderer d3d12)
else()
    set(blue_default_renderer vulkan)
endif()
# Caches from before the record held an automatic "vulkan".
blue_follow_automatic_default(BLUE_DEFAULT_RENDERER STRING ${blue_default_renderer} vulkan)
set(BLUE_DEFAULT_RENDERER "${blue_default_renderer}" CACHE STRING
    "Renderer backend used unless --renderer= or BLUE_RENDERER names another (vulkan, d3d12, metal)")
set_property(CACHE BLUE_DEFAULT_RENDERER PROPERTY STRINGS vulkan d3d12 metal)
string(TOUPPER "${BLUE_DEFAULT_RENDERER}" blue_default_renderer_upper)
if(NOT BLUE_DEFAULT_RENDERER MATCHES "^(vulkan|d3d12|metal)$"
   OR NOT BLUE_RENDERER_${blue_default_renderer_upper})
    message(FATAL_ERROR "BLUE_DEFAULT_RENDERER '${BLUE_DEFAULT_RENDERER}' must name an "
        "enabled backend")
endif()
message(STATUS "Renderer backends: Vulkan ${BLUE_RENDERER_VULKAN}, D3D12 ${BLUE_RENDERER_D3D12}, "
    "Metal ${BLUE_RENDERER_METAL}; default ${BLUE_DEFAULT_RENDERER}")

if(BLUE_RENDERER_VULKAN)
    find_package(Vulkan 1.3 REQUIRED)
endif()
