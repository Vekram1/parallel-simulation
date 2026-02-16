#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build-matrix"
RUN_ROOT="${ROOT_DIR}/runs/matrix"
INDEX_PATH="${ROOT_DIR}/results/results.csv"
INDEX_ENABLED=1
INDEX_PATH_EXPLICIT=0
GENERATOR=""
CORE_BUDGET=0
SWEEPS="1,2,3"
TRACE=1
TRACE_ITERS=30
ALLOW_OVERSUB=0
DRY_RUN=0
MAX_RUNS=0

KERNEL="stencil3"
N_LOCAL=200000
ITERS=120
WARMUP=20
BYTES_PER_POINT=0

RUN_COUNT=0
SKIP_COUNT=0
FAIL_COUNT=0

require_option_value() {
  local opt="$1"
  if [[ $# -lt 2 || -z "${2:-}" || "${2:-}" == --* ]]; then
    echo "[matrix] fail: ${opt} requires a value" >&2
    usage >&2
    exit 2
  fi
}

usage() {
  cat <<'USAGE'
Usage: scripts/run_matrix.sh [options]

Options:
  --build-dir <path>      CMake build directory (default: ./build-matrix)
  --run-root <path>       Root directory for run outputs (default: ./runs/matrix)
  --index <path>          Optional append-only index CSV (default: ./results/results.csv)
  --no-index              Disable aggregate index writes (per-run artifacts remain canonical)
  --generator <name>      CMake generator (default: Ninja if available)
  --core-budget <C>       Core budget for sweep planning (default: auto-detect)
  --sweeps <list>         Comma-separated sweep ids to run (default: 1,2,3)
  --trace <0|1>           Enable trace output in runs (default: 1)
  --trace-iters <M>       Trace iteration window when trace enabled (default: 30)
  --allow-oversub         Allow P*T > core budget (default: off)
  --dry-run               Print commands without executing
  --max-runs <N>          Stop after N runs (default: unlimited)
  --n-local <N>           Local elements per rank (default: 200000)
  --iters <K>             Iterations per run (default: 120)
  --warmup <W>            Warmup iterations per run (default: 20)
  --kernel <name>         Kernel (default: stencil3)
  --bytes-per-point <b>   Extra bytes-per-point knob (default: 0)
  -h, --help              Show help
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir)
      require_option_value "$1" "${2:-}"
      BUILD_DIR="$2"
      shift 2
      ;;
    --run-root)
      require_option_value "$1" "${2:-}"
      RUN_ROOT="$2"
      shift 2
      ;;
    --index)
      require_option_value "$1" "${2:-}"
      INDEX_PATH="$2"
      INDEX_PATH_EXPLICIT=1
      shift 2
      ;;
    --no-index)
      INDEX_ENABLED=0
      shift
      ;;
    --generator)
      require_option_value "$1" "${2:-}"
      GENERATOR="$2"
      shift 2
      ;;
    --core-budget)
      require_option_value "$1" "${2:-}"
      CORE_BUDGET="$2"
      shift 2
      ;;
    --sweeps)
      require_option_value "$1" "${2:-}"
      SWEEPS="$2"
      shift 2
      ;;
    --trace)
      require_option_value "$1" "${2:-}"
      TRACE="$2"
      shift 2
      ;;
    --trace-iters)
      require_option_value "$1" "${2:-}"
      TRACE_ITERS="$2"
      shift 2
      ;;
    --allow-oversub)
      ALLOW_OVERSUB=1
      shift
      ;;
    --dry-run)
      DRY_RUN=1
      shift
      ;;
    --max-runs)
      require_option_value "$1" "${2:-}"
      MAX_RUNS="$2"
      shift 2
      ;;
    --n-local)
      require_option_value "$1" "${2:-}"
      N_LOCAL="$2"
      shift 2
      ;;
    --iters)
      require_option_value "$1" "${2:-}"
      ITERS="$2"
      shift 2
      ;;
    --warmup)
      require_option_value "$1" "${2:-}"
      WARMUP="$2"
      shift 2
      ;;
    --kernel)
      require_option_value "$1" "${2:-}"
      KERNEL="$2"
      shift 2
      ;;
    --bytes-per-point)
      require_option_value "$1" "${2:-}"
      BYTES_PER_POINT="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "[matrix] unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if ! command -v cmake >/dev/null 2>&1; then
  echo "[matrix] fail: cmake not found in PATH" >&2
  exit 1
fi
if ! command -v mpirun >/dev/null 2>&1; then
  echo "[matrix] fail: mpirun not found in PATH" >&2
  exit 1
fi

if [[ -z "${GENERATOR}" ]]; then
  if command -v ninja >/dev/null 2>&1; then
    GENERATOR="Ninja"
  else
    GENERATOR="Unix Makefiles"
  fi
