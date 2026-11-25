# Headless simulation tests: the sim core (blue_sim, blue_world_core), its C ABI
# (blue_sim_c) and the netcode.io transport. Nothing here may depend on rendering,
# shaders, assimp or the cooked sample assets: this executable is also built in the
# headless Linux configuration of the dedicated server (BLUE_SIM_ONLY), where the root
# CMakeLists.txt adds this directory without tests/, so doctest (declared in
# cmake/Dependencies.cmake) may not be available yet.
if(NOT TARGET doctest::doctest)
    FetchContent_MakeAvailable(doctest)
endif()

add_executable(blue_sim_tests
    test_main.cc
    character_state_test.cc
    collision_world_test.cc
    command_builder_test.cc
    damage_test.cc
    det_math_test.cc
    dropped_weapons_replicated_test.cc
    firearm_state_test.cc
    loadout_state_test.cc
    movement_test.cc
    net_bit_stream_test.cc
    net_garbage_test.cc
    net_golden_vectors_test.cc
    net_messages_test.cc
    net_quantize_test.cc
    net_reliable_channel_test.cc
    net_session_clock_sync_test.cc
    net_session_interpolation_test.cc
    net_session_loopback_test.cc
    net_session_prediction_test.cc
    net_session_settings_test.cc
    net_session_state_mapping_test.cc
    net_sequence_test.cc
    net_snapshot_test.cc
    net_transport_test.cc
    net_user_command_test.cc
    netcode_smoke_test.cc
    physics_step_test.cc
    placement_plan_test.cc
    player_sim_test.cc
    projectile_sweep_test.cc
    replay_determinism_test.cc
    sim_c_api_test.cc
    sim_world_test.cc
    tick_clock_test.cc
    weapon_table_test.cc
)

target_include_directories(blue_sim_tests PRIVATE ${CMAKE_SOURCE_DIR})

target_compile_definitions(blue_sim_tests PRIVATE
    # See tests/CMakeLists.txt: lets CHECKs print std::string_view.
    DOCTEST_CONFIG_USE_STD_HEADERS
    # Golden vectors of the wire codec (a copy of the blue-server repository's), checked byte
    # for byte by net_golden_vectors_test.
    BLUE_PROTOCOL_VECTORS_DIR="${CMAKE_SOURCE_DIR}/docs/netcode/vectors"
)

blue_target_defaults(blue_sim_tests ALLOW_EXCEPTIONS_AND_RTTI)

target_link_libraries(blue_sim_tests PRIVATE
    blue_net
    blue_sim
    blue_sim_c
    blue_world_core
    blue_player
    blue_weapon
    blue_physics
    blue_prefab
    blue_scene
    blue_input
    blue_camera
    blue_core
    netcode
    glm::glm
    doctest::doctest
)

add_test(NAME blue_sim_tests COMMAND blue_sim_tests)
