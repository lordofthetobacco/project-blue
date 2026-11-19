# blue_copy_shader_sources(<target>)
#
# Copies the Metal shader sources in shaders/ (*.metal and their *.h includes) next to
# <target>'s
# executable, into <target dir>/shaders, so a shipped build compiles them at startup.
# Files are copied with copy_if_different whenever <target> is built, and the build re-runs
# when a source changes.
#
# The sources are globbed with CONFIGURE_DEPENDS instead of listed: a new shader or include
# file needs no CMake edit, and the generator re-checks the glob on every build, so adding
# one is picked up without a manual reconfigure.
function(blue_copy_shader_sources target)
    set(source_dir "${CMAKE_SOURCE_DIR}/shaders")
    file(GLOB shader_sources CONFIGURE_DEPENDS
        "${source_dir}/*.metal"
        "${source_dir}/*.h")

    set(stamp "${CMAKE_CURRENT_BINARY_DIR}/${target}_shader_sources.stamp")
    add_custom_command(
        OUTPUT "${stamp}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:${target}>/shaders"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                ${shader_sources} "$<TARGET_FILE_DIR:${target}>/shaders"
        COMMAND "${CMAKE_COMMAND}" -E touch "${stamp}"
        DEPENDS ${shader_sources}
        COMMENT "Copying shader sources"
        VERBATIM)

    add_custom_target(${target}_shader_sources DEPENDS "${stamp}")
    add_dependencies(${target} ${target}_shader_sources)
endfunction()
