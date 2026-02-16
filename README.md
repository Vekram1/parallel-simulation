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

## macOS OpenMP Configure Helper

If CMake cannot detect OpenMP on macOS, use:

```bash
scripts/dev/configure-macos-openmp.sh --build-dir build-appleomp --build
```

This helper configures with Apple Clang and Homebrew `libomp` flags, then optionally builds.
