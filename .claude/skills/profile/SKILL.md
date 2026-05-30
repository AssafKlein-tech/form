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

## CRITICAL: pf_profile measures only the paths it wraps — when to switch to VTune

**The PF_TIMER phases only cover code that was explicitly wrapped. A
bottleneck in an unwrapped path is silently mis-charged to whatever
wrapped phase encloses it.** This bit hard on Spin:

- pf_profile reported the mapper "generation-bound, GEN_OTHER = 74% of
  Generator." Two added instrumentation passes (`parform.mapperprof`,
  `parform.mapperprof2`) drilled the residual but never explained it.
- VTune sampling then showed the truth: **~30% of mapper CPU was
  `PMPI_Ssend`** — the end-of-module stats-collection barrier. The
  `MAP_SEND_*` timers never saw it (they wrap the `MPI_Isend` shuffle at
  mpi.c:456, NOT the synchronous Ssend at mpi.c:432). That blocking time
  was charged to `MAP_GENERATOR` because the buffer flush fires inside
  StoreTerm inside the per-term loop → the phantom "GEN_OTHER."

**Rule:** pf_profile is reliable for the *relative split across the phases
it wraps* (mapper send-wait vs reducer recv-wait vs master merge) and for
the decision-matrix knob picks. It is NOT a trustworthy whole-of-CPU
breakdown. If a residual (GEN_OTHER or any phase) is large, or you suspect
MPI-wait is mischarged, **switch to VTune sampling** — it sees every
symbol (incl. libmpi/kernel) with no "did I wrap it" blind spot. Do not
keep adding PF_TIMER passes to chase a residual.

Full VTune recipe + Zeus gotchas: auto-memory **`project_vtune_setup`**
and **`project_vtune_mapper_hotspots`**. One-liners:
- perf is absent on all Zeus compute nodes; VTune 2024.2 is the only
  sampling profiler. The `module load vtune` is broken — the wrapper
  `runs/Adquanta/vtune_wrap.sh` uses the absolute binary
  `/usr/local/intel_oneapi2024/vtune/2024.2/bin64/vtune`.
- must pass `-knob sampling-mode=sw` (no sep driver, paranoid=2 → no HW
  sampling); add `-knob enable-stack-collection=true` for caller chains.
- `vtune_wrap.sh` profiles only `PERF_TARGET_RANK` (default 1, a mapper);
  all other ranks exec parform directly. Result dir lands in
  `$PF_PROFILE_DIR` as `vtune_hotspots_rank1_<host>` — glob it.
- report: `$VT -report hotspots -r <dir> -format csv -csv-delimiter ';'`
  (parse defensively, rows ragged); `-report gprof-cc` for callers.
- **production runs must use a non-PF_PROFILE binary** — the profiler
  build cost ~16-18% wallclock (clock_gettime + PF_LongSingleSend bloat).

## What it captures

Per (module, rank) row in `pf_profile.csv`:

