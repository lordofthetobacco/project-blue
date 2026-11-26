#!/usr/bin/env bash
# Configures and builds the project on macOS with the CMake presets of
# CMakePresets.json. The output goes to out/build/<preset>.
#
# Usage: ./mac_build.sh [debug|release] [--arch arm64|x64] [--test]
#   debug|release  build type (default: release)
#   --arch         target architecture (default: this machine's)
#   --test         run the tests after the build

set -euo pipefail

usage() {
  sed -n '2,8p' "$0" | sed 's/^# \{0,1\}//'
}

config="release"
arch=""
run_tests=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    debug | release)
      config="$1"
      ;;
    --arch)
      [[ $# -ge 2 ]] || { echo "error: --arch needs a value" >&2; exit 2; }
      arch="$2"
      shift
      ;;
    --test)
      run_tests=1
      ;;
    -h | --help)
      usage
      exit 0
      ;;
    *)
      echo "error: unknown argument '$1'" >&2
      usage >&2
      exit 2
      ;;
  esac
  shift
done

if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "error: mac_build.sh builds on macOS only" >&2
  exit 1
fi

if [[ -z "${arch}" ]]; then
  case "$(uname -m)" in
    arm64) arch="arm64" ;;
    x86_64) arch="x64" ;;
    *)
      echo "error: unknown machine '$(uname -m)'; pass --arch arm64|x64" >&2
      exit 1
      ;;
  esac
fi
if [[ "${arch}" != "arm64" && "${arch}" != "x64" ]]; then
  echo "error: --arch must be arm64 or x64, got '${arch}'" >&2
  exit 2
fi

for tool in cmake ninja; do
  if ! command -v "${tool}" > /dev/null; then
    echo "error: ${tool} not found (brew install ${tool})" >&2
    exit 1
  fi
done

preset="macos-${arch}-${config}"
cd "$(dirname "$0")"

echo "==> Configuring ${preset}"
cmake --preset "${preset}"

echo "==> Building ${preset}"
cmake --build --preset "${preset}"

if [[ "${run_tests}" -eq 1 ]]; then
  echo "==> Testing ${preset}"
  ctest --preset "${preset}"
fi

echo "==> Done: out/build/${preset}"
