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

# Headless simulation core only, for the Linux dedicated server (the blue-server repository):
# the libraries behind blue_sim_c, netcode and blue_sim_tests. No renderer, UI, editor, shaders,
# cooking or importers, so it configures without the Vulkan SDK, Slang or windowing packages.
option(BLUE_SIM_ONLY "Build only the headless simulation core (dedicated server)" OFF)

if(NOT BLUE_SIM_ONLY)
    include(Renderers)
endif()

# ProjectOptions first: cmake/Dependencies.cmake uses its MSVC flag helpers.
include(ProjectOptions)
include(Dependencies)

# Path handling and file system access (src/core/path.cc is pure; file_system.cc sits on
# SDL3). One small library because the file system layer depends on the path functions
# and every consumer needs both.
add_library(blue_core STATIC
    src/core/det_math.cc
    src/core/path.cc
    src/core/text.cc
    src/platform/file_system.cc
    src/platform/mapped_file.cc
    src/platform/process_memory.cc
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

# Rigid-body simulation on JoltPhysics. Jolt stays private: no Jolt type appears in the
# public physics headers (its JPH_* definitions still propagate PUBLIC from Jolt itself,
# so any target that includes Jolt headers gets matching settings).
add_library(blue_physics STATIC
    src/physics/physics_runtime.cc
    src/physics/physics_world.cc
    src/physics/character.cc
    src/physics/internal/shape_builder.cc
    src/physics/shape_cooking.cc
    src/physics/ragdoll.cc
)

target_include_directories(blue_physics PUBLIC src)

blue_target_defaults(blue_physics)

target_link_libraries(blue_physics
    PUBLIC blue_scene blue_mesh blue_core glm::glm
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

# First-person player: input actions -> character movement, eye placement, head bob and
# field of view. Jolt stays behind blue_physics' public headers.
add_library(blue_player STATIC
    src/player/fov_controller.cc
    src/player/head_bob.cc
    src/player/movement.cc
    src/player/player_controller.cc
)

target_include_directories(blue_player PUBLIC src)

blue_target_defaults(blue_player)

target_link_libraries(blue_player PUBLIC blue_physics blue_input blue_camera glm::glm)

# Weapon gameplay: loadout switching, firing mechanism, fire-animation driver, ballistic bullets swept against the physics
# world, model calibration, the first-person viewmodel pose and its motion history (for the
# weapon motion blur), throwing weapons and the dropped ones' bodies. No rendering.
add_library(blue_weapon STATIC
    src/weapon/dispersion.cc
    src/weapon/dropped_weapons.cc
    src/weapon/firearm.cc
    src/weapon/loadout.cc
    src/weapon/motion_history.cc
    src/weapon/parts/part_catalog.cc
    src/weapon/parts/part_catalog_file.cc
    src/weapon/parts/weapon_assembly.cc
    src/weapon/parts/weapon_build.cc
    src/weapon/parts/weapon_stats.cc
    src/weapon/parts/weapon_tuning.cc
    src/weapon/projectile_system.cc
    src/weapon/scope.cc
    src/weapon/scope_optics.cc
    src/weapon/scope_view.cc
    src/weapon/target_log.cc
    src/weapon/viewmodel.cc
    src/weapon/weapon_animator.cc
    src/weapon/weapon_def.cc
    src/weapon/weapon_geometry.cc
    src/weapon/weapon_prefab.cc
    src/weapon/weapon_specs.cc
    src/weapon/weapon_throw.cc
    src/weapon/zeroing.cc
)

target_include_directories(blue_weapon PUBLIC src)

blue_target_defaults(blue_weapon)

target_link_libraries(blue_weapon PUBLIC blue_physics blue_camera blue_animation blue_prefab glm::glm)

# Interaction focus: which interactable the player is looking at (reach, aim radius,
# hysteresis). Pure logic; line of sight is the game's job.
add_library(blue_interact STATIC
    src/interact/interaction_focus.cc
)

target_include_directories(blue_interact PUBLIC src)

blue_target_defaults(blue_interact)

target_link_libraries(blue_interact PUBLIC blue_core glm::glm)

# GPU-free world building shared by the runtime world and the headless simulation:
# prefab ids and sources, the placement plan resolved from a scene, and the collision
# world built from cooked shapes. No renderer, no importers (also built for the Linux
# dedicated server).
add_library(blue_world_core STATIC
    src/world/core/collision_world.cc
    src/world/core/placement_plan.cc
    src/world/core/prefab_store.cc
)

target_include_directories(blue_world_core PUBLIC src)

blue_target_defaults(blue_world_core)

target_link_libraries(blue_world_core
    PUBLIC blue_prefab blue_scene blue_physics blue_animation blue_core glm::glm
)

# The per-tick gameplay simulation shared by client prediction and the dedicated server:
# tick clock, user commands, player movement, weapons and damage over a headless world.
# No rendering, no game glue (blue_game), no UI.
add_library(blue_sim STATIC
    src/sim/command_builder.cc
    src/sim/damage.cc
    src/sim/player_sim.cc
    src/sim/sim_world.cc
    src/sim/tick_clock.cc
    src/sim/weapon_table.cc
)

target_include_directories(blue_sim PUBLIC src)

blue_target_defaults(blue_sim)

target_link_libraries(blue_sim
    PUBLIC blue_world_core blue_player blue_weapon blue_physics blue_input blue_camera blue_interact
           blue_core glm::glm
)

# Plain C ABI over blue_sim for the Rust dedicated server (sim/c_api/blue_sim.h).
add_library(blue_sim_c STATIC
    src/sim/c_api/blue_sim.cc
)

target_include_directories(blue_sim_c PUBLIC src)

blue_target_defaults(blue_sim_c)

target_link_libraries(blue_sim_c PRIVATE blue_sim)

# std::thread users (the address resolver, the MCP server) in every configuration.
find_package(Threads REQUIRED)

# Client networking without rendering: the wire codecs of docs/netcode/protocol.md (mirroring
# crates/blue-protocol in the blue-server repository bit for bit) and the netcode.io client
# transport. netcode stays private: no netcode type appears in src/net headers.
add_library(blue_net STATIC
    src/net/address_resolver.cc
    src/net/bit_stream.cc
    src/net/client_session.cc
    src/net/clock_sync.cc
    src/net/interpolation.cc
    src/net/messages.cc
    src/net/net_settings.cc
    src/net/packet.cc
    src/net/players.cc
    src/net/prediction.cc
    src/net/quantize.cc
    src/net/reliable_channel.cc
    src/net/replication.cc
    src/net/sequence.cc
    src/net/snapshot.cc
    src/net/state_mapping.cc
    src/net/transport.cc
    src/net/user_command_codec.cc
    src/net/wire_error.cc
    src/net/world_object.cc
)

target_include_directories(blue_net PUBLIC src)

blue_target_defaults(blue_net)

target_link_libraries(blue_net
    PUBLIC blue_sim blue_core glm::glm
    PRIVATE netcode Threads::Threads $<$<PLATFORM_ID:Windows>:ws2_32>
)

# The networked client without a window (tools/net-probe): connects the real
# ClientSession to a server and plays a scripted command stream. Headless, so
# it also builds in the BLUE_SIM_ONLY configuration.
add_executable(blue-net-probe
    tools/net-probe/main.cc
    tools/net-probe/net_probe.cc
)

target_include_directories(blue-net-probe PRIVATE src ${CMAKE_SOURCE_DIR})

blue_target_defaults(blue-net-probe)

target_link_libraries(blue-net-probe PRIVATE
    blue_net blue_sim blue_world_core blue_physics blue_scene blue_prefab blue_core glm::glm
)

# The dedicated server stops here: its Rust build links blue_sim_c and netcode through the
# link manifest (<build>/blue_sim_c.link and .syslibs, see cmake/ProjectOptions.cmake).
if(BLUE_SIM_ONLY)
    blue_write_link_manifest(blue_sim_c blue_sim_c netcode)
    option(BLUE_BUILD_TESTS "Build the unit tests" ON)
    if(BLUE_BUILD_TESTS)
        enable_testing()
        add_subdirectory(tests/sim)
    endif()
    return()
endif()

include(Shaders)
include(SampleAssets)

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

target_link_libraries(blue_mcp
    PUBLIC blue_core
    PRIVATE Threads::Threads $<$<PLATFORM_ID:Windows>:ws2_32>
)

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

# Cascaded shadow map math for the sun and image-based lighting math
# (equirect environments, SH9 irradiance, BRDF LUT).
add_library(blue_lighting STATIC
    src/lighting/cascades.cc
    src/lighting/environment.cc
)

target_include_directories(blue_lighting PUBLIC src)

blue_target_defaults(blue_lighting)

target_link_libraries(blue_lighting PUBLIC blue_image glm::glm)

# Offline content cooking (editor and blue-pack only, never the game): model import into
# prefabs, BC7/BC5 texture compression with mips, mesh optimisation, collision cooking
# and animation clip editing. The encoders are built optimised even in Debug (see
# cmake/Dependencies.cmake).
add_library(blue_cook STATIC
    src/cook/clip_tools.cc
    src/cook/collision_cook.cc
    src/cook/cook_error.cc
    src/cook/generated_file.cc
    src/cook/model_cook.cc
    src/cook/pose_file.cc
    src/cook/prefab_edits.cc
    src/cook/prefab_split.cc
    src/cook/scene_edits.cc
    src/cook/texture_cook.cc
)

target_include_directories(blue_cook PUBLIC src)

blue_target_defaults(blue_cook)

target_link_libraries(blue_cook
    PUBLIC blue_prefab blue_scene blue_asset blue_physics blue_animation blue_material blue_core glm::glm
    PRIVATE bc7enc_rdo meshoptimizer
)

# Headless cooker: imports models into .pbo prefabs and prints their contents.
add_executable(blue-pack
    tools/blue-pack/main.cc
    tools/blue-pack/sample_parts.cc
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

# SDL events -> per-frame action state and mouse deltas.
add_library(blue_input_mapper STATIC
    src/platform/input_mapper.cc
)

target_include_directories(blue_input_mapper PUBLIC src)

blue_target_defaults(blue_input_mapper)

target_link_libraries(blue_input_mapper PUBLIC blue_input SDL3::SDL3-static glm::glm)

# The Slang library of the Vulkan SDK for the runtime shader compiler (hot reload), as
# target blue_slang; empty without hot reload or without a backend that uses Slang.
# On Windows it is a DLL that is delay-loaded: an executable only needs slang.dll (and
# what it loads: BLUE_SLANG_RUNTIME_DLLS) once a shader is actually recompiled, so only
# the game, the editor and the unit tests get copies (blue_copy_runtime_dlls(... SLANG)).
set(blue_slang_target "")
set(BLUE_SLANG_RUNTIME_DLLS "")
if(BLUE_SHADER_HOT_RELOAD AND (BLUE_RENDERER_VULKAN OR BLUE_RENDERER_D3D12))
    if(Vulkan_INCLUDE_DIR)
        cmake_path(GET Vulkan_INCLUDE_DIR PARENT_PATH blue_vulkan_sdk_dir)
    elseif(DEFINED ENV{VULKAN_SDK})
        file(TO_CMAKE_PATH "$ENV{VULKAN_SDK}" blue_vulkan_sdk_dir)
    else()
        message(FATAL_ERROR "BLUE_SHADER_HOT_RELOAD needs the Vulkan SDK (Slang); "
            "set VULKAN_SDK")
    endif()
    find_path(BLUE_SLANG_INCLUDE_DIR slang/slang.h
        HINTS "${blue_vulkan_sdk_dir}/Include" "${blue_vulkan_sdk_dir}/include"
        DOC "Directory with slang/slang.h (Vulkan SDK)")
    find_library(BLUE_SLANG_LIBRARY slang
        HINTS "${blue_vulkan_sdk_dir}/Lib" "${blue_vulkan_sdk_dir}/lib"
        DOC "The Slang library (Vulkan SDK)")
    if(NOT BLUE_SLANG_INCLUDE_DIR OR NOT BLUE_SLANG_LIBRARY)
        message(FATAL_ERROR "Slang (slang/slang.h, the slang library) not found in the "
            "Vulkan SDK at ${blue_vulkan_sdk_dir}; it ships with SDK >= 1.3.296. "
            "Set -DBLUE_SHADER_HOT_RELOAD=OFF to build without the runtime compiler.")
    endif()
    if(WIN32)
        # slang.dll forwards to slang-compiler.dll; slang-glslang.dll optimizes SPIR-V and
        # dxcompiler.dll emits DXIL. All are release builds used through COM-style
        # interfaces only, so they work with either CRT.
        set(BLUE_SLANG_RUNTIME_DLLS
            "${blue_vulkan_sdk_dir}/Bin/slang.dll"
            "${blue_vulkan_sdk_dir}/Bin/slang-compiler.dll"
            "${blue_vulkan_sdk_dir}/Bin/slang-glslang.dll")
        if(BLUE_RENDERER_D3D12)
            list(APPEND BLUE_SLANG_RUNTIME_DLLS "${blue_vulkan_sdk_dir}/Bin/dxcompiler.dll")
        endif()
        foreach(file IN LISTS BLUE_SLANG_RUNTIME_DLLS)
            if(NOT EXISTS "${file}")
                message(FATAL_ERROR "Vulkan SDK file missing: ${file}")
            endif()
        endforeach()
    endif()
    add_library(blue_slang INTERFACE)
    target_include_directories(blue_slang SYSTEM INTERFACE "${BLUE_SLANG_INCLUDE_DIR}")
    target_link_libraries(blue_slang INTERFACE "${BLUE_SLANG_LIBRARY}")
    set(blue_slang_target blue_slang)
endif()

# Copies the DLLs `target` loads at run time next to it (Windows only; elsewhere they are
# static or found by the loader). SLANG: also the runtime shader compiler's DLLs, for
# executables that recompile shaders (hot reload).
function(blue_copy_runtime_dlls target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "SLANG" "" "")
    if(WIN32)
        # "cmake -E true" when the target loads no DLL (copy_if_different needs a source).
        set(dlls "$<TARGET_RUNTIME_DLLS:${target}>")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E "$<IF:$<BOOL:${dlls}>,copy_if_different,true>"
                "${dlls}" "$<$<BOOL:${dlls}>:$<TARGET_FILE_DIR:${target}>>"
            COMMAND_EXPAND_LISTS)
        if(arg_SLANG AND BLUE_SLANG_RUNTIME_DLLS)
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${BLUE_SLANG_RUNTIME_DLLS} $<TARGET_FILE_DIR:${target}>)
        endif()
    endif()
endfunction()

# Shader support without a GPU device: source change detection, SPIR-V blob helpers, the
# Metal include preprocessor (pure C++) and, with BLUE_SHADER_HOT_RELOAD, the runtime Slang
# compiler. Slang stays private: no Slang type appears in the public headers.
add_library(blue_shader STATIC
    src/shader/shader_preprocessor.cc
    src/shader/shader_watcher.cc
    src/shader/spirv_blob.cc
)
if(blue_slang_target)
    target_sources(blue_shader PRIVATE src/shader/slang_compiler.cc)
    target_compile_definitions(blue_shader PUBLIC BLUE_SHADER_HOT_RELOAD=1)
else()
    target_compile_definitions(blue_shader PUBLIC BLUE_SHADER_HOT_RELOAD=0)
endif()

target_include_directories(blue_shader PUBLIC src)

blue_target_defaults(blue_shader)

target_link_libraries(blue_shader
    PUBLIC blue_core
    PRIVATE ${blue_slang_target}
)
if(blue_slang_target AND WIN32)
    # Delay-loaded (see blue_slang above); link options of a private dependency do not
    # reach the executables by themselves.
    target_link_libraries(blue_shader INTERFACE delayimp)
    target_link_options(blue_shader INTERFACE "/DELAYLOAD:slang.dll")
endif()

# The shader programs: compiled by the build into <build>/bin/shaders (target
# blue_shaders) and listed in the generated render/shader_manifest.h.
include(shaders/programs.cmake)
blue_finalize_shaders()

# SDL3 window, created for the renderer backend's graphics API (platform::WindowGraphics);
# shared by the game and the editor. No graphics API header.
add_library(blue_window STATIC
    src/platform/window.cc
)

target_include_directories(blue_window PUBLIC src)

blue_target_defaults(blue_window)

target_link_libraries(blue_window PUBLIC blue_core SDL3::SDL3-static)

# The renderer's backend-neutral part: public types, per-frame CPU math (frame uniforms,
# temporal jitter, motion blur), the draw list (cull + sort) and the frame plan. Pure C++
# on glm; compiled on every platform, shared by every backend.
add_library(blue_render_common STATIC
    src/render/backend.cc
    src/render/debug_lines.cc
    src/render/draw_list.cc
    src/render/frame_plan.cc
    src/render/frame_uniforms.cc
    src/render/gpu_scope.cc
    src/render/material_params.cc
    src/render/motion_blur_math.cc
    src/render/temporal_math.cc
    src/render/texture_types.cc
)

# The repository root too: render/binding_layout.h includes "shaders/gpu_bindings.h", the
# numbers C++ shares with the shaders.
target_include_directories(blue_render_common PUBLIC src ${PROJECT_SOURCE_DIR})

blue_target_defaults(blue_render_common)

target_link_libraries(blue_render_common PUBLIC
    blue_shader_manifest
    blue_core
    blue_camera
    blue_lighting
    blue_scene
    blue_mesh
    blue_material
    glm::glm
)

# render::Renderer: forwards every call to the backend chosen at startup (one indirect call
# per public call, nothing per draw), and the backend factory. Linked by the game, the
# editor and the tests; no consumer names a backend target or a graphics API library.
add_library(blue_render STATIC
    src/render/backend_factory.cc
    src/render/renderer.cc
)

target_include_directories(blue_render PUBLIC src)

blue_target_defaults(blue_render)

target_link_libraries(blue_render
    PUBLIC
        blue_render_common
        blue_window
        blue_image
        blue_shader
        imgui
    PRIVATE
        $<$<BOOL:${BLUE_RENDERER_VULKAN}>:blue_render_vulkan>
        $<$<BOOL:${BLUE_RENDERER_D3D12}>:blue_render_d3d12>
        $<$<BOOL:${BLUE_RENDERER_METAL}>:blue_render_metal>
)

# The only #if on backends outside the per-backend sources (backend_factory.cc).
target_compile_definitions(blue_render PRIVATE
    BLUE_RENDERER_HAS_VULKAN=$<BOOL:${BLUE_RENDERER_VULKAN}>
    BLUE_RENDERER_HAS_D3D12=$<BOOL:${BLUE_RENDERER_D3D12}>
    BLUE_RENDERER_HAS_METAL=$<BOOL:${BLUE_RENDERER_METAL}>
    BLUE_DEFAULT_RENDERER_NAME="${BLUE_DEFAULT_RENDERER}"
)

if(BLUE_RENDERER_VULKAN)
    add_subdirectory(src/render/vulkan)
endif()
if(BLUE_RENDERER_D3D12)
    add_subdirectory(src/render/d3d12)
endif()
if(BLUE_RENDERER_METAL)
    add_subdirectory(src/render/metal)
endif()

# Dear ImGui on SDL3 (ui::ImGuiLayer; the renderer owns ImGui's renderer backend); shared
# by the game overlay and the editor.
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
    src/world/draw_history.cc
    src/world/gpu_backend.cc
    src/world/prefab_cache.cc
    src/world/prefab_pose.cc
    src/world/world.cc
    src/world/world_physics.cc
)