- 9 wall-clock phase timers (microseconds, via `MPI_Wtime()`):
  - **Mapper:** `MAP_GENERATOR` (Generator loop), `MAP_ENDSORT_TOTAL`
    (mapper's EndSort = hash + pack + send), `MAP_SEND_WAIT`
    (`MPI_Wait` blocked on send-buffer drain), `MAP_SEND_MPI` (`MPI_Isend`).
    In non-MR mode `MAP_SEND_*` covers slave→master sort traffic.
  - **Reducer (MR only):** `RED_RECV_WAIT`, `RED_BUFFER_COPY`
    (term-by-term memcpy of an arrived buffer into the sort patch),
    `RED_MERGE_PATCHES`, `RED_FINAL_SORT`, `RED_FORWARD_WAIT`,
    `RED_FORWARD_MPI`.
  - **Master:** `MAS_DISTRIBUTE`, `MAS_DISTRIBUTE_WAIT` (`PF_Wait4Slave` —
    master idle for a *mapper* to be ready for the next bucket),
    `MAS_FINAL_SORT` (the whole `EndSort` merge tree), `MAS_MERGE_RECV_WAIT`
    (`MPI_Wait` inside `PF_PutIn` — master blocked mid-merge waiting for a
    child's next sorted chunk — a reducer's, or a merger's when the merge
    tier is active; this is the real "master waited for its children"
    number — it is a **sub-component of `MAS_FINAL_SORT`, not additive**
    with it), `MAS_COLLECT` (end-of-module `PF_LongSingleReceive`
    stats loop — effectively the wait for the last rank to finish and report).
  - **Merger (MR, only when `PF_MERGERS>0`):** `MER_MERGE` (the whole
    `EndSort` merge pass `PF_MergerLoop` runs over a group of leaf
    reducers — the merger analogue of `MAS_FINAL_SORT`), `MER_RECV_WAIT`
    (`MPI_Wait` inside `PF_PutIn` blocked for a leaf reducer's next chunk —
    **sub-component of `MER_MERGE`, not additive**), `MER_FORWARD_WAIT`,
    `MER_FORWARD_MPI` (forwarding the merged stream to the master). A
    mapper-merger is a mapper rank (1..G) that runs the full mapper phase
    *then* the merge, so its CSV row carries both the `MAP_*` and the
    `MER_*` timers. The viz tags it `role=merger`, but the **merger panels
    show only the `MER_*` phases** — a merger rank's `MAP_*` work is folded
    into the **mapper** facet instead (`FACET_ROWS` in `pf_profile_viz.py`),
    so merger graphs stay free of mapper information. In the per-rank Gantt
    a merger rank appears as two timeline rows: `rN (mapper)` for `MAP_*`
    and `rN (merger)` for `MER_*`.
- Counters: `bytes_sent`, `bytes_to_master`, `bytes_mer_to_master`
  (merger: bytes forwarded to master), `terms_sent`, `patches_built`,
  `buffers_received` (reducer: chunks consumed in `PF_StoreBuffer`),
  `merge_lbuffer_full` (merge fired because the large buffer ran out of room),
  `merge_max_patches` (merge fired because `lPatch >= MaxPatches`). The two
  merge-reason flags can both fire on the same call so their sum can exceed
  `patches_built`. Useful ratios: `buffers_received / patches_built` =
  buffers absorbed per merge; `merge_max_patches / patches_built` = how
  often the patch-count cap forced a flush vs the byte-size cap.
- `/proc/self/io` diff: `io_rchar`, `io_wchar`, `io_read_bytes`,
  `io_write_bytes`, `io_syscr`, `io_syscw`.
- `getrusage`: `maxrss_kb`, `minflt`, `majflt`, `nvcsw`, `nivcsw`.
- Per-node disk (one row per node-leader): `node_disk_time_in_io_ms`,
  `node_wallclock_us` from `/proc/diskstats` for the FORMTMP device.
  `disk_util_pct = node_disk_time_in_io_ms × 1000 / node_wallclock_us × 100`
  matches what `iostat -x` reports as `%util`.
- Per-node NIC (one row per node-leader): `node_nic_xmit_bytes`,
  `node_nic_rcv_bytes` sampled from
  `/sys/class/infiniband/<dev>/ports/<port>/counters/port_{xmit,rcv}_data{,_extended}`
  (raw counters are in 4-byte units per IB spec; the C side multiplies
  by 4 so the CSV is in bytes). The device is parsed from
  `UCX_NET_DEVICES` (e.g. `mlx5_0:1`); if the env var is unset, both
  columns are -1 and the viz NIC panels render blank. The 64-bit
  `_extended` counters are preferred when present (ConnectX-5+); the
  C side falls back to the 32-bit names with wrap detection (negative
  delta → -1) on older HCAs.
  **Important caveat: these counters are host-wide.** They aggregate
  every UCX/MPI/IPoIB transfer the host issues, regardless of which
  job owns it. NIC numbers from a non-`place=scatter:excl` PBS job
  are advisory only — they will mix in any co-tenant's traffic.

One phase is computed by the viz via subtraction:
`MAP_HASH_PACK = MAP_ENDSORT_TOTAL − MAP_SEND_WAIT − MAP_SEND_MPI`.
A second derived metric `RED_STORE = wallclock − everything else` is the
residual untracked time on the reducer side; for a healthy run it should
be small.

**Software-derived throughput** (no extra capture, computed by the viz
from existing columns):
`map_throughput_mbps = bytes_sent / (t_map_send_wait_us + t_map_send_mpi_us)`
in MB/s — the per-rank rate while MPI was actively sending. Captures
both MR mapper→reducer and non-MR slave→master since `bytes_sent`
covers both. `red_throughput_mbps` is the analogous reducer→master
forward rate (MR-only). NaN where no traffic was sent.

**Hardware-derived throughput** (from the NIC counters above):
`nic_xmit_GBps = node_nic_xmit_bytes / node_wallclock_us / 1e3` —
realized link rate averaged across the *full* module wallclock,
including idle time. Compare to peak (default 25 GB/s for HDR
ConnectX-6 unidirectional; override via `PF_PROFILE_NIC_PEAK_GBPS`).
A high `MAP_SEND_WAIT` with low `nic_xmit_GBps` means the stall is
MPI handshake overhead, not link bandwidth.

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
# 1. Per-node leader detected on every host (one line per host, per resource)
grep "pf_profile: tracking" "$PBS_O_WORKDIR/<jobname>.e"
# expect (per host):
#   [0] pf_profile: tracking disk M:N (/gtmp)
#   [0] pf_profile: tracking NIC mlx5_0:1 (port_xmit_data_extended)

# 2. CSV exists and has rows for all ranks * all modules
wc -l "$PF_PROFILE_DIR/pf_profile.csv"

# 3. Disk %util is non-zero on at least one node-leader row
awk -F, 'NR>1 && $37!="-1" && $38>0 {printf "rank=%s mod=%s util=%.2f%%\n",
        $4, $2, ($37*1000.0/$38)*100}' "$PF_PROFILE_DIR/pf_profile.csv"

