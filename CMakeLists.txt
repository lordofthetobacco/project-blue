cmake_minimum_required(VERSION 3.25)

# Set before project(). Metal 3 with MSL 3.2 (MTLMathMode, MTLLanguageVersion3_2) needs
# macOS 15; the floating-point std::from_chars the engine parses numbers with needs 26.
set(CMAKE_OSX_DEPLOYMENT_TARGET "26.0" CACHE STRING "Minimum macOS version")

project(project-blue
    VERSION 0.1.0
    DESCRIPTION "Lightweight extraction shooter"
    LANGUAGES C CXX)

if(NOT APPLE)
    message(FATAL_ERROR
        "The metal branch renders with Apple's Metal and builds on macOS only; "
        "Windows builds stay on main.")
endif()

# The Metal backend is Objective-C++ (ARC, see cmake/ProjectOptions.cmake).
enable_language(OBJCXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# ProjectOptions first: cmake/Dependencies.cmake uses its MSVC flag helpers.
include(ProjectOptions)
include(Dependencies)
include(ShaderSources)
include(SampleAssets)

# Path handling and file system access (src/core/path.cc is pure; file_system.cc sits on
# SDL3). One small library because the file system layer depends on the path functions
# and every consumer needs both.
add_library(blue_core STATIC
    src/core/path.cc
    src/platform/file_system.cc
)

target_include_directories(blue_core PUBLIC src)

blue_target_defaults(blue_core)

target_link_libraries(blue_core PUBLIC SDL3::SDL3-static)

# Model Context Protocol server (Streamable HTTP on loopback, JSON-RPC, own JSON
# type) used by the editor. Plain sockets; no editor, ImGui or Vulkan code.
add_library(blue_mcp STATIC
    src/mcp/http.cc
    src/mcp/json.cc
    src/mcp/protocol.cc
    src/mcp/server.cc
    src/mcp/tool.cc
    src/mcp/internal/socket.cc
)

target_include_directories(blue_mcp PUBLIC src)

blue_target_defaults(blue_mcp)

find_package(Threads REQUIRED)
target_link_libraries(blue_mcp
    PUBLIC blue_core
    PRIVATE Threads::Threads $<$<PLATFORM_ID:Windows>:ws2_32>
)

# Rigid node animation: transform hierarchy, keyframe clips (glTF sampling rules)
# and a playback cursor. glm only, no allocation per frame.
add_library(blue_animation STATIC
    src/animation/clip.cc
    src/animation/clip_player.cc
    src/animation/hierarchy.cc
    src/animation/transform.cc
)

target_include_directories(blue_animation PUBLIC src)

blue_target_defaults(blue_animation)

target_link_libraries(blue_animation PUBLIC glm::glm)

# PBR material descriptions and .bmat material files (glm + blue_core only).
add_library(blue_material STATIC
    src/material/material.cc
    src/material/material_file.cc
)

target_include_directories(blue_material PUBLIC src)

blue_target_defaults(blue_material)

target_link_libraries(blue_material PUBLIC blue_core glm::glm)

# CPU mesh types (asset::Vertex, MeshData, Bounds) and the builtin shapes: no
# importer code, so prefab and runtime libraries can use them without assimp.
add_library(blue_mesh STATIC
    src/asset/mesh_data.cc
)

target_include_directories(blue_mesh PUBLIC src)

blue_target_defaults(blue_mesh)

target_link_libraries(blue_mesh PUBLIC glm::glm)

# Image decoding (PNG/JPG/HDR through stb) and ORM packing: no importer code, so
# the renderer and the game load the HDR environment without assimp.
add_library(blue_image STATIC
    src/asset/image_decoder.cc
)

target_include_directories(blue_image PUBLIC src)

blue_target_defaults(blue_image)

target_link_libraries(blue_image PUBLIC blue_core PRIVATE stb)

add_library(blue_asset STATIC
    src/asset/animated_model.cc
    src/asset/internal/material_import.cc
    src/asset/internal/mesh_import.cc
    src/asset/model_importer.cc
    src/asset/texture_loader.cc
)

target_include_directories(blue_asset PUBLIC src)

blue_target_defaults(blue_asset)

target_link_libraries(blue_asset
    PUBLIC blue_mesh blue_image blue_core blue_animation blue_material glm::glm
    PRIVATE assimp::assimp stb
)

add_library(blue_scene STATIC
    src/scene/scene.cc
    src/scene/binary_stream.cc
    src/scene/scene_data.cc
    src/scene/mscn_serializer.cc
)

target_include_directories(blue_scene PUBLIC src)

blue_target_defaults(blue_scene)

target_link_libraries(blue_scene PUBLIC blue_core glm::glm)

# Prefab data model and the .pbo file (load-optimised, bulk data viewed in
# place). No GPU, no importers: only the asset vertex types and the scene's
# binary stream helpers.
add_library(blue_prefab STATIC
    src/prefab/prefab.cc
    src/prefab/prefab_file.cc
)

target_include_directories(blue_prefab PUBLIC src)

blue_target_defaults(blue_prefab)

target_link_libraries(blue_prefab
    PUBLIC blue_core blue_animation blue_material blue_mesh glm::glm
    PRIVATE blue_scene
)

# Cascaded shadow map math for the sun and image-based lighting math
# (equirect environments, SH9 irradiance, BRDF LUT).
add_library(blue_lighting STATIC
    src/lighting/cascades.cc
    src/lighting/environment.cc
)

target_include_directories(blue_lighting PUBLIC src)

blue_target_defaults(blue_lighting)

target_link_libraries(blue_lighting PUBLIC blue_image glm::glm)

# Rigid-body simulation on JoltPhysics. Jolt stays private: no Jolt type appears in the
# public physics headers (its JPH_* definitions still propagate PUBLIC from Jolt itself,
# so any target that includes Jolt headers gets matching settings).
add_library(blue_physics STATIC
    src/physics/physics_runtime.cc
    src/physics/physics_world.cc
    src/physics/character.cc
    src/physics/internal/shape_builder.cc
    src/physics/shape_cooking.cc
)

target_include_directories(blue_physics PUBLIC src)

blue_target_defaults(blue_physics)

target_link_libraries(blue_physics
    PUBLIC blue_scene blue_mesh blue_core glm::glm
    PRIVATE Jolt
)

# Offline content cooking (editor and blue-pack only, never the game): model import into
# prefabs, BC7/BC5 texture compression with mips, mesh optimisation, collision cooking
# and animation clip editing. The encoders are built optimised even in Debug (see
# cmake/Dependencies.cmake).
add_library(blue_cook STATIC
    src/cook/clip_tools.cc
    src/cook/collision_cook.cc
    src/cook/cook_error.cc
    src/cook/model_cook.cc
    src/cook/texture_cook.cc
)

target_include_directories(blue_cook PUBLIC src)

blue_target_defaults(blue_cook)

target_link_libraries(blue_cook
    PUBLIC blue_prefab blue_asset blue_physics blue_animation blue_material blue_core glm::glm
    PRIVATE bc7enc_rdo meshoptimizer
)

# Headless cooker: imports models into .pbo prefabs and prints their contents.
add_executable(blue-pack
    tools/blue-pack/main.cc
    tools/blue-pack/samples.cc
)

set_target_properties(blue-pack PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

blue_target_defaults(blue-pack)

target_include_directories(blue-pack PRIVATE ${CMAKE_SOURCE_DIR})

target_link_libraries(blue-pack PRIVATE blue_cook blue_prefab blue_physics blue_scene blue_weapon blue_core)

# Cooks the sample prefabs and range.mscn into <build>/bin/assets when the
# sources or blue-pack change (see cmake/SampleAssets.cmake).
blue_cook_sample_assets()

# Actions, rebindable key bindings and the persisted settings file. SDL only for key
# names and scancodes; no window or event handling here.
add_library(blue_input STATIC
    src/input/action.cc
    src/input/bindings.cc
    src/input/input_settings.cc
)

target_include_directories(blue_input PUBLIC src)

blue_target_defaults(blue_input)

target_link_libraries(blue_input PUBLIC blue_core SDL3::SDL3-static glm::glm)

add_library(blue_camera STATIC
    src/camera/camera_controller.cc
    src/camera/frustum.cc
    src/camera/look.cc
    src/camera/projection.cc
)

target_include_directories(blue_camera PUBLIC src)

blue_target_defaults(blue_camera)

target_link_libraries(blue_camera PUBLIC blue_core blue_input blue_scene glm::glm)

# SDL events -> per-frame action state and mouse deltas.
add_library(blue_input_mapper STATIC
    src/platform/input_mapper.cc
)

target_include_directories(blue_input_mapper PUBLIC src)

blue_target_defaults(blue_input_mapper)

target_link_libraries(blue_input_mapper PUBLIC blue_input SDL3::SDL3-static glm::glm)

# First-person player: input actions -> character movement, eye placement, head bob and
# field of view. Jolt stays behind blue_physics' public headers.
add_library(blue_player STATIC
    src/player/fov_controller.cc
    src/player/head_bob.cc
    src/player/player_controller.cc
)

target_include_directories(blue_player PUBLIC src)

blue_target_defaults(blue_player)

target_link_libraries(blue_player PUBLIC blue_physics blue_input blue_camera glm::glm)

# Weapon gameplay: loadout switching, firing mechanism, fire-animation driver, ballistic bullets swept against the physics
# world, model calibration and the first-person viewmodel pose. No rendering.
add_library(blue_weapon STATIC
    src/weapon/firearm.cc
    src/weapon/loadout.cc
    src/weapon/projectile_system.cc
    src/weapon/target_log.cc
    src/weapon/viewmodel.cc
    src/weapon/weapon_animator.cc
    src/weapon/weapon_geometry.cc
    src/weapon/weapon_prefab.cc
    src/weapon/weapon_specs.cc
)

target_include_directories(blue_weapon PUBLIC src)

blue_target_defaults(blue_weapon)

target_link_libraries(blue_weapon PUBLIC blue_physics blue_camera blue_animation blue_prefab glm::glm)

# Copies the DLLs `target` loads at run time next to it (Windows only; elsewhere they are
# static or found by the loader).
function(blue_copy_runtime_dlls target)
    if(WIN32)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:${target}> $<TARGET_FILE_DIR:${target}>
            COMMAND_EXPAND_LISTS)
    endif()
endfunction()

# Runtime Metal Shading Language compilation (include flattening, then the system's Metal
# compiler) and source change detection. Metal stays private: no Objective-C type appears
# in the public headers.
add_library(blue_shader STATIC
    src/shader/shader_compiler.mm
    src/shader/shader_preprocessor.cc
    src/shader/shader_watcher.cc
)

if(MSVC)
    add_compile_options(/MP)
endif()

target_include_directories(blue_shader PUBLIC src)

blue_target_defaults(blue_shader)

target_link_libraries(blue_shader
    PUBLIC blue_core
    PRIVATE "-framework Metal" "-framework Foundation"
)

# SDL3 window with a Metal layer; shared by the game and the editor.
add_library(blue_window STATIC
    src/platform/window.cc
)

target_include_directories(blue_window PUBLIC src)

blue_target_defaults(blue_window)

target_link_libraries(blue_window PUBLIC blue_core SDL3::SDL3-static)

# The Metal renderer: device, swapchain, render targets, the frame's passes, meshes,
# materials and textures. Linked by the game and the editor; draws to the window's layer or,
# for the editor viewport, to an offscreen image.
#
# Style exception: the project rules ask for .h/.cc pairs and no language extensions. Metal
# is an Objective-C API, so the files that call it are Objective-C++ (.mm, ARC), and the
# internal headers that name Metal types (they include render/metal_types.h) can only be
# included from .mm files. The headers other targets include stay pure C++; see
# tests/render_public_headers_test.cc.
add_library(blue_render STATIC
    src/render/buffer.mm
    src/render/debug_lines.cc
    src/render/encoders.mm
    src/render/environment.mm
    src/render/frame_globals.mm
    src/render/frame_uniforms.cc
    src/render/fullscreen_pass.mm
    src/render/gpu_scope.cc
    src/render/gpu_timer.mm
    src/render/image.mm
    src/render/material_system.mm
    src/render/mesh.mm
    src/render/metal_check.mm
    src/render/metal_context.mm
    src/render/pipeline.mm
    src/render/render_targets.mm
    src/render/renderer.mm
    src/render/samplers.mm
    src/render/shader_programs.mm
    src/render/swapchain.mm
    src/render/texture_cache.mm
    src/render/texture_types.cc
    src/render/passes/bloom_pass.mm
    src/render/passes/present_pass.mm
    src/render/passes/skybox_pass.mm
    src/render/passes/tonemap_pass.mm
    src/render/passes/ui_pass.mm
)

target_include_directories(blue_render PUBLIC src)
# For "shaders/bindings.h", the indices shared with the Metal shaders.
target_include_directories(blue_render PRIVATE ${CMAKE_SOURCE_DIR})

blue_target_defaults(blue_render)

target_link_libraries(blue_render PUBLIC
    blue_core
    blue_window
    blue_mesh
    blue_image
    blue_animation
    blue_material
    blue_scene
    blue_lighting
    blue_camera
    blue_shader
    SDL3::SDL3-static
    glm::glm
    imgui
    "-framework Metal"
    "-framework QuartzCore"
    "-framework CoreGraphics"
    "-framework Foundation"
)

# Dear ImGui on SDL3 + Metal (ui::ImGuiLayer); shared by the game overlay and the editor.
add_library(blue_ui STATIC
    src/ui/imgui_layer.cc
)

target_include_directories(blue_ui PUBLIC src)

blue_target_defaults(blue_ui)

target_link_libraries(blue_ui PUBLIC blue_render imgui)

# Runtime world shared by the game and the editor: the prefab cache (one GPU
# upload per .pbo), scenes resolved into posed prefabs with overrides, lights,
# draw items, picking, and physics bodies from cooked shapes.
add_library(blue_world STATIC
    src/world/gpu_backend.cc
    src/world/prefab_cache.cc
    src/world/prefab_pose.cc
    src/world/world.cc
    src/world/world_physics.cc
)

target_include_directories(blue_world PUBLIC src)

blue_target_defaults(blue_world)

target_link_libraries(blue_world
    PUBLIC blue_prefab blue_scene blue_render blue_physics blue_animation
           blue_material blue_mesh blue_core glm::glm
)

add_executable(project-blue
    src/main.cc
    src/app.cc
    src/ui/perf_overlay.cc
)

set_target_properties(project-blue PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

target_include_directories(project-blue PRIVATE src)

blue_target_defaults(project-blue)

target_link_libraries(project-blue PRIVATE
    blue_core
    blue_render
    blue_ui
    blue_animation
    blue_material
    blue_scene
    blue_lighting
    blue_physics
    blue_input
    blue_camera
    blue_input_mapper
    blue_player
    blue_weapon
    blue_world
    blue_prefab
    blue_shader
    SDL3::SDL3-static
    glm::glm
    imgui
)

blue_copy_runtime_dlls(project-blue)
blue_copy_shader_sources(project-blue)

# Debug builds compile the repo's shader sources directly, so edits apply live without
# rebuilding; Release uses the copies next to the executable. Content comes from the cooked
# assets directory (blue_sample_assets).
target_compile_definitions(project-blue PRIVATE
    $<$<CONFIG:Debug>:BLUE_DEV_SHADER_DIR="${CMAKE_SOURCE_DIR}/shaders">
)

# The editor (blue_editor_core + blue-editor).
add_subdirectory(tools/editor)

add_dependencies(project-blue blue_sample_assets)

option(BLUE_BUILD_TESTS "Build the unit tests" ON)
if(BLUE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
