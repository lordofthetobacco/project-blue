include(FetchContent)

FetchContent_Declare(doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG        v2.4.12
    GIT_SHALLOW    TRUE
    SYSTEM)

FetchContent_MakeAvailable(doctest)

add_executable(blue_tests
    Main.cpp
    ModelImporterTests.cpp
    SceneSerializerTests.cpp
    PlacementTests.cpp
    MeshDataTests.cpp
    SceneRoundTripTests.cpp
    CameraControllerTests.cpp
    InputSettingsTests.cpp
    InputMapperTests.cpp
    ProjectionTests.cpp
)

target_compile_definitions(blue_tests PRIVATE
    BLUE_SAMPLES_DIR="${CMAKE_SOURCE_DIR}/samples"
)

blue_target_defaults(blue_tests)

target_link_libraries(blue_tests PRIVATE blue_scene blue_asset blue_input_mapper doctest::doctest)

add_test(NAME blue_tests COMMAND blue_tests)
