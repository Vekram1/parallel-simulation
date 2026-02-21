#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

require_cmd() {
  local cmd="$1"
  if ! command -v "${cmd}" >/dev/null 2>&1; then
    echo "[tui] fail: missing required command '${cmd}'" >&2
    exit 1
  fi
}

prompt_default() {
  local prompt="$1"
  local default="$2"
  local value
  read -r -p "${prompt} [${default}]: " value || true
  if [[ -z "${value}" ]]; then
    printf '%s' "${default}"
  else
    printf '%s' "${value}"
  fi
}

trim_whitespace() {
  local s="$1"
  s="${s#"${s%%[![:space:]]*}"}"
  s="${s%"${s##*[![:space:]]}"}"
  printf '%s' "${s}"
}

validate_by_type() {
  local value="$1"
  local type="$2"
  shift 2
  case "${type}" in
    pos_int)
      [[ "${value}" =~ ^[0-9]+$ ]] && (( value > 0 ))
      return
      ;;
    nonneg_int)
      [[ "${value}" =~ ^[0-9]+$ ]]
      return
      ;;
    bool)
      case "${value}" in
        0|1|true|false) return 0 ;;
      esac
      return 1
      ;;
    enum)
      local option
      for option in "$@"; do
        if [[ "${value}" == "${option}" ]]; then
          return 0
        fi
      done
      return 1
      ;;
    text)
      return 0
      ;;
    *)
      return 1
      ;;
  esac
}

prompt_value() {
  local prompt="$1"
  local default="$2"
  local required="$3"
  local type="$4"
  shift 4
  local -a options=("$@")
  local suffix
  if [[ "${required}" == "1" ]]; then
    suffix="[${default}]"
  else
    if [[ -n "${default}" ]]; then
      suffix="[${default}, optional]"
    else
      suffix="[optional]"
    fi
  fi

  while true; do
    local input
    read -r -p "${prompt} ${suffix}: " input || true
    input="$(trim_whitespace "${input}")"
    if [[ -z "${input}" ]]; then
      if [[ -n "${default}" ]]; then
        input="${default}"
      elif [[ "${required}" == "0" ]]; then
        printf '%s' ""
        return
      else
        echo "[tui] value required"
        continue
      fi
    fi

    if validate_by_type "${input}" "${type}" "${options[@]-}"; then
      printf '%s' "${input}"
      return
    fi

    if [[ "${type}" == "enum" ]]; then
      echo "[tui] invalid value '${input}', allowed: ${options[*]}"
    else
      echo "[tui] invalid value '${input}' for ${type}"
    fi
  done
}

run_cmd() {
  echo
  echo "[tui] running: $*"
  "$@"
  echo "[tui] done"
  echo
}

show_header() {
  cat <<'HDR'
===============================================
        PhaseGap Guided Launcher (TUI)
===============================================
This launcher wraps common workflows so you don't need
long manual commands each time.
HDR
}

menu() {
  cat <<'MENU'
1) Smoke build + sanity run
2) Quality gate (strict artifacts)
3) Matrix dry-run preview
4) Matrix quick run (small sample)
5) Docker netem smoke (simulated network)
6) Docker multi-container MPI + netem run
7) Netem preset list
8) Show PhaseGap CLI options
9) PhaseGap custom CLI run (all options)
10) Exit
MENU
}

show_phasegap_cli_options() {
  cat <<'OPTS'
------------------------------------------------
PhaseGap CLI Options (from phasegap --help)
------------------------------------------------
Required:
  --mode <phase_nb|phase_blk|nb_test|phase_persist|omp_tasks>
  --ranks <P> --threads <T> --N <local_n> --halo <H> --iters <K> --warmup <W>

Optional:
  --kernel <stencil3|stencil5|axpy>
  --radius <R> --timesteps <S> --poll_every <q>
  --mpi_thread <funneled|serialized|multiple>
  --progress <inline_poll|progress_thread>
  --omp_schedule <static|dynamic|guided> --omp_chunk <c>
  --omp_bind <0|1|false|true> --omp_places <cores|threads>
  --flops_per_point <f> --bytes_per_point <b>
  --check <0|1> --check_every <E> --poison_ghost <0|1>
  --sync <none|barrier_start|barrier_each>
  --trace <0|1> --trace_iters <M> --trace_detail <rank|thread>
  --transport <auto|shm|tcp>
  --out_dir <dir> --run_id <id> --csv <file> --csv_mode <write|append> --manifest <0|1>

Notes:
  - In custom mode, optional prompts can be left blank to skip.
  - Pre-check enforced: halo >= radius * timesteps.
------------------------------------------------
OPTS
}

