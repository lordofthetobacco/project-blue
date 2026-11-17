cmake_minimum_required(VERSION 3.25)

project(project-blue
    VERSION 0.1.0
    DESCRIPTION "Lightweight extraction shooter"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

find_package(Vulkan 1.3 REQUIRED COMPONENTS shaderc_combined)

include(Dependencies)
include(ShaderSources)
include(SampleAssets)
include(ProjectOptions)

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

add_library(blue_asset STATIC
    src/asset/animated_model.cc
    src/asset/internal/material_color.cc
    src/asset/mesh_data.cc
    src/asset/model_importer.cc
)

target_include_directories(blue_asset PUBLIC src)

blue_target_defaults(blue_asset)

target_link_libraries(blue_asset
    PUBLIC blue_core blue_animation glm::glm
    PRIVATE assimp::assimp
)

add_library(blue_scene STATIC
    src/scene/scene.cc
    src/scene/scene_serializer.cc
    src/scene/binary_stream.cc
    src/scene/placement.cc
)

target_include_directories(blue_scene PUBLIC src)

blue_target_defaults(blue_scene)

target_link_libraries(blue_scene PUBLIC blue_core glm::glm)

# Cascaded shadow map math for the sun (glm only).
add_library(blue_lighting STATIC
    src/lighting/cascades.cc
)

target_include_directories(blue_lighting PUBLIC src)

blue_target_defaults(blue_lighting)

target_link_libraries(blue_lighting PUBLIC glm::glm)

# Rigid-body simulation on JoltPhysics. Jolt stays private: no Jolt type appears in the
# public physics headers (its JPH_* definitions still propagate PUBLIC from Jolt itself,
# so any target that includes Jolt headers gets matching settings).
add_library(blue_physics STATIC
    src/physics/physics_runtime.cc
    src/physics/physics_world.cc
    src/physics/scene_physics.cc
    src/physics/character.cc
    src/physics/internal/shape_builder.cc
)

target_include_directories(blue_physics PUBLIC src)

blue_target_defaults(blue_physics)

target_link_libraries(blue_physics
    PUBLIC blue_scene blue_asset blue_core glm::glm
    PRIVATE Jolt
)

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
    src/weapon/weapon_specs.cc
)

target_include_directories(blue_weapon PUBLIC src)

blue_target_defaults(blue_weapon)

target_link_libraries(blue_weapon PUBLIC blue_physics blue_camera blue_animation glm::glm)

# Runtime GLSL -> SPIR-V compilation (shaderc) and source change detection. No Vulkan device
# needed. shaderc stays private: no shaderc type appears in the public headers. The imported
# target links shaderc_combinedd.lib in Debug and shaderc_combined.lib otherwise, matching the
# /MDd vs /MD runtime of this project.
add_library(blue_shader STATIC
    src/shader/shader_compiler.cc
    src/shader/shader_watcher.cc
)

target_include_directories(blue_shader PUBLIC src)

blue_target_defaults(blue_shader)

target_link_libraries(blue_shader
    PUBLIC blue_core
    PRIVATE Vulkan::shaderc_combined
)

if(MSVC)
    # The SDK's static shaderc libraries reference PDBs it does not ship (LNK4099 for every
    # object). Nothing can be done about it in first-party code, so mute it for the linkers.
    target_link_options(blue_shader INTERFACE /ignore:4099)
endif()

add_executable(project-blue
    src/main.cc
    src/app.cc
    src/platform/window.cc
    src/render/vulkan_context.cc
    src/render/swapchain.cc
    src/render/buffer.cc
    src/render/image.cc
    src/render/pipeline.cc
    src/render/shader_programs.cc
    src/render/gpu_timer.cc
    src/render/mesh.cc
    src/render/mesh_cache.cc
    src/render/model_cache.cc
    src/render/renderer.cc
    src/render/vma.cc
    src/render/vk_check.cc
    src/ui/perf_overlay.cc
)

set_target_properties(project-blue PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

target_include_directories(project-blue PRIVATE src)

blue_target_defaults(project-blue)

target_link_libraries(project-blue PRIVATE
    blue_core
    blue_asset
    blue_animation
    blue_scene
    blue_lighting
    blue_physics
    blue_input
    blue_camera
    blue_input_mapper
    blue_player
    blue_weapon
    blue_shader
    Vulkan::Vulkan
    SDL3::SDL3-static
    glm::glm
    GPUOpen::VulkanMemoryAllocator
    imgui
)

blue_copy_shader_sources(project-blue)
blue_copy_samples(project-blue)

# Debug builds compile the repo's shader sources directly, so edits apply live without
# rebuilding, and load the sample models from the repo; Release uses the copies next to
# the executable.
target_compile_definitions(project-blue PRIVATE
    $<$<CONFIG:Debug>:BLUE_DEV_SHADER_DIR="${CMAKE_SOURCE_DIR}/shaders">
    $<$<CONFIG:Debug>:BLUE_DEV_SAMPLES_DIR="${CMAKE_SOURCE_DIR}/samples">
)

option(BLUE_BUILD_TESTS "Build the unit tests" ON)
if(BLUE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
