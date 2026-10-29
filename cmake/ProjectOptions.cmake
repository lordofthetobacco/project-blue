# Warnings and compile definitions shared by every first-party target.
# Libraries expose the GLM/platform definitions publicly because they change glm types in
# headers; executables keep them private.
function(blue_target_defaults target)
    get_target_property(type ${target} TYPE)
    if(type STREQUAL "EXECUTABLE")
        set(visibility PRIVATE)
    else()
        set(visibility PUBLIC)
    endif()

    target_compile_definitions(${target} ${visibility}
        GLM_FORCE_DEPTH_ZERO_TO_ONE
        GLM_FORCE_RADIANS
        $<$<PLATFORM_ID:Windows>:NOMINMAX WIN32_LEAN_AND_MEAN>
    )

    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /permissive- /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
endfunction()
