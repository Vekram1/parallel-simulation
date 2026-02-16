#!/usr/bin/env bash
set -euo pipefail

# PhaseGap quality gate scaffold (bd-1gh.7.2)
#
# Current behavior:
# - Configures and builds in Release mode
# - Runs a tiny sanity command
# - Verifies critical runtime summary fields
# - Optionally enforces CSV/trace artifacts in strict mode

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
RUN_DIR="${ROOT_DIR}/runs/quality-gate"
LOG_DIR="${RUN_DIR}/logs"
STDOUT_LOG="${LOG_DIR}/sanity.stdout.log"
CSV_PATH="${RUN_DIR}/results.csv"
MANIFEST_PATH="${RUN_DIR}/manifest.json"
TRACE_PATH="${RUN_DIR}/trace.json"

STRICT_ARTIFACTS=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --strict-artifacts)
      STRICT_ARTIFACTS=1
      shift
      ;;
    *)
      echo "unknown option: $1" >&2
      echo "usage: scripts/quality_gate.sh [--strict-artifacts]" >&2
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

validate_manifest_schema() {
  local path="$1"
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
}

validate_trace_schema() {
  local path="$1"
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

echo "[gate] configure"
if ! cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release; then
  echo "[gate] fail: configure failed (likely missing MPI/OpenMP toolchain in this environment)" >&2
  exit 1
fi

echo "[gate] build"
if ! cmake --build "${BUILD_DIR}"; then
  echo "[gate] fail: build failed" >&2
  exit 1
fi

if [[ ! -x "${BUILD_DIR}/phasegap" ]]; then
  echo "[gate] fail: missing executable ${BUILD_DIR}/phasegap" >&2
  exit 1
fi

if ! command -v mpirun >/dev/null 2>&1; then
  echo "[gate] fail: mpirun not found in PATH" >&2
  exit 1
fi

require_rg

echo "[gate] sanity run"
mpirun -np 2 "${BUILD_DIR}/phasegap" \
  --mode phase_nb \
  --ranks 2 \
  --threads 1 \
  --N 64 \
  --halo 4 \
  --iters 4 \
  --warmup 1 \
  --trace 1 \
  --trace_iters 2 \
  --out_dir "${RUN_DIR}" \
  --csv "${RUN_DIR}/results.csv" \
  --manifest 1 \
  >"${STDOUT_LOG}" 2>&1

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

if [[ -f "${MANIFEST_PATH}" ]]; then
  echo "[gate] manifest: ok (${MANIFEST_PATH})"
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
  validate_trace_schema "${TRACE_PATH}"
else
  if [[ ${STRICT_ARTIFACTS} -eq 1 ]]; then
    echo "[gate] fail: missing trace (${TRACE_PATH})" >&2
    exit 1
  fi
  echo "[gate] warn: trace missing (expected once trace writer lands): ${TRACE_PATH}"
fi

echo "[gate] pass"
