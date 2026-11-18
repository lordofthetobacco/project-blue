# BC7 (bc7enc), BC4/BC5 (rgbcx) encoders and the BC7 decoder of bc7enc_rdo.
# Upstream's own CMakeLists builds command-line tools with OpenMP and optional
# ISPC, so only the three portable encoder/decoder sources are compiled here.
# Added from cmake/Dependencies.cmake with optimised flags in every
# configuration (see there); this directory only exists to get its own scope.
add_library(bc7enc_rdo STATIC
    ${bc7enc_rdo_SOURCE_DIR}/bc7enc.cpp
    ${bc7enc_rdo_SOURCE_DIR}/bc7decomp.cpp
    ${bc7enc_rdo_SOURCE_DIR}/rgbcx.cpp
)
target_include_directories(bc7enc_rdo SYSTEM PUBLIC ${bc7enc_rdo_SOURCE_DIR})
# The encoders assert inside their innermost loops; C asserts do not affect the
# ABI, so they are compiled out even in Debug.
target_compile_definitions(bc7enc_rdo PRIVATE NDEBUG)
