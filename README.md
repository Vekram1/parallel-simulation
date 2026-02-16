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

Not implemented (CLI recognized but exits with warning):
- `omp_tasks`
- `progress_thread`

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
- timing means: `t_iter_us`, `t_post_us`, `t_interior_us`, `t_wait_us`, `t_boundary_us`, `t_comm_window_us`
- timing distribution: `*_p50_us`, `*_p95_us`
- rank bottleneck indicators: `*_mean_max_us`, `wait_skew`
- overlap/coordination indicators: `wait_frac`, `overlap_ratio`
- progression indicators: `mpi_test_calls`, `mpi_wait_calls`, `polls_to_complete_mean`, `polls_to_complete_p95`

Index/catalog policy:
- `results/results.csv` (when produced by matrix harness) is a non-authoritative run catalog for convenience.
- Per-run artifacts under `runs/...` remain the source of truth for provenance/debugging.

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

Dry-run planning (no run execution):

```bash
scripts/run_matrix.sh --dry-run --max-runs 4
```

## macOS OpenMP Configure Helper

If CMake cannot detect OpenMP on macOS, use:

```bash
scripts/dev/configure-macos-openmp.sh --build-dir build-appleomp --build
```

This helper configures with Apple Clang and Homebrew `libomp` flags, then optionally builds.
