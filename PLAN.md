# PhaseGap — Project Plan (1-node MPI+OpenMP Coordination Microbenchmark)

## Goal
Build a small, reproducible benchmark on a single machine (M1 Max) that **quantifies and visualizes the coordination bottleneck** in phase-separated hybrid codes:

- Post halo communication (MPI nonblocking)
- Compute interior work in parallel (OpenMP)
- Wait for communication (exposed idle time)
- Compute boundary work

Primary outputs:
- **CSV metrics** (time breakdowns, wait fraction, overlap ratio)
- **Chrome/Perfetto trace** showing per-rank timelines of comm/compute/wait

---

## Quickstart (first successful run)
Build:
- `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release`
- `cmake --build build -j`

Run (macOS baseline example):
- `mpirun -np 4 ./build/phasegap --mode phase_nb --ranks 4 --threads 2 --N 200000 --halo 256 --iters 400 --warmup 50 --trace 1 --trace_iters 60 --out_dir runs/demo`

Expected artifacts in `runs/demo/`:
- `results.csv` (one row per run)
- `manifest.json` (full configuration + environment capture)
- `trace.json` (Chrome trace)

Acceptance check (smoke test):
- `phase_blk` should show `overlap_ratio ~ 0`
- increasing `--threads` should shrink interior spans and make wait spans visually pop in the trace

---

## Scope / Non-Goals / Threats to Validity
### Scope
- Single-node hybrid coordination effects using MPI + OpenMP with a halo/stencil structure.
- Emphasis is on **orchestration** and **exposed idle**, not absolute peak FLOPS or peak bandwidth.

### Non-Goals
- Not a “best stencil implementation” benchmark (we intentionally keep the kernel simple and parameterized).
- Not a proxy for true multi-node contention, routing, or topology effects.

### Threats to Validity (we will record + control where possible)
- MPI implementation progress semantics (async progress on/off, polling effectiveness).
- Thread placement / affinity / P-cores vs E-cores on Apple Silicon.
- Power/thermal throttling and OS jitter.
- Oversubscription and scheduling artifacts (we will avoid in headline sweeps).

---

## Core Thesis
Even on one node, increasing compute parallelism (more OpenMP threads) shrinks interior compute time, but communication completion and synchronization do not shrink proportionally. This increases **exposed wait** (`MPI_Waitall`) and reduces **overlap efficiency**, revealing a coordination bottleneck analogous to what shows up at scale.

---

## MPI Threading & Progress Model (Must Be Explicit)
Initialization:
- Use `MPI_Init_thread` and record `provided` thread support level.

Baseline contract:
- Default is `MPI_THREAD_FUNNELED`: only the OpenMP master thread may call MPI.

Progress notes:
- `nb_test` effectiveness depends on MPI progress semantics.
- We record MPI implementation/version and any progress-related env/settings used.

New knobs:
- `--mpi_thread {funneled,serialized,multiple}` (best-effort; we record what MPI actually provides)
- `--progress {inline_poll,progress_thread}`
  - `inline_poll`: master thread polls (baseline)
  - `progress_thread`: dedicated helper thread performs `MPI_Test*` (requires `MPI_THREAD_MULTIPLE`)

Progress instrumentation (required):
- `mpi_test_calls` and `mpi_wait_calls` per iteration
- `t_poll` = time spent inside polling (`MPI_Test*`) for `nb_test`
- `polls_to_complete` (mean/p95): polls before both halos complete

---

## High-Level Comparisons (What We Test)

### A) Orchestration Modes (`--mode`)
We compare schedules that differ only in how comm/compute coordination is orchestrated:

1. `phase_nb` (baseline)
   - `MPI_Irecv/Isend -> interior compute -> MPI_Waitall -> boundary compute`
2. `phase_blk`
   - blocking exchange (e.g., `MPI_Sendrecv`) so overlap is impossible
3. `nb_test`
   - nonblocking + **chunked** interior compute with periodic `MPI_Test*` polling
   - parameterized by `--poll_every` (points per poll) to control polling frequency
4. `phase_persist` (recommended)
   - persistent halo exchange requests (`MPI_Send_init`/`MPI_Recv_init` + `MPI_Startall`)
   - isolates steady-state coordination from per-iter request allocation/dispatch overhead
