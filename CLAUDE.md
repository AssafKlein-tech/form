# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

FORM is a Symbolic Manipulation System for high-energy physics. It reads symbolic expressions from files and performs algebraic transformations.

This branch (`MRmpi`) is a research fork of ParFORM adding an optional **Map-Reduce parallel sort** mode. Goals vs standard ParFORM:
- **Less disk I/O** — reducers deduplicate terms before they reach the master, so no duplicate terms are written to disk across workers.
- **Faster mappers** — mappers only generate and hash-route terms; they do not sort or write to disk themselves.
- **Smaller master merge tree** — master receives from reducers only, not from all workers.

The mode is enabled per-module with `on mapreduce;` in a `.frm` script and activated at runtime with `-r<N>` (N = percentage of workers to assign as reducers). When disabled the code falls back to standard ParFORM behavior.

## Build Commands

```bash
# First-time setup
autoreconf -i
./configure --enable-parform   # MRmpi requires parform

# Build parform
make -C sources parform

# Build all enabled targets
make
```

**`make parvorm` is currently broken** — its source lists are commented out in
`sources/Makefile`, so the link line mangles flags (e.g. `-lz` → `lz: command not
found`) and the rule exits with `Error 127 (ignored)`. For a debug build of
parform, force a rebuild of the normal target with debug flags instead:

```bash
rm -f sources/parform-*.o sources/parform
make -C sources parform CFLAGS="-g -O0"
```

**Configure flags of note:**
- `--enable-parform` — build `parform` (MPI) — required for MRmpi
- `--enable-debug` — build debug variant `parvorm` (see note above — currently broken)
- `--with-zstd` — zstd compression
- `--enable-coverage`, `--enable-profile` — coverage/profiling builds

## Running Tests

```bash
# Full test suite via make
make check

# Quick MRmpi smoke tests (9 MPI processes, uses hostfile)
cd tests/simple_tests && bash runall.sh

# Single MRmpi run
mpirun -np 9 parform small_test_red.frm   # uses 'on mapreduce;' inside the .frm
```

`tests/simple_tests/` contains hand-crafted `.frm` scripts for MRmpi. `tests/iostat/` contains I/O measurement scripts.

After any change to `sort.c` / `parallel.c` / `mpi.c`, the correctness check is to diff the run output against the same `.frm` script with `off mapreduce;` — the two must produce identical final FORM output.

## Production runs (Adquanta cluster)

`runs/Adquanta/` holds the PBS jobs for the real workload. Four siblings:
- `form_spin_test.pbs` — 1 node × 60 ranks, non-MR (`Spin2_h5_45.frm`)
- `form_spin_test_mr.pbs` — 1 node × 60 ranks MR (`Spin2_h5_45_mr.frm`, `-r12`)
- `form_spin_test_4n.pbs` — 4 nodes × 16 ranks (excl), non-MR
- `form_spin_test_mr_4n.pbs` — 4 nodes × 16 ranks MR (`-r12`) — **the winning configuration** (40,778 s wallclock for the v3 round)

Logs go to `tmp/test_spin_<layout>_v[N].{o,e,mem.log}` where `<layout>` ∈ `{org, mr, 4n, mr_4n}` and `[N]` is the round. **v3 is the first complete + correct production round** (after items 2/3/4 + LongMulti broadcast fix + toPolynomial restored). v1/v2 had `toPolynomial onlyfunctions G;` commented out as a workaround for the fixed `MPI_ERR_TRUNCATE` bug — those rounds **skipped polynomial reduction** and produced output in raw `G(...)` form. See the auto-memory `project_spin_runs.md` for the full archive.

**form.set choice depends on whether the heavy sort is MR-active or serial.** Older buffer-tuning sweeps used `bench_compress_mr.frm`, which has `off parallel;` (line 121) before the heavy `.sort:Diagram Loaded` — that runs the sort **serial on the master**, not MR. Buffer tunings derived from that bench tune the wrong code path. The MR-active heavy bench is `bench_compress_mr_keepmr.frm` (with `off parallel;` commented out) or `bench_heavy.frm` (keepmr + `#call dummyindices`). For MR-tuned form.set values from current sweeps see the memory skill ("Reference: what the Spin run actually used").

