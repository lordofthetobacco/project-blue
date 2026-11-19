# Third-party dependencies, pinned to release tags.
include(FetchContent)

set(FETCHCONTENT_QUIET OFF)

# The whole project links statically.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# --- SDL3 -------------------------------------------------------------------
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)

FetchContent_Declare(SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG        release-3.4.18
    GIT_SHALLOW    TRUE
    SYSTEM)

# --- GLM --------------------------------------------------------------------
set(GLM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLM_ENABLE_CXX_20 ON CACHE BOOL "" FORCE)

FetchContent_Declare(glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG        1.0.1
    GIT_SHALLOW    TRUE
    SYSTEM)

# --- Dear ImGui, docking branch (no CMake support upstream; target defined below)
# Docking is only switched on by the editor (ImGuiConfigFlags_DockingEnable).
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.92.9b-docking
    GIT_SHALLOW    TRUE
    SYSTEM)

# --- ImGuizmo (editor transform gizmos; MIT). Only ImGuizmo.cpp is compiled, as
# a target defined below against our ImGui (checked to build with 1.92.9b-docking).
FetchContent_Declare(imguizmo
    URL      https://github.com/CedricGuillemet/ImGuizmo/archive/105e4fde45e4e74aae8973d98750820ef7aca7e2.tar.gz
    URL_HASH SHA256=d625a9f3207f15b7a5b7706269747676f17b7e4df00f898ced9480e5063d493a
    SOURCE_SUBDIR  no-cmake
    SYSTEM)

