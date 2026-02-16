#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE_TAG="phasegap-netem:latest"
IFACE="lo"
PRESET="wan_mild"
DELAY_MS="20"
JITTER_MS="5"
LOSS_PCT="0.20"
RATE=""
NP=2
THREADS=2
N_LOCAL=256
HALO=8
ITERS=8
WARMUP=2
BUILD_DIR="/work/build-docker-netem"
RUN_DIR="/work/runs/docker-netem-smoke"
TRANSPORT="tcp"
MODE="phase_nb"
DRY_RUN=0
NO_IMAGE_BUILD=0

usage() {
  cat <<'USAGE'
Usage: scripts/docker_netem_smoke.sh [options]

Builds/runs PhaseGap inside Docker and applies Linux tc/netem impairment inside
container namespace for a seamless simulated-network smoke run.

Options:
  --image <tag>         Docker image tag (default: phasegap-netem:latest)
  --iface <name>        Interface inside container (default: lo)
  --preset <name>       Netem preset (default: wan_mild)
  --delay-ms <ms>       Netem delay override (default: 20)
  --jitter-ms <ms>      Netem jitter override (default: 5)
  --loss-pct <pct>      Netem loss override (default: 0.20)
  --rate <rate>         Optional bandwidth cap (example: 50mbit)
  --np <P>              MPI ranks (default: 2)
  --threads <T>         OMP threads (default: 2)
  --n-local <N>         Local points per rank (default: 256)
  --halo <H>            Halo width (default: 8)
  --iters <K>           Iterations (default: 8)
  --warmup <W>          Warmup iterations (default: 2)
  --transport <name>    Transport hint (default: tcp)
  --mode <name>         Mode (default: phase_nb)
  --no-image-build      Skip docker build and reuse existing image
  --dry-run             Print docker commands only
  -h, --help            Show help
USAGE
}

require_pos_int() {
  local opt="$1"
  local value="$2"
  if [[ ! "${value}" =~ ^[1-9][0-9]*$ ]]; then
    echo "[docker-netem] fail: ${opt} must be a positive integer (got '${value}')" >&2
    exit 2
  fi
}

require_option_value() {
  local opt="$1"
  if [[ $# -lt 2 || -z "${2:-}" || "${2:-}" == --* ]]; then
    echo "[docker-netem] fail: ${opt} requires a value" >&2
    usage >&2
    exit 2
  fi
}

require_cmd() {
  local cmd="$1"
  if ! command -v "${cmd}" >/dev/null 2>&1; then
    echo "[docker-netem] fail: required command '${cmd}' not found" >&2
    exit 1
  fi
}

run_cmd() {
  echo "[docker-netem] $*"
  if (( DRY_RUN == 1 )); then
    return 0
  fi
  "$@"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --image)
      require_option_value "$1" "${2:-}"
      IMAGE_TAG="$2"
      shift 2
      ;;
    --iface)
      require_option_value "$1" "${2:-}"
      IFACE="$2"
      shift 2
      ;;
    --preset)
      require_option_value "$1" "${2:-}"
      PRESET="$2"
      shift 2
      ;;
    --delay-ms)
      require_option_value "$1" "${2:-}"
      DELAY_MS="$2"
      shift 2
      ;;
    --jitter-ms)
      require_option_value "$1" "${2:-}"
      JITTER_MS="$2"
      shift 2
      ;;
    --loss-pct)
      require_option_value "$1" "${2:-}"
      LOSS_PCT="$2"
      shift 2
      ;;
    --rate)
      require_option_value "$1" "${2:-}"
      RATE="$2"
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
    --transport)
      require_option_value "$1" "${2:-}"
      TRANSPORT="$2"
      shift 2
      ;;
    --mode)
      require_option_value "$1" "${2:-}"
      MODE="$2"
      shift 2
      ;;
    --no-image-build)
      NO_IMAGE_BUILD=1
      shift
      ;;
    --dry-run)
      DRY_RUN=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "[docker-netem] unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

require_pos_int "--np" "${NP}"
require_pos_int "--threads" "${THREADS}"
require_pos_int "--n-local" "${N_LOCAL}"
require_pos_int "--halo" "${HALO}"
require_pos_int "--iters" "${ITERS}"
require_pos_int "--warmup" "${WARMUP}"
if (( ITERS <= WARMUP )); then
  echo "[docker-netem] fail: require --iters > --warmup (got ${ITERS} <= ${WARMUP})" >&2
  exit 2
fi

if (( DRY_RUN == 0 )); then
  require_cmd docker
fi

if (( NO_IMAGE_BUILD == 0 )); then
  run_cmd docker build -f "${ROOT_DIR}/docker/Dockerfile.phasegap" -t "${IMAGE_TAG}" "${ROOT_DIR}"
fi

NETEM_ARGS=(
  --iface "${IFACE}"
  --preset "${PRESET}"
  --delay-ms "${DELAY_MS}"
  --jitter-ms "${JITTER_MS}"
  --loss-pct "${LOSS_PCT}"
)
if [[ -n "${RATE}" ]]; then
  NETEM_ARGS+=(--rate "${RATE}")
fi

INNER_CMD="set -euo pipefail
export OMPI_ALLOW_RUN_AS_ROOT=1
export OMPI_ALLOW_RUN_AS_ROOT_CONFIRM=1
cleanup() { scripts/netem_off.sh --iface ${IFACE} --quiet || true; }
trap cleanup EXIT
scripts/netem_on.sh ${NETEM_ARGS[*]}
scripts/smoke_build.sh --build-dir ${BUILD_DIR} --run-dir ${RUN_DIR} --np ${NP} --threads ${THREADS} --n-local ${N_LOCAL} --halo ${HALO} --iters ${ITERS} --warmup ${WARMUP}
OMP_NUM_THREADS=${THREADS} mpirun -np ${NP} ${BUILD_DIR}/phasegap --mode ${MODE} --transport ${TRANSPORT} --ranks ${NP} --threads ${THREADS} --N ${N_LOCAL} --halo ${HALO} --iters ${ITERS} --warmup ${WARMUP} --trace 1 --trace_iters 3 --out_dir ${RUN_DIR} --csv ${RUN_DIR}/results.csv --manifest 1
trap - EXIT
cleanup
"

run_cmd docker run --rm --cap-add NET_ADMIN --cap-add NET_RAW \
  -v "${ROOT_DIR}:/work" \
  -w /work \
  "${IMAGE_TAG}" \
  bash -lc "${INNER_CMD}"

echo "[docker-netem] complete; artifacts in runs/docker-netem-smoke"
