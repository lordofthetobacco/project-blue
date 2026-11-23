# Every shader program of the renderer (cmake/Shaders.cmake). The order is the order of
# render::ShaderProgram. A program a backend never asks for costs build time only.

# Scene geometry.
blue_shader_program(NAME mesh SOURCE mesh.slang VS VertexMain FS FragmentMain
    DEFINES ALPHA_MASK=0 DOUBLE_SIDED=0)
blue_shader_program(NAME mesh_masked SOURCE mesh.slang VS VertexMain FS FragmentMain
    DEFINES ALPHA_MASK=1 DOUBLE_SIDED=0)
blue_shader_program(NAME mesh_double SOURCE mesh.slang VS VertexMain FS FragmentMain
    DEFINES ALPHA_MASK=0 DOUBLE_SIDED=1)
blue_shader_program(NAME mesh_masked_double SOURCE mesh.slang VS VertexMain FS FragmentMain
    DEFINES ALPHA_MASK=1 DOUBLE_SIDED=1)
blue_shader_program(NAME depth SOURCE depth.slang VS VertexMain)
blue_shader_program(NAME depth_masked SOURCE depth.slang VS VertexMain FS MaskedFragmentMain)
blue_shader_program(NAME shadow SOURCE shadow.slang VS VertexMain)
blue_shader_program(NAME skybox SOURCE skybox.slang VS VertexMain FS FragmentMain)

# Sights.
blue_shader_program(NAME reticle SOURCE reticle.slang VS VertexMain FS FragmentMain)
blue_shader_program(NAME scope_lens SOURCE scope_lens.slang VS VertexMain FS FragmentMain)

# Weapon motion blur.
blue_shader_program(NAME motion_velocity SOURCE motion_velocity.slang
    VS VertexMain FS FragmentMain)
blue_shader_program(NAME motion_tile_max SOURCE motion_tile_max.slang
    VS VertexMain FS FragmentMain)
blue_shader_program(NAME motion_blur SOURCE motion_blur_gather.slang
    VS VertexMain FS FragmentMain)

# Post-processing.
blue_shader_program(NAME ssao SOURCE ssao.slang VS VertexMain FS FragmentMain)
blue_shader_program(NAME ssao_blur SOURCE ssao_blur.slang VS VertexMain FS FragmentMain)
blue_shader_program(NAME bloom_down SOURCE bloom_down.slang VS VertexMain FS FragmentMain)
blue_shader_program(NAME bloom_up SOURCE bloom_up.slang VS VertexMain FS FragmentMain)
blue_shader_program(NAME tonemap SOURCE tonemap.slang VS VertexMain FS FragmentMain)
blue_shader_program(NAME present SOURCE present.slang VS VertexMain FS FragmentMain)

# Editor overlays.
blue_shader_program(NAME line SOURCE line.slang VS VertexMain FS FragmentMain)
blue_shader_program(NAME selection_mask_hidden SOURCE depth.slang
    VS VertexMain FS SelectionMaskFragmentMain DEFINES VISIBLE=0)
blue_shader_program(NAME selection_mask_visible SOURCE depth.slang
    VS VertexMain FS SelectionMaskFragmentMain DEFINES VISIBLE=1)
blue_shader_program(NAME selection_outline SOURCE selection_outline.slang
    VS VertexMain FS FragmentMain)

# Environment filtering.
blue_shader_program(NAME env_equirect_to_cube SOURCE env_equirect_to_cube.slang
    VS VertexMain FS FragmentMain)
blue_shader_program(NAME env_prefilter SOURCE env_prefilter.slang VS VertexMain FS FragmentMain)
blue_shader_program(NAME env_brdf_lut SOURCE env_brdf_lut.slang VS VertexMain FS FragmentMain)

# Tests only: the Direct3D 12 upload test (tests/render/d3d12_render_test.cc).
blue_shader_program(NAME test_texture_grid SOURCE test_texture_grid.slang
    VS VertexMain FS FragmentMain)