5. `omp_tasks` (stretch)
   - express interior/boundary and comm completion as OpenMP tasks to mimic runtime co-scheduling

### B) Partitioning of cores across ranks vs threads
Hold total runnable threads approximately constant and vary `(P, T)` splits to isolate coordination effects:
- Choose `C = number of performance cores`
- Run splits where `P * T ≈ C` (avoid oversubscription in headline results)

### C) Communication regimes
Sweep halo width / message size and (optionally) enable network impairment in Linux to push into latency/bandwidth-bound regimes.

---

## Benchmark Specification

### Kernel: 1D Ring Halo Exchange with Ghost Zones
- `P` MPI ranks arranged in a ring
- Each rank owns a local array with ghost zones
- Halo width `H` (elements) exchanged with left/right neighbors each iteration

Data layout per rank uses ghost zones:
- allocate `A[-H .. N+H-1]` logically (implemented as `A[0 .. N+2H-1]`)
- owned points are updated; ghost zones are read-only stencil inputs.

### Derived parameters (correctness-critical)
Let:
- `R = stencil radius` (derived from kernel unless explicitly set)
- `S = timesteps per halo exchange`
- `B = R * S` = boundary dependency width (owned points that depend on halo data)

Correctness invariant:
- require `H >= B` (unless running an explicit invalid/diagnostic mode).

Interpretation:
- if `H > B`, communication is intentionally larger than mathematical minimum.

### Compute Kernel: 1D Stencil Update (Explicit)
We implement a simple stencil time-step to mimic PDE-style updates.

Example (radius-1, 3-point stencil):
- `Anew[i] = c0*A[i] + c1*(A[i-1] + A[i+1])`

Knobs:
- `--kernel {stencil3,stencil5,axpy}` (default: `stencil3`)
- `--radius R` (derived from kernel if not set)
- `--timesteps S` = number of local stencil steps per halo exchange (default: 1)

Compute/memory intensity knobs (separate concerns):
- `--flops_per_point f` (default: 0; extra arithmetic loop)
- `--bytes_per_point b` (default: 0; extra streaming reads/writes to a side buffer)

Purpose:
- preserve the shape of halo/stencil codes while enabling a controlled compute-bound -> coordination-bound transition.

### Per-iteration execution model (explicit)
1. Master thread posts receives:
   - `MPI_Irecv(left_halo)`
   - `MPI_Irecv(right_halo)`
2. Master thread posts sends:
   - `MPI_Isend(send_left)`
   - `MPI_Isend(send_right)`
3. Enter a single OpenMP parallel region:
   - `omp for` interior stencil update (owned points excluding boundary band)
   - `omp master` progresses comm / waits (mode-dependent)
   - `omp for` boundary stencil update (first/last `B` owned points; halo-dependent)
4. Swap buffers (`A <-> Anew`)

Notes:
- ghost zones are read-only inputs for the owned-point update.
- interior means indices `[B .. N-B-1]` (empty if `N < 2B`).

### OpenMP Implementation Contract (Explicit)
- Use a **single OpenMP parallel region** per iteration to avoid repeated fork/join overhead:
  - `#pragma omp parallel`
  - `#pragma omp for` interior
  - `#pragma omp master` performs MPI progression/wait (mode-dependent)
  - `#pragma omp for` boundary
- Default schedule: `static`. Expose as a knob to test scheduling sensitivity.

New knobs:
- `--omp_schedule {static,dynamic,guided}`
- `--omp_chunk c` (meaningful for dynamic/guided)
- `--omp_bind {true,false}` (maps to `OMP_PROC_BIND`)
- `--omp_places {cores,threads}` (maps to `OMP_PLACES` best-effort)

---

