#!/usr/bin/env bash
set -euo pipefail

# PhaseGap quality gate scaffold (bd-1gh.7.2)
#
# Current behavior:
# - Configures and builds in Release mode
# - Runs a tiny sanity command
# - Verifies critical runtime summary fields
# - Runs metrics invariant checker
# - Optionally enforces CSV/trace artifacts in strict mode

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build-quality-gate"
RUN_DIR="${ROOT_DIR}/runs/quality-gate"
LOG_DIR="${RUN_DIR}/logs"
STDOUT_LOG="${LOG_DIR}/sanity.stdout.log"
RUN_START_MARKER="${LOG_DIR}/run.start.marker"
CSV_PATH="${RUN_DIR}/results.csv"
MANIFEST_PATH="${RUN_DIR}/manifest.json"
TRACE_PATH="${RUN_DIR}/trace.json"
PHASE_BLK_DIR="${RUN_DIR}/phase-blk-acceptance"
PHASE_NB_T1_DIR="${RUN_DIR}/phase-nb-t1-acceptance"
PHASE_NB_T2_DIR="${RUN_DIR}/phase-nb-t2-acceptance"
PHASE_BLK_LOG="${LOG_DIR}/phase-blk-acceptance.stdout.log"
PHASE_NB_T1_LOG="${LOG_DIR}/phase-nb-t1-acceptance.stdout.log"
PHASE_NB_T2_LOG="${LOG_DIR}/phase-nb-t2-acceptance.stdout.log"
GENERATOR=""
NP=2
THREADS=1
N_LOCAL=64
HALO=4
ITERS=4
WARMUP=1

STRICT_ARTIFACTS=0
SKIP_BUILD=0

require_option_value() {
  local opt="$1"
  if [[ $# -lt 2 || -z "${2:-}" || "${2:-}" == --* ]]; then
    echo "missing value for ${opt}" >&2
    echo "usage: scripts/quality_gate.sh [--strict-artifacts] [--skip-build] [--build-dir DIR] [--run-dir DIR] [--generator NAME] [--np P] [--threads T] [--n-local N] [--halo H] [--iters K] [--warmup W]" >&2
    exit 2
  fi
}

float_le() {
  local value="$1"
  local bound="$2"
  python3 - "$value" "$bound" <<'PY'
import sys
v = float(sys.argv[1])
b = float(sys.argv[2])
sys.exit(0 if v <= b else 1)
PY
}

float_ge() {
  local value="$1"
  local bound="$2"
  python3 - "$value" "$bound" <<'PY'
import sys
v = float(sys.argv[1])
b = float(sys.argv[2])
sys.exit(0 if v >= b else 1)
PY
}

run_case() {
  local mode="$1"
  local threads="$2"
  local out_dir="$3"
  local csv_path="$4"
  local stdout_log="$5"
  local trace_flag="$6"

  mkdir -p "${out_dir}"
  : >"${stdout_log}"
  local run_cmd=()
  if [[ "${NP}" -eq 1 ]]; then
    run_cmd=(
      "${BUILD_DIR}/phasegap"
      --mode "${mode}"
      --ranks "${NP}"
      --threads "${threads}"
      --N "${N_LOCAL}"
      --halo "${HALO}"
      --iters "${ITERS}"
      --warmup "${WARMUP}"
      --trace "${trace_flag}"
      --trace_iters 2
      --out_dir "${out_dir}"
      --csv "${csv_path}"
      --manifest 1
    )
  else
    run_cmd=(
      mpirun -np "${NP}" "${BUILD_DIR}/phasegap"
      --mode "${mode}"
      --ranks "${NP}"
      --threads "${threads}"
      --N "${N_LOCAL}"
      --halo "${HALO}"
      --iters "${ITERS}"
      --warmup "${WARMUP}"
      --trace "${trace_flag}"
      --trace_iters 2
      --out_dir "${out_dir}"
      --csv "${csv_path}"
      --manifest 1
    )
  fi

  if ! OMP_NUM_THREADS="${threads}" "${run_cmd[@]}" >"${stdout_log}" 2>&1; then
    echo "[gate] fail: ${mode} run failed (threads=${threads})" >&2
    cat "${stdout_log}" >&2
    exit 1
  fi
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --strict-artifacts)
      STRICT_ARTIFACTS=1
      shift
      ;;
    --skip-build)
      SKIP_BUILD=1
      shift
      ;;
    --build-dir)
      require_option_value "$1" "${2:-}"
      BUILD_DIR="$2"
      shift 2
      ;;
    --run-dir)
      require_option_value "$1" "${2:-}"
      RUN_DIR="$2"
      LOG_DIR="${RUN_DIR}/logs"
      STDOUT_LOG="${LOG_DIR}/sanity.stdout.log"
      RUN_START_MARKER="${LOG_DIR}/run.start.marker"
      CSV_PATH="${RUN_DIR}/results.csv"
      MANIFEST_PATH="${RUN_DIR}/manifest.json"
      TRACE_PATH="${RUN_DIR}/trace.json"
      shift 2
      ;;
    --generator)
      require_option_value "$1" "${2:-}"
      GENERATOR="$2"
      shift 2
      ;;
    --np)
      require_option_value "$1" "${2:-}"
      NP="$2"
      shift 2
      ;;
    --threads)
      require_option_value "$1" "${2:-}"
      THREADS="$2"
      shift 2
      ;;
    --n-local)
      require_option_value "$1" "${2:-}"
      N_LOCAL="$2"
      shift 2
      ;;
    --halo)
      require_option_value "$1" "${2:-}"
      HALO="$2"
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
    *)
      echo "unknown option: $1" >&2
      echo "usage: scripts/quality_gate.sh [--strict-artifacts] [--skip-build] [--build-dir DIR] [--run-dir DIR] [--generator NAME] [--np P] [--threads T] [--n-local N] [--halo H] [--iters K] [--warmup W]" >&2
      exit 2
      ;;
  esac
