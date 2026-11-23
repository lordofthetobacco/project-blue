# blue-editor: Prefab and Scene mode editor (plan: stateless-dancing-adleman).
#
# Sources of the mode directories (viewport/, prefab_mode/, scene_mode/) are globbed so
# their owners add files without touching this file:
#   - `*_edit.cc` and the viewport's pure helpers (editor_camera, gizmo, debug_draw): no
#     GPU objects, no panels. They go into blue_editor_core, which blue_tests links, so
#     they can be unit tested.
#   - every other `.cc`: panels and GPU code, compiled into blue-editor only.
# mcp/ (the MCP server integration) follows the same split: its pure parts (mcp_args,
# mcp_export, png_encode) go into blue_editor_core, the tools and the host into blue-editor.
# Includes are rooted at tools/: "editor/document.h", "editor/prefab_mode/....h".

file(GLOB blue_editor_mode_sources CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/viewport/*.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/prefab_mode/*.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/scene_mode/*.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/mcp/*.cc)
set(blue_editor_core_pattern
    "(_edit|/editor_camera|/gizmo|/debug_draw|/mcp_args|/mcp_export|/png_encode)\\.cc$")
set(blue_editor_core_mode_sources ${blue_editor_mode_sources})
list(FILTER blue_editor_core_mode_sources INCLUDE REGEX "${blue_editor_core_pattern}")
set(blue_editor_ui_mode_sources ${blue_editor_mode_sources})
list(FILTER blue_editor_ui_mode_sources EXCLUDE REGEX "${blue_editor_core_pattern}")

# Documents, undo history, selection, text commands and the modes' pure editing logic.
add_library(blue_editor_core STATIC
    commands.cc
    document.cc
    editor_options.cc
    editor_settings.cc
    memory_report.cc
    selection.cc
    ${blue_editor_core_mode_sources}
)

target_include_directories(blue_editor_core PUBLIC ${PROJECT_SOURCE_DIR}/tools)

blue_target_defaults(blue_editor_core)

target_link_libraries(blue_editor_core PUBLIC
    blue_core blue_prefab blue_scene blue_cook blue_render blue_window blue_mcp imgui
    imguizmo glm::glm)
# png_encode.cc compiles stb_image_write.
target_link_libraries(blue_editor_core PRIVATE stb)

# The executable needs the runtime world (blue_world) and its own sources; until both
# exist in the tree only the core library is built.
if(TARGET blue_world AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/main.cc)
    add_executable(blue-editor
        asset_browser.cc
        dialogs.cc
        drag_drop.cc
        editor_app.cc
        editor_context.cc
        main.cc
        ${blue_editor_ui_mode_sources}
    )

    set_target_properties(blue-editor PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

    blue_target_defaults(blue-editor)

    target_link_libraries(blue-editor PRIVATE
        blue_editor_core
        blue_world
        blue_render
        blue_ui
        blue_cook
        blue_window
        SDL3::SDL3-static
        imgui
    )

    blue_copy_runtime_dlls(blue-editor SLANG)
    # The cooked samples (<build>/bin/assets: range.mscn, *.pbo) the browser starts in.
    if(TARGET blue_sample_assets)
        add_dependencies(blue-editor blue_sample_assets)
    endif()
    add_dependencies(blue-editor blue_shaders)

    # Debug builds watch the repo's shader sources (live edits), like the game.
    target_compile_definitions(blue-editor PRIVATE
        $<$<CONFIG:Debug>:BLUE_DEV_SHADER_DIR="${CMAKE_SOURCE_DIR}/shaders">
    )
endif()