# --- assimp (model importers; only the formats we ship) -----------------------
set(ASSIMP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ASSIMP_TOOLS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(ASSIMP_INSTALL OFF CACHE BOOL "" FORCE)
set(ASSIMP_NO_EXPORT ON CACHE BOOL "" FORCE)
set(ASSIMP_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
# Bundled zlib only on Windows. Its classic-Mac block in zutil.h redefines fdopen to
# NULL when TARGET_OS_MAC is set, which breaks against the macOS 26+ SDK stdio.h; macOS
# and Linux ship a system zlib that assimp picks up through find_package.
if(WIN32)
  set(ASSIMP_BUILD_ZLIB ON CACHE BOOL "" FORCE)
else()
  set(ASSIMP_BUILD_ZLIB OFF CACHE BOOL "" FORCE)
endif()
set(ASSIMP_INJECT_DEBUG_POSTFIX OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_OBJ_IMPORTER ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_FBX_IMPORTER ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_GLTF_IMPORTER ON CACHE BOOL "" FORCE)

# The patch drops the /Od assimp appends to its MSVC debug flags (see below).
FetchContent_Declare(assimp
    GIT_REPOSITORY https://github.com/assimp/assimp.git
    GIT_TAG        v6.0.2
    GIT_SHALLOW    TRUE
    PATCH_COMMAND  ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_LIST_DIR}/third_party/assimp_debug_flags.cmake
    SYSTEM)

# --- stb (header-only; only stb_image is used, compiled in src/asset/image_decoder.cc)
# Pinned to a commit: stb has no release tags.
FetchContent_Declare(stb
    URL      https://github.com/nothings/stb/archive/fe3a2a73bfd5f38f8117d223896de72cc2aea30c.tar.gz
    URL_HASH SHA256=06f97b11cf13645e6762c34a248d748c154235b04adf50d10e8266ae6d4ea555
    SYSTEM)

# --- bc7enc_rdo (BC7 + BC4/BC5 encoders, BC7 decoder; MIT or public domain) -----
# Pinned to a commit: no release tags. Only the portable C++ encoders are used
# (no ISPC), see cmake/third_party/bc7enc_rdo. SOURCE_SUBDIR names a directory
# without a CMakeLists.txt, so FetchContent only downloads the sources.
FetchContent_Declare(bc7enc_rdo
    URL      https://github.com/richgel999/bc7enc_rdo/archive/b9438627eef73a1157e84201b6fa6eb2ffd6d9f0.tar.gz
    URL_HASH SHA256=bbb33d1dbb6178a3a2c044956c3cc0b8ea9fc2f2903824379dd86f87d53b64c6
    SOURCE_SUBDIR  no_cmake
    SYSTEM)

# --- meshoptimizer (vertex cache / fetch optimisation; MIT) ---------------------
# v1.3, pinned by commit.
set(MESHOPT_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_Declare(meshoptimizer
    URL      https://github.com/zeux/meshoptimizer/archive/9e1f07b159d3cb777f1c67ed31fc11fd117986f4.tar.gz
    URL_HASH SHA256=e753b45fbc410031bd69cb278180ae31fa0d5430d0c3e662e3a3b100467ce0b3
    SYSTEM)

# --- JoltPhysics ---------------------------------------------------------------
# /MD like the rest of the project; OVERRIDE_CXX_FLAGS OFF keeps Jolt from rewriting the
# global CMAKE_CXX_FLAGS that first-party targets inherit.
set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)
set(OVERRIDE_CXX_FLAGS OFF CACHE BOOL "" FORCE)
set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
set(JPH_USE_DX12 OFF CACHE BOOL "" FORCE)
set(JPH_USE_VK OFF CACHE BOOL "" FORCE)
set(JPH_USE_MTL OFF CACHE BOOL "" FORCE)
set(JPH_USE_CPU_COMPUTE OFF CACHE BOOL "" FORCE)
set(ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(ENABLE_OBJECT_STREAM OFF CACHE BOOL "" FORCE)
set(DEBUG_RENDERER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
set(PROFILER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
set(TARGET_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(TARGET_HELLO_WORLD OFF CACHE BOOL "" FORCE)
set(TARGET_PERFORMANCE_TEST OFF CACHE BOOL "" FORCE)
set(TARGET_SAMPLES OFF CACHE BOOL "" FORCE)
set(TARGET_VIEWER OFF CACHE BOOL "" FORCE)

FetchContent_Declare(JoltPhysics
    GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
    GIT_TAG        v5.6.0
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  Build
    SYSTEM)

FetchContent_MakeAvailable(SDL3 glm imgui imguizmo stb)

# The asset-cooking dependencies are compiled optimised even in Debug: assimp
# parses m16.obj in ~1.7 s at /Od, and BC7 encoding is far too slow without
# optimisation. CMAKE_<LANG>_FLAGS_DEBUG is read per directory, so the
# replacement only reaches the subdirectories added while it is set. The
# runtime library (/MDd, from CMAKE_MSVC_RUNTIME_LIBRARY) and with it the
# debug STL ABI stay the same as for first-party code.
set(blue_saved_c_flags_debug "${CMAKE_C_FLAGS_DEBUG}")
set(blue_saved_cxx_flags_debug "${CMAKE_CXX_FLAGS_DEBUG}")
if(MSVC)
    set(CMAKE_C_FLAGS_DEBUG "/O2 /Ob2")
    set(CMAKE_CXX_FLAGS_DEBUG "/O2 /Ob2")
else()
    set(CMAKE_C_FLAGS_DEBUG "-O2")
    set(CMAKE_CXX_FLAGS_DEBUG "-O2")
endif()

# assimp is the one dependency built as a shared library (DLL), placed in
# <build>/bin next to the executables; blue_tests copies it next to itself.
# Do not make it static again: assimp is compiled with exceptions
# (_HAS_EXCEPTIONS=1) and throws/catches DeadlyImportError on normal paths
# (its glTF 1 importer rejects every .glb that way). First-party code is
# compiled with _HAS_EXCEPTIONS=0, where MSVC's <exception> defines a
# different std::exception (non-owning message pointer, other layout). Both
# emit the inline std::exception / std::runtime_error members as COMDATs
# with the same names, and in a static link the linker keeps whichever
# object it loads first, so assimp's exceptions could be built and destroyed
# by mismatched code (heap corruption in ~DeadlyImportError, depending on
# link order). Inside the DLL assimp only sees its own copies.
set(BUILD_SHARED_LIBS ON)
set(ASSIMP_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin" CACHE STRING "" FORCE)
set(ASSIMP_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin" CACHE STRING "" FORCE)
FetchContent_MakeAvailable(assimp)
unset(BUILD_SHARED_LIBS)
set_target_properties(assimp PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

FetchContent_MakeAvailable(meshoptimizer bc7enc_rdo)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/third_party/bc7enc_rdo
    ${CMAKE_BINARY_DIR}/_deps/bc7enc_rdo-target)
set(CMAKE_C_FLAGS_DEBUG "${blue_saved_c_flags_debug}")
set(CMAKE_CXX_FLAGS_DEBUG "${blue_saved_cxx_flags_debug}")

# Jolt only adds /arch:AVX2 and its JPH_USE_* definitions when CMAKE_VS_PLATFORM_NAME is
# x64, which is empty under Ninja. Set it just for Jolt so it keeps its default AVX2 build.
if(NOT CMAKE_VS_PLATFORM_NAME AND CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64")
    set(CMAKE_VS_PLATFORM_NAME x64)
    FetchContent_MakeAvailable(JoltPhysics)
    unset(CMAKE_VS_PLATFORM_NAME)
else()
    FetchContent_MakeAvailable(JoltPhysics)
endif()

# Every sub-project above has copied the default flags; from here on the root directory
# (first-party code and the libraries below) compiles without /EHsc and the Release /O2,
# see cmake/ProjectOptions.cmake.
blue_remove_msvc_default_flags()

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_metal.mm
)
target_include_directories(imgui SYSTEM PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
# The Metal backend is written for ARC.
target_compile_options(imgui PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
target_link_libraries(imgui PUBLIC
    SDL3::SDL3-static
    "-framework Metal"
    "-framework QuartzCore"
    "-framework Foundation"
)
blue_restore_msvc_default_flags(imgui)

add_library(imguizmo STATIC ${imguizmo_SOURCE_DIR}/src/ImGuizmo.cpp)
target_include_directories(imguizmo SYSTEM PUBLIC ${imguizmo_SOURCE_DIR}/src)
target_link_libraries(imguizmo PUBLIC imgui)
blue_restore_msvc_default_flags(imguizmo)

# stb has no CMake project; SYSTEM keeps its warnings out of /W4 /WX builds.
add_library(stb INTERFACE)
target_include_directories(stb SYSTEM INTERFACE ${stb_SOURCE_DIR})
