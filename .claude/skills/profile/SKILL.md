---
name: profile
description: Reference for the per-phase parform profiler (`--enable-mr-profile`). Use when the user asks to profile a run, find bottlenecks, decide which knob to tune, or compare MR vs non-MR. Covers building the profile binary, writing PBS jobs that run it, reading the CSV, the visualization tool, and the decision matrix.
---

This codebase has a built-in per-phase profiler for parform that captures
where every rank spent its time at module granularity, with `iostat`-style
disk %util on the FORMTMP device. It is the recommended starting point for
any "where is time going / which knob next" question.

The profiler works for both **MR** (`on mapreduce;`) and **non-MR** parform
runs — the same binary, same CSV schema. This is what enables direct
side-by-side comparison.

## What it captures

Per (module, rank) row in `pf_profile.csv`:

- 9 wall-clock phase timers (microseconds, via `MPI_Wtime()`):
  - **Mapper:** `MAP_GENERATOR` (Generator loop), `MAP_ENDSORT_TOTAL`
    (mapper's EndSort = hash + pack + send), `MAP_SEND_WAIT`
    (`MPI_Wait` blocked on send-buffer drain), `MAP_SEND_MPI` (`MPI_Isend`).
    In non-MR mode `MAP_SEND_*` covers slave→master sort traffic.
  - **Reducer (MR only):** `RED_RECV_WAIT`, `RED_MERGE_PATCHES`,
    `RED_FINAL_SORT`, `RED_FORWARD_WAIT`, `RED_FORWARD_MPI`.
  - **Master:** `MAS_DISTRIBUTE`, `MAS_DISTRIBUTE_WAIT` (`PF_Wait4Slave`),
    `MAS_FINAL_SORT`, `MAS_COLLECT`.
- Counters: `bytes_sent`, `bytes_to_master`, `terms_sent`, `patches_built`.
- `/proc/self/io` diff: `io_rchar`, `io_wchar`, `io_read_bytes`,
  `io_write_bytes`, `io_syscr`, `io_syscw`.
- `getrusage`: `maxrss_kb`, `minflt`, `majflt`, `nvcsw`, `nivcsw`.
- Per-node disk (one row per node-leader): `node_disk_time_in_io_ms`,
  `node_wallclock_us` from `/proc/diskstats` for the FORMTMP device.
  `disk_util_pct = node_disk_time_in_io_ms × 1000 / node_wallclock_us × 100`
  matches what `iostat -x` reports as `%util`.

Two phases are computed by the viz via subtraction:
`MAP_HASH_PACK = MAP_ENDSORT_TOTAL − MAP_SEND_WAIT − MAP_SEND_MPI` and
`RED_STORE = wallclock − everything else`.

## Build a profile binary

```bash
module load openmpi/5.0.3-pbs
export LD_LIBRARY_PATH=/usr/local/openmpi-5.0.3-pbs/lib:$LD_LIBRARY_PATH
autoreconf -i                                    # only if you've edited build files
./configure --enable-parform --enable-mr-profile
make -C sources parform
cp sources/parform ~/bin/parform.profile         # convention: matches ~/bin/parform.* siblings
```

The profile build composes with `-O2` (default) — overhead is well under
0.001% of wallclock (no `MPI_Wtime` in any per-term loop; coarse brackets
only, finer phases derived by subtraction). A production parform built
without `--enable-mr-profile` is 100% byte-equivalent to non-profile builds
elsewhere in the project.

## Run a job with profiling on

Profile builds need only one extra knob beyond a normal PBS:

```bash
export PF_PROFILE_DIR=/home/assafklein/form/tmp   # or wherever; default is "."
rm -f "$PF_PROFILE_DIR/pf_profile.csv"            # avoid appending to stale data
```

Multi-node parform on Zeus REQUIRES the UCX/RDMA wiring (the default
`openmpi` module's TCP wireup hangs across nodes — see the
`project_cluster_mpi` memory):

```bash
module load openmpi/5.0.3-pbs
export LD_LIBRARY_PATH=/usr/local/openmpi-5.0.3-pbs/lib:$LD_LIBRARY_PATH
export OMPI_MCA_pml=ucx
export OMPI_MCA_pml_ucx_priority=100
export OMPI_MCA_osc=ucx
export OMPI_MCA_btl='^tcp,openib'
export UCX_TLS=rc,sm,self
export UCX_NET_DEVICES=mlx5_0:1
```

Single-node profile runs also work with the default `openmpi` +
`pml=ob1, btl=self,sm` wiring — UCX is only needed for cross-node.

The canonical reference PBS is
[runs/Adquanta/smoke_pf_profile.pbs](runs/Adquanta/smoke_pf_profile.pbs).
For longer Spin-style runs, copy `form_spin_test_mr_4n.pbs` (or its
non-MR sibling), swap in `parform.profile`, add the env block above, and
add `export PF_PROFILE_DIR=...; rm -f ...`.

## Verify the run

After the job returns, three checks (all should pass):

```bash
# 1. Per-node leader detected on every host (one line per host)
grep "pf_profile: tracking" "$PBS_O_WORKDIR/<jobname>.e"
# expect:  [0] pf_profile: tracking disk M:N (/gtmp)  ... etc

# 2. CSV exists and has rows for all ranks * all modules
wc -l "$PF_PROFILE_DIR/pf_profile.csv"

# 3. Disk %util is non-zero on at least one node-leader row
awk -F, 'NR>1 && $37!="-1" && $38>0 {printf "rank=%s mod=%s util=%.2f%%\n",
        $4, $2, ($37*1000.0/$38)*100}' "$PF_PROFILE_DIR/pf_profile.csv"
```

If `tracking disk` says a path other than `/gtmp` (e.g. it landed on
`.` or `/tmp`), something earlier in FORM's tempdir priority chain is
overriding `FORMTMP`: probably a stray `tempdir=` in `./form.set` or a
`-T` flag on `parform`. The chain is `-T` → `form.set tempdir/tempsortdir`
→ `FORMTMPSORT` → `FORMTMP` → `"."`. See [startup.c:740-752](sources/startup.c#L740).

## Visualize

```bash
python3 -m pip install --user pandas plotly       # one-time
python3 scripts/pf_profile_viz.py <run_dir>       # single run
python3 scripts/pf_profile_viz.py <mr_dir> <org_dir>   # MR-vs-org compare
```

Produces `pf_profile_report.html` (or `pf_profile_compare.html`) in the
run dir — self-contained interactive plotly with five panels:

- **Phase breakdown** — stacked bar per module, faceted mapper/reducer/master
- **Per-rank Gantt** — horizontal stack per rank for a chosen module
- **Imbalance heatmap** — rank × phase, total time across all modules
- **OS counters** — bytes_written, ctxt switches, MaxRSS, disk %util
- **Decision matrix** — flags the rule(s) the run matches and prints the
  recommended next knob to tune (see below)

To open the HTML on a remote VS Code session: install the **Live Preview**
extension and right-click → Show Preview. Or `python3 -m http.server` and
open the URL via VS Code's Simple Browser. Or `scp` the file to your
laptop and open in any local browser — the HTML is self-contained.

## Decision matrix (what to do with the data)

The viz computes per-module dominant patterns and prints a table mapping
each to the recommended next optimization. The rules are in
[scripts/pf_profile_viz.py:DECISION_MATRIX](scripts/pf_profile_viz.py).
Cheat-sheet (paraphrased):

| Pattern | Knob first |
|---|---|
| Mappers high `MAP_SEND_WAIT`, reducers low `RED_RECV_WAIT` | Increase `-r<N>` (more reducers) |
| Mappers low `MAP_SEND_WAIT`, reducers high `RED_RECV_WAIT` | Decrease `-r<N>` (more mappers) |
| Reducers high `disk_util_pct` (>75%) | Increase `largesize` / `smallext` in `form.set` |
| Reducers `RED_MERGE_PATCHES > RED_FINAL_SORT` AND disk-bound | Increase `filepatches` |
| Mappers `MAP_HASH_PACK` dominates | Code: hash kernel review |
| Master `MAS_FINAL_SORT` dominates AND reducers idle | More reducers OR widen master merge tree |
| Master `MAS_DISTRIBUTE_WAIT` dominates | Bigger `mProcessBucketSize` |
| Any role: `nivcsw` > 50 | OS oversubscription — fewer ranks per node, `--bind-to core` |
| Reducer `RED_FINAL_SORT` variance > 30% | Hash skew |
| MR slower than org despite less I/O | Reduce reducer % OR check `PF_SHUFFLE_NOCOMPRESS` |

These are heuristics. Threshold values are guesses; treat them as starting
points and adjust after a few real runs.

## Files (where to look / edit)

- [sources/pf_profile.h](sources/pf_profile.h) — phase enum, timer macros,
  `PF_OSCounters` struct, prototypes. **Add a new phase here.**
- [sources/pf_profile.c](sources/pf_profile.c) — OS snapshot,
  `/proc/diskstats` lookup, CSV writer, node-leader split.
- [sources/parallel.c](sources/parallel.c) — `PF_Processor` brackets all
  master/mapper/reducer phases; `PF_StoreBuffer` brackets `RED_RECV_WAIT`
  and `RED_MERGE_PATCHES`; `PF_ForwardTermsToMaster` brackets
  `RED_FINAL_SORT`. Stat aggregation extends the existing
  `PF_LongSinglePack` chain inside `#ifdef PF_PROFILE`.
- [sources/mpi.c](sources/mpi.c) — `PF_ISendSbuf` brackets `SEND_WAIT`/
  `SEND_MPI`. Attribution: MR mapper→reducer and non-MR slave→master are
  both `MAP_SEND_*`; only MR reducer→master is `RED_FORWARD_*`.
- [scripts/pf_profile_viz.py](scripts/pf_profile_viz.py) — visualization
  script. `DECISION_MATRIX` is the rule list.
- [scripts/run_with_iostat.sh](scripts/run_with_iostat.sh) — optional
  sidecar that runs `iostat -x 1` per node alongside `mpirun` for
  cross-checking the per-node `%util` numbers.
- [runs/Adquanta/smoke_pf_profile.pbs](runs/Adquanta/smoke_pf_profile.pbs)
  — reference PBS for a 2-node × 5-rank smoke (~30 s, queues fast).

## Adding a new phase

1. Add `PF_PHASE_<NAME>` to the enum in [sources/pf_profile.h](sources/pf_profile.h).
2. Add `PF_TIMER_BEGIN(<NAME>)` / `PF_TIMER_END(<NAME>)` around the code
   region in `parallel.c` / `mpi.c` / `sort.c`. **Coarse brackets only**
   — never inside a per-term loop.
3. Add a column name to the CSV header in
   `pf_profile_dump_master_csv` and an `fprintf` slot in
   `write_csv_row` (both in [sources/pf_profile.c](sources/pf_profile.c)).
4. Add the column tuple to `PHASE_COLS` in
   [scripts/pf_profile_viz.py](scripts/pf_profile_viz.py) so the viz
   picks it up.
5. Rebuild, smoke, verify the new column populates.

## Common pitfalls

- **CSV is appended, not overwritten.** `pf_profile_dump_master_csv` opens
  with `fopen("a")`. Always `rm -f $PF_PROFILE_DIR/pf_profile.csv` before
  a fresh run, or `pf_profile.csv` will accumulate rows from previous
  runs and the viz will plot a mix.
- **The legacy "Mapper [N]: Send time X" / "Reducer [N]: Sort time Y"
  prints only fire under `#ifdef PF_PROFILE`.** Production builds drop
  them — only the `We have N mappers and M reducers` line at
  [parallel.c:1715](sources/parallel.c#L1715) stays in production.
- **Node-leader detection.** `MPI_Comm_split_type(MPI_COMM_TYPE_SHARED)`
  picks one rank per host. With `--map-by node` round-robin, that's
  usually rank 0 on each host. With block-fill, the lowest global rank on
  each host. For **multi-node MR** runs always pass `--map-by node` (and
  see the `project_mr_rank_placement` memory).
- **`disk_util_pct` is per-node, not per-rank.** Only one row per module
  per node has `node_disk_*` populated; others are `-1`. The viz handles
  this (filters to leader rows for the disk panel).
