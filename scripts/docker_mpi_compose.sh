#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
COMPOSE_FILE="${ROOT_DIR}/docker/docker-compose.mpi.yml"
DOCKER_MPI_DIR="${ROOT_DIR}/.docker-mpi"
IMAGE_TAG="phasegap-netem:latest"
IFACE="eth0"
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
MODE="phase_nb"
TRANSPORT="tcp"
BUILD_DIR="/work/build-docker-compose"
RUN_DIR="/work/runs/docker-mpi-compose"
NO_IMAGE_BUILD=0
NO_NETEM=0
KEEP_UP=0
DRY_RUN=0
MAX_WORKERS=10

usage() {
  cat <<'USAGE'
Usage: scripts/docker_mpi_compose.sh [options]

Runs PhaseGap across multiple Docker containers (launcher + worker1..worker10)
using OpenMPI over SSH, with optional netem shaping on worker links.
Worker selection rule: first N workers are used and hostfile is emitted
with one slot per worker, enforcing N hosts = N ranks.

Options:
  --image <tag>           Docker image tag (default: phasegap-netem:latest)
  --np <P>                MPI ranks (default: 2)
  --threads <T>           OMP threads per rank (default: 2)
  --slots-per-worker <S>  Deprecated (ignored; hostfile always uses slots=1)
  --n-local <N>           Local points per rank (default: 256)
  --halo <H>              Halo width (default: 8)
  --iters <K>             Iterations (default: 8)
  --warmup <W>            Warmup (default: 2)
  --mode <name>           Mode (default: phase_nb)
  --transport <name>      Transport hint (default: tcp)
  --iface <name>          Worker interface for netem (default: eth0)
  --preset <name>         Netem preset (default: wan_mild)
  --delay-ms <ms>         Netem delay override (default: 20)
  --jitter-ms <ms>        Netem jitter override (default: 5)
  --loss-pct <pct>        Netem loss override (default: 0.20)
  --rate <rate>           Optional netem rate cap (example: 50mbit)
  --no-netem              Disable netem shaping
  --no-image-build        Skip image build and reuse existing image
  --keep-up               Keep compose stack running after completion
  --dry-run               Print actions only
  -h, --help              Show help
USAGE
}

require_pos_int() {
  local opt="$1"
  local value="$2"
  if [[ ! "${value}" =~ ^[1-9][0-9]*$ ]]; then
    echo "[docker-mpi] fail: ${opt} must be a positive integer (got '${value}')" >&2
    exit 2
  fi
}

require_cmd() {
  local cmd="$1"
  if ! command -v "${cmd}" >/dev/null 2>&1; then
    echo "[docker-mpi] fail: required command '${cmd}' not found" >&2
    exit 1
  fi
}

require_option_value() {
  local opt="$1"
  if [[ $# -lt 2 || -z "${2:-}" || "${2:-}" == --* ]]; then
    echo "[docker-mpi] fail: ${opt} requires a value" >&2
    usage >&2
    exit 2
  fi
}

run_cmd() {
  echo "[docker-mpi] $*"
  if (( DRY_RUN == 1 )); then
    return 0
  fi
  "$@"
}

compose_cmd() {
  if command -v docker >/dev/null 2>&1 && docker compose version >/dev/null 2>&1; then
    echo "docker compose"
    return
  fi
  if command -v docker-compose >/dev/null 2>&1; then
    echo "docker-compose"
    return
  fi
  echo ""
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --image)
      require_option_value "$1" "${2:-}"
      IMAGE_TAG="$2"
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
    --slots-per-worker)
      require_option_value "$1" "${2:-}"
      echo "[docker-mpi] warn: --slots-per-worker is deprecated and ignored (using slots=1 per worker)" >&2
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
    --mode)
      require_option_value "$1" "${2:-}"
      MODE="$2"
      shift 2
      ;;
    --transport)
      require_option_value "$1" "${2:-}"
      TRANSPORT="$2"
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
    --no-netem)
      NO_NETEM=1
      shift
      ;;
    --no-image-build)
      NO_IMAGE_BUILD=1
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
      echo "[docker-mpi] unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if (( ITERS <= WARMUP )); then
  echo "[docker-mpi] fail: require --iters > --warmup (got ${ITERS} <= ${WARMUP})" >&2
  exit 2
fi
require_pos_int "--np" "${NP}"
require_pos_int "--threads" "${THREADS}"
require_pos_int "--n-local" "${N_LOCAL}"
require_pos_int "--halo" "${HALO}"
require_pos_int "--iters" "${ITERS}"
require_pos_int "--warmup" "${WARMUP}"
if (( NP > MAX_WORKERS )); then
  echo "[docker-mpi] fail: np=${NP} exceeds supported worker count=${MAX_WORKERS}" >&2
  exit 2
fi

ACTIVE_WORKERS=()
for i in $(seq 1 "${NP}"); do
  ACTIVE_WORKERS+=("worker${i}")
done

if (( DRY_RUN == 0 )); then
  require_cmd docker
  require_cmd ssh-keygen
fi