done

mkdir -p "${LOG_DIR}"

fail_or_warn() {
  local fail_msg="$1"
  local warn_msg="$2"
  if [[ ${STRICT_ARTIFACTS} -eq 1 ]]; then
    echo "[gate] fail: ${fail_msg}" >&2
    exit 1
  fi
  echo "[gate] warn: ${warn_msg}"
}

require_rg() {
  if ! command -v rg >/dev/null 2>&1; then
    echo "[gate] fail: rg (ripgrep) not found in PATH" >&2
    exit 1
  fi
}

require_python3() {
  if ! command -v python3 >/dev/null 2>&1; then
    echo "[gate] fail: python3 not found in PATH" >&2
    exit 1
  fi
}

require_file_contains() {
  local path="$1"
  local pattern="$2"
  local fail_msg="$3"
  local warn_msg="$4"
  if rg -q "${pattern}" "${path}"; then
    return 0
  fi
  fail_or_warn "${fail_msg}" "${warn_msg}"
}

require_csv_column() {
  local path="$1"
  local column="$2"
  local header
  header="$(head -n 1 "${path}")"
  if [[ ",${header}," == *",${column},"* ]]; then
    return 0
  fi
  fail_or_warn \
    "csv header missing ${column} (${path})" \
    "csv header missing ${column} (${path})"
}

csv_last_field_by_name() {
  local path="$1"
  local field_name="$2"
  awk -F',' -v field="${field_name}" '
    NR == 1 {
      for (i = 1; i <= NF; ++i) {
        if ($i == field) {
          col = i
          break
        }
      }
      next
    }
    { value = (col > 0 ? $col : "") }
    END {
      if (col == 0) {
        exit 2
      }
      print value
    }
  ' "${path}"
}

require_artifact_fresh() {
  local path="$1"
  local marker="$2"
  if [[ "${marker}" -nt "${path}" ]]; then
    fail_or_warn \
      "artifact is stale (older than current run): ${path}" \
      "artifact appears stale (older than current run): ${path}"
  fi
}

validate_manifest_schema() {
  local path="$1"
  if ! python3 -m json.tool "${path}" >/dev/null 2>&1; then
    fail_or_warn \
      "manifest is not valid JSON (${path})" \
      "manifest is not valid JSON (${path})"
    return 0
  fi

  require_file_contains \
    "${path}" "\"timestamp\"" \
    "manifest missing required key: timestamp (${path})" \
    "manifest missing key timestamp (${path})"
  require_file_contains \
    "${path}" "\"git_sha\"" \
    "manifest missing required key: git_sha (${path})" \
    "manifest missing key git_sha (${path})"
  require_file_contains \
    "${path}" "\"mpi_thread_provided\"" \
    "manifest missing required key: mpi_thread_provided (${path})" \
    "manifest missing key mpi_thread_provided (${path})"
}

