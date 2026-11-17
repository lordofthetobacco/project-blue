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
    scene_serializer_test.cc
    scene_test.cc
    placement_test.cc
    mesh_data_test.cc
    scene_round_trip_test.cc
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
    frustum_test.cc
    result_test.cc
    path_test.cc
    file_system_test.cc
    physics_world_test.cc
    scene_physics_test.cc
    house_sample_test.cc
    character_test.cc
    cascades_test.cc
    firearm_test.cc
    projectile_system_test.cc
    loadout_test.cc
    target_log_test.cc
    viewmodel_test.cc
    weapon_animator_test.cc
    weapon_geometry_test.cc
    shader_compiler_test.cc
    shader_watcher_test.cc
)

target_include_directories(blue_tests PRIVATE ${CMAKE_SOURCE_DIR})

target_compile_definitions(blue_tests PRIVATE
    BLUE_SAMPLES_DIR="${CMAKE_SOURCE_DIR}/samples"
    BLUE_SHADER_SOURCE_DIR="${CMAKE_SOURCE_DIR}/shaders"
)

blue_target_defaults(blue_tests ALLOW_EXCEPTIONS_AND_RTTI)

target_link_libraries(blue_tests PRIVATE blue_core blue_scene blue_asset blue_animation blue_input blue_camera blue_input_mapper blue_physics blue_player blue_weapon blue_lighting blue_shader doctest::doctest)

add_test(NAME blue_tests COMMAND blue_tests)
