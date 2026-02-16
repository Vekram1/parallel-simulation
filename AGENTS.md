# AGENTS.md — PhaseGap (C++ MPI+OpenMP Coordination Microbenchmark)

## What this file governs (read once)
This file is the operating protocol for coding agents working in this repo.
**PLAN.md + this file are the docs-of-record.**
If AGENTS.md conflicts with PLAN.md, STOP and reconcile via a doc diff.

## Operating mode (read once)
This repo is operated via:
- **Beads (`bd`)** as the source of truth for work tracking (issue IDs are the unit of work)
- **Agent Mail (MCP)** for durable coordination:
  - threads, acknowledgments, and advisory file reservations (leases)
  - note: your Agent Mail stack is `br` (beads-rust) backed
- **bv** for robot-only triage/planning signals

Non-negotiable: **PLAN.md + this file govern all work.** If a worker instruction conflicts with PLAN.md, STOP.

---

## 60-second contract summary (do not improvise)
Core invariants from PLAN.md that MUST be preserved:

**Benchmark semantics**
- **Stencil realism:** compute is a halo-dependent stencil update with ghost zones (not arbitrary "math burn" only).
- **Iteration structure (phase-separated):**
  1) post halo comm (nonblocking)
  2) interior compute (parallel)
  3) wait/progress completion (exposed idle)
  4) boundary compute (parallel)

**Threading + MPI contracts**
- **Threading contract:** use a *single OpenMP parallel region per iteration* unless PLAN.md says otherwise.
- **MPI threading:** default is `MPI_THREAD_FUNNELED` (only the OpenMP master thread calls MPI) unless PLAN explicitly opts into another level.
- If MPI progress/polling is studied (`nb_test`), it must be **parameterized** (e.g., poll frequency) and recorded.

**Measurement validity**
- Timers are monotonic; warmup excluded from stats.
- Report mean + p50 + p95 (or equivalent robust stats).
- Do not add barriers/waits that change overlap semantics without updating PLAN.md.

**Reproducibility + comparability**
- Record: platform metadata, MPI implementation/version, OpenMP env, git SHA, hostname, timestamp.
- Headline sweeps avoid oversubscription: prefer `P*T <= C` (or record oversub explicitly).

---

## Codex instruction pickup (mandatory)
1) Always read **AGENTS.md + PLAN.md** before proposing or executing repo-changing work.
2) If context is missing critical constraints (semantics, threading, metrics), STOP and request a doc refresh/paste.
3) Use Agent Mail thread summaries to regain context across sessions.

---

## Beads-first workflow (required)
1) Work must be tied to a **Beads issue ID**.
   - If none exists: `bd create "..." --type task --priority 2`
2) Claim: `bd update <id> --claim`
3) All coordination uses Agent Mail with:
   - `thread_id = <bd-id>`
   - subject prefix: `[<bd-id>] ...`
4) Close work: `bd close <id> --reason "..."` and sync `.beads/` if your repo config requires it.

**Rule:** no work expected to take > ~2 minutes without a Beads issue.

---