# 4. NIC TX bytes non-zero on the node-leader (only meaningful on excl jobs)
awk -F, 'NR==1{for(i=1;i<=NF;i++)c[$i]=i;next} $c["node_nic_xmit_bytes"]>0 {
        printf "rank=%s mod=%s xmit=%.1f MB rcv=%.1f MB\n",
        $c["rank"], $c["module"],
        $c["node_nic_xmit_bytes"]/1e6, $c["node_nic_rcv_bytes"]/1e6}' \
        "$PF_PROFILE_DIR/pf_profile.csv" | head
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
run dir — self-contained interactive plotly. Every panel carries a one-line
"how to read this" blurb and there's a jump-to nav at the top. Panels:

- **Module dominance** — per-module wallclock sorted, with a cumulative-% line.
  Optimize the tall bars; the line tells you when you've covered most of the run.
- **Effective utilization** — `Σ(rank wallclock) / (module wall × #ranks)` per
  module, with idle core-seconds on the second axis. The headline "is parallelism
  working" number; a thesis-grade metric for the MR-vs-org story.
- **Phase breakdown** — stacked phase time per module, faceted per role
  (mapper / reducer / merger / master — the merger column appears only when
  the run used `PF_MERGERS>0`, and stacks only the `MER_*` phases; the merger
  ranks' mapper-phase work is counted in the mapper column instead).
  The mapper bar intentionally over-stacks — `MAP_ENDSORT_TOTAL` already
  contains `SEND_WAIT`/`SEND_MPI`/`HASH_PACK`; use **Phase coverage** for the
  non-double-counted view.
- **Phase coverage** — disjoint phases + an explicit `UNACCOUNTED` residual
  (`wallclock − Σ phases`). A big grey segment on a role = profiler blind spot,
  candidate for a new `PF_TIMER` bracket.
- **Critical path** — per module, the time *envelope* of each pipeline stage
  (`master distribute → mapper generator → reducer shuffle/ingest → reducer final
  sort → reducer→master forward → master collect → master final sort`) drawn
  against the module-wall backdrop. Gaps between stages and leading/trailing slack
  vs the backdrop are pipeline fill+drain — what overlap can't hide.
- **MR effectiveness** — the headline MR metric: **dedup ratio** = `Σ bytes_to_master
  / Σ bytes_sent` (`<1` ⇒ reducers shrank the stream before the master), shuffle
  volume in/out per module, disk-write bytes by role, and mean shipped term size
  (`bytes_sent / terms_sent`). Non-MR runs fall back to slave→master bytes (ratio ≈ 1).
- **Decision matrix** — flags the rule(s) the run matches → next knob (see below).
- **Tail statistics** — table: per role per phase, mean / p50 / p95 / max (s) and
  max/mean over all `(module, rank)` rows. Wallclock is set by the slowest rank;
  watch the tail, not the means the other panels show.
- **Straggler gap** — `(slowest-rank wall − mean) / mean` per module, mappers &
  reducers — the load imbalance the role-mean panels hide.
- **Wait graph** — sankey: flows from the role that holds things up to the role
  that idles (`MAP_GETTERM_WAIT`←master, `MAP_SEND_WAIT`←reducers, `RED_RECV_WAIT`
  ←mappers, `RED_FORWARD_WAIT`←master, `MAS_DISTRIBUTE_WAIT`←mappers,
  `MAS_MERGE_RECV_WAIT`←children (master blocked mid-merge for its next
  chunk — the real "master waited for reducers"), `MAS_COLLECT`←reducers
  (end-of-module stats tail)). With a merge tier active you also get
  `MER_RECV_WAIT`←reducers and `MER_FORWARD_WAIT`←master flows. Values are
  core-seconds of wait. Tells you which side of a rank the bottleneck is on.
- **Imbalance heatmap** — per-rank wallclock, mapper top, the merge tier
  (reducer → merger → master, blank-row separated) fused below.
- **Merge attribution** — per module, summed over reducers: MergePatches firings
  caused by the large buffer running out (`merge_lbuffer_full` → bump
  `largesize`/`smallext`) vs the patch-count cap (`merge_max_patches` → bump
  `filepatches`), plus `buffers_received / patches_built`. Picks the knob the
  decision matrix only guesses at. (Both reasons can fire on one call so the stack
  can exceed `patches_built`.)
- **Software throughput** — per-rank MB/s heatmap (mapper TX, reducer→master
  forward, and merger→master forward when the merge tier is active),
  derived from `bytes / send-time`. NIC peak in the title is from
  `PF_PROFILE_NIC_PEAK_GBPS` (default 25 GB/s).
- **Throughput reconciliation** — wire vs software per module: realized NIC TX
  GB/s (host-wide, node-leader — advisory unless `place=scatter:excl`) vs offered
  shuffle load (`Σ bytes_sent / module wall`) vs the mean per-mapper rate while
  actively sending. High while-active rate but low NIC ⇒ stalls are
  handshake/rendezvous overhead, not link bandwidth.
- **OS counters** — bytes_written, ctxt switches, MaxRSS, disk %util,
  and the NIC TX/RX GB/s line plot with a horizontal peak reference.
- **Per-rank Gantt** — horizontal stack per rank, one figure per module.

The two heavyweight per-module panels — **critical path** and **per-rank
Gantt** — are limited to the modules that each take **≥10% of run
wallclock** (or, if none clear that bar, the 5 heaviest); the omitted module
ids are listed in a note at the top of the report. They still appear in every
cross-module summary panel (module dominance, phase breakdown, OS counters,
etc.). This keeps the HTML small on runs with one dominant module and a long
tail of tiny ones. The cutoff is `_significant_modules(df, frac=0.10,
top_n=5)` in the viz script.

Compare mode (`<mr_dir> <org_dir>`) writes `pf_profile_compare.html` with a
**headline panel** (run wallclock, sort-side & master disk writes, bytes to
master, with % change vs org), the MR-vs-org phase breakdown, and the MR run's
MR-effectiveness / critical-path / wait-graph panels plus both decision matrices.
Run the single-run report on the org dir for its versions of the rest.

All of the panels above are **viz-only** — they read columns already in
`pf_profile.csv`, so they work against historical CSVs with no rebuild. Older
CSVs missing a column are backfilled in `load_csv`.

Set `PF_PROFILE_NIC_PEAK_GBPS` before running the viz to match your
fabric (e.g. `12.5` for EDR, `25` for HDR unidir, `50` for NDR).

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
| Merger `MER_RECV_WAIT > 0.5 × MER_MERGE` | Leaf reducers slow → more `-r<N>`, bigger reducer `largesize`, or more `PF_MERGERS` (narrower fan-in) |
| Merger `MER_FORWARD_WAIT > 0.3 × wallclock` | Master is the slow consumer downstream — adding mergers won't help |
| MR slower than org despite less I/O | Reduce reducer % OR check `PF_SHUFFLE_NOCOMPRESS` |
| `nic_xmit_GBps > 0.85 × peak` on any node-leader | NIC saturated → fewer mpiprocs/node OR faster fabric |
| Mappers high `MAP_SEND_WAIT` AND `nic_xmit_GBps < 0.30 × peak` | NIC idle → increase `PF_SBUFS`, check `UCX_RNDV_THRESH` |

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
  `RED_FINAL_SORT`; `PF_MergerLoop` brackets `MER_MERGE`; `PF_PutIn`
  brackets the three `PF_WaitRbuf` calls with a runtime-indexed timer that
  picks `MER_RECV_WAIT` when `PF.in_merger_phase` else `MAS_MERGE_RECV_WAIT`.
  Stat aggregation extends the existing `PF_LongSinglePack` chain inside
  `#ifdef PF_PROFILE`.
- [sources/mpi.c](sources/mpi.c) — `PF_ISendSbuf` brackets `SEND_WAIT`/
  `SEND_MPI`. Attribution (checked in this order): `PF.in_merger_phase` ⇒
  `MER_FORWARD_*` + `bytes_mer_to_master`; an MR reducer (`PF.me >=
  nummappers`) forwarding upstream — to master or to its merger — ⇒
  `RED_FORWARD_*` + `bytes_to_master`; everything else (MR mapper→reducer,
  non-MR slave→master) ⇒ `MAP_SEND_*` + `bytes_sent`.
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
- **NIC counters are host-wide.** Same per-node-leader pattern as disk,
  but the IB sysfs counters aggregate every UCX/MPI/IPoIB transfer the
  host sees, including from co-tenant jobs. Numbers are advisory unless
  the PBS job ran with `place=scatter:excl`. Don't draw conclusions
  about saturation from a non-exclusive run.
- **NIC counter wrap.** On older HCAs without `port_xmit_data_extended`,
  the 32-bit counter wraps at ~16 GB. The C side detects negative deltas
  and emits `-1`; the viz skips those rows. Long modules on old HCAs
  may show gaps.