## CLI Parameters (Required)
- `--mode {phase_nb,phase_blk,nb_test,phase_persist,omp_tasks}`
- `--ranks P`
- `--threads T`
- `--N N` (local elements per rank)
- `--halo H` (halo width in elements)
- `--kernel {stencil3,stencil5,axpy}`
- `--radius R`
- `--timesteps S`
- `--poll_every q` (for `nb_test`)
- `--mpi_thread {funneled,serialized,multiple}`
- `--progress {inline_poll,progress_thread}`
- `--omp_schedule {static,dynamic,guided}`
- `--omp_chunk c`
- `--omp_bind {true,false}`
- `--omp_places {cores,threads}`
- `--flops_per_point f`
- `--bytes_per_point b`
- `--iters K`
- `--warmup W`
- `--check {0,1}` (default: 1)
- `--check_every E` (default: 50)
- `--poison_ghost {0,1}` (debug-only)
- `--sync {none,barrier_start,barrier_each}` (default: none; diagnostic only)
- `--trace {0,1}`
- `--trace_iters M`
- `--trace_detail {rank,thread}`
- `--transport {auto,shm,tcp}` (best-effort control)
- `--out_dir DIR` (required for serious runs; e.g. `runs/<run_id>/`)
- `--run_id ID` (default: timestamp+hash; used for file naming)
- `--csv FILE` (default: `<out_dir>/results.csv`)
- `--csv_mode {write,append}` (default: write)
- `--manifest {0,1}` (default: 1; writes `<out_dir>/manifest.json`)

---

## Metrics (CSV Output)

### Output schema versioning (must-have)
- Add `schema_version` to CSV rows (start at `1`).
- Add `trace_schema_version` to trace metadata.
- Emit `manifest.json` per run containing:
  - full normalized CLI args
  - derived params (`B`, bytes, etc.)
  - build info (compiler, flags, git SHA)
  - runtime env capture (MPI/OpenMP vars)
  - platform info (uname, CPU model/topology best-effort)

### Per-phase timings (microseconds)
For iterations after warmup:
- `t_post` (Irecv/Isend posting time)
- `t_interior`
- `t_wait` (`MPI_Waitall` or equivalent completion window)
- `t_boundary`
- `t_poll` (time in polling loops)
- `t_comm_window`
- `t_iter`

### Timing definitions (normalized across modes)
Define `t_comm_window` as:
- start: timestamp immediately before first exchange-start call
  - `phase_nb/nb_test/phase_persist`: before first `Irecv` or `Startall`
  - `phase_blk`: before `Sendrecv` (or first blocking exchange call)
- end: timestamp when both halo receives are complete (boundary data usable)

This keeps `overlap_ratio` comparable across orchestration modes.

### Aggregates
- mean, p50, p95 for the above
- `wait_frac = t_wait_mean / t_iter_mean`

### Cross-rank aggregation (critical)
We report both:
- `*_mean_avg` = average of per-rank means (overall typical rank)
- `*_mean_max` = max of per-rank means (bottleneck rank)

Imbalance indicator:
- `wait_skew = t_wait_mean_max / max(t_wait_mean_avg, eps)`

### Overlap metric
Define:
- `t_ideal_hide = min(t_comm_window, t_interior)`
- `t_hidden = clamp(t_comm_window - t_wait, 0, t_ideal_hide)`
- `overlap_ratio = t_hidden / t_ideal_hide` (0 if denom = 0)

### Communication volume + effective bandwidth
Derived per iteration:
- `msg_bytes = H * sizeof(elem)` (per neighbor, one direction)
- `bytes_total = 2 * msg_bytes` (left + right sends; symmetric receives)

Report:
- `bw_effective = bytes_total / t_comm_window_mean` (bytes/us or GB/s)

### Suggested CSV columns
- `schema_version,platform,hostname,git_sha,timestamp,run_id`
- `mode,progress,transport,impairment`
- `mpi_impl,mpi_version,mpi_thread_requested,mpi_thread_provided`
- `omp_num_threads,omp_proc_bind,omp_places,omp_schedule,omp_chunk`
- `P,T,N,H,kernel,radius,timesteps,B,flops_per_point,bytes_per_point,iters,warmup,poll_every`
- `t_iter_mean,t_iter_p50,t_iter_p95`
- `t_post_mean_avg,t_post_mean_max`
- `t_interior_mean_avg,t_interior_mean_max`
- `t_wait_mean_avg,t_wait_mean_max`
- `t_boundary_mean_avg,t_boundary_mean_max`
- `t_poll_mean_avg,t_poll_mean_max`
- `wait_frac,wait_skew,overlap_ratio`
- `mpi_test_calls_mean,mpi_wait_calls_mean,polls_to_complete_p95`
- `checksum64,msg_bytes,bytes_total,bw_effective`

