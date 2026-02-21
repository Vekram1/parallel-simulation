#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

HOSTFILE=""
SCENARIOS="lan_clean,wan_mild,constrained_50"
SCENARIO_KIND="netem"
MODES="phase_blk,phase_nb,nb_test,phase_persist,omp_tasks"
TRIALS=10
THREADS=1
N_LOCAL=4096
HALO=8
ITERS=60
WARMUP=10
TRANSPORT="tcp"
OUT_ROOT="${ROOT_DIR}/runs/multihost"
BINARY="${ROOT_DIR}/build/phasegap"
SSH_USER=""
SSH_OPTS="-o BatchMode=yes -o ConnectTimeout=5"
NETEM_IFACE="eth0"
APPLY_SCENARIO=1
RESTORE_SCENARIO=1
DRY_RUN=0
DOCKER_SIM=0
DOCKER_COMPOSE_FILE="${ROOT_DIR}/docker/docker-compose.mpi.yml"
DOCKER_PROJECT=""
DOCKER_LAUNCHER_SERVICE="launcher"
LAUNCHER_WORKDIR="/work"
MPIRUN_HOSTFILE=""

usage() {
  cat <<'USAGE'
Usage: scripts/run_multihost_scenarios.sh --hostfile <path> [options]

Run N-hosts=N-ranks PhaseGap sweeps across multiple network scenarios.
Each scenario is applied across hosts, then all modes/trials are executed.

Required:
  --hostfile <path>           MPI hostfile with one host per line (slots recommended: 1)

Options:
  --scenarios <csv>           Scenario labels/presets (default: lan_clean,wan_mild,constrained_50)
  --scenario-kind <kind>      Scenario backend: netem|tcpcc|label (default: netem)
  --iface <name>              Network interface for netem (default: eth0)
  --modes <csv>               Modes to run (default: phase_blk,phase_nb,nb_test,phase_persist,omp_tasks)
  --trials <N>                Trials per (scenario,mode) (default: 10)
  --threads <T>               OMP threads per rank (default: 1)
  --n-local <N>               --N local points per rank (default: 4096)
  --halo <H>                  --halo width (default: 8)
  --iters <K>                 --iters (default: 60)
  --warmup <W>                --warmup (default: 10)
  --transport <name>          --transport value (default: tcp)
  --out-root <dir>            Output root (default: runs/multihost)
  --binary <path>             PhaseGap binary (default: ./build/phasegap)
  --ssh-user <user>           Optional SSH user for remote commands
  --ssh-opts "<opts>"         Extra SSH options
  --docker-sim                Use docker compose backend (workerN + launcher)
  --docker-compose-file <p>   Compose file for --docker-sim
  --docker-project <name>     Compose project name for --docker-sim
  --docker-launcher <name>    Launcher service name (default: launcher)
  --launcher-workdir <path>   Workdir inside launcher (default: /work)
  --no-apply-scenario         Skip scenario apply; run scenarios as labels only
  --no-restore-scenario       Do not restore host state after sweep
  --no-apply-tcp-cc           Deprecated alias for --no-apply-scenario
  --no-restore-tcp-cc         Deprecated alias for --no-restore-scenario
  --dry-run                   Print commands without executing
  -h, --help                  Show this help

Notes:
  - Script enforces N hosts == N ranks.
  - netem mode applies preset via scripts/netem_on.sh on each host.
  - tcpcc mode applies:
      sudo sysctl -w net.ipv4.tcp_congestion_control=<scenario>
USAGE
}

