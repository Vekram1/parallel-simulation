#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
RUN_DIR="${ROOT_DIR}/runs/smoke"
LOG_PATH=""
GENERATOR=""
NP=2
THREADS=2
N_LOCAL=256
HALO=8
ITERS=8
WARMUP=2
JOBS=""
CMAKE_ARGS=()
AUTO_CXX_COMPILER=""

usage() {
  cat <<'USAGE'
Usage: scripts/smoke_build.sh [options]

Options:
  --build-dir <path>   Build directory (default: ./build)
  --run-dir <path>     Run output directory (default: ./runs/smoke)
  --generator <name>   CMake generator (default: Ninja if available, else Unix Makefiles)
  --np <P>             MPI ranks for smoke run (default: 2)
  --threads <T>        OMP threads for smoke run (default: 2)
  --n-local <N>        Local points per rank (default: 256)
  --halo <H>           Halo width (default: 8)
  --iters <K>          Total iterations (default: 8)
  --warmup <W>         Warmup iterations (default: 2)
  --jobs <J>           Build parallelism for cmake --build
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir)
      BUILD_DIR="$2"
      shift 2
      ;;
    --run-dir)
      RUN_DIR="$2"
      shift 2
      ;;
    --generator)
      GENERATOR="$2"
      shift 2
      ;;
    --np)
      NP="$2"
      shift 2
      ;;
    --threads)
      THREADS="$2"
      shift 2
      ;;
    --n-local)
      N_LOCAL="$2"
      shift 2
      ;;
    --halo)
      HALO="$2"
      shift 2
      ;;
    --iters)
      ITERS="$2"
      shift 2
      ;;
    --warmup)
      WARMUP="$2"
      shift 2
      ;;
    --jobs)
      JOBS="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "[smoke] unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if [[ -z "${GENERATOR}" ]]; then
  if command -v ninja >/dev/null 2>&1; then
    GENERATOR="Ninja"
  else
    GENERATOR="Unix Makefiles"
  fi
fi

if ! command -v cmake >/dev/null 2>&1; then
  echo "[smoke] fail: cmake not found in PATH" >&2
  exit 1
fi
if ! command -v mpirun >/dev/null 2>&1; then
  echo "[smoke] fail: mpirun not found in PATH" >&2
  exit 1
fi
if ! command -v python3 >/dev/null 2>&1; then
  echo "[smoke] fail: python3 not found in PATH" >&2
  exit 1
fi

if (( ITERS <= WARMUP )); then
  echo "[smoke] fail: require --iters > --warmup (got iters=${ITERS}, warmup=${WARMUP})" >&2
  exit 1
fi

mkdir -p "${RUN_DIR}"
LOG_PATH="${RUN_DIR}/smoke.stdout.log"

if [[ "$(uname -s)" == "Darwin" ]]; then
  if [[ -z "${SDKROOT:-}" ]] && command -v xcrun >/dev/null 2>&1; then
    SDKROOT="$(xcrun --sdk macosx --show-sdk-path)"
    export SDKROOT
  fi
  if [[ -z "${CXX:-}" ]]; then
    AUTO_CXX_COMPILER="/usr/bin/clang++"
    export CXX="${AUTO_CXX_COMPILER}"
  fi
  if [[ -n "${SDKROOT:-}" ]]; then
    CMAKE_ARGS+=("-DCMAKE_OSX_SYSROOT=${SDKROOT}")
  fi
  if [[ -n "${AUTO_CXX_COMPILER}" ]]; then
    CMAKE_ARGS+=("-DCMAKE_CXX_COMPILER=${AUTO_CXX_COMPILER}")
  fi
  if command -v brew >/dev/null 2>&1; then
    LIBOMP_PREFIX="$(brew --prefix libomp 2>/dev/null || true)"
    if [[ -n "${LIBOMP_PREFIX}" && -f "${LIBOMP_PREFIX}/lib/libomp.dylib" ]]; then
      CMAKE_ARGS+=(
        "-DOpenMP_ROOT=${LIBOMP_PREFIX}"
        "-DOpenMP_CXX_FLAGS=-Xpreprocessor -fopenmp -I${LIBOMP_PREFIX}/include"
        "-DOpenMP_CXX_LIB_NAMES=omp"
        "-DOpenMP_omp_LIBRARY=${LIBOMP_PREFIX}/lib/libomp.dylib"
      )
    fi
  fi
fi

echo "[smoke] configure (generator=${GENERATOR})"
if ! cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -G "${GENERATOR}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  "${CMAKE_ARGS[@]}"; then
  echo "[smoke] fail: configure failed" >&2
  echo "[smoke] hint: verify a working C++ toolchain and OpenMP-capable compiler are configured." >&2
  echo "[smoke] hint: on macOS, ensure SDK/linker setup is valid; if needed set CC/CXX explicitly." >&2
  exit 1
fi

echo "[smoke] build"
if [[ -n "${JOBS}" ]]; then
  if ! cmake --build "${BUILD_DIR}" --parallel "${JOBS}"; then
    echo "[smoke] fail: build failed" >&2
    exit 1
  fi
else
  if ! cmake --build "${BUILD_DIR}"; then
    echo "[smoke] fail: build failed" >&2
    exit 1
  fi
fi

if [[ ! -f "${BUILD_DIR}/compile_commands.json" ]]; then
  echo "[smoke] fail: missing compile_commands.json in ${BUILD_DIR}" >&2
  exit 1
fi

echo "[smoke] run"
: > "${LOG_PATH}"
if ! OMP_NUM_THREADS="${THREADS}" mpirun -np "${NP}" "${BUILD_DIR}/phasegap" \
  --mode phase_nb \
  --ranks "${NP}" \
  --threads "${THREADS}" \
  --N "${N_LOCAL}" \
  --halo "${HALO}" \
  --iters "${ITERS}" \
  --warmup "${WARMUP}" \
  --trace 0 \
  --out_dir "${RUN_DIR}" \
  --csv "${RUN_DIR}/results.csv" \
  --manifest 1 \
  > "${LOG_PATH}" 2>&1; then
  echo "[smoke] fail: smoke run failed; see ${LOG_PATH}" >&2
  cat "${LOG_PATH}" >&2
  exit 1
fi

echo "[smoke] validate metrics"
python3 "${ROOT_DIR}/scripts/check_metrics.py" \
  --log "${LOG_PATH}" \
  --expect-measured-iters "$((ITERS - WARMUP))"

echo "[smoke] pass"