if [[ ! -f "${COMPOSE_FILE}" ]]; then
  echo "[docker-mpi] fail: compose file not found at ${COMPOSE_FILE}" >&2
  exit 1
fi

COMPOSE_BIN="$(compose_cmd)"
if [[ -z "${COMPOSE_BIN}" ]]; then
  if (( DRY_RUN == 1 )); then
    COMPOSE_BIN="docker compose"
  else
    echo "[docker-mpi] fail: docker compose not found (need 'docker compose' or 'docker-compose')" >&2
    exit 1
  fi
fi

if (( DRY_RUN == 1 )); then
  echo "[docker-mpi] dry-run: would ensure key/hostfile state under ${DOCKER_MPI_DIR}"
  echo "[docker-mpi] dry-run: hostfile => ${ACTIVE_WORKERS[*]} (slots=1 each)"
else
  mkdir -p "${DOCKER_MPI_DIR}"
  if [[ ! -f "${DOCKER_MPI_DIR}/id_ed25519" ]]; then
    run_cmd ssh-keygen -t ed25519 -N '' -f "${DOCKER_MPI_DIR}/id_ed25519"
  fi
  run_cmd cp "${DOCKER_MPI_DIR}/id_ed25519.pub" "${DOCKER_MPI_DIR}/authorized_keys"
  run_cmd chmod 600 "${DOCKER_MPI_DIR}/id_ed25519" "${DOCKER_MPI_DIR}/authorized_keys"

  : > "${DOCKER_MPI_DIR}/hostfile"
  for w in "${ACTIVE_WORKERS[@]}"; do
    echo "${w} slots=1" >> "${DOCKER_MPI_DIR}/hostfile"
  done
fi

export PHASEGAP_IMAGE="${IMAGE_TAG}"

if (( NO_IMAGE_BUILD == 0 )); then
  run_cmd docker build -f "${ROOT_DIR}/docker/Dockerfile.phasegap" -t "${IMAGE_TAG}" "${ROOT_DIR}"
fi

cleanup() {
  if (( DRY_RUN == 1 )); then
    return
  fi
  if (( NO_NETEM == 0 )); then
    for w in "${ACTIVE_WORKERS[@]}"; do
      ${COMPOSE_BIN} -f "${COMPOSE_FILE}" exec -T "${w}" bash -lc "cd /work && scripts/netem_off.sh --iface ${IFACE} --quiet || true" >/dev/null 2>&1 || true
    done
  fi
  if (( KEEP_UP == 0 )); then
    ${COMPOSE_BIN} -f "${COMPOSE_FILE}" down >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

run_cmd ${COMPOSE_BIN} -f "${COMPOSE_FILE}" up -d "${ACTIVE_WORKERS[@]}" launcher

SSH_WAIT="for h in ${ACTIVE_WORKERS[*]}; do
  until ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -i /docker-mpi/id_ed25519 root@\"\${h}\" true >/dev/null 2>&1; do sleep 1; done
done"
run_cmd ${COMPOSE_BIN} -f "${COMPOSE_FILE}" exec -T launcher bash -lc "${SSH_WAIT}"

if (( NO_NETEM == 0 )); then
  NETEM_CMD="scripts/netem_on.sh --iface ${IFACE} --preset ${PRESET} --delay-ms ${DELAY_MS} --jitter-ms ${JITTER_MS} --loss-pct ${LOSS_PCT}"
  if [[ -n "${RATE}" ]]; then
    NETEM_CMD+=" --rate ${RATE}"
  fi
  for w in "${ACTIVE_WORKERS[@]}"; do
    run_cmd ${COMPOSE_BIN} -f "${COMPOSE_FILE}" exec -T "${w}" bash -lc "cd /work && ${NETEM_CMD}"
  done
fi

MPI_RUN_CMD="
set -euo pipefail
cd /work
cmake -S /work -B ${BUILD_DIR} -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build ${BUILD_DIR}
mkdir -p ${RUN_DIR}
export OMP_NUM_THREADS=${THREADS}
export OMPI_ALLOW_RUN_AS_ROOT=1
export OMPI_ALLOW_RUN_AS_ROOT_CONFIRM=1
mpirun --hostfile /docker-mpi/hostfile -np ${NP} \
  --mca plm_rsh_args '-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -i /docker-mpi/id_ed25519' \
  --mca btl tcp,self \
  --mca btl_tcp_if_include ${IFACE} \
  ${BUILD_DIR}/phasegap \
  --mode ${MODE} \
  --transport ${TRANSPORT} \
  --ranks ${NP} \
  --threads ${THREADS} \
  --N ${N_LOCAL} \
  --halo ${HALO} \
  --iters ${ITERS} \
  --warmup ${WARMUP} \
  --trace 1 \
  --trace_iters 3 \
  --out_dir ${RUN_DIR} \
  --csv ${RUN_DIR}/results.csv \
  --manifest 1
"

run_cmd ${COMPOSE_BIN} -f "${COMPOSE_FILE}" exec -T launcher bash -lc "${MPI_RUN_CMD}"

echo "[docker-mpi] complete; artifacts in runs/docker-mpi-compose"
