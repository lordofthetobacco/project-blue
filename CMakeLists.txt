cmake_minimum_required(VERSION 3.25)

project(project-blue
    VERSION 0.1.0
    DESCRIPTION "Lightweight extraction shooter"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

find_package(Vulkan 1.3 REQUIRED COMPONENTS glslc)

include(Dependencies)
include(CompileShaders)
include(ProjectOptions)

add_library(blue_asset STATIC
    src/asset/MeshData.cpp
    src/asset/ModelImporter.cpp
)

target_include_directories(blue_asset PUBLIC src)

blue_target_defaults(blue_asset)

target_link_libraries(blue_asset
    PUBLIC glm::glm
    PRIVATE assimp::assimp
)

add_library(blue_scene STATIC
    src/scene/Scene.cpp
    src/scene/SceneSerializer.cpp
    src/scene/BinaryStream.cpp
    src/scene/Placement.cpp
)

target_include_directories(blue_scene PUBLIC src)

blue_target_defaults(blue_scene)

target_link_libraries(blue_scene PUBLIC glm::glm)

add_library(blue_camera STATIC
    src/camera/CameraController.cpp
    src/camera/Projection.cpp
    src/input/InputSettings.cpp
)

target_include_directories(blue_camera PUBLIC src)

blue_target_defaults(blue_camera)

target_link_libraries(blue_camera PUBLIC blue_scene glm::glm)

add_library(blue_input_mapper STATIC
    src/platform/InputMapper.cpp
)

target_include_directories(blue_input_mapper PUBLIC src)

blue_target_defaults(blue_input_mapper)

target_link_libraries(blue_input_mapper PUBLIC blue_camera SDL3::SDL3-static glm::glm)

add_executable(project-blue
    src/main.cpp
    src/App.cpp
    src/platform/Window.cpp
    src/render/VulkanContext.cpp
    src/render/Swapchain.cpp
    src/render/Buffer.cpp
    src/render/Image.cpp
    src/render/Pipeline.cpp
    src/render/GpuTimer.cpp
    src/render/Mesh.cpp
    src/render/MeshCache.cpp
    src/render/Renderer.cpp
    src/render/Vma.cpp
    src/ui/PerfOverlay.cpp
)

set_target_properties(project-blue PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

target_include_directories(project-blue PRIVATE src)

blue_target_defaults(project-blue)

target_link_libraries(project-blue PRIVATE
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
