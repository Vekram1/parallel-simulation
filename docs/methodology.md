# PhaseGap Methodology: Validity and Interpretation Limits

This note defines how to interpret PhaseGap results without over-claiming.
It complements `PLAN.md` and is part of the benchmark contract surface.

## Purpose and scope

PhaseGap is a single-node MPI+OpenMP coordination microbenchmark.
It is designed to quantify exposed coordination cost in a phase-separated halo pattern:

1. post halo exchange
2. compute interior
3. wait/progress for halo completion
4. compute boundary

The benchmark target is orchestration behavior (`wait_frac`, `overlap_ratio`, `wait_skew`), not absolute peak FLOPS or peak network throughput.

## Non-goals

- It is not a claim of multi-node production scalability.
- It is not an optimized stencil kernel competition.
- It is not a full-network benchmark of topology, congestion, and routing.

## Threats to validity

### MPI progress semantics

`nb_test` and overlap behavior depend on MPI implementation details.
Some MPI stacks progress aggressively during compute; others require explicit polling or waits.
Interpret mode-to-mode deltas only with MPI implementation/version and thread level (`MPI_Init_thread` provided value) recorded.

Implication:
A higher or lower overlap ratio can reflect MPI progress policy, not only algorithmic orchestration quality.

### Thread placement and core heterogeneity

Affinity and placement (`OMP_PROC_BIND`, `OMP_PLACES`, schedule/chunk) can dominate observed wait behavior, especially on heterogeneous cores.
Runs with weak or inconsistent binding can produce noisy or misleading coordination signatures.

Implication:
Do not compare runs unless placement policy is captured and intentionally controlled.

### Oversubscription and scheduler interference

If `P * T` exceeds effective cores, scheduling artifacts may inflate wait and skew.
Headline sweeps should avoid oversubscription; exploratory oversubscribed runs must be explicitly labeled.

Implication:
Wait inflation under oversubscription is not direct evidence of communication bottlenecks.

### Thermal and power drift

Laptop and workstation thermal behavior can change iteration timing during long sweeps.
Background system activity also perturbs distributions.

Implication:
Use warmup, p50/p95 reporting, and stable power conditions. Prefer multiple repeats for conclusions.

### Synchronization artifacts

Diagnostic sync modes (`barrier_start`, `barrier_each`) are useful for debugging but can distort real overlap behavior.
`barrier_each` in particular can inject artificial serialization.

Implication:
Do not mix diagnostic-sync and baseline runs in one comparison table without explicit labeling.

### Single-node transport limits

Single-node transport (especially shared-memory fast paths) may hide multi-node effects.
Conversely, Linux impairment experiments are synthetic and do not replicate full cluster behavior.

Implication:
Treat impairment results as sensitivity analysis, not direct cluster prediction.

### Kernel simplification

The stencil kernel is intentionally simple to isolate coordination effects.
Real applications can include additional dependencies, cache behavior, and communication patterns.

Implication:
PhaseGap conclusions should transfer at the coordination-pattern level, not as absolute performance forecasts.

## Interpretation guidance

### Metrics

- `wait_frac`: fraction of iteration exposed as wait. Higher means coordination dominates more of wall time.
- `overlap_ratio`: achieved hide relative to ideal hide window. Lower means poorer comm/compute overlap.
- `wait_skew`: imbalance signal across ranks. High skew suggests straggler or progress asymmetry.

Use all three together. A single metric is not sufficient to diagnose root cause.

### Traces

Use traces to validate causality hypotheses:

- interior spans shrink with larger `T`
- wait spans become more visible as compute shrinks
- boundary spans should occur only after completion

If trace signatures and CSV metrics disagree, treat results as suspect and investigate instrumentation or run conditions.

### Minimum provenance for publishable comparisons

For any figure/table intended as evidence, capture and retain:

- git SHA and timestamp
- host/platform metadata
- MPI implementation/version and requested/provided thread level
- OpenMP env (`OMP_NUM_THREADS`, `OMP_PROC_BIND`, `OMP_PLACES`, schedule/chunk)
- full CLI config including mode, `P`, `T`, `N`, `H`, kernel/radius/timesteps, warmup/iters

### Reporting rules

- Always report warmup policy and robust stats (mean + p50 + p95).
- Keep mode timing boundaries normalized across modes (`t_comm_window` definition from `PLAN.md`).
- Explicitly state whether runs are baseline single-node or impaired Linux sensitivity runs.
- Avoid causal language beyond evidence (for example, do not claim network causes without supporting diagnostics).
