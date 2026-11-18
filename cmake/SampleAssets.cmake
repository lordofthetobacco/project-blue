# Copies the sample assets in samples/ (meshes, their material libraries and sidecar .bmat
# files, the glTF pistol, environment maps, textures and material files) into a samples/
# directory next to <target>'s executable, so Release builds find them without depending
# on the working directory. Debug builds read the repository copy directly
# (BLUE_DEV_SAMPLES_DIR), so the copy only matters for other configurations.
function(blue_copy_samples target)
    set(source_dir "${CMAKE_SOURCE_DIR}/samples")
    set(target_samples "$<TARGET_FILE_DIR:${target}>/samples")
    file(GLOB sample_files CONFIGURE_DEPENDS
        "${source_dir}/*.obj"
        "${source_dir}/*.mtl"
        "${source_dir}/*.bmat")
    # The glTF pistol keeps its subdirectory, so the same relative path works in every
    # configuration.
    file(GLOB glock_files CONFIGURE_DEPENDS "${source_dir}/glock/*.glb" "${source_dir}/glock/*.bmat")
    set(glock_copy "")
    if(glock_files)
        set(glock_copy COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                ${glock_files} "${target_samples}/glock")
    endif()

    # Directories copied as a whole, subdirectories included (texture sets live in one
    # folder each). Material files refer to textures by relative path, so the layout must
    # stay the same.
    set(tree_copies "")
    set(tree_files "")
    foreach(tree env textures materials)
        file(GLOB_RECURSE files CONFIGURE_DEPENDS "${source_dir}/${tree}/*")
        if(files)
            list(APPEND tree_files ${files})
            list(APPEND tree_copies COMMAND "${CMAKE_COMMAND}" -E copy_directory
                    "${source_dir}/${tree}" "${target_samples}/${tree}")
        endif()
    endforeach()

    set(stamp "${CMAKE_CURRENT_BINARY_DIR}/${target}_samples.stamp")
    add_custom_command(
        OUTPUT "${stamp}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${target_samples}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                ${sample_files} "${target_samples}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${target_samples}/glock"
        ${glock_copy}
        ${tree_copies}
        COMMAND "${CMAKE_COMMAND}" -E touch "${stamp}"
        DEPENDS ${sample_files} ${glock_files} ${tree_files}
        COMMENT "Copying sample assets"
        VERBATIM)

    add_custom_target(${target}_samples DEPENDS "${stamp}")
    add_dependencies(${target} ${target}_samples)
endfunction()