**toPolynomial + MR gotcha:** if a `.frm` has `on mapreduce;` and uses `toPolynomial onlyfunctions`, you MUST add `off mapreduce;` (alongside `off parallel;`) before the procedure that calls toPolynomial. Otherwise `parallel.c:1705` fires "ERROR: Calling Map Reduce without parallel" — toPolynomial forces the module non-parallel for its poly arithmetic, and the lingering `sMRflag = MAPREDUCE` trips the guard. `Spin2_h5_45_mr.frm` line 152 has the canonical fix.

**form.set lookup gotcha:** [tools.c:580](sources/tools.c#L580) opens `./form.set` from CWD before honoring `-S`. If the run dir has a tform-tuned `form.set`, parform reads it and OOMs (~25 GB/rank floor from the 96 M sortiosize). Either rename the tform file aside (`form.set.tform`) or write parform values directly into `./form.set`.

**Multi-node MR rank-placement gotcha:** [parallel.c:1666](sources/parallel.c#L1666) assigns reducers as the **highest-numbered ranks** (`role = (PF.me < PF.nummappers) ? MAPPER : REDUCER`). With OpenMPI's default block-fill mapping over `$PBS_NODEFILE`, that puts **all reducers on the last node** — which then carries 100% of the sort-side disk I/O, RSS, and mapper-fan-in NIC traffic while the other nodes do only mapping. Any multi-node MR `.pbs` MUST pass `--map-by node` to mpirun so ranks round-robin across hosts and reducers spread evenly. Already wired into `form_spin_test_mr_4n.pbs`, `form_spin_test_mr_4n_2x.pbs`, and `form_spin_test_4n.pbs` (latter for parity). **Pre-v5 multi-node MR wallclocks are biased toward "one hot node" and don't measure true scaling.** Proper fix is to make reducer rank selection stride-based in parallel.c:1666 — not done yet.

Cluster-side env vars (set in the PBS jobs):
- `FORMTMP=/gtmp` — local tmp, NOT NFS — sort files must not go to NFS or the run dies on I/O.
- `FORM_IGNORE_DEPRECATION=1` — silences ParFORM deprecation warning.

**Transport: RDMA (UCX/InfiniBand) is the default for all PBS scripts as of 2026-05-09.** TCP-BTL was retired because at ≥30 ranks/node it crashed repeatedly with MPI "Socket closed" errors (verified with three independent trials at 32 ranks/node × 4 nodes). At 16 ranks/node TCP works but RDMA matches it within noise, so all production scripts use one config. The required stanza:

```sh
module load openmpi/5.0.3-pbs   # the only OMPI 5 module on Zeus with UCX
export LD_LIBRARY_PATH=/usr/local/openmpi-5.0.3-pbs/lib:$LD_LIBRARY_PATH

export OMPI_MCA_pml=ucx
export OMPI_MCA_pml_ucx_priority=100
export OMPI_MCA_osc=ucx
export OMPI_MCA_btl='^tcp,openib'        # crash loudly if UCX init fails
export UCX_TLS=dc_mlx5,sysv,self         # DC for the mapper→reducer fan-in; sysv (NOT posix shm) for intra-node
export UCX_NET_DEVICES=mlx5_0:1
export UCX_RNDV_THRESH=256k              # push MR shuffle chunks to rendezvous (frees the eager pool; ≈ 64m on wallclock)
export UCX_ZCOPY_THRESH=1024
```

**`UCX_TLS` must use `sysv` not `sm` (= POSIX shm).** Node `n099` has a
broken `/dev/shm` — `shm_open(... O_CREAT)` fails with `Permission denied`, so
any `UCX_TLS` containing `sm` (which prefers POSIX shm) makes every rank on
that node abort at `MPI_Init` with "Failed to create UCP worker" → the whole
job dies in <1 s. `sysv` (System-V shared-memory segments) gives the same
intra-node fast path and works on n099 too, so it's the safe default
everywhere. **Do NOT add `cma`** — Cross-Memory-Attach is not compiled into
this UCX build, so `UCX_TLS=...,cma,...` is silently dropped and just spams one
"transport 'cma' is not available" warn line per rank. See
`project_ucx_thin_alloc.md`. `UCX_RNDV_THRESH` was lowered 64m→256k after the
rndv comparison showed them wallclock-equivalent; 256k keeps the eager
descriptor pool small (avoids the `mm_recv_desc`/`rc_recv_desc` chunk-alloc OOM
seen at high rank counts).
*The 6 keeper spin PBS (`form_spin_test_mr_4n_{r9,r12,r15,32r,cores_os,cores_prog}.pbs`)
use the `dc_mlx5,sysv,self` / `256k` stanza as of 2026-05-12. Some surviving
`bench_*` / `form_spin_test{,_mr,_4n}` siblings still carry the old
`dc_mlx5,sm,self` / `64m` values — fix when next touched.*

Three TCP-only files are kept as comparison artifacts: `bench_heavy_tcp.pbs`, `bench_keepmr_128_tcp.pbs`, `verify_keepmr_recreated.pbs`. Don't add TCP-only scripts beyond those. See `project_rdma_findings.md` for the full empirical record.

## Runtime tuning (MRmpi)

| Env var | Default | Cap | Purpose |
|---|---|---|---|
| `PF_SBUFS` | 2 | 10 | Cyclic send-buffer slots per (mapper, destination) |
| `PF_RBUFS` | 2 | 2 | Cyclic receive-buffer slots per (reducer, source mapper) — see note below |
| `PF_MERGERS` | 0 | min(M−1, R) | Mapper-merger tier — `G` mapper ranks (1..G) also merge reducer groups (0 = off). See "MRmpi reducer-merge tier" below. |
| `PF_LOG` | 0 | — | ParFORM logging verbosity |
| `PF_STATS` | 10 | — | Stats interval |

Caps and parsing live in [sources/parallel.c:2409-2422](sources/parallel.c#L2409). **Gotcha:** `PF_RBUFS` clamps silently — no warning if you set it above the cap.

**`PF_RBUFS` cap is 2 by design ([sources/parallel.c:2425](sources/parallel.c#L2425)):**

The effective queue depth is 1 per (reducer, source) regardless of value: `PF_ReducerInit` and `PF_InitTree` pre-post only the `active=0` slot, and `PF_StoreBuffer` / `PF_PutIn` keep depth at 1 by posting `next` inside the consume step. Setting `PF_RBUFS > 2` would not add real pipelining — the chunk transfer is bandwidth-limited (chunks above the 1 MB OpenMPI eager limit go through rendezvous) and the receiver memcpy is ≥10× faster than network transfer, so concurrent CTS handshakes share the same TCP link with zero added throughput. Production `tmp/test_spin_mr_v3.o` confirms zero `Wait time for Reducers` across all mappers under PF_SBUFS=3, PF_RBUFS=2; the multi-node case [tmp/test_spin_mr_4n_v3.o](tmp/test_spin_mr_4n_v3.o) shows ≤0.5% mapper stall (cross-node TCP-bandwidth bound, not match-latency bound).

`PF_RBUFS=3` was historically buggy: `PF_InitTree:424` only IRecv'd the active slot, and `PF_PutIn:589`'s `newterms` branch waited on slot `next` without arming it. The 0→1→2→0 cycle hit unarmed slot 2 on the 2nd wrap → `MPI_Get_count` on uninitialized `type[2]` → `MPI_ERR_TYPE`. The cap-clamp at line 2425 makes that path unreachable. If a future workload ever needs deeper queueing (e.g. PF_SBUFS raised >2, or interconnect changes from TCP to RDMA), reopen with the multi-deep IRecv design (pre-post all slots in InitTree+ReducerInit, restructure consume/re-arm in PutIn+StoreBuffer, MPI_Cancel cleanup at ENDSHUFFLE/ENDBUFFER) — see [/home/assafklein/.claude/plans/eager-cuddling-wreath.md](.claude/plans/eager-cuddling-wreath.md) for the deferred plan.

See the sort skill for what each buffer actually does.

## Profiling / finding bottlenecks

For any "where is time going / which knob next" question on a parform run — MR or non-MR — use the built-in per-phase profiler. **Full reference is in [.claude/skills/profile/SKILL.md](.claude/skills/profile/SKILL.md)** (auto-loaded when you ask to profile, find bottlenecks, or compare MR vs org).

Quick path:

```bash
# 1. Build a profile binary (one-time, then redo only on source edits)
module load openmpi/5.0.3-pbs
export LD_LIBRARY_PATH=/usr/local/openmpi-5.0.3-pbs/lib:$LD_LIBRARY_PATH
autoreconf -i
./configure --enable-parform --enable-mr-profile
make -C sources parform
cp sources/parform ~/bin/parform.profile

# 2. Run your job with parform.profile (PBS or local) -- see the skill for env wiring.
#    The master writes ${PF_PROFILE_DIR:-.}/pf_profile.csv with one row per (module, rank).

# 3. Visualize
python3 scripts/pf_profile_viz.py <run_dir>                    # single run
python3 scripts/pf_profile_viz.py <mr_dir> <org_dir>           # MR vs org compare
```

The `--enable-mr-profile` flag is gated by `#ifdef PF_PROFILE`. In production builds without it, every timer macro expands to `((void)0)` and `pf_profile.c` compiles to nothing — zero overhead. Only the MR-active announcement at [parallel.c:1715](sources/parallel.c#L1715) stays in production; the master "finished sending terms" line, the per-rank wait/working summary, and `[N|module] Endsort,Collect,Broadcast done` are all gated behind `PF_PROFILE`.

The profiler captures **MR and non-MR runs with the same CSV schema**, so the visualization's compare mode lines them up directly. Reducer-specific phases (`RED_*`) are zero on non-MR rows; mapper phases (`MAP_*`) cover slave→master sort traffic in non-MR mode. The visualization includes a **decision matrix** that prints the recommended next optimization (knob or code change) given the observed bottleneck pattern. See the skill for the full rule list and the iostat-style `disk_util_pct` derivation.

## Reference: skills

Skill files in `.claude/skills/` carry deep reference material — auto-loaded by topic:

- `.claude/skills/sort/SKILL.md` — full sort-pipeline reference (regular FORM / TFORM / ParFORM / MRmpi). Read this BEFORE touching `sort.c`, `parallel.c`, or `mpi.c`.
- `.claude/skills/term/SKILL.md` — FORM term memory layout, coefficient extraction, delta compression wire format. Read this when reading or writing term bytes (hash routing, compression, scratch-file format).
- `.claude/skills/profile/SKILL.md` — per-phase profiler (`--enable-mr-profile`) for MR or non-MR parform runs. Read this when asked to profile a job, find bottlenecks, decide which knob to tune next, or compare MR vs org.

## Architecture

### Expression Pipeline

1. **Parsing** — `compiler.c` compiles the FORM language; `pre.c` handles the preprocessor; `token.c` tokenizes input.
2. **Storage** — `store.c` persists expressions to disk.
3. **Pattern matching / Transformation** — `pattern.c`, `findpat.c`, `execute.c`, `transform.c`.
4. **Sorting** — `sort.c` is performance-critical; canonical ordering is central to the system.
5. **Normalization** — `normal.c` + `comexpr.c`; `compcomm.c` handles common sub-expression collection (largest file, ~200 KB).

Core structs are in `sources/structs.h` (~3000+ lines). Key type aliases (`WORD`, `LONG`, `ULONG`) are in `sources/ftypes.h`.

### MRmpi Map-Reduce Extension

The map-reduce mode is controlled by three flags in `AC` (`structs.h`):
- `MRflag` — global setting (`on/off mapreduce;` in FORM script, parsed in `compcomm.c`)
- `mMRflag` — per-module effective flag
- `sMRflag` — state machine: `NO_MAPREDUCE` / `MAPREDUCE_FIRST` / `MAPREDUCE` / `MAPREDUCE_LAST` (constants in `ftypes.h`)



Key per-role code paths:
- **Mapper (`sort.c` `PF_LowMRsort`)** — terms are hashed and routed to a reducer via `PF_WISendSbuf` instead of being written to the local sort file. Each mapper holds a per-reducer send buffer (`PF.sbufs[dest]`).
- **Reducer (`parallel.c` `PF_StoreBuffer` / `PF_ReducerInit`)** — receives terms from all mappers, stores patches to its local sort buffer, runs a standard merge sort, then forwards the sorted stream to the master.
- **Master** — runs the merge tree as usual but receives only from reducers (or from mergers when the merge tier is on), shrinking the fan-in.
- **Merger (`parallel.c` `PF_MergerLoop`)** — optional; see "MRmpi reducer-merge tier" below.

New MPI message tags (`parallel.h`):
- `PF_SHUFFLE_MSGTAG` (110) — mapper → reducer: term data
- `PF_ENDSHUFFLE_MSGTAG` (111) — mapper → reducer: end of data
- `PF_ENDSHUFFLEALL_MSGTAG` (112) — signals all shuffling is complete

`-r<N>` CLI flag (parsed in `startup.c`) sets `AM.ReducerPer`.

### MRmpi reducer-merge tier (mapper-as-merger)

An optional second merge layer between reducers and the master, enabled with
`PF_MERGERS=G` (env, default 0 = off). The lowest `G` mapper ranks (`1..G`)
overlay a **merger** role: after finishing their mapper phase they each consume
the sorted streams of a group of leaf reducers, merge them, and forward one
combined stream to the master — shrinking the master's loser-tree fan-in from
`R` reducers to `G` mergers.

- Leaf reducer `r` → merger `1 + ((r − M) % G)` (round-robin; no node-locality
  yet — a node-local placement is planned in `.claude/plans/node-local-mergers.md`).
- The merger **reuses the master's `PF_EndSort` merge body**: `PF.in_merger_phase`
  gates all merger-specific routing, `pf_loser_src_to_rank()` is the single
  src→MPI-rank rule, `PF_MergerLoop()` (`parallel.c`) is the ~50-line wrapper.
- The K-prefix drain runs inside the merger phase too — `pf_emit_compressed_bulk`
  MPI-sends the merged run upstream instead of `WriteFile`-to-disk.

**`PF_InitTree` rbuf-array gotcha:** `PF.rbufs` is allocated once and cached
across modules, so it must be sized to the rank's *maximum* `numtasks`
(`alloc_numtasks = PF.is_merger ? numtasks : PF.numtasks`) — not the per-module
value. A parallel non-MR module needs all `nummappers` leaves even after MR
modules sized the master's tree to `nummergers+1`; getting this wrong overflows
the cached array and hangs the run (cost an 11.5 h hung Spin job).

**Status:** Phase 1 (the merger role) is committed on `MRmpi`; the drain-on-merger,
the `PF_InitTree` fix, and the `MER_*` profiler phases are **uncommitted** on the
working tree. Binary: `~/bin/parform.mergerdrain`. Full reference: the sort skill
and `project_merger_wip.md`.

### Parallel Processing Files

- `mpi.c` — MPI communication; `PF_WISendSbuf` routes sends to reducer or master
- `parallel.c` + `parallel.h` — full MRmpi role dispatch loop; `PARALLELVARS` struct holds `nummappers`, `numreducers`, `nummergers`, `sbufs`; `PF_MergerLoop` (merger tier)

## Key Files Quick Reference

| File | Role |
|------|------|
| `sources/structs.h` | All core data structures (includes `MRflag`, `ReducerPer`) |
| `sources/ftypes.h` | FORM type aliases + MRmpi state flag constants |
| `sources/startup.c` | `main()`, command-line parsing, `-r<N>` flag |
| `sources/compcomm.c` | `on mapreduce;` keyword handling; per-module flag logic |
| `sources/execute.c` | `sMRflag` state machine transitions between modules |
| `sources/sort.c` | Sorting; `PF_LowMRsort()` decides mapper vs normal path |
| `sources/parallel.c` | Master/mapper/reducer/merger dispatch; `PF_StoreBuffer`, `PF_ReducerInit`, `PF_MergerLoop` |
| `sources/parallel.h` | `PARALLELVARS` struct; new MPI tags |
| `sources/mpi.c` | `PF_WISendSbuf` — routes sends to reducer or master |
| `sources/form3.h` | Master include, platform abstractions |
| `tests/simple_tests/` | MRmpi test scripts |
| `configure.ac` | Autoconf build configuration |
