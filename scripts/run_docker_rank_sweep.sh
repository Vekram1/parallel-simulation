#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
COMPOSE_FILE="${ROOT_DIR}/docker/docker-compose.mpi.yml"
DOCKER_PROJECT=""
MIN_RANKS=1
MAX_RANKS=10
SCENARIOS="lan_clean,wan_mild,constrained_50"
SCENARIO_KIND="netem"
IFACE="eth0"
MODES="phase_blk,phase_nb,nb_test,phase_persist,omp_tasks"
TRIALS=10
THREADS=1
N_LOCAL=4096
HALO=8
ITERS=60
WARMUP=10
TRANSPORT="tcp"
OUT_ROOT="${ROOT_DIR}/runs/multihost-rank-sweep"
START_STACK=1
KEEP_UP=0
DRY_RUN=0
SEEN_MIN_RANKS=0
SEEN_MAX_RANKS=0

usage() {
  cat <<'USAGE'
Usage: scripts/run_docker_rank_sweep.sh [options]

Run N-hosts=N-ranks docker-sim sweeps for n=min..max, where each n uses hostfile:
  worker1
  worker2
  ...
  workerN

Options:
  --min-ranks <n>            Minimum ranks (default: 1)
  --max-ranks <n>            Maximum ranks (default: 10)
  --min-rank <n>             Alias for --min-ranks
  --max-rank <n>             Alias for --max-ranks
  --scenarios <csv>          Scenario labels/presets (default: lan_clean,wan_mild,constrained_50)
  --scenario-kind <kind>     Scenario backend: netem|tcpcc|label (default: netem)
  --iface <name>             Network iface for netem scenarios (default: eth0)
  --modes <csv>              PhaseGap modes (default: phase_blk,phase_nb,nb_test,phase_persist,omp_tasks)
  --trials <n>               Trials per (scenario,mode) (default: 10)
  --threads <n>              OMP threads (default: 1)
  --n-local <n>              Local N per rank (default: 4096)
  --halo <n>                 Halo width (default: 8)
  --iters <n>                Iterations (default: 60)
  --warmup <n>               Warmup iterations (default: 10)
  --transport <name>         Transport label for phasegap (default: tcp)
  --out-root <dir>           Output root (default: runs/multihost-rank-sweep)

  --compose-file <path>      docker-compose file (default: docker/docker-compose.mpi.yml)
  --docker-project <name>    docker compose project name
  --no-start-stack           Do not run docker compose up -d
  --keep-up                  Keep compose stack up when done
  --dry-run                  Print commands without executing
  -h, --help                 Show help
USAGE
}

require_option_value() {
  local opt="$1"
  if [[ $# -lt 2 || -z "${2:-}" || "${2:-}" == --* ]]; then
    echo "[rank-sweep] fail: ${opt} requires a value" >&2
    usage >&2
    exit 2
  fi
}

require_pos_int() {
  local opt="$1"
  local value="$2"
  if [[ ! "${value}" =~ ^[1-9][0-9]*$ ]]; then
    echo "[rank-sweep] fail: ${opt} must be a positive integer (got '${value}')" >&2
    exit 2
  fi
}

run_cmd() {
  echo "[rank-sweep] $*"
  if (( DRY_RUN == 1 )); then
    return 0
  fi
  "$@"
}

COMPOSE_BIN=()
detect_compose() {
  if command -v docker >/dev/null 2>&1 && docker compose version >/dev/null 2>&1; then
    COMPOSE_BIN=(docker compose)
    return 0
  fi
  if command -v docker-compose >/dev/null 2>&1; then
    COMPOSE_BIN=(docker-compose)
    return 0
  fi
  echo "[rank-sweep] fail: docker compose not found" >&2
  exit 1
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --min-ranks|--min-rank)
      if (( SEEN_MIN_RANKS == 1 )); then
        echo "[rank-sweep] fail: --min-ranks provided more than once" >&2
        echo "[rank-sweep] hint: use --min-ranks <a> --max-ranks <b> for inclusive [a,b]" >&2
        exit 2
      fi
      require_option_value "$1" "${2:-}"
      MIN_RANKS="$2"
      SEEN_MIN_RANKS=1
      shift 2
      ;;
    --max-ranks|--max-rank)
      if (( SEEN_MAX_RANKS == 1 )); then
        echo "[rank-sweep] fail: --max-ranks provided more than once" >&2
        echo "[rank-sweep] hint: use --min-ranks <a> --max-ranks <b> for inclusive [a,b]" >&2
        exit 2
      fi
      require_option_value "$1" "${2:-}"
      MAX_RANKS="$2"
      SEEN_MAX_RANKS=1
      shift 2
      ;;
    --scenarios)
      require_option_value "$1" "${2:-}"
      SCENARIOS="$2"
      shift 2
      ;;
    --scenario-kind)
      require_option_value "$1" "${2:-}"
      SCENARIO_KIND="$2"
      shift 2
      ;;
    --iface)
      require_option_value "$1" "${2:-}"
      IFACE="$2"
      shift 2
      ;;
    --modes)
      require_option_value "$1" "${2:-}"
      MODES="$2"
      shift 2
      ;;
    --trials)
      require_option_value "$1" "${2:-}"
      TRIALS="$2"
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
    --out-root)
      require_option_value "$1" "${2:-}"
      OUT_ROOT="$2"
      shift 2
      ;;
    --compose-file)
      require_option_value "$1" "${2:-}"
      COMPOSE_FILE="$2"
      shift 2
      ;;
    --docker-project)
      require_option_value "$1" "${2:-}"
      DOCKER_PROJECT="$2"
      shift 2
      ;;
    --no-start-stack)
      START_STACK=0
      shift
      ;;
    --keep-up)
      KEEP_UP=1
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
      echo "[rank-sweep] fail: unknown option '$1'" >&2
      usage >&2
      exit 2
      ;;
  esac
