# Headless render tests (design §5): blue_render_tests renders fixed scenes
# without a window and compares them with the goldens in golden/. Needs a GPU
# (or a software Vulkan device with --adapter=software); CTest label "gpu".

# Image comparison, PNG I/O and contact sheets: pure C++, unit tested in
# blue_tests (image_compare_test.cc), no GPU.
add_library(blue_image_compare STATIC
    image_compare.cc
    png_io.cc
)

target_include_directories(blue_image_compare PUBLIC ${CMAKE_SOURCE_DIR})

blue_target_defaults(blue_image_compare)

target_link_libraries(blue_image_compare PUBLIC blue_core PRIVATE blue_image stb)

add_executable(blue_render_tests
    render_test_main.cc
    render_test.cc
    scenes.cc
)

target_include_directories(blue_render_tests PRIVATE ${CMAKE_SOURCE_DIR})

target_compile_definitions(blue_render_tests PRIVATE
    BLUE_SHADER_SOURCE_DIR="${CMAKE_SOURCE_DIR}/shaders"
    BLUE_RENDER_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden"
    BLUE_RENDER_TEST_OUT_DIR="${CMAKE_BINARY_DIR}/render_test_out"
)

blue_target_defaults(blue_render_tests ALLOW_EXCEPTIONS_AND_RTTI)

target_link_libraries(blue_render_tests PRIVATE
    blue_render blue_image_compare blue_core glm::glm doctest::doctest)

blue_copy_runtime_dlls(blue_render_tests)

add_test(NAME blue_render_tests COMMAND blue_render_tests)
set_tests_properties(blue_render_tests PROPERTIES LABELS gpu)
