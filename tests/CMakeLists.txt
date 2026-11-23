include(FetchContent)

FetchContent_Declare(doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG        v2.4.12
    GIT_SHALLOW    TRUE
    SYSTEM)

FetchContent_MakeAvailable(doctest)

add_executable(blue_tests
    test_main.cc
    model_importer_test.cc
    animated_model_test.cc
    animation_clip_test.cc
    animation_clip_player_test.cc
    animation_hierarchy_test.cc
    scene_test.cc
    mesh_data_test.cc
    debug_lines_test.cc
    frame_uniforms_test.cc
    motion_blur_math_test.cc
    motion_history_test.cc
    temporal_math_test.cc
    draw_history_test.cc
    draw_list_test.cc
    frame_plan_test.cc
    backend_test.cc
    brdf_reference_test.cc
    editor_viewport_test.cc
    mscn_scene_data_test.cc
    mscn_serializer_test.cc
    prefab_test.cc
    prefab_file_test.cc
    prefab_edits_test.cc
    prefab_split_test.cc
    scene_edits_test.cc
    generated_file_test.cc
    world_test.cc
    world_physics_test.cc
    world_sample_test.cc
    camera_controller_test.cc
    action_test.cc
    bindings_test.cc
    input_settings_test.cc
    input_mapper_test.cc
    look_test.cc
    head_bob_test.cc
    fov_controller_test.cc
    player_controller_test.cc
    projection_test.cc
    render_public_headers_test.cc
    frustum_test.cc
    result_test.cc
    path_test.cc
    file_system_test.cc
    physics_world_test.cc
    house_sample_test.cc
    character_test.cc
    cascades_test.cc
    firearm_test.cc
    projectile_system_test.cc
    loadout_test.cc
    interaction_focus_test.cc
    weapon_throw_test.cc
    dropped_weapons_test.cc
    target_log_test.cc
    viewmodel_test.cc
    weapon_animator_test.cc
    weapon_geometry_test.cc
    weapon_prefab_test.cc
    weapon_def_test.cc
    part_catalog_test.cc
    weapon_build_test.cc
    weapon_stats_test.cc
    weapon_tuning_test.cc
    weapon_assembly_test.cc
    dispersion_test.cc
    scope_test.cc
    zeroing_test.cc
    scene_pickups_test.cc
    weapon_builds_test.cc
    part_stash_test.cc
    weapon_modding_test.cc
    inspect_screen_rules_test.cc
    window_pair_test.cc
    shader_preprocessor_test.cc
    shader_watcher_test.cc
    material_file_test.cc
    image_decoder_test.cc
    environment_test.cc
    sample_content_test.cc
    texture_upload_test.cc
    editor_core_test.cc
    cook_clip_test.cc
    cook_collision_test.cc
    cook_model_test.cc
    cook_samples_test.cc
    cook_texture_test.cc
    editor_prefab_edit_test.cc
    editor_scene_edit_test.cc
    editor_mcp_args_test.cc
    editor_mcp_settings_test.cc
    editor_memory_test.cc
    mcp_http_test.cc
    mcp_json_test.cc
    mcp_protocol_test.cc
    mcp_server_test.cc
    render/image_compare_test.cc
)

# Unit tests of a backend's GPU-free code, built with that backend.
if(BLUE_RENDERER_VULKAN)
    target_sources(blue_tests PRIVATE
        vulkan/shader_compiler_test.cc
        vulkan/staging_upload_test.cc
        vulkan/vk_format_test.cc
    )
    target_link_libraries(blue_tests PRIVATE blue_render_vulkan)
endif()

target_include_directories(blue_tests PRIVATE ${CMAKE_SOURCE_DIR})

target_compile_definitions(blue_tests PRIVATE
    BLUE_SAMPLES_DIR="${CMAKE_SOURCE_DIR}/samples"
    BLUE_ASSETS_DIR="${CMAKE_BINARY_DIR}/bin/assets"
    BLUE_SHADER_SOURCE_DIR="${CMAKE_SOURCE_DIR}/shaders"
    # doctest then includes the standard <ostream> and <string> it otherwise
    # forward-declares, so CHECKs can print std::string_view with MSVC's
    # standard library too.
    DOCTEST_CONFIG_USE_STD_HEADERS
)

blue_target_defaults(blue_tests ALLOW_EXCEPTIONS_AND_RTTI)

target_link_libraries(blue_tests PRIVATE blue_core blue_scene blue_material blue_asset blue_animation blue_input blue_camera blue_input_mapper blue_physics blue_player blue_interact blue_weapon blue_game blue_lighting blue_shader blue_render blue_cook blue_prefab blue_world blue_editor_core blue_mcp blue_image_compare doctest::doctest)

# assimp is a DLL (see cmake/Dependencies.cmake); copy it next to the runner.
blue_copy_runtime_dlls(blue_tests)

# cook_samples_test reads the cooked sample assets.
add_dependencies(blue_tests blue_sample_assets)

add_test(NAME blue_tests COMMAND blue_tests)

# Headless GPU render tests (blue_render_tests, label gpu) and the image
# comparison library blue_tests unit tests.
add_subdirectory(render)