done

require_pos_int "--min-ranks" "${MIN_RANKS}"
require_pos_int "--max-ranks" "${MAX_RANKS}"
require_pos_int "--trials" "${TRIALS}"
require_pos_int "--threads" "${THREADS}"
require_pos_int "--n-local" "${N_LOCAL}"
require_pos_int "--halo" "${HALO}"
require_pos_int "--iters" "${ITERS}"
require_pos_int "--warmup" "${WARMUP}"

if (( MAX_RANKS < MIN_RANKS )); then
  echo "[rank-sweep] fail: require max-ranks >= min-ranks" >&2
  exit 2
fi
if (( MAX_RANKS > 10 )); then
  echo "[rank-sweep] fail: max-ranks cannot exceed 10 (worker1..worker10)" >&2
  exit 2
fi
if (( WARMUP >= ITERS )); then
  echo "[rank-sweep] fail: require --iters > --warmup (got ${ITERS} <= ${WARMUP})" >&2
  exit 2
fi

if [[ ! -f "${COMPOSE_FILE}" ]]; then
  echo "[rank-sweep] fail: compose file not found: ${COMPOSE_FILE}" >&2
  exit 1
fi

detect_compose
COMPOSE_ARGS=(-f "${COMPOSE_FILE}")
if [[ -n "${DOCKER_PROJECT}" ]]; then
  COMPOSE_ARGS+=(-p "${DOCKER_PROJECT}")
fi

if (( START_STACK == 1 )); then
  run_cmd "${COMPOSE_BIN[@]}" "${COMPOSE_ARGS[@]}" up -d \
    launcher worker1 worker2 worker3 worker4 worker5 worker6 worker7 worker8 worker9 worker10
fi

cleanup() {
  if (( KEEP_UP == 1 || DRY_RUN == 1 )); then
    return 0
  fi
  "${COMPOSE_BIN[@]}" "${COMPOSE_ARGS[@]}" down >/dev/null 2>&1 || true
}
trap cleanup EXIT

mkdir -p "${OUT_ROOT}"
echo "[rank-sweep] range=[${MIN_RANKS},${MAX_RANKS}] (inclusive)"

for n in $(seq "${MIN_RANKS}" "${MAX_RANKS}"); do
  hf="/tmp/hosts-${n}.txt"
  : > "${hf}"
  for i in $(seq 1 "${n}"); do
    echo "worker${i}" >> "${hf}"
  done

  cmd=(
    "${ROOT_DIR}/scripts/run_multihost_scenarios.sh"
    --docker-sim
    --hostfile "${hf}"
    --scenarios "${SCENARIOS}"
    --scenario-kind "${SCENARIO_KIND}"
    --iface "${IFACE}"
    --modes "${MODES}"
    --trials "${TRIALS}"
    --threads "${THREADS}"
    --n-local "${N_LOCAL}"
    --halo "${HALO}"
    --iters "${ITERS}"
    --warmup "${WARMUP}"
    --transport "${TRANSPORT}"
    --out-root "${OUT_ROOT}/ranks-${n}"
    --docker-compose-file "${COMPOSE_FILE}"
  )
  if [[ -n "${DOCKER_PROJECT}" ]]; then
    cmd+=(--docker-project "${DOCKER_PROJECT}")
  fi
  run_cmd "${cmd[@]}"
done

echo "[rank-sweep] complete: ${OUT_ROOT}"