smoke_flow() {
  local np threads n_local halo iters warmup
  np="$(prompt_default "MPI ranks" "2")"
  threads="$(prompt_default "OMP threads" "2")"
  n_local="$(prompt_default "N local" "256")"
  halo="$(prompt_default "Halo" "8")"
  iters="$(prompt_default "Iters" "8")"
  warmup="$(prompt_default "Warmup" "2")"
  run_cmd "${ROOT_DIR}/scripts/smoke_build.sh" \
    --build-dir "${ROOT_DIR}/build" \
    --run-dir "${ROOT_DIR}/runs/smoke" \
    --np "${np}" \
    --threads "${threads}" \
    --n-local "${n_local}" \
    --halo "${halo}" \
    --iters "${iters}" \
    --warmup "${warmup}"
}

quality_gate_flow() {
  local np threads n_local halo iters warmup
  np="$(prompt_default "MPI ranks" "2")"
  threads="$(prompt_default "OMP threads" "2")"
  n_local="$(prompt_default "N local" "128")"
  halo="$(prompt_default "Halo" "4")"
  iters="$(prompt_default "Iters" "6")"
  warmup="$(prompt_default "Warmup" "2")"
  run_cmd "${ROOT_DIR}/scripts/quality_gate.sh" \
    --strict-artifacts \
    --build-dir "${ROOT_DIR}/build-quality-gate" \
    --run-dir "${ROOT_DIR}/runs/quality-gate" \
    --np "${np}" \
    --threads "${threads}" \
    --n-local "${n_local}" \
    --halo "${halo}" \
    --iters "${iters}" \
    --warmup "${warmup}"
}

matrix_dry_run_flow() {
  local sweeps max_runs core_budget
  sweeps="$(prompt_default "Sweeps (comma list from 1,2,3)" "1,2")"
  max_runs="$(prompt_default "Max runs" "8")"
  core_budget="$(prompt_default "Core budget" "8")"
  run_cmd "${ROOT_DIR}/scripts/run_matrix.sh" \
    --dry-run \
    --sweeps "${sweeps}" \
    --max-runs "${max_runs}" \
    --core-budget "${core_budget}" \
    --n-local 128 \
    --iters 6 \
    --warmup 2
}

matrix_quick_run_flow() {
  local sweeps max_runs core_budget
  sweeps="$(prompt_default "Sweeps (comma list from 1,2,3)" "1")"
  max_runs="$(prompt_default "Max runs" "4")"
  core_budget="$(prompt_default "Core budget" "8")"
  run_cmd "${ROOT_DIR}/scripts/run_matrix.sh" \
    --sweeps "${sweeps}" \
    --max-runs "${max_runs}" \
    --core-budget "${core_budget}" \
    --n-local 128 \
    --iters 6 \
    --warmup 2 \
    --trace 1 \
    --trace-iters 3
}

docker_netem_flow() {
  local np threads delay jitter loss rate
  np="$(prompt_default "MPI ranks" "2")"
  threads="$(prompt_default "OMP threads" "2")"
  delay="$(prompt_default "Delay ms" "20")"
  jitter="$(prompt_default "Jitter ms" "5")"
  loss="$(prompt_default "Loss pct" "0.2")"
  read -r -p "Rate cap (blank for none): " rate || true

  if [[ -n "${rate}" ]]; then
    run_cmd "${ROOT_DIR}/scripts/docker_netem_smoke.sh" \
      --np "${np}" \
      --threads "${threads}" \
      --delay-ms "${delay}" \
      --jitter-ms "${jitter}" \
      --loss-pct "${loss}" \
      --rate "${rate}"
  else
    run_cmd "${ROOT_DIR}/scripts/docker_netem_smoke.sh" \
      --np "${np}" \
      --threads "${threads}" \
      --delay-ms "${delay}" \
      --jitter-ms "${jitter}" \
      --loss-pct "${loss}"
  fi
}

