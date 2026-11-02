# Copies the sample models in samples/ (meshes and their material libraries) into a
# samples/ directory next to <target>'s executable, so Release builds find them without
# depending on the working directory. Debug builds read the repository copy directly
# (BLUE_DEV_SAMPLES_DIR), so the copy only matters for other configurations.
function(blue_copy_samples target)
    set(source_dir "${CMAKE_SOURCE_DIR}/samples")
    file(GLOB sample_files CONFIGURE_DEPENDS
        "${source_dir}/*.obj"
        "${source_dir}/*.mtl")

    set(stamp "${CMAKE_CURRENT_BINARY_DIR}/${target}_samples.stamp")
    add_custom_command(
        OUTPUT "${stamp}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:${target}>/samples"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                ${sample_files} "$<TARGET_FILE_DIR:${target}>/samples"
        COMMAND "${CMAKE_COMMAND}" -E touch "${stamp}"
        DEPENDS ${sample_files}
        COMMENT "Copying sample models"
        VERBATIM)

    add_custom_target(${target}_samples DEPENDS "${stamp}")
    add_dependencies(${target} ${target}_samples)
endfunction()
