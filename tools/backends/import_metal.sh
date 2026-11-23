#!/usr/bin/env bash
# Materialises the Metal renderer of branch `metal` in this tree, moved into
# its own directories and namespaces (design: out/briefs/X-design.md §2.3).
#
#   tools/backends/import_metal.sh            writes the files and `git add`s them
#   tools/backends/import_metal.sh --check    re-derives them and diffs against the
#                                             tree; prints nothing and exits 0 when equal
#   tools/backends/import_metal.sh --emit DIR writes them into DIR instead (to edit
#                                             them and refresh import_metal.patch)
#
# Every Metal-only file is `git show <ref>:<source>` passed through the mechanical
# rules below, then the hand edits of import_metal.patch (next to this script) are
# applied on top. The shared files (backend-neutral code the Vulkan renderer took
# from the metal branch) are not written; --check verifies that the ones taken
# verbatim are still identical.
#
# The ref defaults to origin/metal; set METAL_REF to use another one (e.g. after
# a later Metal change, to see what a re-import would change).
#
# Runs with bash, git, sed, diff and mktemp only (Git Bash, Linux, macOS; BSD sed
# compatible: no -i, no \| or \b).

set -euo pipefail

readonly ref="${METAL_REF:-origin/metal}"
readonly root="$(git rev-parse --show-toplevel)"
readonly script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly patch_file="${script_dir}/import_metal.patch"

mode="import"
emit_dir=""
case "${1:-}" in
  "") ;;
  --check) mode="check" ;;
  --emit)
    mode="emit"
    emit_dir="$(cd "${2:?--emit needs a directory}" && pwd)"
    ;;
  *)
    echo "usage: $0 [--check | --emit DIR]" >&2
    exit 2
    ;;
esac

cd "${root}"
git rev-parse --verify --quiet "${ref}^{commit}" >/dev/null || {
  echo "import_metal: unknown ref '${ref}' (git fetch origin metal?)" >&2
  exit 1
}

# The Metal-only headers of src/render (everything else there is shared).
readonly metal_render_headers="buffer.h draw_list.h encoders.h environment.h
frame_globals.h fullscreen_pass.h gpu_timer.h image.h material_system.h mesh.h
metal_check.h metal_context.h metal_types.h pipeline.h recording.h
render_targets.h renderer_impl.h samplers.h shader_programs.h staging_upload.h
swapchain.h texture_cache.h"

# The shared headers of src/render on the metal branch; A1 took them over.
readonly shared_render_files="brdf_reference.h debug_lines.cc debug_lines.h
frame_config.h frame_uniforms.cc frame_uniforms.h gpu_scope.cc gpu_scope.h
memory_stats.h motion_blur_math.cc motion_blur_math.h renderer.h scene_view.h
temporal_math.cc temporal_math.h texture_types.cc texture_types.h"

# Shared files that must still equal the metal branch's version byte for byte
# (line endings aside). The other shared files carry documented deltas
# (X-design.md §2.4): renderer.h, memory_stats.h, app.*, main.cc, window.*,
# perf_overlay.cc, imgui_layer.cc, the editor's backend option, the extended
# render_public_headers_test.cc and comment-only edits of mesh_data.h,
# frustum.h, projection.h, lighting/environment.*.
readonly verbatim_shared="src/core/extent.h
src/render/brdf_reference.h src/render/debug_lines.cc src/render/debug_lines.h
src/render/frame_config.h src/render/frame_uniforms.cc
src/render/frame_uniforms.h src/render/gpu_scope.cc src/render/gpu_scope.h
src/render/motion_blur_math.cc src/render/motion_blur_math.h
src/render/scene_view.h src/render/temporal_math.cc src/render/temporal_math.h
src/render/texture_types.cc src/render/texture_types.h
src/shader/shader_preprocessor.cc src/shader/shader_preprocessor.h
src/shader/shader_watcher.cc src/shader/shader_watcher.h
src/ui/imgui_layer.h src/world/draw_history.cc src/world/draw_history.h
src/world/world.cc src/world/world.h src/world/gpu_backend.cc
src/world/gpu_backend.h src/world/prefab_cache.h
tools/editor/editor_context.cc tools/editor/editor_context.h
tools/editor/viewport/viewport_panel.cc tools/editor/viewport/viewport_panel.h
tests/temporal_math_test.cc tests/draw_history_test.cc
tests/shader_preprocessor_test.cc tests/frame_uniforms_test.cc
tests/brdf_reference_test.cc tests/environment_test.cc
tests/motion_blur_math_test.cc tests/texture_upload_test.cc
tests/world_fixtures.h"

contains() {  # contains <word> <list>
  case " $(echo $2) " in *" $1 "*) return 0 ;; esac
  return 1
}

