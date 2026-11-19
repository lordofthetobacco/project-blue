# FetchContent PATCH_COMMAND for assimp, run in its source directory (cmake -P).
#
# assimp's top-level CMakeLists.txt appends "/D_DEBUG /Zi /Od" to the MSVC debug
# flags, but cmake/Dependencies.cmake builds it optimised in Debug ("/O2 /Ob2");
# the /Od would then win (or, overridden again, make cl print a D9025 note for
# every file). Only the /Od is dropped. Safe to run more than once.
set(file "CMakeLists.txt")
set(original "\${CMAKE_CXX_FLAGS_DEBUG} /D_DEBUG /Zi /Od\"")
set(patched "\${CMAKE_CXX_FLAGS_DEBUG} /D_DEBUG /Zi\"")

file(READ "${file}" content)
string(FIND "${content}" "${original}" original_at)
if(original_at EQUAL -1)
    string(FIND "${content}" "${patched}" patched_at)
    if(patched_at EQUAL -1)
        message(FATAL_ERROR "assimp_debug_flags.cmake: the debug flag line changed upstream")
    endif()
    return()
endif()
string(REPLACE "${original}" "${patched}" content "${content}")
file(WRITE "${file}" "${content}")
