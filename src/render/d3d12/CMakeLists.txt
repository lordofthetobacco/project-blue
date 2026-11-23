# The Direct3D 12 renderer backend (namespace blue::render::d3d12): device, resources,
# descriptor heaps, root signatures, pipelines and the frame (design §4). Built when
# BLUE_RENDERER_D3D12 is ON (Windows only). Only blue_render links it
# (CreateD3d12Renderer()); Direct3D 12-only tests link it directly.

include(FetchContent)

# --- Dependencies (pinned, hashed; design §4.1) ---------------------------------------------
# The Agility SDK: the D3D12 runtime (D3D12Core.dll, d3d12SDKLayers.dll) shipped next to the
# executables, and the matching headers. Newest retail version on 2026-11-23.
set(BLUE_AGILITY_SDK_VERSION "1.620.2")
FetchContent_Declare(blue_agility_sdk
    URL      https://api.nuget.org/v3-flatcontainer/microsoft.direct3d.d3d12/${BLUE_AGILITY_SDK_VERSION}/microsoft.direct3d.d3d12.${BLUE_AGILITY_SDK_VERSION}.nupkg
    URL_HASH SHA256=70489669c2bbeaf318b62d72b0ac3e753d521387bfc848c9763791c4e920bfcc
    DOWNLOAD_NAME microsoft.direct3d.d3d12.${BLUE_AGILITY_SDK_VERSION}.zip
    SOURCE_SUBDIR no-cmake)
# WARP from NuGet: the render tests' software adapter is the same everywhere.
set(BLUE_WARP_VERSION "1.0.22")
FetchContent_Declare(blue_warp
    URL      https://api.nuget.org/v3-flatcontainer/microsoft.direct3d.warp/${BLUE_WARP_VERSION}/microsoft.direct3d.warp.${BLUE_WARP_VERSION}.nupkg
    URL_HASH SHA256=afc058f4d1066edc4231a38f3ab3592b1930f7287d4ac700e6a056e0874bb0be
    DOWNLOAD_NAME microsoft.direct3d.warp.${BLUE_WARP_VERSION}.zip
    SOURCE_SUBDIR no-cmake)
# D3D12 Memory Allocator (GPUOpen); built below as blue_d3d12ma, not through its CMake.
FetchContent_Declare(blue_d3d12ma
    URL      https://github.com/GPUOpen-LibrariesAndSDKs/D3D12MemoryAllocator/archive/refs/tags/v3.3.0.tar.gz
    URL_HASH SHA256=04e3ef68cf2608c14c74a0b0190fb3c5e3cbb62e22fb8282ea62a9411d054133
    SOURCE_SUBDIR no-cmake)
FetchContent_MakeAvailable(blue_agility_sdk blue_warp blue_d3d12ma)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(blue_d3d12_arch x64)
else()
    message(FATAL_ERROR "BLUE_RENDERER_D3D12: only x64 builds are supported")
endif()
set(blue_agility_bin "${blue_agility_sdk_SOURCE_DIR}/build/native/bin/${blue_d3d12_arch}")
set(BLUE_AGILITY_RUNTIME_DLLS
    "${blue_agility_bin}/D3D12Core.dll"
    "${blue_agility_bin}/d3d12SDKLayers.dll"
    CACHE INTERNAL "The Agility SDK runtime, copied to <exe dir>/D3D12")
set(BLUE_WARP_DLL "${blue_warp_SOURCE_DIR}/build/native/bin/${blue_d3d12_arch}/d3d10warp.dll"
    CACHE INTERNAL "WARP from NuGet, copied next to the render tests")
foreach(file IN LISTS BLUE_AGILITY_RUNTIME_DLLS BLUE_WARP_DLL)
    if(NOT EXISTS "${file}")
        message(FATAL_ERROR "Direct3D 12 runtime file missing: ${file}")
    endif()
endforeach()

# The Agility SDK headers (d3d12.h, dxgiformat.h, ...) ahead of the Windows SDK's, and the
# system libraries; SYSTEM keeps their warnings out of /W4 /WX builds.
add_library(blue_d3d12_headers INTERFACE)
target_include_directories(blue_d3d12_headers SYSTEM INTERFACE
    "${blue_agility_sdk_SOURCE_DIR}/build/native/include")
target_link_libraries(blue_d3d12_headers INTERFACE d3d12 dxgi dxguid)

add_library(blue_d3d12ma STATIC "${blue_d3d12ma_SOURCE_DIR}/src/D3D12MemAlloc.cpp")
target_include_directories(blue_d3d12ma SYSTEM PUBLIC "${blue_d3d12ma_SOURCE_DIR}/include")
target_link_libraries(blue_d3d12ma PUBLIC blue_d3d12_headers)
target_compile_definitions(blue_d3d12ma PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
blue_restore_msvc_default_flags(blue_d3d12ma)

# ImGui's Direct3D 12 renderer backend (the renderer owns it, design §1.5).
target_sources(imgui PRIVATE ${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp)
target_link_libraries(imgui PRIVATE blue_d3d12_headers)

# --- The backend ----------------------------------------------------------------------------
add_library(blue_render_d3d12 STATIC
    buffer.cc
    d3d12_check.cc
    d3d12_context.cc
    d3d12_renderer.cc
    descriptor_heaps.cc
    format.cc
    fullscreen_pass.cc
    gpu_timer.cc
    image.cc
    material_system.cc
    mesh.cc
    pipeline.cc
    ring_allocator.cc
    root_layout.cc
    root_signature.cc
    samplers.cc
    shader_programs.cc
    slot_allocator.cc
    staging_upload.cc
    swapchain.cc
    texture_cache.cc
    upload_ring.cc
)

target_include_directories(blue_render_d3d12 PUBLIC ${PROJECT_SOURCE_DIR}/src)

blue_target_defaults(blue_render_d3d12)

# The backend's headers name Direct3D, DXGI and D3D12MA types, so whoever includes them (the
# backend's own tests) gets them; consumers of render::Renderer never do (they link
# blue_render).
target_link_libraries(blue_render_d3d12
    PUBLIC
        blue_render_common
        blue_window
        blue_image
        blue_shader
        imgui
        blue_d3d12_headers
        blue_d3d12ma
)

# blue_add_d3d12_runtime(<target> [WARP])
#   For an executable that creates a Direct3D 12 device: compiles the Agility SDK exports into
#   it (agility_exports.cc) and copies D3D12Core.dll and d3d12SDKLayers.dll to
#   <exe dir>/D3D12/ after each build. WARP: also d3d10warp.dll (NuGet WARP) next to it.
function(blue_add_d3d12_runtime target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "WARP" "" "")
    target_sources(${target} PRIVATE "${PROJECT_SOURCE_DIR}/src/render/d3d12/agility_exports.cc")
    target_link_libraries(${target} PRIVATE blue_d3d12_headers)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/D3D12"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${BLUE_AGILITY_RUNTIME_DLLS}
            "$<TARGET_FILE_DIR:${target}>/D3D12"
        VERBATIM)
    if(arg_WARP)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${BLUE_WARP_DLL}"
                "$<TARGET_FILE_DIR:${target}>"
            VERBATIM)
    endif()
endfunction()
