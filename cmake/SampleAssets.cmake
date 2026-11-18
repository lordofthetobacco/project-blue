# Defines the target blue_sample_assets, which runs `blue-pack samples` to cook
# the game's sample prefabs (.pbo) and the default range scene (range.mscn)
# from samples/ into <build>/bin/assets/. It reruns only when a sample source
# or blue-pack itself changes (stamp file). Debug builds use the fast texture
# compression; other configurations the high quality one. Executables that
# load the assets add_dependencies() on it.
function(blue_cook_sample_assets)
    set(source_dir "${CMAKE_SOURCE_DIR}/samples")
    set(out_dir "${CMAKE_BINARY_DIR}/bin/assets")
    file(GLOB_RECURSE sources CONFIGURE_DEPENDS "${source_dir}/*")
    set(stamp "${CMAKE_BINARY_DIR}/blue_sample_assets.stamp")
    add_custom_command(
        OUTPUT "${stamp}"
        COMMAND $<TARGET_FILE:blue-pack> samples
                --source "${source_dir}" --out "${out_dir}"
                $<$<CONFIG:Debug>:--fast>
        COMMAND "${CMAKE_COMMAND}" -E touch "${stamp}"
        DEPENDS blue-pack ${sources}
        COMMENT "Cooking sample assets into ${out_dir}"
        # Drops the empty --fast argument outside Debug.
        COMMAND_EXPAND_LISTS
        VERBATIM)
    add_custom_target(blue_sample_assets DEPENDS "${stamp}")
endfunction()