## Agent Mail (br-backed) — coordination + file reservations (mandatory for multi-agent)
Principles:
- Use Agent Mail for durable coordination: messages, ACKs, and reservations.
- Reserve files before editing shared/core surfaces (PLAN.md, AGENTS.md, src/**, scripts/**, CMakeLists.txt).

Minimum workflow:
1) Bootstrap session (agent-run, every session):
   - `macro_start_session(human_key="<ABS_REPO_PATH>", program="codex", model="<codex-model>", task_description="<bd-id>: <short task>")`
2) Prepare thread (if continuing):
   - `macro_prepare_thread(project_key="<ABS_REPO_PATH>", thread_id="<bd-id>", agent_name="<your-agent-name>")`
3) Reserve paths before editing:
   - `file_reservation_paths(project_key="<ABS_REPO_PATH>", agent_name="<you>", paths=[...], ttl_seconds=3600, exclusive=true, reason="<bd-id>")`
4) Announce start (ACK required if coordination is needed):
   - `send_message(..., thread_id="<bd-id>", subject="[<bd-id>] Starting", body_md="Reserving: ...", ack_required=true)`
5) When done:
   - `release_file_reservations(project_key="<ABS_REPO_PATH>", agent_name="<you>")`
   - `send_message(..., thread_id="<bd-id>", subject="[<bd-id>] Completed", body_md="Summary + paths + commands")`

### Agent Mail server prerequisite (human-run)
Agent Mail must be running and reachable by Codex via MCP.
- Start via your `br`-backed workflow (whatever you use locally to launch Agent Mail),
  or a repo script if provided (e.g., `./scripts/run_server_with_token.sh`).
If the mail tools/resources are unavailable inside Codex, STOP and ask the human to start/fix the server.

---

## RULE 1 — ABSOLUTE SAFETY INVARIANT (DO NOT VIOLATE)
You may NOT delete, overwrite, or irreversibly modify files/data unless the user provides the exact command and explicit approval in the same session.

Forbidden without exact user command (examples):
- `rm -rf`, `rm -r`, `unlink`
- `git reset --hard`, `git clean -fd`, `git restore --source`
- `git push --force` / `--force-with-lease`
- overwriting redirection: `>` / `2>` / `tee` without `-a` when it can clobber files
- destructive cleanup of benchmark outputs (`results/`, `traces/`) if they are the only copy

## Generated files (never edit manually)
- `results/` and `traces/` are generated outputs. Prefer regeneration over hand-editing.
- If the repo decides to commit any generated artifacts, document the generator command adjacent (README or script).

If a destructive command is executed, record in the response:
- Exact user text authorizing it
- Exact command run
- When it was run (timestamp)

---

## Git version control rules (required)

### Safe default workflow (always)
1) Inspect before edits:
   - `git status`
   - `git diff` (and `git diff --staged` when staging)
2) Keep diffs small and reviewable. Prefer multiple small commits over one massive commit.
3) If Beads exports are tracked in this repo:
   - ensure `.beads/` changes are committed in the same logical change set as code changes.
4) Prefer non-destructive recovery:
   - `git stash push -u -m "<bd-id>: wip"` (instead of resets/cleans)
   - create a backup branch tag for safety if needed: `git branch backup/<name>`

### IRREVERSIBLE / HISTORY-DESTROYING GIT ACTIONS (forbidden without exact user command)
Absolutely forbidden unless the user gives the **exact command and explicit approval** in the same session:
- `git reset --hard`
- `git clean -fd` (or `-fdx`)
- `git checkout -- <path>` / `git restore --source ...` (can discard work)
- `git reflog expire` / aggressive `git gc` (can make recovery harder)
- deleting branches: `git branch -D ...` or remote deletions
- force pushes: `git push --force` / `--force-with-lease`

Rules:
1) If you are not 100% sure what a command will discard/overwrite, do not propose or run it. Ask first.
2) Prefer safe tools: `git status`, `git diff`, `git stash`, or copying to backups.
3) After approval: restate the command verbatim, list what it will affect, and wait for confirmation.
4) When a destructive command is run, record in your response:
   - the exact user text authorizing it
   - the command run
   - when you ran it (timestamp)
If that audit trail is missing, treat the operation as not performed.

### Branching discipline (recommended)
- Prefer feature branches for non-trivial work:
  - naming: `bd-<id>-<short-slug>` (example: `bd-42-nb-test-mode`)
- Avoid long-lived branches with mixed concerns; split by Beads issues when possible.
- Never delete branches (local or remote) without explicit user approval.

### Commit hygiene (recommended)
- Include Beads ID in commit messages when applicable:
  - example: `Add phase_persist mode (bd-42)`
- Do not “drive-by reformat” unrelated code; benchmark diffs must remain attributable.
- Don’t commit huge generated artifacts accidentally:
  - treat `build/`, large `traces/`, and bulk `results/` as intentionally managed outputs per repo policy.

### Pushing policy (default safe mode)
- Do **not** push to any remote unless the user explicitly asks in this session.
- If asked to push, use this sequence:
  1) `git status`
  2) run the repo’s quality gates (build + minimal sanity run if code changed)
  3) `git add -p` (prefer staging hunks)
  4) `git commit -m "..."` (include Beads ID when relevant)
  5) `git pull --rebase` (only if you understand the impact; ask if conflicts arise)
  6) `git push`
  7) `git status` must show clean working tree

---

## Tooling rules (C++ benchmark)
- Build system: **CMake** (preferred generator: Ninja)
- Language standard: C++17 or C++20 (match PLAN.md)
- Parallel deps: MPI + OpenMP
- Do not add new third-party libs/submodules without explicit user approval.
- Prefer small, explicit diffs; avoid large refactors unless requested.

Recommended commands (examples; adapt to environment):
- Configure: `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release`
- Build: `cmake --build build`
- Run: `mpirun -np <P> ./build/phasegap --mode ...`
- Threads: set `OMP_NUM_THREADS=<T>` in the run harness
- Optional affinity (record if used): `OMP_PROC_BIND`, `OMP_PLACES`, `OMP_SCHEDULE`

---

## Proposed directory structure (PhaseGap)
Keep the repo layout simple and benchmark-oriented. Generated outputs go in `results/` and `traces/` only.

```text
phasegap/
├── AGENTS.md
├── PLAN.md
├── README.md
├── CMakeLists.txt
├── cmake/                    # (optional) toolchain/helpers, Find modules, etc.
├── include/                  # public headers (if the project grows beyond a single TU)
├── src/
│   ├── main.cpp
│   ├── cli/                  # argument parsing, config struct
│   ├── mpi/                  # rank topology, neighbor setup, request lifecycle, init_thread
│   ├── omp/                  # OMP helpers (schedule/bind capture, utilities)
│   ├── modes/                # orchestration: phase_nb, phase_blk, nb_test, phase_persist, omp_tasks
│   ├── kernels/              # stencil kernels + work knobs
│   ├── stats/                # timers, quantiles, aggregation, CSV writer
│   └── trace/                # Chrome/Perfetto trace writer
├── scripts/
│   ├── run_matrix.sh
│   ├── (optional) analyze.py
│   └── (optional) netem_on.sh / netem_off.sh   # Linux only
├── results/                  # generated CSV outputs (committed or gitignored per repo policy)
├── traces/                   # generated trace JSON files
├── tests/                    # (optional) correctness/unit tests for kernels/stats
└── .beads/
```

Notes:
- `build/` is out-of-tree and should be gitignored.
- If you add fixtures (e.g., golden CSV schemas), put them under `tests/fixtures/`.

---

## PhaseGap implementation contracts (do not drift)
### Stencil + ghost zones
- Local arrays include ghost zones sized by halo width `H`.
- Interior compute must not read ghost zones.
- Boundary compute must read ghost zones and therefore must occur after comm completion (or verified completion).

### OpenMP structure
Default pattern per iteration (unless PLAN changes):
- One `#pragma omp parallel` region per iteration:
  - `#pragma omp for` interior stencil update
  - `#pragma omp master` performs MPI wait/progress for that mode
  - `#pragma omp for` boundary stencil update

### MPI structure
Default baseline:
- `MPI_Init_thread` and record the provided thread level.
- FUNNELED baseline: only the OpenMP master thread calls MPI routines.
- If experimenting with progress (`MPI_Test*`), parameterize and record polling frequency.

---

## bv protocol (robot-only)
- Start-of-session triage: `bv --robot-triage`
- Dependency-aware plan: `bv --robot-plan`
- Priority/insights: `bv --robot-insights` or `bv --robot-priority`

Rules:
- Use ONLY `--robot-*` flags (bare `bv` is forbidden).
- Treat bv output as authoritative for prioritization signals only.
  Benchmark semantics remain governed by PLAN.md.

---

## Quality gates (minimum)
Before closing an issue with code changes:
- Build in Release mode
- Run a small sanity run (tiny N/H/iters) that:
  - produces a CSV row
  - (if enabled) emits a valid trace file
- Ensure outputs include required metadata fields (git SHA, timestamp, P/T, mode, etc.)

---

## Session completion (required)
Always end sessions with:
- Changes (bullets with file paths)
- Beads updates (IDs + status)
- Commands executed (build/run), including failures if any
- Coordination notes (Agent Mail thread summary posted, reservations released)

Recommended completion template:
- Changes:
  - <file>: <what changed + why>
- Beads:
  - claimed: <id>
  - closed/updated: <id> (<status>)
- Commands:
  - <command> (ok/fail)
- Agent Mail:
  - thread_id: <bd-id>
  - reservations released: yes/no
