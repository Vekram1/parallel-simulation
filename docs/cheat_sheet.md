# Two paradigms

See also: `docs/methodology.md` for threats-to-validity and interpretation limits when reading PhaseGap results.

## OpenMP / Threads = shared-memory parallelism
- One process, one address space.
- Threads can read/write the same arrays directly.
- Coordination cost is mostly:
  - barriers, locks/atomics
  - cache coherence / false sharing
  - NUMA effects (on machines that have NUMA)
- Communication is implicit (loads/stores).

## MPI = distributed-memory parallelism
- Many processes (“ranks”), separate address spaces.
- A rank cannot directly load another rank’s memory.
- Data must move via explicit calls (`MPI_Send`, `MPI_Irecv`, …).
- Coordination cost is:
  - message latency/bandwidth
  - progress semantics (does communication advance while you compute?)
  - synchronization (wait/test)
- Communication is explicit (messages).

---

# Why MPI exists at all (even on one machine)
Because it scales across nodes (where memory physically isn’t shared). OpenMP can’t magically make RAM on another node “shared.” MPI is basically the portable standard for “your memory ends here; talk to neighbors by sending bytes.”

On a single node, MPI still matters because:
- it mimics the structure of multi-node codes
- it lets you study the coordination pattern (post → compute → wait → boundary)
- it shows overheads like:
  - request management
  - progress/polling
  - barriers and stragglers across ranks

---

# Hybrid MPI + OpenMP (what your plan is doing)
- MPI gives you `P` ranks (process-level parallelism / domain decomposition).
- OpenMP gives you `T` threads per rank to speed up local compute.

So each iteration looks like:
1. MPI: exchange boundary data (halo) with neighbors
2. OpenMP: compute interior fast (`parallel for`)
3. MPI: wait/test until halo arrives
4. OpenMP: compute boundary (needs halo)

The key phenomenon you’re measuring:
- as `T` increases, interior compute shrinks,
- but message latency + synchronization doesn’t shrink proportionally,
- so exposed wait grows ⇒ “coordination bottleneck.”

---

# A simple analogy
- OpenMP: multiple cooks in one kitchen sharing the same pantry (memory). They can grab ingredients directly.
- MPI: multiple kitchens. If kitchen A needs ingredients from kitchen B, someone has to package it, send it, and wait for delivery.
- Hybrid code: each kitchen has multiple cooks.

---

# The one sentence that makes MPI “click”
MPI is the standard way to write parallel programs where “parallelism” is not just computation, but explicit, structured communication between independent memory domains.

---

# Variable definitions (PhaseGap plan)

## Parallelism / execution
- `P`: number of MPI ranks (processes).  
  Each rank has its own address space. Ranks communicate via MPI messages.
- `T`: number of OpenMP threads per rank.  
  Threads share memory within a rank (within that process).
- `C`: number of performance cores available on the machine (used for “avoid oversubscription” guidance).

## Domain / data layout
- `N`: number of owned elements per rank (the local problem size for that rank, excluding ghost zones).
- `H`: halo width in elements (per side).  
  Number of elements each rank sends to and receives from each neighbor per halo exchange.  
  Also the size of each ghost zone (left ghost and right ghost).
- Ghost zones / ghost elements: the `H` elements on each side of the owned region that hold neighbor data (read-only inputs to boundary updates).
- Ring topology: ranks are arranged in a 1D ring; each rank has a left neighbor and right neighbor with wrap-around.

## Stencil / kernel parameters
- `kernel`: which update formula you run, e.g. `{stencil3, stencil5, axpy}`.
- `R`: stencil radius (how far the stencil reaches). Examples:
  - `stencil3` ⇒ `R = 1` (uses `i±1`)
  - `stencil5` ⇒ `R = 2` (uses `i±2`)
- `S`: timesteps per halo exchange.  
  Number of local stencil steps you compute before doing the next halo exchange.
- `B`: boundary dependency width, defined as `B = R * S`.  
  Number of owned elements near each edge whose updates depend on halo data after `S` steps. Used to define:
  - interior region (doesn’t need halo)
  - boundary band (needs halo)
- Correctness invariant: `H >= B` (otherwise boundary updates would require neighbor data you didn’t communicate).
- `alpha` / `work`: extra per-point arithmetic “burn” factor to scale compute intensity (if you keep that knob).

## Run control
- `K` / `iters`: number of measured iterations.
- `W` / `warmup`: number of warmup iterations excluded from stats.
- `mode`: orchestration schedule, e.g. `{phase_nb, phase_blk, nb_test, phase_persist, omp_tasks}`.
- `poll_every`: for `nb_test`, how frequently you poll (`MPI_Test*`) during chunked interior compute (controls polling overhead vs responsiveness).

