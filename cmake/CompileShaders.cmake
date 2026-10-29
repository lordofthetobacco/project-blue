# blue_compile_shaders(<target> <shader>...)
#
# Compiles GLSL sources to SPIR-V with glslc into <target runtime dir>/shaders/<name>.spv
# and makes <target> depend on them. The target must set RUNTIME_OUTPUT_DIRECTORY
# (single-config generators, as used by CMakePresets.json).
function(blue_compile_shaders target)
    get_target_property(runtime_dir ${target} RUNTIME_OUTPUT_DIRECTORY)
    if(NOT runtime_dir)
        message(FATAL_ERROR "blue_compile_shaders: ${target} has no RUNTIME_OUTPUT_DIRECTORY")
    endif()

    set(spirv_dir "${runtime_dir}/shaders")
    set(depfile_dir "${CMAKE_CURRENT_BINARY_DIR}/shader-deps")
    set(spirv_files "")

    foreach(shader IN LISTS ARGN)
        get_filename_component(source "${shader}" ABSOLUTE)
        get_filename_component(name "${shader}" NAME)
        set(output "${spirv_dir}/${name}.spv")
        set(depfile "${depfile_dir}/${name}.d")

        add_custom_command(
            OUTPUT  "${output}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${spirv_dir}" "${depfile_dir}"
            COMMAND Vulkan::glslc --target-env=vulkan1.3 -O -Werror
                    -MD -MF "${depfile}" "${source}" -o "${output}"
            DEPENDS "${source}"
            DEPFILE "${depfile}"
            COMMENT "Compiling shader ${name}"
            VERBATIM)

        list(APPEND spirv_files "${output}")
    endforeach()

    add_custom_target(${target}_shaders DEPENDS ${spirv_files})
    add_dependencies(${target} ${target}_shaders)
endfunction()
