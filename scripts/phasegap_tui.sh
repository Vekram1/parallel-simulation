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
8) Exit
MENU
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

main() {
  require_cmd bash
  show_header
  while true; do
    menu
    read -r -p "Select action [1-8]: " choice || true
    case "${choice:-}" in
      1) smoke_flow ;;
      2) quality_gate_flow ;;
      3) matrix_dry_run_flow ;;
      4) matrix_quick_run_flow ;;
      5) docker_netem_flow ;;
      6) docker_mpi_compose_flow ;;
      7) run_cmd "${ROOT_DIR}/scripts/netem_on.sh" --list-presets ;;
      8)
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