fi

if (( ITERS <= WARMUP )); then
  echo "[matrix] fail: require --iters > --warmup (got ${ITERS} <= ${WARMUP})" >&2
  exit 1
fi

if (( INDEX_ENABLED == 0 && INDEX_PATH_EXPLICIT == 1 )); then
  echo "[matrix] warn: both --index and --no-index were provided; ignoring --index and disabling aggregate writes"
fi

detect_core_budget() {
  if [[ "${CORE_BUDGET}" -gt 0 ]]; then
    echo "${CORE_BUDGET}"
    return
  fi
  if [[ "$(uname -s)" == "Darwin" ]]; then
    local perf_cores
    perf_cores="$(sysctl -n hw.perflevel0.physicalcpu 2>/dev/null || true)"
    if [[ -n "${perf_cores}" ]]; then
      echo "${perf_cores}"
      return
    fi
    local phys
    phys="$(sysctl -n hw.physicalcpu 2>/dev/null || true)"
    if [[ -n "${phys}" ]]; then
      echo "${phys}"
      return
    fi
  fi
  if command -v nproc >/dev/null 2>&1; then
    nproc
    return
  fi
  getconf _NPROCESSORS_ONLN
}

sanitize_run_id() {
  local raw="$1"
  printf '%s' "${raw}" | tr '[:upper:]' '[:lower:]' | tr -c 'a-z0-9._-' '_'
}

ensure_index_header() {
  if (( DRY_RUN == 1 || INDEX_ENABLED == 0 )); then
    return
  fi
  local index_dir
  index_dir="$(dirname "${INDEX_PATH}")"
  mkdir -p "${index_dir}"
  if [[ ! -f "${INDEX_PATH}" ]]; then
    echo "timestamp,run_id,sweep,mode,ranks,threads,halo,flops_per_point,bytes_per_point,status,run_dir" \
      >"${INDEX_PATH}"
  fi
}

append_index_row() {
  local row="$1"
  if (( DRY_RUN == 1 || INDEX_ENABLED == 0 )); then
    return
  fi
  echo "${row}" >>"${INDEX_PATH}"
}

configure_and_build() {
  if (( DRY_RUN == 1 )); then
    echo "[matrix] dry-run: would configure/build in ${BUILD_DIR}"
    return
  fi

  if [[ "$(uname -s)" == "Darwin" ]] && [[ -x "${ROOT_DIR}/scripts/dev/configure-macos-openmp.sh" ]]; then
    "${ROOT_DIR}/scripts/dev/configure-macos-openmp.sh" \
      --build-dir "${BUILD_DIR}" \
      --generator "${GENERATOR}" \
      --build \
      -- -DCMAKE_BUILD_TYPE=Release
    return
  fi

  cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -G "${GENERATOR}" -DCMAKE_BUILD_TYPE=Release
  cmake --build "${BUILD_DIR}"
}

run_one() {
  local sweep="$1"
  local mode="$2"
  local ranks="$3"
  local threads="$4"
  local halo="$5"
  local flops="$6"

  local work="$((ranks * threads))"
  if (( ALLOW_OVERSUB == 0 && work > CORE_BUDGET )); then
    echo "[matrix] skip oversubscription: sweep=${sweep} mode=${mode} P=${ranks} T=${threads} C=${CORE_BUDGET}"
    ((SKIP_COUNT += 1))
    return
  fi

  if (( MAX_RUNS > 0 && RUN_COUNT >= MAX_RUNS )); then
    return
  fi

  local stamp run_id run_dir log_path
  stamp="$(date -u +%Y%m%dT%H%M%SZ)"
  run_id="$(sanitize_run_id "${stamp}_sw${sweep}_${mode}_p${ranks}_t${threads}_h${halo}_f${flops}_b${BYTES_PER_POINT}")"
  run_dir="${RUN_ROOT}/${run_id}"
  log_path="${run_dir}/run.stdout.log"

  local -a cmd=(
    "${BUILD_DIR}/phasegap"
    --mode "${mode}"
    --ranks "${ranks}"
    --threads "${threads}"
    --N "${N_LOCAL}"
    --halo "${halo}"
    --iters "${ITERS}"
    --warmup "${WARMUP}"
    --kernel "${KERNEL}"
    --flops_per_point "${flops}"
    --bytes_per_point "${BYTES_PER_POINT}"
    --trace "${TRACE}"
    --trace_iters "${TRACE_ITERS}"
    --out_dir "${run_dir}"
    --csv "${run_dir}/results.csv"
    --manifest 1
  )

  echo "[matrix] run sweep=${sweep} mode=${mode} P=${ranks} T=${threads} H=${halo} flops=${flops} run_id=${run_id}"
  ((RUN_COUNT += 1))
  if (( DRY_RUN == 1 )); then
    echo "OMP_NUM_THREADS=${threads} mpirun -np ${ranks} ${cmd[*]}"
    return
  fi

  mkdir -p "${run_dir}"
  set +e
  OMP_NUM_THREADS="${threads}" mpirun -np "${ranks}" "${cmd[@]}" >"${log_path}" 2>&1
  local rc=$?
  set -e
  if (( rc != 0 )); then
    ((FAIL_COUNT += 1))
    echo "[matrix] fail rc=${rc} run_id=${run_id} (see ${log_path})"
    append_index_row "$(date -u +%Y-%m-%dT%H:%M:%SZ),${run_id},${sweep},${mode},${ranks},${threads},${halo},${flops},${BYTES_PER_POINT},fail,${run_dir}"
    return
  fi

  append_index_row "$(date -u +%Y-%m-%dT%H:%M:%SZ),${run_id},${sweep},${mode},${ranks},${threads},${halo},${flops},${BYTES_PER_POINT},ok,${run_dir}"
}

