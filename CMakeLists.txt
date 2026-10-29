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

find_package(Vulkan 1.3 REQUIRED COMPONENTS glslc)

include(Dependencies)
include(CompileShaders)
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

add_library(blue_asset STATIC
    src/asset/mesh_data.cc
    src/asset/model_importer.cc
)

target_include_directories(blue_asset PUBLIC src)

blue_target_defaults(blue_asset)

target_link_libraries(blue_asset
    PUBLIC blue_core glm::glm
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

# Rigid-body simulation on JoltPhysics. Jolt stays private: no Jolt type appears in the
# public physics headers (its JPH_* definitions still propagate PUBLIC from Jolt itself,
# so any target that includes Jolt headers gets matching settings).
add_library(blue_physics STATIC
    src/physics/physics_runtime.cc
    src/physics/physics_world.cc
    src/physics/internal/shape_builder.cc
)

target_include_directories(blue_physics PUBLIC src)

blue_target_defaults(blue_physics)

target_link_libraries(blue_physics
    PUBLIC blue_scene blue_asset blue_core glm::glm
    PRIVATE Jolt
)

add_library(blue_camera STATIC
    src/camera/camera_controller.cc
    src/camera/projection.cc
    src/input/input_settings.cc
)

target_include_directories(blue_camera PUBLIC src)

blue_target_defaults(blue_camera)

target_link_libraries(blue_camera PUBLIC blue_core blue_scene glm::glm)

add_library(blue_input_mapper STATIC
    src/platform/input_mapper.cc
)

target_include_directories(blue_input_mapper PUBLIC src)

blue_target_defaults(blue_input_mapper)

target_link_libraries(blue_input_mapper PUBLIC blue_camera SDL3::SDL3-static glm::glm)

add_executable(project-blue
    src/main.cc
    src/app.cc
    src/platform/window.cc
    src/render/vulkan_context.cc
    src/render/swapchain.cc
    src/render/buffer.cc
    src/render/image.cc
    src/render/pipeline.cc
    src/render/gpu_timer.cc
    src/render/mesh.cc
    src/render/mesh_cache.cc
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
    blue_scene
    blue_camera
    blue_input_mapper
    Vulkan::Vulkan
    SDL3::SDL3-static
    glm::glm
    GPUOpen::VulkanMemoryAllocator
    imgui
)

blue_compile_shaders(project-blue
    shaders/mesh.vert
    shaders/mesh.frag
)

option(BLUE_BUILD_TESTS "Build the unit tests" ON)
if(BLUE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
