#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
BUILD_TYPE="Release"
GENERATOR=""
DO_BUILD=0
EXTRA_CMAKE_ARGS=()

usage() {
  cat <<'USAGE'
Usage: scripts/dev/configure-macos-openmp.sh [options] [-- <extra cmake args>]

Options:
  --build-dir <path>    CMake build directory (default: ./build)
  --build-type <type>   CMake build type (default: Release)
  --generator <name>    CMake generator (default: Ninja if available, else Unix Makefiles)
  --build               Also run cmake --build after configure
  -h, --help            Show this help

Examples:
  scripts/dev/configure-macos-openmp.sh --build-dir build-appleomp --build
  scripts/dev/configure-macos-openmp.sh -- -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
USAGE
}

require_option_value() {
  local opt="$1"
  if [[ $# -lt 2 || -z "${2:-}" || "${2:-}" == --* ]]; then
    echo "[configure-macos-openmp] fail: ${opt} requires a value" >&2
    usage >&2
    exit 2
  fi
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir)
      require_option_value "$1" "${2:-}"
      BUILD_DIR="$2"
      shift 2
      ;;
    --build-type)
      require_option_value "$1" "${2:-}"
      BUILD_TYPE="$2"
      shift 2
      ;;
    --generator)
      require_option_value "$1" "${2:-}"
      GENERATOR="$2"
      shift 2
      ;;
    --build)
      DO_BUILD=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    --)
      shift
      while [[ $# -gt 0 ]]; do
        EXTRA_CMAKE_ARGS+=("$1")
        shift
      done
      break
      ;;
    *)
      echo "[configure-macos-openmp] unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "[configure-macos-openmp] fail: this helper is intended for macOS only" >&2
  exit 1
fi

if ! command -v cmake >/dev/null 2>&1; then
  echo "[configure-macos-openmp] fail: cmake not found in PATH" >&2
  exit 1
fi
if ! command -v brew >/dev/null 2>&1; then
  echo "[configure-macos-openmp] fail: Homebrew not found. Install Homebrew and libomp first." >&2
  exit 1
fi

if [[ -z "${GENERATOR}" ]]; then
  if command -v ninja >/dev/null 2>&1; then
    GENERATOR="Ninja"
  else
    GENERATOR="Unix Makefiles"
  fi
fi

LIBOMP_PREFIX="$(brew --prefix libomp 2>/dev/null || true)"
if [[ -z "${LIBOMP_PREFIX}" || ! -f "${LIBOMP_PREFIX}/lib/libomp.dylib" ]]; then
  echo "[configure-macos-openmp] fail: libomp not found via Homebrew" >&2
  echo "[configure-macos-openmp] hint: brew install libomp" >&2
  exit 1
fi

if [[ -z "${SDKROOT:-}" ]] && command -v xcrun >/dev/null 2>&1; then
  SDKROOT="$(xcrun --sdk macosx --show-sdk-path)"
  export SDKROOT
fi

CXX_COMPILER="${CXX:-/usr/bin/clang++}"

CMAKE_CMD=(
  cmake
  -S "${ROOT_DIR}"
  -B "${BUILD_DIR}"
  -G "${GENERATOR}"
  -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
  -DCMAKE_CXX_COMPILER="${CXX_COMPILER}"
  -DOpenMP_CXX_FLAGS="-Xpreprocessor -fopenmp -I${LIBOMP_PREFIX}/include"
  -DOpenMP_CXX_LIB_NAMES=omp
  -DOpenMP_omp_LIBRARY="${LIBOMP_PREFIX}/lib/libomp.dylib"
)

if [[ -n "${SDKROOT:-}" ]]; then
  CMAKE_CMD+=("-DCMAKE_OSX_SYSROOT=${SDKROOT}")
fi
if [[ ${#EXTRA_CMAKE_ARGS[@]} -gt 0 ]]; then
  CMAKE_CMD+=("${EXTRA_CMAKE_ARGS[@]}")
fi

echo "[configure-macos-openmp] configuring ${BUILD_DIR} (generator=${GENERATOR}, compiler=${CXX_COMPILER})"
"${CMAKE_CMD[@]}"

if [[ ${DO_BUILD} -eq 1 ]]; then
  echo "[configure-macos-openmp] building ${BUILD_DIR}"
  cmake --build "${BUILD_DIR}"
fi

echo "[configure-macos-openmp] done"