run_sweep_1() {
  local p=4
  local -a halos=(16 256 4096)
  local -a flops=(0 64)
  local -a modes=(phase_blk phase_nb nb_test phase_persist)
  local max_threads="$((CORE_BUDGET / p))"
  if (( max_threads < 1 )); then
    max_threads=1
  fi

  local -a thread_values=()
  local t=1
  while (( t <= max_threads )); do
    thread_values+=("${t}")
    t=$((t * 2))
  done
  if (( thread_values[${#thread_values[@]} - 1] != max_threads )); then
    thread_values+=("${max_threads}")
  fi

  local h f mode threads
  for h in "${halos[@]}"; do
    for f in "${flops[@]}"; do
      for mode in "${modes[@]}"; do
        for threads in "${thread_values[@]}"; do
          run_one "1" "${mode}" "${p}" "${threads}" "${h}" "${f}"
        done
      done
    done
  done
}

run_sweep_2() {
  local -a pairs=("1:${CORE_BUDGET}" "2:$((CORE_BUDGET / 2))" "4:$((CORE_BUDGET / 4))" "${CORE_BUDGET}:1")
  local -a modes=(phase_blk phase_nb nb_test phase_persist)
  local h=256
  local f=0

  local pair p t mode
  for pair in "${pairs[@]}"; do
    p="${pair%%:*}"
    t="${pair##*:}"
    if (( p < 1 || t < 1 )); then
      continue
    fi
    for mode in "${modes[@]}"; do
      run_one "2" "${mode}" "${p}" "${t}" "${h}" "${f}"
    done
  done
}

run_sweep_3() {
  local p=4
  local t=4
  local f=0
  local -a modes=(phase_nb nb_test phase_persist)
  local -a halos=(1 2 4 8 16 32 64 128 256 512 1024 2048 4096 8192)
  local mode h
  for h in "${halos[@]}"; do
    for mode in "${modes[@]}"; do
      run_one "3" "${mode}" "${p}" "${t}" "${h}" "${f}"
    done
  done
}

mkdir -p "${RUN_ROOT}"
ensure_index_header
CORE_BUDGET="$(detect_core_budget)"

echo "[matrix] root=${ROOT_DIR}"
echo "[matrix] build_dir=${BUILD_DIR}"
echo "[matrix] run_root=${RUN_ROOT}"
if (( INDEX_ENABLED == 1 )); then
  echo "[matrix] index=${INDEX_PATH}"
else
  echo "[matrix] index=disabled (--no-index)"
fi
echo "[matrix] sweeps=${SWEEPS}"
echo "[matrix] core_budget=${CORE_BUDGET}"
echo "[matrix] trace=${TRACE} trace_iters=${TRACE_ITERS}"
echo "[matrix] dry_run=${DRY_RUN} allow_oversub=${ALLOW_OVERSUB}"

configure_and_build

IFS=',' read -r -a sweep_list <<<"${SWEEPS}"
for sw in "${sweep_list[@]}"; do
  case "${sw}" in
    1) run_sweep_1 ;;
    2) run_sweep_2 ;;
    3) run_sweep_3 ;;
    "")
      ;;
    *)
      echo "[matrix] warn: unknown sweep id '${sw}' (expected 1,2,3)"
      ;;
  esac
  if (( MAX_RUNS > 0 && RUN_COUNT >= MAX_RUNS )); then
    break
  fi
done

echo "[matrix] complete: runs=${RUN_COUNT} skipped=${SKIP_COUNT} failed=${FAIL_COUNT}"
if (( FAIL_COUNT > 0 )); then
  exit 1
fi
