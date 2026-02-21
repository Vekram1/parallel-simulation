# PhaseGap

Single-node C++ MPI+OpenMP coordination microbenchmark focused on phase-separated halo/stencil behavior.

## Quickstart

Build:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Run (example):

```bash
OMP_NUM_THREADS=2 mpirun -np 2 ./build/phasegap \
  --mode phase_nb \
  --ranks 2 \
  --threads 2 \
  --N 256 \
  --halo 8 \
  --iters 8 \
  --warmup 2 \
  --out_dir runs/demo \
  --csv runs/demo/results.csv \
  --manifest 1
```

## Guided Launcher (TUI)

For a more seamless operator workflow, use the interactive launcher:

```bash
scripts/phasegap_tui.sh
```

It provides menu-driven flows for:
- smoke run
- strict quality gate
- matrix dry-run/quick-run
- Docker netem smoke
- Docker multi-container MPI + netem run
- netem preset listing

## First Successful Run

Run a small baseline that should generate all core artifacts:

```bash
OMP_NUM_THREADS=2 mpirun -np 2 ./build/phasegap \
  --mode phase_nb \
  --ranks 2 \
  --threads 2 \
  --N 128 \
  --halo 4 \
  --iters 6 \
  --warmup 2 \
  --trace 1 \
  --trace_iters 3 \
  --out_dir runs/first-success \
  --csv runs/first-success/results.csv \
  --manifest 1
```

Expected files in `runs/first-success/`:
- `results.csv`
- `manifest.json`
- `trace.json` (when `--trace 1`)
- `run.stdout.log` if executed via `scripts/run_matrix.sh`

## Modes

Supported orchestration modes:
- `phase_nb`: nonblocking halo post (`Irecv/Isend`) -> interior -> `Waitall` -> boundary
- `phase_blk`: blocking exchange baseline (expected overlap near zero)
- `nb_test`: nonblocking + periodic `MPI_Testall` progression with polling metrics
- `phase_persist`: persistent request path (`MPI_*_init` + `MPI_Startall`)
- `omp_tasks`: stretch mode using OpenMP `taskloop` orchestration with baseline-compatible phase boundaries

Progress options:
- `inline_poll`: baseline behavior
- `progress_thread`: helper-thread `MPI_Testall` progression when MPI provides `MPI_THREAD_MULTIPLE`; otherwise falls back to `inline_poll` with warning

## Quality Gates

Local smoke gate (configure + build + tiny run + metric invariant checks):

```bash
scripts/smoke_build.sh
```

Useful options:

```bash
scripts/smoke_build.sh --help
scripts/smoke_build.sh --build-dir build-smoke --run-dir runs/smoke-local --np 2 --threads 2
```

Metric checker only (against an existing smoke log):

```bash
python3 scripts/check_metrics.py --log runs/smoke/smoke.stdout.log --expect-measured-iters 6
```

What the smoke gate enforces:
- CMake configure/build succeeds and emits `compile_commands.json`
- Sanity run completes and prints the PhaseGap summary line
- Timing/interpretation invariants hold (`t_comm_window_us >= t_wait_us`, nonnegative times, valid overlap range, measured-iteration consistency)

Quality gate scaffold (build + sanity run + artifact/schema checks):

```bash
scripts/quality_gate.sh --strict-artifacts
```

Useful options:

```bash
scripts/quality_gate.sh --build-dir build-gate --run-dir runs/quality-gate --np 2 --threads 2 --n-local 128 --halo 4 --iters 6 --warmup 2 --strict-artifacts
```

CI gate:
- GitHub Actions workflow at `.github/workflows/build.yml` runs the same smoke script on Ubuntu with Open MPI.
- CI also uploads `runs/ci-smoke/smoke.stdout.log` as an artifact for debugging failures.

## Output Artifact Guide

Per-run directory (`--out_dir`) is canonical source of truth.

Required/primary artifacts:
- `results.csv`: schema-versioned metrics row(s) for the run
- `manifest.json`: config + environment + runtime summary metadata
- `trace.json`: Chrome/Perfetto trace when tracing is enabled