---

## Trace Visualization (Chrome/Perfetto)

### Trace format
Emit Chrome trace JSON when `--trace=1`:
- `pid = MPI rank`
- always include a per-rank iteration lane (`tid=0`)
- per-thread compute spans use `tid=omp_thread_id+1` when `--trace_detail=thread`

Trace controls:
- `--trace_iters M` (default small, e.g., 50) traces only the last `M` measured iterations
- `--trace_detail {rank,thread}` controls per-rank vs per-thread compute spans

Duration events per iteration:
- `comm_post`
- `interior_compute`
- `waitall` (or completion span)
- `boundary_compute`

Counter events (recommended):
- `bytes_total` (per iteration)
- `mpi_test_calls` (per iteration; 0 for modes without polling)
- `t_wait` and `wait_frac` (per iteration)

Metadata events:
- run configuration (`P,T,N,H,kernel,radius,timesteps,B,flops_per_point,bytes_per_point,mode`)
- MPI/OpenMP environment summary (version, thread level, binding)
- `trace_schema_version`

### Acceptance
- Trace loads in `chrome://tracing` or Perfetto UI
- Phases are clearly visible
- Increasing threads makes interior shrink and wait stand out

---

## Platforms

### Platform A: Native macOS (baseline)
Purpose:
- show coordination effects even with fast intra-node transports

### Platform B: Linux environment (VM/container) with TCP + impairment (optional but strong)
Purpose:
- force MPI traffic over TCP (loopback or veth)
- apply `netem` to simulate latency/jitter/loss/bandwidth caps
- show the knee occurs earlier under worse comm conditions

Impairment presets (example labels):
- `none`
- `lat_1ms`
- `lat_10ms`
- `jitter_5ms_2ms`
- `loss_0.1`
- `bw_100mbit`

---

## Experiment Matrix (Default Sweeps)

### Sweep 1 — Wait fraction vs threads (core story)
Fixed:
- `P = 4`
- `H in {small, medium, large}` (e.g., 16 / 256 / 4096)
- `flops_per_point in {0, 64}`

Sweep:
- `T = 1,2,4,...` up to `floor(C/P)`

Compare:
- modes: `phase_blk`, `phase_nb`, `nb_test`, `phase_persist` (and `omp_tasks` if implemented)

Output:
- `wait_frac` vs `T`
- `overlap_ratio` vs `T`
- `wait_skew` vs `T`

### Sweep 2 — Rank/thread split with constant core budget
Choose `C = # performance cores`.

Run splits with `P*T ~= C`:
- e.g. `(1,C), (2,C/2), (4,C/4), (C,1)` (rounded as needed)

Fixed:
- `H` and intensity knobs

Output:
- how coordination changes as ranks increase while total threads remain approximately constant

### Sweep 3 — Message size sensitivity
Fixed:
- `P=4`, `T=4`, `flops_per_point=0`, `bytes_per_point=0`

Sweep:
- `H` geometrically (e.g., 1..8192)

Compare:
- `phase_nb` vs `nb_test` vs `phase_persist`

### Sweep 4 — Impairment A/B (Linux only)
Repeat Sweep 1 under `lat_*` / `bw_*` presets and compare knee shifts.

---

## Reference Code Architecture (Recommended)
Directories:
- `src/core/` timers, stats, phase registry, csv writer, manifest writer, trace writer
- `src/kernels/` stencil kernels (`stencil3`/`stencil5`/`axpy`)
- `src/modes/` orchestration modes (`phase_nb`/`phase_blk`/`nb_test`/`phase_persist`/`omp_tasks`)
- `src/mpi/` neighbor setup, request lifecycle, threading init

Meta-programming (to prevent drift):
- Define phases once using X-macro `PHASES(X)`:
  - generates enum, string table, CSV columns, trace labels
- Define kernels similarly with `KERNELS(X)` so CLI choices and dispatch stay consistent.