validate_csv_schema() {
  local path="$1"
  require_csv_column "${path}" "schema_version"
  require_csv_column "${path}" "mode"
  require_csv_column "${path}" "checksum64"

  if [[ "$(wc -l <"${path}")" -lt 2 ]]; then
    fail_or_warn \
      "csv does not contain a data row (${path})" \
      "csv does not contain a data row (${path})"
    return 0
  fi

  local tail_schema
  if ! tail_schema="$(csv_last_field_by_name "${path}" "schema_version")"; then
    fail_or_warn \
      "could not resolve csv schema_version column (${path})" \
      "could not resolve csv schema_version column (${path})"
    return 0
  fi
  if [[ ! "${tail_schema}" =~ ^[0-9]+$ ]]; then
    fail_or_warn \
      "csv last row schema_version is not an integer: '${tail_schema}' (${path})" \
      "csv last row schema_version is not an integer: '${tail_schema}' (${path})"
  fi

  local tail_mode
  if ! tail_mode="$(csv_last_field_by_name "${path}" "mode")"; then
    fail_or_warn \
      "could not resolve csv mode column (${path})" \
      "could not resolve csv mode column (${path})"
    return 0
  fi
  if [[ "${tail_mode}" != "phase_nb" ]]; then
    fail_or_warn \
      "csv last row mode expected phase_nb, got '${tail_mode}' (${path})" \
      "csv last row mode expected phase_nb, got '${tail_mode}' (${path})"
  fi
}

validate_trace_schema() {
  local path="$1"
  if ! python3 -m json.tool "${path}" >/dev/null 2>&1; then
    fail_or_warn \
      "trace is not valid JSON (${path})" \
      "trace is not valid JSON (${path})"
    return 0
  fi

  require_file_contains \
    "${path}" "\"traceEvents\"" \
    "trace missing traceEvents array (${path})" \
    "trace missing traceEvents array (${path})"

  local label
  for label in comm_post interior_compute waitall boundary_compute; do
    require_file_contains \
      "${path}" "${label}" \
      "trace missing expected phase label '${label}' (${path})" \
      "trace missing expected phase label '${label}' (${path})"
  done
}

if ! command -v cmake >/dev/null 2>&1; then
  echo "[gate] fail: cmake not found in PATH" >&2
  exit 1
fi

if [[ "${NP}" -gt 1 ]]; then
  if ! command -v mpirun >/dev/null 2>&1; then
    echo "[gate] fail: mpirun not found in PATH (required when --np > 1)" >&2
    exit 1
  fi
fi

require_rg
require_python3

if (( ITERS <= WARMUP )); then
  echo "[gate] fail: require --iters > --warmup (got iters=${ITERS}, warmup=${WARMUP})" >&2
  exit 1
fi

echo "[gate] configure/build args: build_dir=${BUILD_DIR} run_dir=${RUN_DIR} np=${NP} threads=${THREADS} n_local=${N_LOCAL} halo=${HALO} iters=${ITERS} warmup=${WARMUP} skip_build=${SKIP_BUILD}"

if [[ ${SKIP_BUILD} -eq 0 ]]; then
  echo "[gate] configure"
  if [[ -n "${GENERATOR}" ]]; then
    CMAKE_CONFIGURE_CMD=(cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release -G "${GENERATOR}")
  else
    CMAKE_CONFIGURE_CMD=(cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release)
  fi
  if ! "${CMAKE_CONFIGURE_CMD[@]}"; then
    echo "[gate] fail: configure failed (likely missing MPI/OpenMP toolchain in this environment)" >&2
    echo "[gate] hint: if you see 'ld: library System not found' on macOS, run xcode-select --install and verify SDK tools in your shell." >&2
    echo "[gate] hint: if OpenMP is missing, use scripts/dev/configure-macos-openmp.sh for the build dir." >&2
    exit 1
  fi

  echo "[gate] build"
  if ! cmake --build "${BUILD_DIR}"; then
    echo "[gate] fail: build failed" >&2
    exit 1
  fi
else
  echo "[gate] skip-build enabled; using existing executable in ${BUILD_DIR}"
fi

if [[ ! -x "${BUILD_DIR}/phasegap" ]]; then
  echo "[gate] fail: missing executable ${BUILD_DIR}/phasegap" >&2
  exit 1
fi

echo "[gate] sanity run"
: >"${RUN_START_MARKER}"
run_case "phase_nb" "${THREADS}" "${RUN_DIR}" "${RUN_DIR}/results.csv" "${STDOUT_LOG}" 1

if ! rg -q "phasegap skeleton ready" "${STDOUT_LOG}"; then
  echo "[gate] fail: sanity output missing run summary" >&2
  cat "${STDOUT_LOG}" >&2
  exit 1
fi

for required_field in checksum64= mode= kernel= B=; do
  if ! rg -q "${required_field}" "${STDOUT_LOG}"; then
    echo "[gate] fail: sanity output missing field '${required_field}'" >&2
    cat "${STDOUT_LOG}" >&2
    exit 1
  fi
done