Common metric fields to inspect first:
- transport provenance: `transport_requested`, `transport_effective`
- timing means: `t_iter_us`, `t_post_us`, `t_interior_us`, `t_wait_us`, `t_boundary_us`, `t_comm_window_us`
- timing distribution: `*_p50_us`, `*_p95_us`
- rank bottleneck indicators: `*_mean_max_us`, `wait_skew`
- overlap/coordination indicators: `wait_frac`, `overlap_ratio`
- progression indicators: `mpi_test_calls`, `mpi_wait_calls`, `polls_to_complete_mean`, `polls_to_complete_p95`

Index/catalog policy:
- `results/results.csv` (when produced by matrix harness) is a non-authoritative run catalog for convenience.
- Per-run artifacts under `runs/...` remain the source of truth for provenance/debugging.

## Results Interpretation Guide

Use this section to turn CSV and trace outputs into quick diagnoses.

Core metric meanings:
- `wait_frac`: fraction of iteration time spent in exposed wait.
  - Higher values indicate coordination pressure or communication delay.
- `overlap_ratio`: how much communication time was hidden by interior compute.
  - `1.0` means near-ideal hiding, `0.0` means little or no overlap.
- `wait_skew`: cross-rank imbalance indicator (`wait_mean_max / wait_mean_avg`).
  - Near `1.0` is balanced; higher values indicate one or more straggler ranks.
- `mpi_test_calls` and `polls_to_complete_*` (for `nb_test`):
  - Higher polling counts can indicate completion latency, but can also reflect too-aggressive polling cadence.

Expected signatures by mode:
- `phase_blk`:
  - `overlap_ratio` should be near zero (baseline no-overlap behavior).
  - `t_wait_us` usually tracks most of the communication window.
- `phase_nb`:
  - should generally improve overlap relative to `phase_blk` when interior work is nontrivial.
  - if `overlap_ratio` remains near zero, communication may be effectively serialized or interior is too small.
- `nb_test`:
  - compare against `phase_nb` to see whether polling cadence helps completion timing.
  - check `mpi_test_calls` and `polls_to_complete_p95` for progress cost/completion behavior.
- `phase_persist`:
  - intended to reduce request setup overhead in steady state.
  - compare `t_post_us` and `t_iter_us` against `phase_nb` under same `(P,T,N,H)` settings.

Thread-scaling interpretation (fixed ranks and halo):
- If threads increase and `t_interior_us` drops while `t_wait_us`/`wait_frac` rises, coordination is becoming dominant.
- If both `t_interior_us` and `t_wait_us` drop, communication likely still scales with compute regime.
- Use `wait_skew` to distinguish global slowdown from rank-local imbalance.

Trace reading patterns (`trace.json`):
- Healthy overlap:
  - interior spans overlap communication window and wait spans stay short.
- Coordination bottleneck:
  - short interior spans followed by visibly long wait spans.
- Imbalance:
  - one rank shows longer waits or delayed boundary start relative to others.
- Polling-heavy behavior (`nb_test`):
  - elevated `mpi_test_calls` counters with minimal wait reduction suggests poll cadence tuning is needed.

Common anomaly diagnostics:
- `wait_frac` unexpectedly negative or `overlap_ratio` outside `[0,1]`:
  - indicates metric pipeline drift; run `scripts/check_metrics.py` and quality gate.
- `phase_blk` shows high overlap:
  - likely orchestration semantics drift; verify blocking path ordering.
- very high `wait_skew` with modest mean wait:
  - likely rank outlier/noise; check affinity, per-rank trace lanes, and host contention.
- large run-to-run variation:
  - stabilize `OMP_PROC_BIND`/`OMP_PLACES`, avoid oversubscription (`P*T <= C`), and re-run with warmup.

## Matrix Harness

Sweep automation:

```bash
scripts/run_matrix.sh --help
```

Example:

```bash
scripts/run_matrix.sh \
  --build-dir build-matrix \
  --run-root runs/matrix \
  --index results/results.csv \
  --sweeps 1,2 \
  --trace 1 \
  --trace-iters 30
```

To keep per-run artifacts as the only outputs (no aggregate catalog writes), add:

```bash
scripts/run_matrix.sh --no-index --sweeps 1
```

Dry-run planning (no run execution):

```bash
scripts/run_matrix.sh --dry-run --max-runs 4
```

`--dry-run` is planning-only and does not require `cmake`/`mpirun` in `PATH`.