target_include_directories(blue_world PUBLIC src)

blue_target_defaults(blue_world)

target_link_libraries(blue_world
    PUBLIC blue_world_core blue_prefab blue_scene blue_render blue_physics blue_animation
           blue_material blue_mesh blue_core glm::glm
)

# Game-side glue between the world, the weapons and the interaction focus (scene
# weapon pick-ups, weapons built from the part catalogue). No rendering of its
# own; unit tested.
add_library(blue_game STATIC
    src/game/character_animator.cc
    src/game/character_model.cc
    src/game/inspect_screen_rules.cc
    src/game/part_stash.cc
    src/game/practice_dummies.cc
    src/game/ragdoll_rig.cc
    src/game/ragdolls.cc
    src/game/remote_players.cc
    src/game/scene_pickups.cc
    src/game/weapon_builds.cc
    src/game/weapon_modding.cc
    src/ui/main_menu_rules.cc
)

target_include_directories(blue_game PUBLIC src)

blue_target_defaults(blue_game)

target_link_libraries(blue_game PUBLIC blue_world blue_sim blue_weapon blue_interact blue_input
    glm::glm)

add_executable(project-blue
    src/main.cc
    src/app.cc
    src/ui/main_menu.cc
    src/ui/perf_overlay.cc
    src/ui/weapon_inspect_screen.cc
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
    blue_interact
    blue_weapon
    blue_game
    blue_net
    blue_world
    blue_prefab
    blue_shader
    SDL3::SDL3-static
    glm::glm
    imgui
)

blue_copy_runtime_dlls(project-blue SLANG)
if(BLUE_RENDERER_D3D12)
    # The Agility SDK exports and runtime (src/render/d3d12/CMakeLists.txt).
    blue_add_d3d12_runtime(project-blue)
endif()
# The compiled shaders in <build>/bin/shaders, next to the executable.
add_dependencies(project-blue blue_shaders)

# Debug builds watch the repo's shader sources, so edits apply live without rebuilding
# (hot reload). Content comes from the cooked assets directory (blue_sample_assets).
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