require_option_value() {
  local opt="$1"
  if [[ $# -lt 2 || -z "${2:-}" || "${2:-}" == --* ]]; then
    echo "[multihost] fail: ${opt} requires a value" >&2
    usage >&2
    exit 2
  fi
}

require_pos_int() {
  local opt="$1"
  local value="$2"
  if [[ ! "${value}" =~ ^[1-9][0-9]*$ ]]; then
    echo "[multihost] fail: ${opt} must be a positive integer (got '${value}')" >&2
    exit 2
  fi
}

trim() {
  local s="$1"
  s="${s#"${s%%[![:space:]]*}"}"
  s="${s%"${s##*[![:space:]]}"}"
  printf '%s' "${s}"
}

require_scenario_kind() {
  case "$1" in
    netem|tcpcc|label) return 0 ;;
    *)
      echo "[multihost] fail: --scenario-kind must be one of netem|tcpcc|label (got '$1')" >&2
      exit 2
      ;;
  esac
}

run_cmd() {
  echo "[multihost] $*"
  if (( DRY_RUN == 1 )); then
    return 0
  fi
  "$@"
}

run_remote() {
  local host="$1"
  local cmd="$2"
  if (( DOCKER_SIM == 1 )); then
    echo "[multihost] docker exec ${host} ${cmd}"
    if (( DRY_RUN == 1 )); then
      return 0
    fi
    "${COMPOSE_BIN[@]}" exec -T "${host}" sh -lc "${cmd}"
    return 0
  fi
  local target="${host}"
  if [[ -n "${SSH_USER}" ]]; then
    target="${SSH_USER}@${host}"
  fi
  echo "[multihost] ssh ${target} ${cmd}"
  if (( DRY_RUN == 1 )); then
    return 0
  fi
  ssh ${SSH_OPTS} "${target}" "${cmd}"
}

run_launcher_cmd() {
  local cmd_escaped
  cmd_escaped="$(printf '%q ' "$@")"
  if (( DOCKER_SIM == 1 )); then
    echo "[multihost] docker exec ${DOCKER_LAUNCHER_SERVICE}: cd ${LAUNCHER_WORKDIR} && ${cmd_escaped}"
    if (( DRY_RUN == 1 )); then
      return 0
    fi
    "${COMPOSE_BIN[@]}" exec -T "${DOCKER_LAUNCHER_SERVICE}" bash -lc "cd ${LAUNCHER_WORKDIR} && ${cmd_escaped}"
    return 0
  fi
  run_cmd "$@"
}

map_to_launcher_path() {
  local p="$1"
  if [[ "${p}" == /* ]]; then
    if [[ "${p}" == "${ROOT_DIR}"* ]]; then
      local rel="${p#${ROOT_DIR}/}"
      printf '%s/%s' "${LAUNCHER_WORKDIR}" "${rel}"
      return 0
    fi
    printf '%s' "${p}"
    return 0
  fi
  printf '%s/%s' "${LAUNCHER_WORKDIR}" "${p}"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --hostfile)
      require_option_value "$1" "${2:-}"
      HOSTFILE="$2"
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
      NETEM_IFACE="$2"
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
    --binary)
      require_option_value "$1" "${2:-}"
      BINARY="$2"
      shift 2
      ;;
    --ssh-user)
      require_option_value "$1" "${2:-}"
      SSH_USER="$2"
      shift 2
      ;;
    --ssh-opts)
      require_option_value "$1" "${2:-}"
      SSH_OPTS="$2"
      shift 2
      ;;
    --docker-sim)
      DOCKER_SIM=1
      shift
      ;;
    --docker-compose-file)
      require_option_value "$1" "${2:-}"
      DOCKER_COMPOSE_FILE="$2"
      shift 2
      ;;
    --docker-project)
      require_option_value "$1" "${2:-}"
      DOCKER_PROJECT="$2"
      shift 2
      ;;
    --docker-launcher)
      require_option_value "$1" "${2:-}"
      DOCKER_LAUNCHER_SERVICE="$2"
      shift 2
      ;;
    --launcher-workdir)
      require_option_value "$1" "${2:-}"
      LAUNCHER_WORKDIR="$2"
      shift 2
      ;;
    --no-apply-scenario)
      APPLY_SCENARIO=0
      shift
      ;;
    --no-restore-scenario)
      RESTORE_SCENARIO=0
      shift
      ;;
    --no-apply-tcp-cc)
      APPLY_SCENARIO=0
      shift
      ;;
    --no-restore-tcp-cc)
      RESTORE_SCENARIO=0
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
      echo "[multihost] fail: unknown option '$1'" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if [[ -z "${HOSTFILE}" ]]; then
  echo "[multihost] fail: --hostfile is required" >&2
  usage >&2
  exit 2
fi
if [[ ! -f "${HOSTFILE}" ]]; then
  echo "[multihost] fail: hostfile not found: ${HOSTFILE}" >&2
  exit 1
fi
if [[ ! -x "${BINARY}" ]]; then
  if (( DOCKER_SIM == 0 )); then
    echo "[multihost] fail: binary not executable: ${BINARY}" >&2
    exit 1
  fi
fi

require_pos_int "--trials" "${TRIALS}"
require_pos_int "--threads" "${THREADS}"
require_pos_int "--n-local" "${N_LOCAL}"
require_pos_int "--halo" "${HALO}"
require_pos_int "--iters" "${ITERS}"
require_pos_int "--warmup" "${WARMUP}"
require_scenario_kind "${SCENARIO_KIND}"
if (( WARMUP >= ITERS )); then
  echo "[multihost] fail: require --iters > --warmup (got ${ITERS} <= ${WARMUP})" >&2
  exit 2
fi

HOSTS=()
while IFS= read -r host; do
  host="$(trim "${host}")"
  if [[ -z "${host}" || "${host}" == \#* ]]; then
    continue
  fi
  set -- ${host}
  HOSTS+=("$1")
done < "${HOSTFILE}"

if [[ "${#HOSTS[@]}" -eq 0 ]]; then
  echo "[multihost] fail: hostfile contains no hosts" >&2
  exit 1
fi

NP="${#HOSTS[@]}"

if (( DOCKER_SIM == 1 )); then
  if (( DRY_RUN == 0 )); then
    if ! command -v docker >/dev/null 2>&1; then
      echo "[multihost] fail: docker not found for --docker-sim" >&2
      exit 1
    fi
    if ! command -v ssh-keygen >/dev/null 2>&1; then
      echo "[multihost] fail: ssh-keygen not found for --docker-sim" >&2
      exit 1
    fi
  fi
  COMPOSE_BIN=()
  if command -v docker >/dev/null 2>&1 && docker compose version >/dev/null 2>&1; then
    COMPOSE_BIN=(docker compose)
  elif command -v docker-compose >/dev/null 2>&1; then
    COMPOSE_BIN=(docker-compose)
  else
    echo "[multihost] fail: docker compose not found for --docker-sim" >&2
    exit 1
  fi
  if [[ ! -f "${DOCKER_COMPOSE_FILE}" ]]; then
    echo "[multihost] fail: compose file not found: ${DOCKER_COMPOSE_FILE}" >&2
    exit 1
  fi
  COMPOSE_BIN+=(-f "${DOCKER_COMPOSE_FILE}")
  if [[ -n "${DOCKER_PROJECT}" ]]; then
    COMPOSE_BIN+=(-p "${DOCKER_PROJECT}")
  fi
  if (( DRY_RUN == 0 )); then
    running_services="$("${COMPOSE_BIN[@]}" ps --services --status running)"
    if ! printf '%s\n' "${running_services}" | grep -Fxq "${DOCKER_LAUNCHER_SERVICE}"; then
      echo "[multihost] fail: launcher service '${DOCKER_LAUNCHER_SERVICE}' is not running" >&2
      exit 1
    fi
    for host in "${HOSTS[@]}"; do
      if ! printf '%s\n' "${running_services}" | grep -Fxq "${host}"; then
        echo "[multihost] fail: worker service '${host}' is not running" >&2
        exit 1
      fi
    done
  fi

  default_host_binary="${ROOT_DIR}/build/phasegap"
  if [[ "${BINARY}" == "${default_host_binary}" ]]; then
    BINARY="${LAUNCHER_WORKDIR}/build-docker-compose/phasegap"
  else
    BINARY="$(map_to_launcher_path "${BINARY}")"
  fi

  if [[ "${OUT_ROOT}" == /* && "${OUT_ROOT}" != "${ROOT_DIR}"* ]]; then
    echo "[multihost] warn: out-root '${OUT_ROOT}' is outside repo; launcher may not see this path" >&2
  fi

  mkdir -p "${ROOT_DIR}/.docker-mpi"
  if [[ ! -f "${ROOT_DIR}/.docker-mpi/id_ed25519" ]]; then
    run_cmd ssh-keygen -t ed25519 -N '' -f "${ROOT_DIR}/.docker-mpi/id_ed25519"
  fi
  run_cmd cp "${ROOT_DIR}/.docker-mpi/id_ed25519.pub" "${ROOT_DIR}/.docker-mpi/authorized_keys"
  run_cmd chmod 600 "${ROOT_DIR}/.docker-mpi/id_ed25519" "${ROOT_DIR}/.docker-mpi/authorized_keys"

  if (( DRY_RUN == 0 )); then
    for host in "${HOSTS[@]}"; do
      "${COMPOSE_BIN[@]}" exec -T "${host}" sh -lc \
        "mkdir -p /root/.ssh && chmod 700 /root/.ssh && cp /docker-mpi/authorized_keys /root/.ssh/authorized_keys && chmod 600 /root/.ssh/authorized_keys"
    done
    cp "${HOSTFILE}" "${ROOT_DIR}/.docker-mpi/hostfile.runtime"
  fi
  MPIRUN_HOSTFILE="/docker-mpi/hostfile.runtime"
else
  MPIRUN_HOSTFILE="${HOSTFILE}"
fi

if (( DOCKER_SIM == 0 )) && [[ "${SCENARIO_KIND}" == "netem" ]] && (( APPLY_SCENARIO == 1 )); then
  echo "[multihost] fail: netem scenario apply currently supports --docker-sim only" >&2
  echo "[multihost] hint: use --scenario-kind label --no-apply-scenario for external hosts" >&2
  exit 2
fi

IFS=',' read -r -a SCENARIO_ARR <<< "${SCENARIOS}"
IFS=',' read -r -a MODE_ARR <<< "${MODES}"

for i in "${!SCENARIO_ARR[@]}"; do SCENARIO_ARR[$i]="$(trim "${SCENARIO_ARR[$i]}")"; done
for i in "${!MODE_ARR[@]}"; do MODE_ARR[$i]="$(trim "${MODE_ARR[$i]}")"; done

if [[ "${#SCENARIO_ARR[@]}" -eq 0 || -z "${SCENARIO_ARR[0]}" ]]; then
  echo "[multihost] fail: no scenarios provided" >&2
  exit 2
fi
if [[ "${#MODE_ARR[@]}" -eq 0 || -z "${MODE_ARR[0]}" ]]; then
  echo "[multihost] fail: no modes provided" >&2
  exit 2
fi

STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
RUN_ROOT="${OUT_ROOT}/scenarios-${SCENARIO_KIND}-${STAMP}"
INDEX_CSV="${RUN_ROOT}/index.csv"
SUMMARY_CSV="${RUN_ROOT}/summary.csv"

if (( DRY_RUN == 0 )); then
  mkdir -p "${RUN_ROOT}"
  echo "timestamp,scenario_kind,scenario,mode,trial,hosts,ranks,threads,status,run_dir" > "${INDEX_CSV}"
  echo "scenario_kind,scenario,mode,trial,t_iter_us,t_wait_us,t_interior_us,t_boundary_us,wait_frac,overlap_ratio,csv_path" > "${SUMMARY_CSV}"
fi

ORIG_CC_HOSTS=()
ORIG_CC_VALUES=()
if [[ "${SCENARIO_KIND}" == "tcpcc" ]] && (( APPLY_SCENARIO == 1 )); then
  for host in "${HOSTS[@]}"; do
    current_cmd="sysctl -n net.ipv4.tcp_congestion_control"
    target="${host}"
    if (( DOCKER_SIM == 0 )) && [[ -n "${SSH_USER}" ]]; then
      target="${SSH_USER}@${host}"
    fi
    echo "[multihost] read current tcp_cc from ${target}"
    if (( DRY_RUN == 1 )); then
      ORIG_CC_HOSTS+=("${host}")
      ORIG_CC_VALUES+=("unknown")
    else
      if (( DOCKER_SIM == 1 )); then
        cc_value="$("${COMPOSE_BIN[@]}" exec -T "${host}" sh -lc "${current_cmd}" | tr -d '\r' | tr -d '\n')"
      else
        cc_value="$(ssh ${SSH_OPTS} "${target}" "${current_cmd}" | tr -d '\r' | tr -d '\n')"
      fi
      if [[ -z "${cc_value}" ]]; then
        echo "[multihost] fail: unable to read tcp congestion control on ${target}" >&2
        exit 1
      fi
      ORIG_CC_HOSTS+=("${host}")
      ORIG_CC_VALUES+=("${cc_value}")
    fi
  done
fi

restore_scenarios() {
  if (( APPLY_SCENARIO == 0 || RESTORE_SCENARIO == 0 )); then
    return 0
  fi
  if [[ "${SCENARIO_KIND}" == "tcpcc" ]]; then
    for i in "${!ORIG_CC_HOSTS[@]}"; do
      host="${ORIG_CC_HOSTS[$i]}"
      local_cc="${ORIG_CC_VALUES[$i]}"
      if [[ -z "${local_cc}" || "${local_cc}" == "unknown" ]]; then
        continue
      fi
      if (( DOCKER_SIM == 1 )); then
        run_remote "${host}" "sysctl -w net.ipv4.tcp_congestion_control=${local_cc} >/dev/null"
      else
        run_remote "${host}" "sudo sysctl -w net.ipv4.tcp_congestion_control=${local_cc} >/dev/null"
      fi
    done
    return 0
  fi
  if [[ "${SCENARIO_KIND}" == "netem" ]]; then
    for host in "${HOSTS[@]}"; do
      if (( DOCKER_SIM == 1 )); then
        run_remote "${host}" "cd /work && scripts/netem_off.sh --iface ${NETEM_IFACE} --quiet || true"
      else
        run_remote "${host}" "sudo tc qdisc del dev ${NETEM_IFACE} root >/dev/null 2>&1 || true"
      fi
    done
    return 0
  fi
}

trap restore_scenarios EXIT

echo "[multihost] hostfile=${HOSTFILE}"
if (( DOCKER_SIM == 1 )); then
  echo "[multihost] hostfile_in_launcher=${MPIRUN_HOSTFILE}"
fi
echo "[multihost] hosts=${#HOSTS[@]} ranks=${NP} threads=${THREADS}"
echo "[multihost] scenario_kind=${SCENARIO_KIND}"
echo "[multihost] scenarios=${SCENARIOS}"
echo "[multihost] modes=${MODES}"
echo "[multihost] run_root=${RUN_ROOT}"
if (( DOCKER_SIM == 1 )); then
  echo "[multihost] backend=docker-sim compose_file=${DOCKER_COMPOSE_FILE} launcher=${DOCKER_LAUNCHER_SERVICE}"
fi

for scenario in "${SCENARIO_ARR[@]}"; do
  if [[ -z "${scenario}" ]]; then
    continue
  fi
  if (( APPLY_SCENARIO == 1 )); then
    for host in "${HOSTS[@]}"; do
      if [[ "${SCENARIO_KIND}" == "netem" ]]; then
        if (( DOCKER_SIM == 1 )); then
          run_remote "${host}" "cd /work && scripts/netem_on.sh --iface ${NETEM_IFACE} --preset ${scenario}"
        else
          run_remote "${host}" "echo '[multihost] fail: netem scenario apply requires docker-sim in this script' >&2; exit 1"
        fi
      elif [[ "${SCENARIO_KIND}" == "tcpcc" ]]; then
        if (( DOCKER_SIM == 1 )); then
          run_remote "${host}" "sysctl -w net.ipv4.tcp_congestion_control=${scenario} >/dev/null"
        else
          run_remote "${host}" "sudo sysctl -w net.ipv4.tcp_congestion_control=${scenario} >/dev/null"
        fi
      fi
    done
  fi

  for mode in "${MODE_ARR[@]}"; do
    if [[ -z "${mode}" ]]; then
      continue
    fi
    for trial in $(seq 1 "${TRIALS}"); do
      run_dir="${RUN_ROOT}/${scenario}/${mode}/trial${trial}"
      csv_path="${run_dir}/results.csv"
      exec_run_dir="${run_dir}"
      exec_csv_path="${csv_path}"
      if (( DOCKER_SIM == 1 )); then
        exec_run_dir="$(map_to_launcher_path "${run_dir}")"
        exec_csv_path="$(map_to_launcher_path "${csv_path}")"
      fi
      mkdir_cmd=(mkdir -p "${run_dir}")
      run_cmd "${mkdir_cmd[@]}"

      ts="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
      cmd=(
        env
        OMP_NUM_THREADS="${THREADS}"
        OMP_PROC_BIND=true
        OMP_PLACES=cores
      )
      if (( DOCKER_SIM == 1 )); then
        cmd+=(
          OMPI_ALLOW_RUN_AS_ROOT=1
          OMPI_ALLOW_RUN_AS_ROOT_CONFIRM=1
        )
      fi
      cmd+=(
        mpirun
        --hostfile "${MPIRUN_HOSTFILE}"
        -np "${NP}"
      )
      if (( DOCKER_SIM == 1 )); then
        cmd+=(
          --mca plm_rsh_args "-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -i /docker-mpi/id_ed25519"
        )
      fi
      cmd+=(
        "${BINARY}"
        --mode "${mode}"
        --ranks "${NP}"
        --threads "${THREADS}"
        --N "${N_LOCAL}"
        --halo "${HALO}"
        --iters "${ITERS}"
        --warmup "${WARMUP}"
        --transport "${TRANSPORT}"
        --trace 0
        --out_dir "${exec_run_dir}"
        --csv "${exec_csv_path}"
        --manifest 1
      )

      status="ok"
      if ! run_launcher_cmd "${cmd[@]}"; then
        status="fail"
      fi

      if (( DRY_RUN == 0 )); then
        echo "${ts},${SCENARIO_KIND},${scenario},${mode},${trial},${NP},${NP},${THREADS},${status},${run_dir}" >> "${INDEX_CSV}"
        if [[ "${status}" == "ok" && -f "${csv_path}" ]]; then
          row="$(tail -n 1 "${csv_path}")"
          python3 - "${SCENARIO_KIND}" "${scenario}" "${mode}" "${trial}" "${csv_path}" "${SUMMARY_CSV}" <<'PY'
import csv
import sys
scenario_kind, scenario, mode, trial, csv_path, summary = sys.argv[1:]
with open(csv_path, newline='') as f:
    rows = list(csv.DictReader(f))
if not rows:
    sys.exit(0)
r = rows[-1]
with open(summary, "a", newline="") as f:
    w = csv.writer(f)
    w.writerow([
        scenario_kind, scenario, mode, trial,
        r.get("t_iter_us", ""),
        r.get("t_wait_us", ""),
        r.get("t_interior_us", ""),
        r.get("t_boundary_us", ""),
        r.get("wait_frac", ""),
        r.get("overlap_ratio", ""),
        csv_path,
    ])
PY
          echo "[multihost] ok scenario=${scenario} mode=${mode} trial=${trial}"
        else
          echo "[multihost] fail scenario=${scenario} mode=${mode} trial=${trial}"
        fi
      fi
    done
  done
done

echo "[multihost] complete: ${RUN_ROOT}"
if (( DRY_RUN == 0 )); then
  echo "[multihost] wrote index: ${INDEX_CSV}"
  echo "[multihost] wrote summary: ${SUMMARY_CSV}"
fi