### Mode API contract (prevents metric drift)
Modes should implement a common interface:
- `mode_begin_iter(ctx)`
- `mode_post_exchange(ctx)` (marks `comm_window_start`)
- `mode_progress_or_wait(ctx)` (marks completion)
- `mode_end_iter(ctx)`

Only measurement core defines:
- when `t_comm_window` starts/ends
- how `overlap_ratio` is computed
- how phase spans map to trace labels

---

## Automation Scripts

### `scripts/run_matrix.sh`
- runs sweeps (arrays for `P,T,H,mode,impairment,intensity knobs`)
- sets `OMP_NUM_THREADS=T`
- launches `mpirun -np P ./phasegap ...`
- creates per-run directory:
  - `runs/{run_id}/manifest.json`
  - `runs/{run_id}/results.csv`
  - `runs/{run_id}/trace.json` (if enabled)
- optionally appends one summary line to `results/results.csv` as a non-authoritative index

### `scripts/netem_on.sh` + `scripts/netem_off.sh` (Linux only)
- apply/remove netem settings on the target interface used by MPI traffic

### `scripts/analyze.py` (recommended)
- reads run directories or `results/results.csv`
- produces plots:
  - `wait_frac` vs `T` (faceted by `H/intensity/mode`)
  - `overlap_ratio` vs `T`
  - `wait_skew` vs `T` (imbalance)
- selects representative trace and prints a Perfetto open hint

---

## Milestones (Implementation Order)

### M1 — Skeleton + Correctness
- CLI parsing
- ring neighbor logic
- one iteration exchange + compute works
- enforce `H >= B` invariant
- checksum support + optional poison-ghost debug path

### M2 — Measurement Core + CSV/Manifest
- monotonic timer utilities
- normalized phase timing collection
- progress instrumentation (`mpi_test_calls`, `t_poll`, `polls_to_complete`)
- warmup skip + summary stats
- cross-rank reduction + CSV + `manifest.json` output

### M3 — Trace Output
- trace writer with `trace_schema_version`
- trace iteration window (`--trace_iters`)
- optional per-thread detail (`--trace_detail=thread`)
- counter tracks (`bytes_total`, `mpi_test_calls`, `t_wait`, `wait_frac`)
- validate in Chrome/Perfetto

### M4 — Modes
- implement `phase_blk`
- implement `nb_test` (`--poll_every` chunked polling)
- implement `phase_persist`
- optional `progress_thread` path when `MPI_THREAD_MULTIPLE` is available
- (stretch) `omp_tasks`

### M5 — Sweep Harness + Analysis
- `scripts/run_matrix.sh` produces per-run outputs
- optional index CSV aggregation
- `scripts/analyze.py` produces default figures

### M6 — Linux + Impairment (optional but recommended)
- build/run in Linux environment
- force TCP transport (best effort)
- netem presets
- rerun key sweeps under impairment

---

## Guardrails / Repro Notes
- Main sweeps: avoid oversubscription (`P*T <= C`)
- Prefer using performance cores first; record CPU/core topology in metadata
- Record OpenMP settings: `OMP_PROC_BIND`, `OMP_PLACES`, schedule/chunk
- Always run warmup iterations; report p50/p95 to handle jitter

### Correctness + anti-optimization (required)
- `--check {0,1}` (default: 1) and `--check_every E` (default: 50)
- compute lightweight checksum of `Anew` on each rank and `MPI_Allreduce` it
  - emit `checksum64` in CSV and manifest
  - prevents dead-code elimination and catches indexing drift
- debug-only option `--poison_ghost {0,1}` fills ghost zones with NaNs between iterations to catch illegal reads

Diagnostics:
- `--sync=barrier_start` helps align iteration-zero timestamps for traces.
- `--sync=barrier_each` is debugging only and inflates comm/sync effects.

### Build & Environment Capture (must-have)
- Provide a `CMakeLists.txt` with explicit MPI + OpenMP flags.
- On each run, record:
  - `uname -a`, hostname, git SHA
  - MPI implementation/version and thread support provided
  - OpenMP env: `OMP_NUM_THREADS`, `OMP_PROC_BIND`, `OMP_PLACES`, schedule
  - CPU topology summary (best-effort; include P-core/E-core counts when available)
- Recommend stable power/thermal conditions (plugged in; avoid background load).

---