if ! python3 "${ROOT_DIR}/scripts/check_metrics.py" --log "${STDOUT_LOG}" --expect-measured-iters "$((ITERS - WARMUP))"; then
  echo "[gate] fail: metrics checker reported invariant violation" >&2
  exit 1
fi

echo "[gate] acceptance run: phase_blk no-overlap baseline"
run_case "phase_blk" "${THREADS}" "${PHASE_BLK_DIR}" "${PHASE_BLK_DIR}/results.csv" "${PHASE_BLK_LOG}" 0
if ! python3 "${ROOT_DIR}/scripts/check_metrics.py" --log "${PHASE_BLK_LOG}" --expect-measured-iters "$((ITERS - WARMUP))"; then
  echo "[gate] fail: phase_blk metrics checker reported invariant violation" >&2
  exit 1
fi
blk_overlap="$(csv_last_field_by_name "${PHASE_BLK_DIR}/results.csv" "overlap_ratio")"
if ! float_le "${blk_overlap}" "0.05"; then
  echo "[gate] fail: phase_blk overlap_ratio too high for no-overlap baseline (got ${blk_overlap}, max 0.05)" >&2
  exit 1
fi

echo "[gate] acceptance run: phase_nb thread-scaling visibility check"
run_case "phase_nb" "1" "${PHASE_NB_T1_DIR}" "${PHASE_NB_T1_DIR}/results.csv" "${PHASE_NB_T1_LOG}" 0
run_case "phase_nb" "2" "${PHASE_NB_T2_DIR}" "${PHASE_NB_T2_DIR}/results.csv" "${PHASE_NB_T2_LOG}" 0
if ! python3 "${ROOT_DIR}/scripts/check_metrics.py" --log "${PHASE_NB_T1_LOG}" --expect-measured-iters "$((ITERS - WARMUP))"; then
  echo "[gate] fail: phase_nb T=1 metrics checker reported invariant violation" >&2
  exit 1
fi
if ! python3 "${ROOT_DIR}/scripts/check_metrics.py" --log "${PHASE_NB_T2_LOG}" --expect-measured-iters "$((ITERS - WARMUP))"; then
  echo "[gate] fail: phase_nb T=2 metrics checker reported invariant violation" >&2
  exit 1
fi
nb_wait_t1="$(csv_last_field_by_name "${PHASE_NB_T1_DIR}/results.csv" "wait_frac")"
nb_wait_t2="$(csv_last_field_by_name "${PHASE_NB_T2_DIR}/results.csv" "wait_frac")"
# Visibility check with tolerance to reduce false negatives on noisy hosts.
if ! float_ge "${nb_wait_t2}" "$(python3 - "${nb_wait_t1}" <<'PY'
import sys
w1 = float(sys.argv[1])
print(w1 - 0.02)
PY
)"; then
  echo "[gate] fail: phase_nb wait_frac did not increase/stay comparable when threads rose (T1=${nb_wait_t1}, T2=${nb_wait_t2})" >&2
  exit 1
fi

if [[ -f "${MANIFEST_PATH}" ]]; then
  echo "[gate] manifest: ok (${MANIFEST_PATH})"
  require_artifact_fresh "${MANIFEST_PATH}" "${RUN_START_MARKER}"
  validate_manifest_schema "${MANIFEST_PATH}"
else
  if [[ ${STRICT_ARTIFACTS} -eq 1 ]]; then
    echo "[gate] fail: missing manifest (${MANIFEST_PATH})" >&2
    exit 1
  fi
  echo "[gate] warn: manifest missing (expected once manifest writer lands): ${MANIFEST_PATH}"
fi

if [[ -f "${CSV_PATH}" ]]; then
  echo "[gate] csv: ok (${CSV_PATH})"
  require_artifact_fresh "${CSV_PATH}" "${RUN_START_MARKER}"
  validate_csv_schema "${CSV_PATH}"
else
  if [[ ${STRICT_ARTIFACTS} -eq 1 ]]; then
    echo "[gate] fail: missing csv (${CSV_PATH})" >&2
    exit 1
  fi
  echo "[gate] warn: csv missing (expected once CSV writer lands): ${CSV_PATH}"
fi

if [[ -f "${TRACE_PATH}" ]]; then
  echo "[gate] trace: ok (${TRACE_PATH})"
  require_artifact_fresh "${TRACE_PATH}" "${RUN_START_MARKER}"
  validate_trace_schema "${TRACE_PATH}"
else
  if [[ ${STRICT_ARTIFACTS} -eq 1 ]]; then
    echo "[gate] fail: missing trace (${TRACE_PATH})" >&2
    exit 1
  fi
  echo "[gate] warn: trace missing (expected once trace writer lands): ${TRACE_PATH}"
fi

echo "[gate] pass"