docker_mpi_compose_flow() {
  local np threads delay jitter loss rate
  np="$(prompt_default "MPI ranks" "2")"
  threads="$(prompt_default "OMP threads" "2")"
  delay="$(prompt_default "Delay ms" "20")"
  jitter="$(prompt_default "Jitter ms" "5")"
  loss="$(prompt_default "Loss pct" "0.2")"
  read -r -p "Rate cap (blank for none): " rate || true

  if [[ -n "${rate}" ]]; then
    run_cmd "${ROOT_DIR}/scripts/docker_mpi_compose.sh" \
      --np "${np}" \
      --threads "${threads}" \
      --delay-ms "${delay}" \
      --jitter-ms "${jitter}" \
      --loss-pct "${loss}" \
      --rate "${rate}"
  else
    run_cmd "${ROOT_DIR}/scripts/docker_mpi_compose.sh" \
      --np "${np}" \
      --threads "${threads}" \
      --delay-ms "${delay}" \
      --jitter-ms "${jitter}" \
      --loss-pct "${loss}"
  fi
}

custom_phasegap_flow() {
  require_cmd mpirun
  local phasegap_bin mode ranks threads n_local halo iters warmup
  local kernel radius timesteps poll_every mpi_thread progress omp_schedule omp_chunk
  local omp_bind omp_places flops_per_point bytes_per_point check check_every poison_ghost
  local sync trace trace_iters trace_detail transport out_dir run_id csv csv_mode manifest
  local effective_kernel effective_radius effective_timesteps
  local -a cmd

  echo
  show_phasegap_cli_options

  phasegap_bin="$(prompt_value "PhaseGap binary path" "${ROOT_DIR}/build/phasegap" "1" "text")"
  if [[ ! -x "${phasegap_bin}" ]]; then
    echo "[tui] fail: binary is not executable: ${phasegap_bin}"
    return
  fi

  mode="$(prompt_value "Mode" "phase_nb" "1" "enum" \
    phase_nb phase_blk nb_test phase_persist omp_tasks)"
  ranks="$(prompt_value "MPI ranks" "2" "1" "pos_int")"
  threads="$(prompt_value "OMP threads" "2" "1" "pos_int")"
  n_local="$(prompt_value "N local (--N)" "256" "1" "pos_int")"
  halo="$(prompt_value "Halo (--halo)" "8" "1" "pos_int")"
  iters="$(prompt_value "Iters (--iters)" "8" "1" "pos_int")"
  warmup="$(prompt_value "Warmup (--warmup)" "2" "1" "nonneg_int")"
  if (( warmup >= iters )); then
    echo "[tui] fail: warmup must be less than iters (got warmup=${warmup}, iters=${iters})"
    return
  fi

  kernel="$(prompt_value "Kernel" "" "0" "enum" stencil3 stencil5 axpy)"
  radius="$(prompt_value "Radius (--radius)" "" "0" "pos_int")"
  timesteps="$(prompt_value "Timesteps (--timesteps)" "" "0" "pos_int")"
  poll_every="$(prompt_value "Poll every (--poll_every)" "" "0" "pos_int")"
  mpi_thread="$(prompt_value "MPI thread level" "" "0" "enum" funneled serialized multiple)"
  progress="$(prompt_value "Progress mode" "" "0" "enum" inline_poll progress_thread)"
  omp_schedule="$(prompt_value "OMP schedule" "" "0" "enum" static dynamic guided)"
  omp_chunk="$(prompt_value "OMP chunk (--omp_chunk)" "" "0" "nonneg_int")"
  omp_bind="$(prompt_value "OMP bind (--omp_bind)" "" "0" "bool")"
  omp_places="$(prompt_value "OMP places (--omp_places)" "" "0" "enum" cores threads)"
  flops_per_point="$(prompt_value "Flops per point" "" "0" "nonneg_int")"
  bytes_per_point="$(prompt_value "Bytes per point" "" "0" "nonneg_int")"
  check="$(prompt_value "Check correctness (--check)" "" "0" "bool")"
  check_every="$(prompt_value "Check every (--check_every)" "" "0" "pos_int")"
  poison_ghost="$(prompt_value "Poison ghost (--poison_ghost)" "" "0" "bool")"
  sync="$(prompt_value "Sync mode (--sync)" "" "0" "enum" none barrier_start barrier_each)"
  trace="$(prompt_value "Trace (--trace)" "" "0" "bool")"
  trace_iters="$(prompt_value "Trace iters (--trace_iters)" "" "0" "pos_int")"
  trace_detail="$(prompt_value "Trace detail (--trace_detail)" "" "0" "enum" rank thread)"
  transport="$(prompt_value "Transport (--transport)" "" "0" "enum" auto shm tcp)"
  out_dir="$(prompt_value "Output dir (--out_dir)" "" "0" "text")"
  run_id="$(prompt_value "Run id (--run_id)" "" "0" "text")"
  csv="$(prompt_value "CSV path (--csv)" "" "0" "text")"
  csv_mode="$(prompt_value "CSV mode (--csv_mode)" "" "0" "enum" write append)"
  manifest="$(prompt_value "Manifest (--manifest)" "" "0" "bool")"

  effective_kernel="${kernel:-stencil3}"
  if [[ -n "${radius}" ]]; then
    effective_radius="${radius}"
  else
    case "${effective_kernel}" in
      stencil5) effective_radius="2" ;;
      *) effective_radius="1" ;;
    esac
  fi
  effective_timesteps="${timesteps:-1}"
  if (( halo < effective_radius * effective_timesteps )); then
    echo "[tui] fail: invalid halo configuration in TUI pre-check: require halo >= radius*timesteps"
    echo "[tui] details: halo=${halo}, radius=${effective_radius}, timesteps=${effective_timesteps}"
    return
  fi

  cmd=(
    "${phasegap_bin}"
    --mode "${mode}"
    --ranks "${ranks}"
    --threads "${threads}"
    --N "${n_local}"
    --halo "${halo}"
    --iters "${iters}"
    --warmup "${warmup}"
  )
  [[ -n "${kernel}" ]] && cmd+=(--kernel "${kernel}")
  [[ -n "${radius}" ]] && cmd+=(--radius "${radius}")
  [[ -n "${timesteps}" ]] && cmd+=(--timesteps "${timesteps}")
  [[ -n "${poll_every}" ]] && cmd+=(--poll_every "${poll_every}")
  [[ -n "${mpi_thread}" ]] && cmd+=(--mpi_thread "${mpi_thread}")
  [[ -n "${progress}" ]] && cmd+=(--progress "${progress}")
  [[ -n "${omp_schedule}" ]] && cmd+=(--omp_schedule "${omp_schedule}")
  [[ -n "${omp_chunk}" ]] && cmd+=(--omp_chunk "${omp_chunk}")
  [[ -n "${omp_bind}" ]] && cmd+=(--omp_bind "${omp_bind}")
  [[ -n "${omp_places}" ]] && cmd+=(--omp_places "${omp_places}")
  [[ -n "${flops_per_point}" ]] && cmd+=(--flops_per_point "${flops_per_point}")
  [[ -n "${bytes_per_point}" ]] && cmd+=(--bytes_per_point "${bytes_per_point}")
  [[ -n "${check}" ]] && cmd+=(--check "${check}")
  [[ -n "${check_every}" ]] && cmd+=(--check_every "${check_every}")
  [[ -n "${poison_ghost}" ]] && cmd+=(--poison_ghost "${poison_ghost}")
  [[ -n "${sync}" ]] && cmd+=(--sync "${sync}")
  [[ -n "${trace}" ]] && cmd+=(--trace "${trace}")
  [[ -n "${trace_iters}" ]] && cmd+=(--trace_iters "${trace_iters}")
  [[ -n "${trace_detail}" ]] && cmd+=(--trace_detail "${trace_detail}")
  [[ -n "${transport}" ]] && cmd+=(--transport "${transport}")
  [[ -n "${out_dir}" ]] && cmd+=(--out_dir "${out_dir}")
  [[ -n "${run_id}" ]] && cmd+=(--run_id "${run_id}")
  [[ -n "${csv}" ]] && cmd+=(--csv "${csv}")
  [[ -n "${csv_mode}" ]] && cmd+=(--csv_mode "${csv_mode}")
  [[ -n "${manifest}" ]] && cmd+=(--manifest "${manifest}")

  run_cmd env OMP_NUM_THREADS="${threads}" mpirun -np "${ranks}" "${cmd[@]}"
}

main() {
  require_cmd bash
  show_header
  while true; do
    menu
    read -r -p "Select action [1-10]: " choice || true
    case "${choice:-}" in
      1) smoke_flow ;;
      2) quality_gate_flow ;;
      3) matrix_dry_run_flow ;;
      4) matrix_quick_run_flow ;;
      5) docker_netem_flow ;;
      6) docker_mpi_compose_flow ;;
      7) run_cmd "${ROOT_DIR}/scripts/netem_on.sh" --list-presets ;;
      8) show_phasegap_cli_options ;;
      9) custom_phasegap_flow ;;
      10)
        echo "[tui] bye"
        exit 0
        ;;
      *)
        echo "[tui] invalid choice: ${choice:-<empty>}"
        ;;
    esac
  done
}

main "$@"