Input constraints (fail-fast validation):
- `--trace` must be `0` or `1`
- `--sweeps` must be a comma-separated subset of `1,2,3`
- `--trace-iters`, `--n-local`, `--iters` must be positive integers
- `--core-budget`, `--max-runs`, `--warmup`, `--bytes-per-point` must be non-negative integers

## Report Pack (Recommended)

Generate a standard interpretation pack (plots + confidence summary) from one or more run roots:

```bash
python3 scripts/analyze.py \
  --input runs/matrix \
  --out-dir runs/analysis-report
```

Outputs include:
- `wait_frac_vs_threads.png`
- `overlap_ratio_vs_threads.png`
- `wait_skew_vs_threads.png`
- `phase_stack_vs_threads.png`
- `wait_p50_p95_vs_threads.png`
- `nbtest_poll_tradeoff.png` (when `nb_test` rows exist)
- `metric_summary.csv`
- `confidence_summary.md`
- `dashboard.html` (single-file report view for quick sharing)

## Linux Netem Workflow

Linux-only impairment helpers are available for communication sensitivity studies:
- `scripts/netem_on.sh`
- `scripts/netem_off.sh`

Safety and scope notes:
- real apply/clear requires root (`sudo`) and Linux `tc` (iproute2)
- `--dry-run` preview does not require root or Linux
- applies qdisc at interface level, so it affects all traffic on that interface
- always clear impairment with `scripts/netem_off.sh` after experiments

List available impairment presets:

```bash
scripts/netem_on.sh --list-presets
```

Apply a preset (example):

```bash
sudo scripts/netem_on.sh --iface eth0 --preset wan_mild
```

Apply explicit overrides (example with bandwidth cap):

```bash
sudo scripts/netem_on.sh \
  --iface eth0 \
  --preset constrained_50 \
  --delay-ms 25 \
  --jitter-ms 6 \
  --loss-pct 0.3 \
  --rate 50mbit
```

Clear impairment:

```bash
sudo scripts/netem_off.sh --iface eth0
```

Preview commands without changing the system:

```bash
scripts/netem_on.sh --iface eth0 --preset wan_mild --dry-run
scripts/netem_off.sh --iface eth0 --dry-run
```

## Docker Simulated-Network Smoke

For a turnkey simulated-network test path, use:

```bash
scripts/docker_netem_smoke.sh
```

What it does:
- builds a Linux OpenMPI toolchain image (`docker/Dockerfile.phasegap`)
- applies netem impairment inside the container namespace
- runs smoke build + a traced `phase_nb` run with `--transport tcp`
- always clears netem on exit (success or failure)

Example with explicit impairment overrides:

```bash
scripts/docker_netem_smoke.sh \
  --np 2 \
  --threads 2 \
  --delay-ms 25 \
  --jitter-ms 6 \
  --loss-pct 0.3 \
  --rate 50mbit
```

## Docker Multi-Container MPI (Recommended for Link Realism)

For a launcher + worker topology across separate containers, use:

```bash
scripts/docker_mpi_compose.sh
```

What it does:
- starts `launcher`, `worker1`, `worker2` via `docker compose`
- provisions SSH trust for OpenMPI remote launch
- optionally applies netem shaping on worker `eth0`
- runs `mpirun` across workers using a generated hostfile
- clears netem and tears down compose stack automatically (unless `--keep-up`)

Example:

```bash
scripts/docker_mpi_compose.sh \
  --np 2 \
  --threads 2 \
  --delay-ms 25 \
  --jitter-ms 6 \
  --loss-pct 0.3 \
  --rate 50mbit
```

Useful options:
- `--no-netem` to run clean multi-container MPI without impairment
- `--no-image-build` to reuse an existing image
- `--keep-up` to keep containers running for inspection/debugging
- `--dry-run` to preview all actions

Operational notes:
- first real run provisions SSH material under `.docker-mpi/` for launcher->worker MPI startup
- `--dry-run` now prints plan only and does not mutate `.docker-mpi/`

## macOS OpenMP Configure Helper

If CMake cannot detect OpenMP on macOS, use:

```bash
scripts/dev/configure-macos-openmp.sh --build-dir build-appleomp --build
```

This helper configures with Apple Clang and Homebrew `libomp` flags, then optionally builds.