# Prints "<source> <destination>" for every Metal-only file of ${ref}; fails on a
# file of src/render that is neither listed as Metal-only nor as shared, so a new
# file on the metal branch is classified by a person, not guessed.
list_moves() {
  local path name
  while IFS= read -r path; do
    case "${path}" in
      src/render/passes/*)
        echo "${path} src/render/metal/passes/${path#src/render/passes/}" ;;
      src/render/*.mm)
        echo "${path} src/render/metal/${path#src/render/}" ;;
      src/render/*)
        name="${path#src/render/}"
        if contains "${name}" "${metal_render_headers}"; then
          echo "${path} src/render/metal/${name}"
        elif ! contains "${name}" "${shared_render_files}"; then
          echo "import_metal: ${path} on ${ref} is neither Metal-only nor" \
               "shared; classify it in $0" >&2
          return 1
        fi ;;
      src/shader/shader_compiler.h | src/shader/shader_compiler.mm | \
      src/shader/shader_library_metal.h)
        echo "${path} src/shader/metal/${path#src/shader/}" ;;
      shaders/*.metal | shaders/*.h)
        echo "${path} shaders/metal/${path#shaders/}" ;;
      tests/staging_upload_test.mm | tests/shader_compiler_test.cc)
        echo "${path} tests/metal/${path#tests/}" ;;
    esac
  done < <(git ls-tree -r --name-only "${ref}" -- src/render src/shader shaders \
             tests/staging_upload_test.mm tests/shader_compiler_test.cc)
}

moves="$(list_moves)"
readonly moves

# Rule 1 + 2: includes of moved headers. Generated from the move table (exact
# paths, never a pattern over all includes).
include_rules=()
while read -r source destination; do
  case "${source}" in
    src/render/*.h | src/shader/*.h | shaders/bindings.h)
      from="${source#src/}"
      to="${destination#src/}"
      include_rules+=(-e "s|^#include \"${from//./\\.}\"|#include \"${to}\"|")
      ;;
  esac
done <<< "${moves}"

# Applies the mechanical rules to stdin; $1 is the destination path.
transform() {
  local destination="$1"
  local -a rules=("${include_rules[@]}")
  # Rule 3: header guards follow the new path.
  rules+=(
    -e 's|^#ifndef BLUE_RENDER_\([A-Z0-9_]*_H_\)$|#ifndef BLUE_RENDER_METAL_\1|'
    -e 's|^#define BLUE_RENDER_\([A-Z0-9_]*_H_\)$|#define BLUE_RENDER_METAL_\1|'
    -e 's|^#endif  // BLUE_RENDER_\([A-Z0-9_]*_H_\)$|#endif  // BLUE_RENDER_METAL_\1|'
    -e 's|^#ifndef BLUE_SHADER_\([A-Z0-9_]*_H_\)$|#ifndef BLUE_SHADER_METAL_\1|'
    -e 's|^#define BLUE_SHADER_\([A-Z0-9_]*_H_\)$|#define BLUE_SHADER_METAL_\1|'
    -e 's|^#endif  // BLUE_SHADER_\([A-Z0-9_]*_H_\)$|#endif  // BLUE_SHADER_METAL_\1|'
    -e 's|^#ifndef BLUE_SHADERS_\([A-Z0-9_]*_H_\)$|#ifndef BLUE_SHADERS_METAL_\1|'
    -e 's|^#define BLUE_SHADERS_\([A-Z0-9_]*_H_\)$|#define BLUE_SHADERS_METAL_\1|'
    -e 's|^#endif  // BLUE_SHADERS_\([A-Z0-9_]*_H_\)$|#endif  // BLUE_SHADERS_METAL_\1|'
    # Rule 4: the backend's namespaces.
    -e 's|^namespace blue::render {$|namespace blue::render::metal {|'
    -e 's|^}  // namespace blue::render$|}  // namespace blue::render::metal|'
    -e 's|^namespace blue::shader {$|namespace blue::shader::metal {|'
    -e 's|^}  // namespace blue::shader$|}  // namespace blue::shader::metal|'
  )
  case "${destination}" in
    shaders/metal/*) sed "${rules[@]}" ;;
    *)
      # Rule 5: the Metal shader compiler's types moved to shader::metal;
      # shader::CompileMode, ShaderError and ShaderWatcher stay shared.
      sed "${rules[@]}" | sed -E \
        -e 's/(^|[^:A-Za-z0-9_])shader::(ShaderLibrary|CompiledShader|ShaderCompiler|MetalLibrary)([^A-Za-z0-9_]|$)/\1shader::metal::\2\3/g'
      ;;
  esac
}

# Writes every Metal-only file into directory $1, then applies the hand edits.
generate() {
  local out="$1" source destination
  while read -r source destination; do
    mkdir -p "${out}/$(dirname "${destination}")"
    git show "${ref}:${source}" | transform "${destination}" \
      > "${out}/${destination}"
  done <<< "${moves}"
  if [[ -s "${patch_file}" ]]; then
    (cd "${out}" && git -c core.autocrlf=false apply --whitespace=nowarn \
      "${patch_file}")
  fi
}

work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT

generate "${work}"

if [[ "${mode}" == "emit" ]]; then
  cp -R "${work}/." "${emit_dir}/"
  exit 0
fi

if [[ "${mode}" == "import" ]]; then
  while read -r source destination; do
    mkdir -p "$(dirname "${destination}")"
    cp "${work}/${destination}" "${destination}"
    git -c core.safecrlf=false add -- "${destination}"
  done <<< "${moves}"
  echo "import_metal: imported $(wc -l <<< "${moves}" | tr -d ' ') files from ${ref}"
  exit 0
fi

status=0
while read -r source destination; do
  if [[ ! -f "${destination}" ]]; then
    echo "missing: ${destination}"
    status=1
  elif ! diff -u --strip-trailing-cr "${work}/${destination}" "${destination}"; then
    status=1
  fi
done <<< "${moves}"
# Files in the Metal directories that the import does not produce.
readonly destinations="$(cut -d' ' -f2 <<< "${moves}")"
while IFS= read -r path; do
  case "${path}" in */CMakeLists.txt) continue ;; esac  # Build files (M1).
  if ! grep -qxF -- "${path}" <<< "${destinations}"; then
    echo "not from ${ref}: ${path}"
    status=1
  fi
done < <(git ls-files -- src/render/metal src/shader/metal shaders/metal tests/metal)
for path in ${verbatim_shared}; do
  if ! git show "${ref}:${path}" | diff -u --strip-trailing-cr - "${path}" \
       > "${work}/shared.diff"; then
    echo "shared file differs from ${ref}: ${path}"
    cat "${work}/shared.diff"
    status=1
  fi
done
exit "${status}"