## MPI / OpenMP configuration
- `mpi_thread_requested`: requested MPI thread level: `{funneled, serialized, multiple}`.
- `mpi_thread_provided`: actual thread level returned by `MPI_Init_thread`.
- `omp_schedule`: OpenMP loop schedule `{static, dynamic, guided}`.
- `omp_chunk`: chunk size for OpenMP scheduling (relevant mainly for dynamic/guided).
- `omp_bind`: whether threads are bound to CPU resources (maps to `OMP_PROC_BIND`).
- `omp_places`: placement policy `{cores, threads}` (maps to `OMP_PLACES`, best-effort).
- `transport`: best-effort MPI transport selector `{auto, shm, tcp}`.

## Timing / metrics (per rank, per iteration)
- `t_post`: time to post nonblocking receives/sends (or start exchange).
- `t_interior`: time spent computing the interior region.
- `t_wait`: time spent waiting for communication completion (`MPI_Waitall` or equivalent completion window).
- `t_boundary`: time spent computing the boundary band.
- `t_comm_window`: total communication window duration (defined consistently across modes: from “exchange start” to “both halos complete”).
- `t_iter`: total iteration time.

Derived:
- `wait_frac`: `t_wait_mean / t_iter_mean`.
- `wait_skew`: imbalance indicator across ranks: `t_wait_mean_max / max(t_wait_mean_avg, eps)`.
- `overlap_ratio`: fraction of the ideal overlap achieved (as defined in the plan).

Communication size:
- `msg_bytes`: bytes sent to one neighbor per exchange: `H * sizeof(element)`.
- `bytes_total`: total bytes sent per rank per exchange (both directions): `2 * msg_bytes`.
- `bw_effective`: effective bandwidth estimate: `bytes_total / t_comm_window_mean`.

---

# PhaseGap CLI → Symbol Cheat Sheet

## Parallelism / layout
- `--ranks P` → `P`: number of MPI ranks (processes)
- `--threads T` → `T`: OpenMP threads per rank (per process)
- `--N N` → `N`: owned elements per rank (local problem size, excluding ghosts)
- `--halo H` → `H`: halo width (elements per side exchanged with each neighbor)

## Kernel / math
- `--kernel {stencil3,stencil5,axpy}` → `kernel`: update type (also implies default radius)
- `--radius R` → `R`: stencil radius (if omitted, derived from kernel)
- `--timesteps S` → `S`: timesteps per halo exchange

Derived (not a CLI flag unless you add one):
- `B = R * S`: boundary dependency width
- Correctness invariant: `H >= B`

## Orchestration mode
- `--mode {phase_nb,phase_blk,nb_test,phase_persist,omp_tasks}` → `mode`: coordination schedule variant
- `--poll_every q` → `q`: polling granularity for `nb_test` (points per poll / chunk size between `MPI_Test*` calls)

## MPI threading / transport
- `--mpi_thread {funneled,serialized,multiple}` → `mpi_thread_requested`: requested MPI thread support level  
  (actual support recorded as `mpi_thread_provided` from `MPI_Init_thread`)
- `--transport {auto,shm,tcp}` → `transport`: best-effort transport selection (implementation-dependent)

## OpenMP scheduling / affinity
- `--omp_schedule {static,dynamic,guided}` → `omp_schedule`
- `--omp_chunk c` → `omp_chunk` (mainly meaningful for dynamic/guided)
- `--omp_bind {true,false}` → `omp_bind` (maps to `OMP_PROC_BIND`)
- `--omp_places {cores,threads}` → `omp_places` (maps to `OMP_PLACES`, best-effort)

## Run control
- `--iters K` → `K`: number of measured iterations
- `--warmup W` → `W`: number of warmup iterations (excluded from stats)

## Compute intensity knob
- `--work alpha` → `alpha`: extra arithmetic “burn” factor per point update  
  (If you adopt improved split knobs later: `--flops_per_point f`, `--bytes_per_point b`.)

## Tracing
- `--trace {0,1}` → trace enable
- `--trace_iters M` → `M`: number of final measured iterations to trace
- `--trace_detail {rank,thread}` → trace granularity:
  - `rank`: one compute lane per rank
  - `thread`: compute spans per OpenMP thread (with a rank-level lane for readability)

## Output
- `--out results.csv` → output CSV path (append/write semantics per your implementation)

---

## Quick sanity mapping (what you usually vary in sweeps)
- Coordination effect vs threads: vary `T` while holding `P`, `H`, `alpha` fixed
- Message-size sensitivity: vary `H`
- Stencil dependence: vary `R` (via kernel or explicit radius)
- Compute/comm ratio: vary `alpha` and/or `S`
- Scheduling sensitivity: vary `omp_schedule`, `omp_chunk`, binding/places
- Progress sensitivity: vary `mode` + `poll_every` + `mpi_thread_requested`