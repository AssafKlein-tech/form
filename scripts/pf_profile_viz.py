#!/usr/bin/env python3
"""MRmpi profile visualization.

Reads pf_profile.csv files produced by parform compiled with --enable-mr-profile
and produces a self-contained interactive HTML report. Panels (single-run):

- module dominance        — per-module wallclock, sorted, with cumulative %
- effective utilization   — Σ(rank wall) / (module wall × #ranks) + idle core-s
- phase breakdown         — stacked phase time per module, faceted by role
- phase coverage          — disjoint phases + an explicit UNACCOUNTED residual
- critical path           — per-module stage time envelopes vs module-wall backdrop
- mr effectiveness        — dedup ratio (out/in), shuffle volume, per-role disk
- decision matrix         — dominant bottleneck pattern → next knob to try
- tail statistics         — p50/p95/max per phase per role
- straggler gap           — (slowest rank wall − mean) / mean, per module
- wait graph              — sankey of who-blocks-whom (core-seconds of wait)
- imbalance heatmap       — per-rank wallclock, mapper top / merge tier below
- merge attribution       — MergePatches firings: largesize-full vs filepatches-cap
- software throughput     — per-rank MB/s while MPI was actively sending
- throughput reconciliation — wire (NIC) vs software (offered / while-active)
- OS counters             — bytes_written, ctxt switches, RSS, disk %util, NIC
- per-rank Gantt          — one figure per module

When the run used the mapper-merger tier (PF_MERGERS>0), ranks 1..G carry the
role "merger": they run the full mapper phase and then a second merge pass
(MER_MERGE / MER_RECV_WAIT / MER_FORWARD_*) over a group of leaf reducers.
The merger facet shows only the merge-tier (MER_*) phases; a merger rank's
mapper-phase work is folded into the Mapper facet instead (FACET_ROWS), so
the merger panels stay free of mapper information.

Compare mode adds a headline MR-vs-org panel (run wallclock, sort-side & master
disk writes, bytes to master) and shows phase breakdown / critical-path /
wait-graph for the MR run.

Usage:
    python pf_profile_viz.py <run_dir>                # single-run report
    python pf_profile_viz.py <mr_dir> <org_dir>       # MR-vs-org compare
"""

import argparse
import os
import sys
from pathlib import Path

# Cap BLAS/OpenMP thread pools BEFORE importing pandas/numpy. On a login node with
# a low RLIMIT_NPROC, OpenBLAS otherwise tries to spawn one thread per core at
# import and aborts with a flood of "blas_thread_init: pthread_create failed".
# The viz does no heavy linalg, so single-threaded is fine.
for _v in ("OPENBLAS_NUM_THREADS", "OMP_NUM_THREADS", "MKL_NUM_THREADS",
           "NUMEXPR_NUM_THREADS"):
    os.environ.setdefault(_v, "1")

import pandas as pd
import plotly.graph_objects as go
from plotly.subplots import make_subplots
import plotly.io as pio


PHASE_COLS = [
    ("t_map_generator_us",     "MAP_GENERATOR",     "mapper"),
    ("t_map_endsort_total_us", "MAP_ENDSORT_TOTAL", "mapper"),
    ("t_map_send_wait_us",     "MAP_SEND_WAIT",     "mapper"),
    ("t_map_send_mpi_us",      "MAP_SEND_MPI",      "mapper"),
    ("t_map_getterm_wait_us",  "MAP_GETTERM_WAIT",  "mapper"),
    ("t_red_recv_wait_us",     "RED_RECV_WAIT",     "reducer"),
    ("t_red_buffer_copy_us",   "RED_BUFFER_COPY",   "reducer"),
    ("t_red_merge_patches_us", "RED_MERGE_PATCHES", "reducer"),
    ("t_red_final_sort_us",    "RED_FINAL_SORT",    "reducer"),
    ("t_red_forward_wait_us",  "RED_FORWARD_WAIT",  "reducer"),
    ("t_red_forward_mpi_us",   "RED_FORWARD_MPI",   "reducer"),
    ("t_mas_distribute_us",    "MAS_DISTRIBUTE",    "master"),
    ("t_mas_distribute_wait_us","MAS_DISTRIBUTE_WAIT","master"),
    ("t_mas_final_sort_us",    "MAS_FINAL_SORT",    "master"),
    ("t_mas_collect_us",       "MAS_COLLECT",       "master"),
    ("t_mas_merge_recv_wait_us","MAS_MERGE_RECV_WAIT","master"),
    ("t_mer_merge_us",         "MER_MERGE",         "merger"),
    ("t_mer_recv_wait_us",     "MER_RECV_WAIT",     "merger"),
    ("t_mer_forward_wait_us",  "MER_FORWARD_WAIT",  "merger"),
    ("t_mer_forward_mpi_us",   "MER_FORWARD_MPI",   "merger"),
    # Master-bypass merger-tier phases (partitioned redistribution + gather).
    # On an input-partitioned module the merger distributes node-local buckets
    # (MER_DISTRIBUTE, the merger's analogue of MAS_DISTRIBUTE); MER_GATHER is
    # the merger streaming its file up at the chain exit. On the master,
    # MAS_GATHER isolates the chain-exit re-globalization from a classic non-MR
    # merge (MAS_FINAL_SORT), and MAS_MERGERDONE is the only master work on a
    # bypassed (output-partitioned) module.
    ("t_mer_distribute_us",      "MER_DISTRIBUTE",      "merger"),
    ("t_mer_distribute_wait_us", "MER_DISTRIBUTE_WAIT", "merger"),
    ("t_mer_gather_us",          "MER_GATHER",          "merger"),
    ("t_mas_gather_us",          "MAS_GATHER",          "master"),
    ("t_mas_mergerdone_us",      "MAS_MERGERDONE",      "master"),
    # Mapper-attack instrumentation: sub-phases that decompose the
    # MAP_GENERATOR / MAP_ENDSORT_TOTAL parents. All marked NON_ADDITIVE so
    # they show up in Gantt + per-rank breakdown but don't double-count
    # against the parent in the stacked phase-breakdown panel.
    ("t_map_small_flush_total_us", "MAP_SMALL_FLUSH_TOTAL", "mapper"),
    ("t_map_splitmerge_us",        "MAP_SPLITMERGE",        "mapper"),
    ("t_map_compress_batch_us",    "MAP_COMPRESS_BATCH",    "mapper"),
    ("t_map_hash_route_us",        "MAP_HASH_ROUTE",        "mapper"),
    ("t_map_delta_compress_us",    "MAP_DELTA_COMPRESS",    "mapper"),
    ("t_map_sbuf_copy_us",         "MAP_SBUF_COPY",         "mapper"),
    ("t_map_testsub_us",           "MAP_TESTSUB",           "mapper"),
    ("t_map_normalize_us",         "MAP_NORMALIZE",         "mapper"),
    ("t_map_preppoly_us",          "MAP_PREPPOLY",          "mapper"),
    ("t_map_storeterm_us",         "MAP_STORETERM",         "mapper"),
    ("t_red_store_total_us",       "RED_STORE_TOTAL",       "reducer"),
]

# Phases that are a sub-component of another phase (not additive with the
# rest of their role's bar). They still get their own Gantt bar but are
# skipped in the stacked phase-breakdown so the bar isn't double-counted.
# MAS_MERGE_RECV_WAIT ⊂ MAS_FINAL_SORT; MER_RECV_WAIT ⊂ MER_MERGE.
# The mapper-attack sub-phases sit inside MAP_GENERATOR / MAP_ENDSORT_TOTAL
# and RED_STORE_TOTAL sits inside the reducer wallclock (it parents the
# existing RED_BUFFER_COPY / RED_MERGE_PATCHES that are themselves additive).
NON_ADDITIVE_PHASES = {
    "MAS_MERGE_RECV_WAIT", "MER_RECV_WAIT", "MER_DISTRIBUTE_WAIT",
    "MAP_SMALL_FLUSH_TOTAL", "MAP_SPLITMERGE", "MAP_COMPRESS_BATCH",
    "MAP_HASH_ROUTE", "MAP_DELTA_COMPRESS", "MAP_SBUF_COPY",
    "MAP_TESTSUB", "MAP_NORMALIZE", "MAP_PREPPOLY", "MAP_STORETERM",
    "RED_STORE_TOTAL",
}

# Display order for roles. A run without a merger tier simply has no
# "merger" rows and panels skip the absent role.
ROLE_ORDER = ("mapper", "reducer", "merger", "master")

# Which phase-role tags a given row-role displays. A mapper-merger rank runs
# the full mapper phase and THEN the merge pass, but the merger panels show
# only the merge-tier (MER_*) phases — the mapper phase of a merger rank is
# deliberately not surfaced here.
ROLE_OWNS = {
    "mapper":  ("mapper",),
    "reducer": ("reducer",),
    "merger":  ("merger",),
    "master":  ("master",),
}

ROLE_PHASES = {
    role: [c for c, _, r in PHASE_COLS if r in owns]
    for role, owns in ROLE_OWNS.items()
}

# Which CSV row-roles feed each panel facet. A mapper-merger rank carries the
# CSV role "merger" but also ran the full mapper phase, so it feeds the
# "mapper" facet too — its mapper-phase work is surfaced there (combined with
# ROLE_OWNS, which keeps only the mapper phases in that facet). The merger
# facet is fed solely by merger rows and shows only the MER_* phases.
FACET_ROWS = {
    "mapper":  ("mapper", "merger"),
    "reducer": ("reducer",),
    "merger":  ("merger",),
    "master":  ("master",),
}


def facet_df(df: pd.DataFrame, facet: str) -> pd.DataFrame:
    """Rows that feed a given panel facet (merger ranks feed 'mapper' too)."""
    return df[df["role"].isin(FACET_ROWS[facet])]


def present_roles(df: pd.DataFrame) -> list:
    """Facets that have feeding rows in this run, in display order. The
    'mapper' facet is present whenever there are mapper *or* merger rows."""
    have = set(df["role"].unique())
    return [r for r in ROLE_ORDER
            if any(rr in have for rr in FACET_ROWS[r])]

DECISION_MATRIX = [
    {
        "id": "reducer_back_pressure",
        "test": lambda d: d["mapper"]["t_map_send_wait_us"] > 0.10 * d["mapper"]["wallclock_us"]
                          and d["reducer"]["t_red_recv_wait_us"] < 0.20 * d["reducer"]["wallclock_us"],
        "diagnosis": "Reducer back-pressured (slow consumer): mappers stall on full sbufs.",
        "knob": "Increase -r<N> (more reducers).",
        "rationale": "More reducers -> less per-reducer load -> mappers don't block on sbuf drain.",
    },
    {
        "id": "reducer_starved",
        "test": lambda d: d["mapper"]["t_map_send_wait_us"] < 0.05 * d["mapper"]["wallclock_us"]
                          and d["reducer"]["t_red_recv_wait_us"] > 0.50 * d["reducer"]["wallclock_us"],
        "diagnosis": "Reducer starved (slow producer): reducers idle waiting for mapper terms.",
        "knob": "Decrease -r<N> (more mappers).",
        "rationale": "More mappers -> faster combined term-generation rate.",
    },
    {
        "id": "disk_bound_io",
        "test": lambda d: d["reducer"]["disk_util_pct"] > 75.0,
        "diagnosis": "Reducers disk-bound (FORMTMP %util > 75%).",
        "knob": "Increase form.set largesize / smallext.",
        "rationale": "Larger in-memory sort -> fewer disk patches -> less I/O.",
    },
    {
        "id": "patch_flush_dominates",
        "test": lambda d: d["reducer"]["t_red_merge_patches_us"] > d["reducer"]["t_red_final_sort_us"]
                          and d["reducer"]["disk_util_pct"] > 50.0,
        "diagnosis": "Patch-flush dominates reducer time.",
        "knob": "Increase form.set filepatches.",
        "rationale": "More patches per merge -> fewer merge passes.",
    },
    {
        "id": "hash_cpu_bound",
        "test": lambda d: d["mapper"]["t_map_hash_pack_us"] > 0.50 * d["mapper"]["wallclock_us"]
                          and d["mapper"]["t_map_send_wait_us"] < 0.10 * d["mapper"]["wallclock_us"],
        "diagnosis": "Mappers hash-pack CPU-bound.",
        "knob": "Code: review the hash kernel (murmur3-AVX512 already in use); consider per-rank load reporting.",
        "rationale": "Hash work is per-term; vectorization is the only lever.",
    },
    # ---- Mapper-attack rules (plan i-want-to-attack-dazzling-gizmo.md). ----
    # Gated by t_map_generator_us > 0 to suppress on non-mapper rows.
    {
        "id": "compress_batch_redundant",
        "test": lambda d: d["mapper"]["t_map_generator_us"] > 0
                          and d["mapper"]["t_map_compress_batch_us"] > 0.05 * d["mapper"]["t_map_generator_us"]
                          and d["mapper"]["t_map_delta_compress_us"] > 0.05 * d["mapper"]["t_map_generator_us"],
        "diagnosis": "Mapper does TWO compression passes per term (ComPress batch + per-reducer delta) and both are >5% of Generator time. The batch pass writes to compressSpace; the per-reducer pass re-compresses against AR.CompressPointers[dst]. Only the second pass matters for the wire.",
        "knob": "Candidate 1a: skip ComPress at sort.c:958 for MR mappers; PutOut already handles raw terms.",
        "rationale": "Removing the first pass cuts one full term walk per small-buffer flush with no reducer-side change.",
    },
    {
        "id": "sort_shift_candidate",
        "test": lambda d: d["mapper"]["t_map_generator_us"] > 0
                          and d["mapper"]["t_map_splitmerge_us"] > 0.04 * d["mapper"]["t_map_generator_us"]
                          and d["reducer"]["wallclock_us"] > 0
                          and d["reducer"]["t_red_recv_wait_us"] > 0.40 * d["reducer"]["wallclock_us"],
        "diagnosis": "Mapper-side SplitMerge is non-trivial (>4% of Generator) AND reducers have headroom (recv_wait >40% of reducer wallclock). The sort work can move to the reducer.",
        "knob": "Candidate 1b: add a SplitMerge inside PF_StoreBuffer (parallel.c:954); skip the mapper SplitMerge. Pick 1b-i (drop wire compression) if map_compression_ratio < 1.5, otherwise 1b-ii (per-destination radix on mapper to preserve compression).",
        "rationale": "Total sort work is conserved; moving it to idle reducers shifts the bottleneck off the critical path.",
    },
    {
        "id": "compression_not_earning",
        "test": lambda d: d["mapper"]["map_bytes_postcompress"] > 0
                          and d["mapper"]["map_compression_ratio"] < 1.2,
        "diagnosis": "Per-reducer delta compression saves <20% of wire bytes -- it's mostly CPU overhead. Likely caused by hash-spread routing breaking shared-prefix opportunities.",
        "knob": "Set PF_SHUFFLE_NOCOMPRESS=1 for a one-shot benchmark. If wallclock improves, the compress block (sort.c:1940-2020) is a candidate to skip on the lowmr_sort path.",
        "rationale": "Compression earns its CPU only when shared-prefix runs are long; murmur3 hashing typically destroys that locality.",
    },
    {
        "id": "normalize_underchanged",
        "test": lambda d: d["mapper"]["t_map_generator_us"] > 0
                          and d["mapper"]["t_map_normalize_us"] > 0.50 * d["mapper"]["t_map_generator_us"]
                          and d["mapper"]["map_norm_clean_in"] > 0
                          and (d["mapper"]["map_norm_changed"] / max(d["mapper"]["map_norm_clean_in"], 1)) < 0.30,
        "diagnosis": "Normalize dominates Generator (>50%) but actually modifies the term less than 30% of the time. Most calls are wasted work.",
        "knob": "Candidate 2b: add an early-out in Normalize (normal.c:193) using a cheap clean-term predicate (DIRTYFLAG check on subterms).",
        "rationale": "If 70%+ of calls return unchanged, even a partial detector that skips a third of them is several % wallclock.",
    },
    {
        "id": "testsub_prev_rule_repeats",
        "test": lambda d: d["mapper"]["t_map_generator_us"] > 0
                          and d["mapper"]["t_map_testsub_us"] > 0.30 * d["mapper"]["t_map_generator_us"]
                          and d["mapper"]["map_terms_in"] > 0
                          and (d["mapper"]["map_testsub_prev_rule_hit"] / max(d["mapper"]["map_terms_in"], 1)) > 0.30,
        "diagnosis": "TestSub is >30% of Generator AND >30% of terms match the same rule as the previous term. An LRU-1 cache would short-circuit a substantial fraction of pattern scans.",
        "knob": "Candidate 2c: cache the last successful rule index in proces.c:Generator; retry it before the full TestSub scan.",
        "rationale": "Hot symbolic expansions tend to emit runs of structurally-similar terms; the cache hit rate measured here is the realised ceiling for the optimization.",
    },
    {
        "id": "gen_other_dominates",
        "test": lambda d: d["mapper"]["t_map_generator_us"] > 0
                          and d["mapper"]["t_map_gen_other_us"] > 0.40 * d["mapper"]["t_map_generator_us"],
        "diagnosis": "Generator residual (MAP_GENERATOR minus TestSub+Normalize+PrepPoly+StoreTerm) is >40% of Generator -- the bottleneck is NOT in the instrumented sub-calls. Likely cache misses on term walks, or an unwrapped helper (PolyFunMul, TakeIDfunction, ReNumber, Deferred).",
        "knob": "Add finer-grained timers (PolyFunMul, TakeIDfunction) and re-profile, OR try candidate 2e (term-arena alignment + prefetch).",
        "rationale": "Without more instrumentation we can't pick between cache-bound and unwrapped-callee; do the cheap instrumentation pass first.",
    },
    {
        "id": "master_merge_recv_bound",
        "test": lambda d: d["master"]["t_mas_merge_recv_wait_us"] > 0.40 * d["master"]["wallclock_us"],
        "diagnosis": "Master spends most of its merge blocked in PF_PutIn waiting for reducers to deliver sorted chunks (MAS_MERGE_RECV_WAIT high) — recv-bound, not merge-CPU-bound.",
        "knob": "More reducers (smaller per-reducer sort ⇒ they finish & forward sooner), OR raise reducer largesize/smallext (less reducer-side merging before forward), OR raise PF_RBUFS only if the cap is lifted (currently 2 — see CLAUDE.md).",
        "rationale": "The master can't merge faster than its slowest child delivers; shortening the reducer critical path or deepening the master's receive queue is the lever, not the merge tree shape.",
    },
    {
        "id": "master_merge_bound",
        "test": lambda d: d["master"]["t_mas_final_sort_us"] > 0.30 * d["master"]["wallclock_us"]
                          and d["master"]["t_mas_merge_recv_wait_us"] > 0.15 * d["master"]["t_mas_final_sort_us"]
                          and d["master"]["t_mas_merge_recv_wait_us"] <= 0.40 * d["master"]["wallclock_us"],
        "diagnosis": "Master merge tree substantial AND meaningfully recv-stalled (MAS_MERGE_RECV_WAIT is 15-40% of MAS_FINAL_SORT) — mixed CPU/recv bound, leaning recv.",
        "knob": "Speeding up reducer delivery (more reducers OR raise reducer largesize/smallext) shortens the master critical path; merge-tree-shape changes won't.",
        "rationale": "A non-trivial share of master merge time is waiting for the next reducer chunk, so reducer-side throughput is a real lever -- but not so dominant that master_merge_recv_bound fires.",
    },
    {
        "id": "master_merge_cpu_bound",
        "test": lambda d: d["master"]["t_mas_final_sort_us"] > 0.30 * d["master"]["wallclock_us"]
                          and d["master"]["t_mas_merge_recv_wait_us"] < 0.15 * d["master"]["t_mas_final_sort_us"],
        "diagnosis": "Master merge substantial but NOT recv-bound (MAS_MERGE_RECV_WAIT tiny vs MAS_FINAL_SORT) — the master is CPU-bound in the loser-tree compare/decompress.",
        "knob": "Fewer/fatter reducers won't help; reducers do not combine like terms (they bypass ComPress, just k-way merge and forward). The lever is upstream: reduce term volume per mapper (algebraic restructuring in the FORM script, or anything that improves mapper-side ComPress yield), OR speed up the master's compare path itself (Compare1 kernel).",
        "rationale": "Merge work is per-term on a single core; widening the tree adds children, not throughput. Every unique term a mapper emits reaches the master, so mapper-side combining + raw term count are the only volume levers.",
    },
    {
        "id": "master_distribute_slow",
        "test": lambda d: d["master"]["t_mas_distribute_wait_us"] > 0.50 * d["master"]["t_mas_distribute_us"]
                          and d["mapper"]["t_map_generator_us"] > 0.0,
        "diagnosis": "Master MAS_DISTRIBUTE_WAIT dominates -- master idles between bucket dispatches.",
        "knob": "Investigate, but do NOT default to bigger mProcessBucketSize.",
        "rationale": "Empirical (4n x 16r, bench_mid_profile.frm, 2026-05-09): bumping bucket 1000 -> 4000 -> 16000 cut MAS_DISTRIBUTE_WAIT 6.8x but TOTAL wallclock got 2.6x WORSE. Bigger buckets serialize the pipeline (slaves stall longer between bigger dispatches, tail-end overlap collapses). Default 1000 was best for this workload.",
    },
    {
        "id": "os_oversubscription",
        "test": lambda d: d["mapper"]["nivcsw"] > 50 or d["reducer"]["nivcsw"] > 50,
        "diagnosis": "High involuntary context switches: kernel preemption.",
        "knob": "Reduce ranks per node OR pin with --bind-to core.",
        "rationale": "Preemption is unrelated to FORM workload; OS contention.",
    },
    {
        "id": "reducer_skew",
        "test": lambda d: d["reducer"]["final_sort_var_pct"] > 30.0,
        "diagnosis": "Reducer load skewed (final-sort variance > 30%).",
        "knob": "Code: investigate hash distribution.",
        "rationale": "Some reducers got more terms than others.",
    },
    {
        "id": "merger_recv_bound",
        "test": lambda d: d["merger"]["t_mer_merge_us"] > 0.0
                          and d["merger"]["t_mer_recv_wait_us"] > 0.50 * d["merger"]["t_mer_merge_us"],
        "diagnosis": "Mapper-mergers spend most of their merge blocked in PF_PutIn waiting for a leaf reducer's next sorted chunk (MER_RECV_WAIT > half of MER_MERGE) — the merge tier is starved by its leaf reducers, not merge-CPU-bound.",
        "knob": "Speed up leaf-reducer delivery: more reducers (-r<N>) so each finishes & forwards sooner, OR raise reducer largesize/smallext, OR widen the merger tier (more PF_MERGERS so each merger fans in fewer reducers).",
        "rationale": "A merger can't merge faster than its slowest leaf reducer delivers; the lever is the reducer critical path or a narrower per-merger fan-in, not merge-tree CPU.",
    },
    {
        "id": "merger_forward_bound",
        "test": lambda d: d["merger"]["t_mer_merge_us"] > 0.0
                          and d["merger"]["t_mer_forward_wait_us"] > 0.30 * d["merger"]["wallclock_us"],
        "diagnosis": "Mapper-mergers stall forwarding the merged stream to the master (MER_FORWARD_WAIT > 30% of merger wallclock) — the master consumes slower than the merge tier produces.",
        "knob": "The bottleneck is downstream at the master, not the merge tier. Check MAS_MERGE_RECV_WAIT / MAS_FINAL_SORT; adding mergers won't help while the master is the slow consumer.",
        "rationale": "Forward-wait means back-pressure from the master's k-way merge; the merge tier is delivering fine.",
    },
    # ---- Master-bypass merger-tier rules (partitioned redistribution). ----
    {
        "id": "merger_distribute_starved",
        "test": lambda d: d["merger"]["t_mer_distribute_us"] > 0.0
                          and d["merger"]["t_mer_distribute_wait_us"] > 0.50 * d["merger"]["t_mer_distribute_us"],
        "diagnosis": "On input-partitioned modules the merger spends most of MER_DISTRIBUTE blocked on PF_Receive(READY) (MER_DISTRIBUTE_WAIT > half of MER_DISTRIBUTE) — its node-local mappers pull buckets slower than the merger can serve them. The distribute side is mapper-bound, not merger-bound.",
        "knob": "The node-local mapper pool is the slow consumer: it has too few mappers per merger (fewer reducer-bearing nodes => bigger per-node mapper sets help), or those mappers are CPU-bound in Generator. This is the work-stealing load-balancer's target — let a drained node's idle mappers pull from a busy donor merger.",
        "rationale": "DISTRIBUTE_WAIT is the merger idling between dispatches, the node-local analogue of MAS_DISTRIBUTE_WAIT; the lever is the consumers (mappers), not the merger.",
    },
    {
        "id": "gather_bound_exit",
        "test": lambda d: d["master"]["t_mas_gather_us"] > 0.30 * d["master"]["wallclock_us"],
        "diagnosis": "The chain-exit re-globalization (MAS_GATHER, a `.sort(gather)` / MAPREDUCE_LAST module) is a large share of master wallclock — the master's one mandatory global merge of the G merger streams dominates.",
        "knob": "This merge is unavoidable where global order is required (print/.store/toPolynomial). Lever options: keep the chain partitioned longer so fewer modules gather; check MER_GATHER and the master recv to see whether the merger streams or the master CPU is the limit; widen the merger tier so each stream is smaller.",
        "rationale": "Unlike MAS_FINAL_SORT on a bypassed module (which should be ~0), MAS_GATHER is real required work; the only levers are gather frequency and per-stream size.",
    },
    {
        "id": "nic_saturated",
        "test": lambda d: _nic_max_gbps(d) > 0.85 * _nic_peak_gbps(),
        "diagnosis": "NIC link saturated (>85% of assumed peak): bandwidth-bound.",
        "knob": "Higher-bandwidth interconnect OR fewer concurrent senders per node (lower mpiprocs).",
        "rationale": "At 85%+ of peak, software tuning has minimal headroom.",
    },
    {
        "id": "nic_underutilized",
        "test": lambda d: d["mapper"]["t_map_send_wait_us"] > 0.10 * d["mapper"]["wallclock_us"]
                          and 0 < _nic_max_gbps(d) < 0.30 * _nic_peak_gbps(),
        "diagnosis": "Mappers stall but NIC is idle: handshake/eager-rendezvous overhead, not link bandwidth.",
        "knob": "Increase PF_SBUFS; check UCX_RNDV_THRESH (currently 64m for production).",
        "rationale": "Send-wait without link saturation means MPI is waiting on receive posting, not the wire.",
    },
]


def _nic_max_gbps(d: dict) -> float:
    """Pull the largest non-NaN nic_xmit_GBps across roles. NaN -> 0."""
    vals = []
    for role in ROLE_ORDER:
        v = d.get(role, {}).get("nic_xmit_GBps", 0.0)
        try:
            v = float(v)
        except (TypeError, ValueError):
            v = 0.0
        if v == v:  # NaN check (NaN != NaN)
            vals.append(v)
    return max(vals) if vals else 0.0


def load_csv(run_dir: Path) -> pd.DataFrame:
    csv_path = run_dir / "pf_profile.csv"
    if not csv_path.exists():
        sys.stderr.write(f"ERROR: {csv_path} not found.\n")
        sys.exit(1)
    df = pd.read_csv(csv_path)
    # Backfill columns that may be missing from older CSVs, so the viz
    # works against historical data without crashing.
    for col in ("buffers_received", "merge_lbuffer_full", "merge_max_patches"):
        if col not in df.columns:
            df[col] = 0
    for col in ("node_nic_xmit_bytes", "node_nic_rcv_bytes"):
        if col not in df.columns:
            df[col] = -1
    for col in (c for c, _, _ in PHASE_COLS):
        if col not in df.columns:
            df[col] = 0
    # Per-phase first/last timestamps (Gantt). -1 means the phase never fired
    # for that rank in that module; downstream charts must filter on that.
    # Backfill so older CSVs render without crashing.
    for col, _, _ in PHASE_COLS:
        for suffix in ("_first_us", "_last_us"):
            ts_col = col.replace("_us", suffix)
            if ts_col not in df.columns:
                df[ts_col] = -1
    for col in ("bytes_sent", "bytes_to_master", "bytes_mer_to_master"):
        if col not in df.columns:
            df[col] = 0
    # Mapper-attack counters: backfill 0 for older CSVs that pre-date the
    # new instrumentation, so the viz can run against historical runs.
    for col in ("map_sbuf_flushes", "map_bytes_precompress", "map_bytes_postcompress",
                "map_bytes_shuffled", "map_terms_in", "map_norm_clean_in",
                "map_norm_changed", "map_testsub_prev_rule_hit",
                "map_testsub_no_match", "red_bytes_received"):
        if col not in df.columns:
            df[col] = 0
    if "nummergers" not in df.columns:
        df["nummergers"] = 0
    # Master-bypass merger-tier counters + per-module chain state. Backfill for
    # CSVs that pre-date the master-bypass instrumentation.
    for col in ("mer_distribute_terms", "mer_partition_bytes"):
        if col not in df.columns:
            df[col] = 0
    if "smrflag" not in df.columns:
        df["smrflag"] = 0
    # Legacy derived "hash+pack" residual. With the new instrumentation
    # MAP_HASH_ROUTE + MAP_DELTA_COMPRESS + MAP_SBUF_COPY are observed
    # directly, so the gap is now small (residual = whatever the timers
    # don't catch, e.g. the int allow_compress declaration). Kept for
    # comparison with historical CSVs.
    df["t_map_hash_pack_us"] = (
        df["t_map_endsort_total_us"]
        - df["t_map_send_wait_us"]
        - df["t_map_send_mpi_us"]
    ).clip(lower=0)
    # Derived: directly-observed sum of mapper PutOut sub-phases. If this
    # ≈ t_map_hash_pack_us, the 14% gap is fully attributed; otherwise the
    # residual = (hash_pack − putout_observed) lives outside the wrapped
    # call sites and merits further instrumentation.
    df["t_map_putout_observed_us"] = (
        df["t_map_hash_route_us"]
        + df["t_map_delta_compress_us"]
        + df["t_map_sbuf_copy_us"]
    )
    # Derived: Generator residual once known sub-phases are subtracted.
    # Large value => bottleneck is cache/memory or an unwrapped sub-call.
    df["t_map_gen_other_us"] = (
        df["t_map_generator_us"]
        - df["t_map_testsub_us"]
        - df["t_map_normalize_us"]
        - df["t_map_preppoly_us"]
        - df["t_map_storeterm_us"]
    ).clip(lower=0)
    # Derived: wire compression ratio. Numerator >> denominator -> compression
    # is paying for itself; numerator ≈ denominator -> compression is mostly
    # CPU overhead with no bytes saved (signal for plan 1b-i).
    df["map_compression_ratio"] = (
        df["map_bytes_precompress"] / df["map_bytes_postcompress"].clip(lower=1)
    )
    df.loc[df["map_bytes_postcompress"] == 0, "map_compression_ratio"] = float("nan")
    df["t_red_store_us"] = (
        df["wallclock_us"]
        - df["t_red_recv_wait_us"]
        - df["t_red_buffer_copy_us"]
        - df["t_red_merge_patches_us"]
        - df["t_red_final_sort_us"]
        - df["t_red_forward_wait_us"]
        - df["t_red_forward_mpi_us"]
    ).clip(lower=0)
    # Per-rank software throughput (MB/s) derived from existing columns:
    # bytes shipped during MPI calls, divided by the time those calls were
    # active. Captures both MR mapper->reducer and non-MR slave->master.
    map_send_us = (df["t_map_send_wait_us"] + df["t_map_send_mpi_us"]).clip(lower=1)
    df["map_throughput_mbps"] = df["bytes_sent"] / map_send_us       # bytes/us = MB/s
    red_fwd_us = (df["t_red_forward_wait_us"] + df["t_red_forward_mpi_us"]).clip(lower=1)
    df["red_throughput_mbps"] = df["bytes_to_master"] / red_fwd_us
    mer_fwd_us = (df["t_mer_forward_wait_us"] + df["t_mer_forward_mpi_us"]).clip(lower=1)
    df["mer_throughput_mbps"] = df["bytes_mer_to_master"] / mer_fwd_us
    df.loc[df["bytes_sent"] == 0,          "map_throughput_mbps"] = float("nan")
    df.loc[df["bytes_to_master"] == 0,     "red_throughput_mbps"] = float("nan")
    df.loc[df["bytes_mer_to_master"] == 0, "mer_throughput_mbps"] = float("nan")

    leader = df[df["node_disk_time_in_io_ms"] >= 0].copy()
    if not leader.empty:
        leader["disk_util_pct"] = (
            leader["node_disk_time_in_io_ms"] * 1000.0 / leader["node_wallclock_us"].clip(lower=1) * 100.0
        )
        # Hardware NIC throughput (GB/s) on the node-leader. Negative
        # diff -> -1 from the C side (counter wrap or non-leader); skip.
        leader["nic_xmit_GBps"] = float("nan")
        leader["nic_rcv_GBps"]  = float("nan")
        ok_xmit = leader["node_nic_xmit_bytes"] >= 0
        ok_rcv  = leader["node_nic_rcv_bytes"]  >= 0
        wall_us = leader["node_wallclock_us"].clip(lower=1)
        leader.loc[ok_xmit, "nic_xmit_GBps"] = (
            leader.loc[ok_xmit, "node_nic_xmit_bytes"] / wall_us[ok_xmit] / 1.0e3
        )
        leader.loc[ok_rcv, "nic_rcv_GBps"] = (
            leader.loc[ok_rcv, "node_nic_rcv_bytes"] / wall_us[ok_rcv] / 1.0e3
        )
        df = df.merge(
            leader[["module", "rank", "disk_util_pct", "nic_xmit_GBps", "nic_rcv_GBps"]],
            on=["module", "rank"], how="left",
        )
    else:
        df["disk_util_pct"] = float("nan")
        df["nic_xmit_GBps"] = float("nan")
        df["nic_rcv_GBps"]  = float("nan")
    return df


def role_aggregates(df: pd.DataFrame) -> dict:
    agg = {}
    # Iterate every role (not just the ones present) so decision-matrix rules
    # can index d["merger"] / d["reducer"] unconditionally — an absent role
    # gets a zero-dict and its rules simply never fire.
    for role in ROLE_ORDER:
        sub = df[df["role"] == role]
        if sub.empty:
            agg[role] = {col: 0.0 for col in df.select_dtypes("number").columns}
            agg[role]["disk_util_pct"] = 0.0
            agg[role]["final_sort_var_pct"] = 0.0
            agg[role]["nivcsw"] = 0
            continue
        d = {col: sub[col].mean() for col in sub.select_dtypes("number").columns}
        d["disk_util_pct"] = sub["disk_util_pct"].dropna().mean() if "disk_util_pct" in sub else 0.0
        if pd.isna(d.get("disk_util_pct", 0.0)):
            d["disk_util_pct"] = 0.0
        if role == "reducer" and "t_red_final_sort_us" in sub:
            mean = sub["t_red_final_sort_us"].mean()
            d["final_sort_var_pct"] = (
                sub["t_red_final_sort_us"].std() / mean * 100.0
                if mean > 0 else 0.0
            )
        d["nivcsw"] = sub["nivcsw"].max() if "nivcsw" in sub else 0
        agg[role] = d
    return agg


def evaluate_decision(df: pd.DataFrame) -> list:
    matched = []
    for module in df["module"].unique():
        m_df = df[df["module"] == module]
        agg = role_aggregates(m_df)
        for rule in DECISION_MATRIX:
            try:
                if rule["test"](agg):
                    matched.append((module, rule))
            except (KeyError, ZeroDivisionError):
                continue
    return matched


def fig_phase_breakdown(df: pd.DataFrame, title_suffix: str = "") -> go.Figure:
    roles = present_roles(df)
    if not roles:
        return go.Figure()
    fig = make_subplots(
        rows=1, cols=len(roles), subplot_titles=[r.capitalize() for r in roles],
        shared_yaxes=False,
    )
    # The merger column shows only the MER_* phases; a merger rank's mapper
    # phase is folded into the Mapper column instead (FACET_ROWS). Phase
    # labels never recur across columns now, but dedupe the legend anyway.
    shown = set()
    for col_idx, role in enumerate(roles, start=1):
        sub = facet_df(df, role)
        for phase_col, phase_label, phase_role in PHASE_COLS:
            if phase_role not in ROLE_OWNS[role] or phase_label in NON_ADDITIVE_PHASES:
                continue
            grouped = sub.groupby("module")[phase_col].mean() / 1.0e6
            sl = phase_label not in shown
            shown.add(phase_label)
            fig.add_trace(
                go.Bar(
                    x=grouped.index, y=grouped.values, name=phase_label,
                    legendgroup=phase_label,
                    showlegend=sl,
                ),
                row=1, col=col_idx,
            )
        fig.update_xaxes(title_text="module", row=1, col=col_idx)
        fig.update_yaxes(title_text="time (s)", row=1, col=col_idx)
    fig.update_layout(
        barmode="stack",
        title=f"Phase breakdown per module per role{title_suffix}",
        height=500,
    )
    return fig


def fig_per_rank_gantt(df: pd.DataFrame, module: int) -> go.Figure:
    """
    Real Gantt timeline. Each rank gets a y-row. For every phase that fired
    on that rank, draw a horizontal bar from t_<phase>_first_us to
    t_<phase>_last_us (the window during which the phase was active at least
    once). Hover shows window size and active-fraction (cumulative duration
    relative to window). With barmode='overlay' interleaved reducer phases
    (RECV/COPY/MERGE) overlap visually -- the "noisy" Gantt the user asked
    for. To switch to a single 'shuffle window' bar, compute the min(first)
    and max(last) across {RECV, COPY, MERGE} per rank and emit one bar.
    """
    sub = df[df["module"] == module]
    # A mapper-merger rank ran the mapper phase and THEN the merge pass.
    # Show it as two timeline rows: a 'mapper' copy carrying the MAP_* bars
    # and the original 'merger' row carrying the MER_* bars. The phase filter
    # (ROLE_OWNS) splits the bars between the two copies, so the merger row
    # shows no mapper information.
    mer = sub[sub["role"] == "merger"]
    if not mer.empty:
        sub = pd.concat([sub, mer.assign(role="mapper")], ignore_index=True)
    # Sort y-axis: master first, then mappers, mergers, reducers, by rank.
    role_order = {"master": 0, "mapper": 1, "merger": 2, "reducer": 3}
    sub = sub.assign(__rorder=sub["role"].map(role_order)).sort_values(["__rorder", "rank"])
    y_labels = [f"r{r} ({role})" for r, role in zip(sub["rank"], sub["role"])]
    fig = go.Figure()
    for phase_col, phase_label, phase_role in PHASE_COLS:
        first_col = phase_col.replace("_us", "_first_us")
        last_col  = phase_col.replace("_us", "_last_us")
        if first_col not in sub.columns or last_col not in sub.columns:
            continue
        # Filter: phase fired (first >= 0), positive window, and the display
        # role owns the phase's role (so mapper rows don't show RED_*; a
        # merger rank's 'mapper' copy shows MAP_* and its 'merger' row MER_*).
        owns = sub["role"].map(lambda rr: phase_role in ROLE_OWNS.get(rr, ()))
        mask = (sub[first_col] >= 0) & (sub[last_col] > sub[first_col]) & owns
        rows = sub[mask]
        if rows.empty:
            continue
        starts_s = rows[first_col] / 1e6
        widths_s = (rows[last_col] - rows[first_col]) / 1e6
        durations_s = rows[phase_col] / 1e6
        labels = [f"r{r} ({role})" for r, role in zip(rows["rank"], rows["role"])]
        active_pct = (durations_s / widths_s * 100.0).clip(upper=100.0)
        hover = [
            f"{phase_label}<br>rank {r} ({role})<br>"
            f"window: {s:.2f}s -> {s + w:.2f}s ({w:.2f}s)<br>"
            f"active: {d:.2f}s ({a:.0f}% of window)"
            for r, role, s, w, d, a in zip(
                rows["rank"], rows["role"], starts_s, widths_s, durations_s, active_pct
            )
        ]
        fig.add_trace(go.Bar(
            x=widths_s, y=labels,
            base=starts_s,
            orientation="h",
            name=phase_label,
            legendgroup=phase_label,
            hovertext=hover,
            hoverinfo="text",
            opacity=0.65,
        ))
    fig.update_layout(
        barmode="overlay",
        title=f"Per-rank Gantt (module {module})",
        xaxis_title="time since module start (s)",
        yaxis_title="rank",
        yaxis=dict(categoryorder="array", categoryarray=y_labels),
        height=max(400, 18 * len(y_labels)),
        bargap=0.15,
    )
    return fig


def fig_imbalance_heatmap(df: pd.DataFrame) -> go.Figure:
    """Imbalance heatmap split by module, two panels.

    - Top: mapper imbalance, rank x module, z = wallclock seconds.
    - Bottom: the merge tier fused — reducer, then merger, then master,
      each present block separated by a blank row. Single colorscale across
      the block so the master row is directly comparable to the reducers
      and mergers feeding it. (No merger tier ⇒ just reducer + master.)
    """
    def _pivot(role: str):
        sub = df[df["role"] == role]
        if sub.empty:
            return None
        return sub.pivot_table(
            index="rank", columns="module",
            values="wallclock_us", aggfunc="mean",
        ).sort_index() / 1.0e6

    p_map = _pivot("mapper")
    p_red = _pivot("reducer")
    p_mer = _pivot("merger")
    p_mas = _pivot("master")

    if all(p is None for p in (p_map, p_red, p_mer, p_mas)):
        return go.Figure()

    # Determine module axis (any non-empty pivot's columns work; they should match).
    for p in (p_map, p_red, p_mer, p_mas):
        if p is not None:
            modules = p.columns.tolist()
            break

    # Fused bottom panel: reducer -> merger -> master (data-flow order toward
    # the master), each present block separated by a blank row.
    fused_y, fused_z, sep_rows, fused_blocks = [], [], [], []
    for role_tag, pv in (("reducer", p_red), ("merger", p_mer), ("master", p_mas)):
        if pv is None:
            continue
        if fused_y:                       # blank separator before this block
            sep_rows.append(len(fused_y))
            fused_y.append(" " * len(sep_rows))   # unique all-blank label
            fused_z.append([float("nan")] * len(modules))
        for r in pv.index.tolist():
            fused_y.append(f"r{r} ({role_tag})")
            fused_z.append(pv.loc[r].reindex(modules).values)
        fused_blocks.append(role_tag)

    panels = []
    if p_map is not None:
        panels.append(("mapper",  "Mapper imbalance — wallclock per module"))
    if fused_y:
        panels.append(("fused",
                       " + ".join(fused_blocks) + " (aside) — wallclock per module"))

    n = len(panels)
    row_heights = []
    if p_map is not None:
        row_heights.append(max(1, len(p_map)))
    if fused_y:
        row_heights.append(max(1, len(fused_y)))

    fig = make_subplots(
        rows=n, cols=1,
        subplot_titles=[t for _, t in panels],
        shared_xaxes=True,
        vertical_spacing=0.10,
        row_heights=[h / sum(row_heights) for h in row_heights],
    )

    total_h = sum(row_heights)
    cumulative = 0
    row_idx = 0
    if p_map is not None:
        row_idx += 1
        slice_top = 1.0 - cumulative / total_h
        slice_bottom = 1.0 - (cumulative + row_heights[row_idx - 1]) / total_h
        cb_y = (slice_top + slice_bottom) / 2.0
        cb_len = max(0.12, (slice_top - slice_bottom) * 0.85)
        fig.add_trace(go.Heatmap(
            z=p_map.values,
            x=p_map.columns.tolist(),
            y=[f"r{r}" for r in p_map.index.tolist()],
            colorscale="Viridis",
            colorbar=dict(title="mapper<br>time (s)", len=cb_len, y=cb_y),
            zmin=0,
            hovertemplate="rank=%{y}<br>module=%{x}<br>time=%{z:.2f}s<extra></extra>",
        ), row=row_idx, col=1)
        fig.update_yaxes(title_text="mapper rank", row=row_idx, col=1)
        cumulative += row_heights[row_idx - 1]

    if fused_y:
        row_idx += 1
        slice_top = 1.0 - cumulative / total_h
        slice_bottom = 1.0 - (cumulative + row_heights[row_idx - 1]) / total_h
        cb_y = (slice_top + slice_bottom) / 2.0
        cb_len = max(0.12, (slice_top - slice_bottom) * 0.85)
        fig.add_trace(go.Heatmap(
            z=fused_z,
            x=modules,
            y=fused_y,
            colorscale="Cividis",
            colorbar=dict(title="merge tier<br>time (s)", len=cb_len, y=cb_y),
            zmin=0,
            hovertemplate="rank=%{y}<br>module=%{x}<br>time=%{z:.2f}s<extra></extra>",
        ), row=row_idx, col=1)
        fig.update_yaxes(title_text="rank", row=row_idx, col=1)
        # Mark each block boundary with a horizontal line.
        for sep in sep_rows:
            fig.add_hline(y=sep - 0.5, line_dash="dot", line_color="black",
                          row=row_idx, col=1)

    fig.update_xaxes(title_text="module", row=n, col=1)
    fig.update_layout(
        title="Imbalance per module (mapper top, merge tier bottom)",
        height=max(500, 22 * total_h + 180),
    )
    return fig


def _nic_peak_gbps() -> float:
    """NIC peak bandwidth assumed for the ref line, in GB/s.

    Default 25.0 -- 200 Gbps HDR ConnectX-6 unidirectional. Override via
    env var if running on a different fabric (e.g. EDR = 12.5).
    """
    try:
        return float(os.environ.get("PF_PROFILE_NIC_PEAK_GBPS", "25.0"))
    except ValueError:
        return 25.0


def fig_software_throughput(df: pd.DataFrame) -> go.Figure:
    """Per-rank software throughput (MB/s) derived from existing CSV columns.

    Two stacked heatmaps: mapper TX rate and reducer->master forward rate.
    NaN cells (no bytes shipped that module) render blank.
    """
    peak_mbps = _nic_peak_gbps() * 1000.0
    panels = []
    # The mapper panel includes merger ranks' mapper-phase TX (FACET_ROWS);
    # the merger panel below shows only the merger->master forward.
    map_pivot = (facet_df(df, "mapper")
                 .pivot_table(index="rank", columns="module",
                              values="map_throughput_mbps", aggfunc="mean"))
    if not map_pivot.empty:
        panels.append(("mapper", map_pivot,
                       f"Mapper TX MB/s per rank (NIC peak ~{peak_mbps:.0f} MB/s)"))

    red_sub = df[df["role"] == "reducer"]
    if not red_sub.empty:
        red_pivot = red_sub.pivot_table(index="rank", columns="module",
                                        values="red_throughput_mbps", aggfunc="mean")
        if red_pivot.dropna(how="all").shape[0] > 0:
            panels.append(("reducer", red_pivot,
                           "Reducer->master forward MB/s per rank"))

    mer_sub = df[df["role"] == "merger"]
    if not mer_sub.empty:
        mer_pivot = mer_sub.pivot_table(index="rank", columns="module",
                                        values="mer_throughput_mbps", aggfunc="mean")
        if mer_pivot.dropna(how="all").shape[0] > 0:
            panels.append(("merger", mer_pivot,
                           "Merger->master forward MB/s per rank"))

    if not panels:
        return go.Figure()

    n = len(panels)
    row_heights = [max(1, len(p)) for _, p, _ in panels]
    fig = make_subplots(
        rows=n, cols=1,
        subplot_titles=[t for _, _, t in panels],
        shared_xaxes=True,
        vertical_spacing=0.12,
        row_heights=[h / sum(row_heights) for h in row_heights],
    )
    total_h = sum(row_heights)
    cumulative = 0
    for i, (role, pivot, _title) in enumerate(panels, start=1):
        slice_top = 1.0 - cumulative / total_h
        slice_bottom = 1.0 - (cumulative + row_heights[i - 1]) / total_h
        cb_y = (slice_top + slice_bottom) / 2.0
        cb_len = max(0.12, (slice_top - slice_bottom) * 0.85)
        fig.add_trace(go.Heatmap(
            z=pivot.values,
            x=pivot.columns.tolist(),
            y=[f"r{r}" for r in pivot.index.tolist()],
            colorscale={"mapper": "Plasma", "reducer": "Cividis",
                        "merger": "Viridis"}.get(role, "Cividis"),
            colorbar=dict(title=f"{role}<br>MB/s", len=cb_len, y=cb_y),
            zmin=0, zmax=peak_mbps,
            hovertemplate="rank=%{y}<br>module=%{x}<br>%{z:.0f} MB/s<extra></extra>",
        ), row=i, col=1)
        fig.update_yaxes(title_text=f"{role} rank", row=i, col=1)
        cumulative += row_heights[i - 1]
    fig.update_xaxes(title_text="module", row=n, col=1)
    fig.update_layout(
        title=("Software throughput per rank "
               "(bytes shipped / time MPI was active; NaN = no traffic)"),
        height=max(500, 22 * total_h + 180),
    )
    return fig


def fig_os_counters(df: pd.DataFrame) -> go.Figure:
    fig = make_subplots(
        rows=3, cols=2,
        subplot_titles=(
            "io_write_bytes per module per role",
            "Involuntary context switches (nivcsw)",
            "MaxRSS (KB)",
            "Disk %util (node-leader rows)",
            "NIC TX GB/s (node-leader; -1 -> blank)",
            "NIC RX GB/s (node-leader; -1 -> blank)",
        ),
    )
    for role in present_roles(df):
        sub = df[df["role"] == role]
        if sub.empty:
            continue
        agg = sub.groupby("module").agg(
            io_write_bytes=("io_write_bytes", "sum"),
            nivcsw=("nivcsw", "sum"),
            maxrss_kb=("maxrss_kb", "max"),
        ).reset_index()
        fig.add_trace(go.Scatter(
            x=agg["module"], y=agg["io_write_bytes"], name=f"{role} io_write_bytes",
            mode="lines+markers",
        ), row=1, col=1)
        fig.add_trace(go.Scatter(
            x=agg["module"], y=agg["nivcsw"], name=f"{role} nivcsw",
            mode="lines+markers",
        ), row=1, col=2)
        fig.add_trace(go.Scatter(
            x=agg["module"], y=agg["maxrss_kb"], name=f"{role} maxrss_kb",
            mode="lines+markers",
        ), row=2, col=1)
    leader = df[df["node_disk_time_in_io_ms"] >= 0]
    if not leader.empty and "disk_util_pct" in leader:
        u = leader.groupby("module")["disk_util_pct"].mean()
        fig.add_trace(go.Scatter(
            x=u.index, y=u.values, name="disk %util", mode="lines+markers",
        ), row=2, col=2)
    peak = _nic_peak_gbps()
    nic_leader = df[df.get("nic_xmit_GBps").notna()] if "nic_xmit_GBps" in df else df.iloc[0:0]
    if not nic_leader.empty:
        for src_col, row_n, label in (("nic_xmit_GBps", 3, "TX GB/s"),
                                      ("nic_rcv_GBps",  3, "RX GB/s")):
            for rank in sorted(nic_leader["rank"].unique()):
                lr = nic_leader[nic_leader["rank"] == rank].sort_values("module")
                fig.add_trace(go.Scatter(
                    x=lr["module"], y=lr[src_col],
                    name=f"node-leader r{rank} {label}",
                    mode="lines+markers",
                ), row=row_n, col=1 if src_col == "nic_xmit_GBps" else 2)
        fig.add_hline(y=peak, line_dash="dash", line_color="red",
                      annotation_text=f"peak {peak:g} GB/s",
                      row=3, col=1)
        fig.add_hline(y=peak, line_dash="dash", line_color="red",
                      annotation_text=f"peak {peak:g} GB/s",
                      row=3, col=2)
    fig.update_layout(height=900, title="OS counters per module")
    return fig


def fig_decision_matrix(matched: list) -> go.Figure:
    if not matched:
        rows = [["—", "—", "No dominant pattern detected.", "—", "—"]]
    else:
        rows = []
        for module, rule in matched:
            rows.append([
                str(module),
                rule["id"],
                rule["diagnosis"],
                rule["knob"],
                rule["rationale"],
            ])
    fig = go.Figure(data=[go.Table(
        header=dict(
            values=["module", "rule", "diagnosis", "next knob to try", "rationale"],
            fill_color="lightgrey", align="left",
        ),
        cells=dict(
            values=list(zip(*rows)),
            align="left",
        ),
    )])
    fig.update_layout(
        title="Decision matrix: recommended next optimization",
        height=400 + 25 * len(rows),
    )
    return fig


# ---------------------------------------------------------------------------
# Derived-insight panels (added 2026-05-12). All viz-only — they read the
# columns already in pf_profile.csv and synthesize. See SECTION_DOCS for the
# one-line "how to read this" blurbs rendered above each panel.
# ---------------------------------------------------------------------------

def _module_wall_us(df: pd.DataFrame, module) -> float:
    """Elapsed wall time of a module. The master is alive for the whole
    module (distribute -> collect -> merge), so its wallclock is the best
    estimate of module elapsed time; fall back to the slowest rank."""
    sub = df[df["module"] == module]
    mas = sub[sub["role"] == "master"]["wallclock_us"]
    if not mas.empty and mas.max() > 0:
        return float(mas.max())
    w = sub["wallclock_us"].max()
    return float(w) if w and w > 0 else 1.0


def _significant_modules(df: pd.DataFrame, frac: float = 0.10, top_n: int = 5):
    """Modules worth a dedicated per-module panel: those >= `frac` of total
    run wallclock. If none clear the bar, the `top_n` heaviest. Returns the
    list sorted by module id (ascending), plus the list of dropped module ids.
    """
    walls = {m: _module_wall_us(df, m) for m in df["module"].unique()}
    total = sum(walls.values()) or 1.0
    keep = [m for m, w in walls.items() if w / total >= frac]
    if not keep:
        keep = [m for m, _ in sorted(walls.items(), key=lambda kv: kv[1], reverse=True)[:top_n]]
    keep_set = set(keep)
    dropped = sorted(m for m in walls if m not in keep_set)
    return sorted(keep), dropped


def _span_s(sub: pd.DataFrame, first_cols, last_cols):
    """Envelope [min(first), max(last)] in seconds over `sub`, ignoring the
    -1 sentinel (phase never fired). Returns None if nothing fired."""
    firsts, lasts = [], []
    for c in first_cols:
        if c in sub.columns:
            v = sub[c]
            v = v[v >= 0]
            if not v.empty:
                firsts.append(float(v.min()))
    for c in last_cols:
        if c in sub.columns:
            v = sub[c]
            v = v[v >= 0]
            if not v.empty:
                lasts.append(float(v.max()))
    if not firsts or not lasts:
        return None
    return min(firsts) / 1e6, max(lasts) / 1e6


def fig_module_dominance(df: pd.DataFrame) -> go.Figure:
    """Which module is the run? Bar of per-module wallclock, sorted, with a
    cumulative-% line — so you optimize the module that actually costs."""
    rows = sorted(((m, _module_wall_us(df, m) / 1e6) for m in df["module"].unique()),
                  key=lambda r: r[1], reverse=True)
    total = sum(w for _, w in rows) or 1.0
    mods = [str(m) for m, _ in rows]
    walls = [w for _, w in rows]
    cum, c = [], 0.0
    for w in walls:
        c += w
        cum.append(c / total * 100.0)
    fig = make_subplots(specs=[[{"secondary_y": True}]])
    fig.add_trace(go.Bar(x=mods, y=walls, name="module wallclock (s)",
                         text=[f"{w / total * 100:.0f}%" for w in walls],
                         textposition="outside"), secondary_y=False)
    fig.add_trace(go.Scatter(x=mods, y=cum, name="cumulative %", mode="lines+markers"),
                  secondary_y=True)
    fig.update_yaxes(title_text="wallclock (s)", secondary_y=False)
    fig.update_yaxes(title_text="cumulative % of run", range=[0, 105], secondary_y=True)
    fig.update_xaxes(title_text="module (sorted by cost)")
    fig.update_layout(title=f"Where the run's time goes — total ≈ {total:.0f}s over {len(rows)} module(s)",
                      height=420)
    return fig


def fig_utilization(df: pd.DataFrame) -> go.Figure:
    """Effective core utilization per module = Σ(rank wallclock) / (module
    wall × #ranks). The dual axis shows idle core-seconds wasted."""
    mods, util, idle = [], [], []
    for module in sorted(df["module"].unique()):
        sub = df[df["module"] == module]
        n = len(sub)
        wall = _module_wall_us(df, module)
        busy = float(sub["wallclock_us"].sum())
        mods.append(str(module))
        util.append(busy / (wall * n) * 100.0 if wall > 0 and n > 0 else 0.0)
        idle.append((wall * n - busy) / 1e6)
    fig = make_subplots(specs=[[{"secondary_y": True}]])
    fig.add_trace(go.Bar(x=mods, y=util, name="effective core utilization %",
                         text=[f"{u:.0f}%" for u in util], textposition="outside"),
                  secondary_y=False)
    fig.add_trace(go.Scatter(x=mods, y=idle, name="idle core-seconds", mode="lines+markers"),
                  secondary_y=True)
    fig.add_hline(y=100, line_dash="dot", secondary_y=False)
    fig.update_yaxes(title_text="utilization %", range=[0, 110], secondary_y=False)
    fig.update_yaxes(title_text="idle core-seconds", secondary_y=True)
    fig.update_xaxes(title_text="module")
    fig.update_layout(title="Effective core utilization per module", height=420)
    return fig


def fig_mr_effectiveness(df: pd.DataFrame) -> go.Figure:
    """The headline MR metric: dedup ratio (out/in), shuffle volume, per-role
    disk writes, mean shipped term size. In a non-MR run reducers are absent
    and 'out' falls back to slave→master bytes (ratio ≈ 1)."""
    has_red = (df["role"] == "reducer").any()
    has_mer = (df["role"] == "merger").any()
    rows = []
    for module in sorted(df["module"].unique()):
        sub = df[df["module"] == module]
        m = sub[sub["role"] == "mapper"]
        r = sub[sub["role"] == "reducer"]
        g = sub[sub["role"] == "merger"]
        s = sub[sub["role"] == "master"]
        # Merger ranks are mapper ranks; their mapper-phase shuffle counts as in.
        bytes_in = float(m["bytes_sent"].sum()) + float(g["bytes_sent"].sum())
        if has_mer and not g.empty:
            bytes_out = float(g["bytes_mer_to_master"].sum())   # mergers are the last hop
        elif has_red and not r.empty:
            bytes_out = float(r["bytes_to_master"].sum())
        else:
            bytes_out = bytes_in
        terms = float(sub["terms_sent"].sum())
        rows.append(dict(
            module=str(module), bytes_in=bytes_in, bytes_out=bytes_out,
            dedup=(bytes_out / bytes_in) if bytes_in > 0 else float("nan"),
            bpt=(bytes_in / terms) if terms > 0 else float("nan"),
            io_map=float(m["io_write_bytes"].sum()),
            io_red=float(r["io_write_bytes"].sum()) if not r.empty else 0.0,
            io_mer=float(g["io_write_bytes"].sum()) if not g.empty else 0.0,
            io_mas=float(s["io_write_bytes"].sum()),
        ))
    mods = [x["module"] for x in rows]
    GB = 1e9
    fig = make_subplots(rows=2, cols=2, subplot_titles=(
        "Shuffle volume per module (GB)",
        "Dedup ratio = bytes to master / bytes shuffled  (&lt;1 ⇒ reducers shrank it)",
        "Disk write bytes by role per module (GB)",
        "Mean shipped term size on the wire (bytes / term)",
    ))
    fig.add_trace(go.Bar(x=mods, y=[x["bytes_in"] / GB for x in rows],
                         name="mapper→reducer in"), 1, 1)
    fig.add_trace(go.Bar(x=mods, y=[x["bytes_out"] / GB for x in rows],
                         name="reducer→master out"), 1, 1)
    fig.add_trace(go.Scatter(x=mods, y=[x["dedup"] for x in rows], mode="lines+markers",
                             name="dedup ratio"), 1, 2)
    fig.add_hline(y=1.0, line_dash="dot", row=1, col=2)
    fig.add_trace(go.Bar(x=mods, y=[x["io_map"] / GB for x in rows], name="mapper io_write"), 2, 1)
    fig.add_trace(go.Bar(x=mods, y=[x["io_red"] / GB for x in rows], name="reducer io_write"), 2, 1)
    if has_mer:
        fig.add_trace(go.Bar(x=mods, y=[x["io_mer"] / GB for x in rows], name="merger io_write"), 2, 1)
    fig.add_trace(go.Bar(x=mods, y=[x["io_mas"] / GB for x in rows], name="master io_write"), 2, 1)
    fig.add_trace(go.Scatter(x=mods, y=[x["bpt"] for x in rows], mode="lines+markers",
                             name="bytes / term"), 2, 2)
    fig.update_yaxes(title_text="GB", row=1, col=1)
    fig.update_yaxes(title_text="ratio", row=1, col=2)
    fig.update_yaxes(title_text="GB", row=2, col=1)
    fig.update_yaxes(title_text="bytes/term", row=2, col=2)
    fig.update_layout(barmode="group", height=820,
                      title="MR effectiveness — dedup, shuffle volume, disk writes")
    return fig


def fig_critical_path(df: pd.DataFrame, modules=None) -> go.Figure:
    """Module-wall decomposition: per module, the time envelope of each
    pipeline stage drawn against the module wallclock backdrop. Gaps between
    consecutive stages and the leading/trailing slack against the backdrop
    are the pipeline fill+drain — the part overlap can't hide.

    `modules` restricts to a subset (default: the significant ones)."""
    if modules is None:
        modules, _ = _significant_modules(df)
    all_mods = sorted(modules)
    first_mod = all_mods[0] if all_mods else None
    has_red = (df["role"] == "reducer").any()
    fig = go.Figure()
    shown = set()
    y_labels = []
    for module in all_mods:
        sub = df[df["module"] == module]
        m = sub[sub["role"] == "mapper"]
        r = sub[sub["role"] == "reducer"]
        g = sub[sub["role"] == "merger"]
        s = sub[sub["role"] == "master"]
        wall_s = _module_wall_us(df, module) / 1e6
        stages = [("module wall", 0.0, wall_s, "lightgray", f"module {module} wallclock {wall_s:.1f}s")]
        sp = _span_s(s, ["t_mas_distribute_first_us"], ["t_mas_distribute_last_us"])
        if sp:
            stages.append(("master distribute", sp[0], sp[1], None, "master term-distribution loop"))
        sp = _span_s(m, ["t_map_generator_first_us"], ["t_map_generator_last_us"])
        if sp:
            stages.append(("mapper generator", sp[0], sp[1], None, "mappers Generator() loop"))
        if has_red and not r.empty:
            sp = _span_s(r, ["t_red_recv_wait_first_us", "t_red_buffer_copy_first_us", "t_red_merge_patches_first_us"],
                            ["t_red_recv_wait_last_us", "t_red_buffer_copy_last_us", "t_red_merge_patches_last_us"])
            if sp:
                stages.append(("reducer shuffle/ingest", sp[0], sp[1], None, "reducers receiving + copying + merging incoming"))
            sp = _span_s(r, ["t_red_final_sort_first_us"], ["t_red_final_sort_last_us"])
            if sp:
                stages.append(("reducer final sort", sp[0], sp[1], None, "reducers EndSort"))
            sp = _span_s(r, ["t_red_forward_wait_first_us", "t_red_forward_mpi_first_us"],
                            ["t_red_forward_wait_last_us", "t_red_forward_mpi_last_us"])
            if sp:
                fwd_sink = "merger" if not g.empty else "master"
                stages.append((f"reducer→{fwd_sink} forward", sp[0], sp[1], None,
                               f"reducers shipping sorted stream to {fwd_sink}"))
        else:
            sp = _span_s(m, ["t_map_send_wait_first_us", "t_map_send_mpi_first_us"],
                            ["t_map_send_wait_last_us", "t_map_send_mpi_last_us"])
            if sp:
                stages.append(("slave→master send", sp[0], sp[1], None, "slaves shipping to master (non-MR)"))
        if not g.empty:
            sp = _span_s(g, ["t_mer_merge_first_us"], ["t_mer_merge_last_us"])
            if sp:
                stages.append(("merger merge", sp[0], sp[1], None,
                               "mapper-mergers k-way merging leaf reducers + forwarding to master"))
        sp = _span_s(s, ["t_mas_collect_first_us"], ["t_mas_collect_last_us"])
        if sp:
            stages.append(("master collect", sp[0], sp[1], None, "master receiving from reducers / slaves"))
        sp = _span_s(s, ["t_mas_final_sort_first_us"], ["t_mas_final_sort_last_us"])
        if sp:
            stages.append(("master final sort", sp[0], sp[1], None, "master merge-tree EndSort"))
        for name, a, b, color, note in stages:
            y = f"m{module} · {name}"
            y_labels.append(y)
            is_wall = (name == "module wall")
            sl = bool(module == first_mod) and (not is_wall) and (name not in shown)
            if sl:
                shown.add(name)
            fig.add_trace(go.Bar(
                x=[max(b - a, wall_s * 0.003)], base=[a], y=[y], orientation="h",
                marker_color=color, name=name, legendgroup=name, showlegend=sl,
                hovertext=[f"{name}: {a:.2f}s → {b:.2f}s ({b - a:.2f}s)<br>{note}"],
                hoverinfo="text", opacity=0.4 if is_wall else 0.85,
            ))
    fig.update_layout(
        barmode="overlay",
        title="Critical-path / module-wall decomposition (stage time envelopes vs module wallclock backdrop)",
        xaxis_title="time since module start (s)", yaxis_title="",
        yaxis=dict(categoryorder="array", categoryarray=y_labels[::-1]),
        height=max(420, 16 * len(y_labels) + 140), bargap=0.2,
    )
    return fig


# Disjoint phase sets per role (no double counting — t_map_endsort_total
# already contains send_wait/send_mpi/hash_pack; t_mas_distribute contains
# distribute_wait). Unaccounted = wallclock - Σ these.
_COVERAGE_SEGMENTS = {
    "mapper":  [("MAP_GENERATOR", "t_map_generator_us"),
                ("MAP_ENDSORT_TOTAL", "t_map_endsort_total_us"),
                ("MAP_GETTERM_WAIT", "t_map_getterm_wait_us")],
    "reducer": [("RED_RECV_WAIT", "t_red_recv_wait_us"),
                ("RED_BUFFER_COPY", "t_red_buffer_copy_us"),
                ("RED_MERGE_PATCHES", "t_red_merge_patches_us"),
                ("RED_FINAL_SORT", "t_red_final_sort_us"),
                ("RED_FORWARD_WAIT", "t_red_forward_wait_us"),
                ("RED_FORWARD_MPI", "t_red_forward_mpi_us")],
    "master":  [("MAS_DISTRIBUTE", "t_mas_distribute_us"),
                ("MAS_FINAL_SORT", "t_mas_final_sort_us"),
                ("MAS_COLLECT", "t_mas_collect_us")],
    # Merger panels show only the merge tier. MER_RECV/FORWARD are
    # sub-components of MER_MERGE, so MER_MERGE is the one disjoint phase.
    # Everything else on a merger rank's wallclock — the mapper phase it
    # ran first, plus any idle wait for its leaf reducers — falls into
    # UNACCOUNTED here by design.
    "merger":  [("MER_MERGE", "t_mer_merge_us")],
}


def fig_coverage(df: pd.DataFrame) -> go.Figure:
    """Per-module stacked bar of the disjoint instrumented phases plus an
    explicit UNACCOUNTED segment (= wallclock − Σ phases). A large grey
    segment on some role means the profiler is blind there — candidate for a
    new PF_TIMER bracket."""
    roles = present_roles(df)
    if not roles:
        return go.Figure()
    fig = make_subplots(rows=1, cols=len(roles),
                        subplot_titles=[r.capitalize() for r in roles])
    shown = set()
    for ci, role in enumerate(roles, start=1):
        sub = df[df["role"] == role]
        if sub.empty:
            continue
        modules = sorted(sub["module"].unique())
        segs = _COVERAGE_SEGMENTS[role]
        data = {lbl: [] for lbl, _ in segs}
        unacc = []
        for mod in modules:
            ms = sub[sub["module"] == mod]
            wall = float(ms["wallclock_us"].mean()) / 1e6
            tot = 0.0
            for lbl, c in segs:
                v = float(ms[c].mean()) / 1e6 if c in ms.columns else 0.0
                data[lbl].append(v)
                tot += v
            unacc.append(max(wall - tot, 0.0))
        x = [str(m) for m in modules]
        for lbl, _ in segs:
            sl = lbl not in shown
            shown.add(lbl)
            fig.add_trace(go.Bar(x=x, y=data[lbl], name=lbl, legendgroup=lbl, showlegend=sl),
                          row=1, col=ci)
        sl = "UNACCOUNTED" not in shown
        shown.add("UNACCOUNTED")
        fig.add_trace(go.Bar(x=x, y=unacc, name="UNACCOUNTED", legendgroup="UNACCOUNTED",
                             marker_color="#cccccc", showlegend=sl), row=1, col=ci)
        fig.update_xaxes(title_text="module", row=1, col=ci)
        fig.update_yaxes(title_text="time (s)", row=1, col=ci)
    fig.update_layout(barmode="stack", height=520,
                      title="Phase coverage per module (disjoint phases + UNACCOUNTED residual)")
    return fig


def fig_tail_stats(df: pd.DataFrame) -> go.Figure:
    """Table: per role per phase, mean / p50 / p95 / max (seconds) over all
    (module, rank) rows, plus max/mean. Wallclock is set by the slowest rank,
    so p95/max matter more than the means the other panels show."""
    role_cols = {
        "mapper":  ["wallclock_us", "t_map_generator_us", "t_map_endsort_total_us",
                    "t_map_send_wait_us", "t_map_hash_pack_us", "t_map_getterm_wait_us"],
        "reducer": ["wallclock_us", "t_red_recv_wait_us", "t_red_buffer_copy_us",
                    "t_red_merge_patches_us", "t_red_final_sort_us", "t_red_forward_wait_us",
                    "t_red_store_us"],
        "merger":  ["wallclock_us", "t_mer_merge_us", "t_mer_recv_wait_us",
                    "t_mer_forward_wait_us", "t_mer_forward_mpi_us"],
        "master":  ["wallclock_us", "t_mas_distribute_us", "t_mas_distribute_wait_us",
                    "t_mas_final_sort_us", "t_mas_merge_recv_wait_us", "t_mas_collect_us"],
    }
    rows = []
    for role, cols in role_cols.items():
        sub = facet_df(df, role)
        if sub.empty:
            continue
        for c in cols:
            if c not in sub.columns:
                continue
            v = sub[c].dropna() / 1e6
            if v.empty or v.max() == 0:
                continue
            mean = v.mean()
            rows.append([role, c.replace("t_", "").replace("_us", ""),
                         f"{mean:.2f}", f"{v.quantile(0.5):.2f}",
                         f"{v.quantile(0.95):.2f}", f"{v.max():.2f}",
                         f"{(v.max() / mean if mean > 0 else 0):.2f}×"])
    fig = go.Figure(data=[go.Table(
        header=dict(values=["role", "phase", "mean s", "p50 s", "p95 s", "max s", "max/mean"],
                    fill_color="lightgrey", align="left"),
        cells=dict(values=list(zip(*rows)) if rows else [[]], align="left"))])
    fig.update_layout(title="Phase tail statistics across all (module, rank) rows",
                      height=300 + 20 * len(rows))
    return fig


def fig_straggler_gap(df: pd.DataFrame) -> go.Figure:
    """Per module: (slowest-rank wallclock − mean) / mean, for mappers and
    reducers — the load imbalance the role-mean panels hide."""
    fig = go.Figure()
    any_data = False
    for role in ("mapper", "reducer", "merger"):
        sub = df[df["role"] == role]
        if sub.empty:
            continue
        modules = sorted(sub["module"].unique())
        gaps = []
        for mod in modules:
            w = sub[sub["module"] == mod]["wallclock_us"]
            mean = w.mean()
            gaps.append((w.max() - mean) / mean * 100.0 if mean and mean > 0 else 0.0)
        fig.add_trace(go.Bar(x=[str(m) for m in modules], y=gaps, name=f"{role} straggler gap %"))
        any_data = True
    if not any_data:
        fig.add_annotation(text="no mapper/reducer rows", showarrow=False)
    fig.update_layout(barmode="group", xaxis_title="module", yaxis_title="% over mean",
                      title="Straggler gap per module — (slowest rank − mean) / mean", height=400)
    return fig


def fig_wait_graph(df: pd.DataFrame) -> go.Figure:
    """Sankey of recorded wait time: each flow goes from the role that holds
    things up to the role that sits idle. Makes "bottleneck is upstream vs
    downstream of this rank" unambiguous. Values are core-seconds summed over
    all ranks and modules."""
    m = df[df["role"] == "mapper"]
    r = df[df["role"] == "reducer"]
    g = df[df["role"] == "merger"]
    s = df[df["role"] == "master"]
    has_mer = not g.empty

    def col(sub, c):
        return float(sub[c].sum()) / 1e6 if (not sub.empty and c in sub.columns) else 0.0

    flows = [
        ("master",   "mappers",  col(m, "t_map_getterm_wait_us"),
         "mappers idle waiting for master to dispatch terms"),
        ("reducers", "mappers",  col(m, "t_map_send_wait_us"),
         "mappers blocked on full send buffers (reducer back-pressure)"),
        ("mappers",  "reducers", col(r, "t_red_recv_wait_us"),
         "reducers idle waiting for mapper terms"),
        ("mappers",  "master",   col(s, "t_mas_distribute_wait_us"),
         "master blocked in PF_Wait4Slave during term distribution"),
        ("reducers", "master",   col(s, "t_mas_collect_us"),
         "master idle in end-of-module collect loop (last rank to report stats)"),
    ]
    if has_mer:
        # Merge tier: reducers feed the mergers, the mergers feed the master.
        flows += [
            ("mergers",  "reducers", col(r, "t_red_forward_wait_us"),
             "reducers blocked forwarding sorted stream to their merger"),
            ("reducers", "mergers",  col(g, "t_mer_recv_wait_us"),
             "mergers blocked mid-merge waiting for a leaf reducer's next sorted chunk (PF_PutIn)"),
            ("master",   "mergers",  col(g, "t_mer_forward_wait_us"),
             "mergers blocked forwarding the merged stream to master"),
            ("mergers",  "master",   col(s, "t_mas_merge_recv_wait_us"),
             "master blocked mid-merge waiting for a merger's next sorted chunk (PF_PutIn)"),
        ]
    else:
        flows += [
            ("master",   "reducers", col(r, "t_red_forward_wait_us"),
             "reducers blocked forwarding sorted stream to master"),
            ("reducers", "master",   col(s, "t_mas_merge_recv_wait_us"),
             "master blocked mid-merge waiting for a reducer's next sorted chunk (PF_PutIn)"),
        ]
    flows = [f for f in flows if f[2] > 1e-6]
    if not flows:
        fig = go.Figure()
        fig.add_annotation(text="no significant wait time recorded", showarrow=False)
        fig.update_layout(title="Wait graph: who blocks whom", height=300)
        return fig
    blockers = {"master": "master ▶ blocks", "mappers": "mappers ▶ block",
                "reducers": "reducers ▶ block", "mergers": "mergers ▶ block"}
    waiters = {"mappers": "mappers (idle)", "reducers": "reducers (idle)",
               "mergers": "mergers (idle)", "master": "master (idle)"}
    bnodes = ("master", "mappers", "reducers") + (("mergers",) if has_mer else ())
    wnodes = ("mappers", "reducers") + (("mergers",) if has_mer else ()) + ("master",)
    node_labels, bidx, widx = [], {}, {}
    for k in bnodes:
        bidx[k] = len(node_labels)
        node_labels.append(blockers[k])
    for k in wnodes:
        widx[k] = len(node_labels)
        node_labels.append(waiters[k])
    val = [f[2] for f in flows]
    fig = go.Figure(go.Sankey(
        node=dict(label=node_labels, pad=24, thickness=18),
        link=dict(source=[bidx[f[0]] for f in flows], target=[widx[f[1]] for f in flows],
                  value=val, label=[f"{f[2]:.1f}s — {f[3]}" for f in flows]),
    ))
    fig.update_layout(title=f"Wait graph: who blocks whom (Σ recorded wait ≈ {sum(val):.0f} core-seconds)",
                      height=480)
    return fig


def fig_merge_attribution(df: pd.DataFrame) -> go.Figure:
    """Per module, summed over reducers: how many MergePatches firings were
    triggered by the large-buffer running out of room (→ bump largesize /
    smallext) vs the patch-count cap (→ bump filepatches). Both can fire on
    one call so the stack can exceed patches_built. Dotted line: buffers
    absorbed per merge (buffers_received / patches_built)."""
    r = df[df["role"] == "reducer"]
    if r.empty:
        fig = go.Figure()
        fig.add_annotation(text="no reducers (non-MR run)", showarrow=False)
        fig.update_layout(title="MergePatches knob attribution", height=300)
        return fig
    modules = sorted(r["module"].unique())
    g = (r.groupby("module")
           .agg(lbuf=("merge_lbuffer_full", "sum"), maxp=("merge_max_patches", "sum"),
                built=("patches_built", "sum"), bufs=("buffers_received", "sum"))
           .reindex(modules))
    x = [str(m) for m in modules]
    fig = make_subplots(specs=[[{"secondary_y": True}]])
    fig.add_trace(go.Bar(x=x, y=g["lbuf"], name="fired: largesize/smallext full → bump largesize"),
                  secondary_y=False)
    fig.add_trace(go.Bar(x=x, y=g["maxp"], name="fired: filepatches cap hit → bump filepatches"),
                  secondary_y=False)
    fig.add_trace(go.Scatter(x=x, y=g["built"], name="patches_built (merges)", mode="lines+markers"),
                  secondary_y=False)
    fig.add_trace(go.Scatter(x=x, y=(g["bufs"] / g["built"].clip(lower=1)),
                             name="buffers absorbed per merge", mode="lines+markers",
                             line=dict(dash="dot")), secondary_y=True)
    fig.update_layout(barmode="stack", height=460,
                      title="MergePatches knob attribution (summed over reducers)")
    fig.update_yaxes(title_text="count", secondary_y=False)
    fig.update_yaxes(title_text="buffers / merge", secondary_y=True)
    fig.update_xaxes(title_text="module")
    return fig


def fig_throughput_reconciliation(df: pd.DataFrame) -> go.Figure:
    """Wire vs software throughput per module. NIC TX GB/s (host-wide
    node-leader counter — advisory unless place=scatter:excl) vs offered
    shuffle load (Σ bytes_sent / module wall) vs the mean per-mapper rate
    while actually sending. High while-active rate but low NIC ⇒ stalls are
    handshake/rendezvous overhead, not link bandwidth."""
    peak = _nic_peak_gbps()
    modules = sorted(df["module"].unique())
    nic, offered, active = [], [], []
    for mod in modules:
        sub = df[df["module"] == mod]
        wall_us = _module_wall_us(df, mod)
        msend = sub[sub["role"] == "mapper"]
        offered.append(float(msend["bytes_sent"].sum()) / wall_us / 1e3 if wall_us > 0 else 0.0)
        rt = msend["map_throughput_mbps"].dropna() if "map_throughput_mbps" in msend else pd.Series(dtype=float)
        active.append(rt.mean() / 1e3 if not rt.empty else float("nan"))
        leaders = sub[sub["nic_xmit_GBps"].notna()] if "nic_xmit_GBps" in sub else sub.iloc[0:0]
        nic.append(leaders["nic_xmit_GBps"].mean() if not leaders.empty else float("nan"))
    x = [str(m) for m in modules]
    fig = go.Figure()
    fig.add_trace(go.Scatter(x=x, y=nic, mode="lines+markers",
                             name="realized NIC TX GB/s (host-wide, node-leader)"))
    fig.add_trace(go.Scatter(x=x, y=offered, mode="lines+markers",
                             name="offered shuffle load GB/s (Σ bytes_sent / module wall)"))
    fig.add_trace(go.Scatter(x=x, y=active, mode="lines+markers",
                             name="per-mapper rate while sending GB/s (mean)"))
    fig.add_hline(y=peak, line_dash="dash", line_color="red",
                  annotation_text=f"assumed line peak {peak:g} GB/s")
    fig.update_layout(xaxis_title="module", yaxis_title="GB/s", height=460,
                      title="Throughput reconciliation — wire (NIC) vs software (offered / while-active)")
    return fig


def fig_compare_headline(mr: pd.DataFrame, org: pd.DataFrame) -> go.Figure:
    """MR-vs-org headline numbers: total run wallclock, sort-side disk writes
    (reducers in MR, slaves in org), master disk writes, and bytes delivered
    to the master. Grouped bars with % change vs org annotated on the MR bar."""
    def stats(d):
        run_wall = sum(_module_wall_us(d, m) for m in d["module"].unique()) / 1e6
        has_red = (d["role"] == "reducer").any()
        sort_io = (d[d["role"] == "reducer"]["io_write_bytes"].sum() if has_red
                   else d[d["role"] == "mapper"]["io_write_bytes"].sum()) / 1e9
        mas_io = d[d["role"] == "master"]["io_write_bytes"].sum() / 1e9
        to_master = (d[d["role"] == "reducer"]["bytes_to_master"].sum() if has_red
                     else d[d["role"] == "mapper"]["bytes_sent"].sum()) / 1e9
        return [run_wall, sort_io, mas_io, to_master]
    labels = ["run wallclock (s)", "sort-side disk write (GB)",
              "master disk write (GB)", "bytes to master (GB)"]
    mr_v, org_v = stats(mr), stats(org)
    pct = [f"{(a / b - 1) * 100:+.0f}%" if b else "" for a, b in zip(mr_v, org_v)]
    fig = go.Figure()
    fig.add_trace(go.Bar(x=labels, y=org_v, name="org", text=[f"{v:.1f}" for v in org_v],
                         textposition="outside"))
    fig.add_trace(go.Bar(x=labels, y=mr_v, name="MR",
                         text=[f"{v:.1f}  ({p})" for v, p in zip(mr_v, pct)],
                         textposition="outside"))
    fig.update_layout(barmode="group", title="MR vs org — headline metrics (% change is MR relative to org)",
                      height=460)
    return fig


# One-line "how to read this" blurbs rendered above each panel in the HTML.
SECTION_DOCS = {
    "module_dominance": "Which module is the run. Optimize the tall bars; the cumulative line tells you when you've covered most of the wall.",
    "effective_utilization": "Σ(rank wallclock) / (module wall × #ranks). Low % ⇒ many cores idle — the headline 'is parallelism working' number.",
    "phase_breakdown": "Stacked phase time per module per role (means). Mapper bar still over-stacks: ENDSORT_TOTAL already contains SEND_WAIT/SEND_MPI/HASH_PACK — see 'phase coverage' for the non-double-counted view. MAS_MERGE_RECV_WAIT (⊂ MAS_FINAL_SORT) and MER_RECV_WAIT (⊂ MER_MERGE) are shown only in the Gantt and tail-stats, not stacked here. The merger column shows only the MER_* phases — a merger rank's mapper-phase work is counted in the Mapper column instead.",
    "phase_coverage": "Disjoint phases + an explicit UNACCOUNTED residual. A big grey segment on a role = profiler blind spot worth a new PF_TIMER bracket — except the Merger facet, whose UNACCOUNTED also holds the merger rank's mapper phase (broken out in the Mapper facet) plus the idle gap before its leaf reducers start delivering.",
    "critical_path": "Per module, the time envelope of each pipeline stage vs the module-wall backdrop. Gaps between stages and leading/trailing slack against the backdrop are pipeline fill+drain — what overlap can't hide.",
    "mr_effectiveness": "The point of this fork. dedup ratio < 1 ⇒ reducers shrank the stream before the master; compare reducer vs master io_write to see disk saved.",
    "decision": "Heuristic rule matches → recommended next knob. Thresholds are starting points, not law.",
    "tail_statistics": "p50 / p95 / max per phase. Wallclock is set by the slowest rank — watch max/mean, not the means the other panels show.",
    "straggler_gap": "(slowest-rank wall − mean) / mean per module. High ⇒ load imbalance; cross-check the imbalance heatmap and (for reducers) hash distribution.",
    "wait_graph": "Recorded wait time as flows from the role that holds things up to the role that idles. The 'mid-merge' flow into the master is MAS_MERGE_RECV_WAIT — time the master sat blocked in PF_PutIn for its next sorted chunk (from reducers, or from mergers when the merge tier is active). With a merge tier you also see reducers→mergers (MER_RECV_WAIT) and master→mergers (MER_FORWARD_WAIT).",
    "imbalance": "Per-rank wallclock heatmap, mapper top, merge tier (reducer → merger → master) below.",
    "merge_attribution": "Why MergePatches fired — large-buffer-full (→ largesize/smallext) vs patch-cap (→ filepatches). Picks the knob the decision matrix only guesses at.",
    "software_throughput": "Per-rank MB/s while MPI was actively sending (bytes / send-time). NaN = no traffic that module.",
    "throughput_reconciliation": "Wire (NIC, host-wide) vs software throughput. High while-active rate but low NIC ⇒ handshake/rendezvous overhead, not bandwidth.",
    "os_counters": "io_write_bytes, context switches, MaxRSS, disk %util, NIC TX/RX with a peak reference line.",
    "compare_headline": "MR-vs-org headline numbers: run wallclock, sort-side & master disk writes, bytes to master. % change is MR relative to org.",
}


def _write_report(out: Path, title: str, intro_html: str, figs: list) -> Path:
    with open(out, "w") as fh:
        fh.write("<!DOCTYPE html><html><head><meta charset='utf-8'>")
        fh.write(f"<title>{title}</title>")
        fh.write("<style>body{font-family:sans-serif;max-width:1400px;margin:24px auto;}"
                 "h1,h2{margin-top:32px;} pre{background:#f4f4f4;padding:8px;}"
                 ".doc{color:#444;font-size:0.92em;margin:4px 0 10px;}"
                 "nav{background:#f7f7f7;padding:12px 16px;border-radius:6px;}"
                 "nav a{margin-right:14px;white-space:nowrap;}</style></head><body>")
        fh.write(f"<h1>{title}</h1>{intro_html}")
        fh.write("<nav><b>jump to:</b> " + " ".join(
            f"<a href='#{name}'>{name.replace('_', ' ')}</a>" for name, _ in figs) + "</nav>")
        first = True
        for name, fig in figs:
            fh.write(f"<h2 id='{name}'>{name.replace('_', ' ')}</h2>")
            if name in SECTION_DOCS:
                fh.write(f"<p class='doc'>{SECTION_DOCS[name]}</p>")
            fh.write(pio.to_html(fig, include_plotlyjs="cdn" if first else False, full_html=False))
            first = False
        fh.write("</body></html>")
    return out


def render_single(run_dir: Path) -> Path:
    df = load_csv(run_dir)
    matched = evaluate_decision(df)
    big_mods, dropped_mods = _significant_modules(df)
    figs = [
        ("module_dominance",          fig_module_dominance(df)),
        ("effective_utilization",     fig_utilization(df)),
        ("phase_breakdown",           fig_phase_breakdown(df)),
        ("phase_coverage",            fig_coverage(df)),
        ("critical_path",             fig_critical_path(df, big_mods)),
        ("mr_effectiveness",          fig_mr_effectiveness(df)),
        ("decision",                  fig_decision_matrix(matched)),
        ("tail_statistics",           fig_tail_stats(df)),
        ("straggler_gap",             fig_straggler_gap(df)),
        ("wait_graph",                fig_wait_graph(df)),
        ("imbalance",                 fig_imbalance_heatmap(df)),
        ("merge_attribution",         fig_merge_attribution(df)),
        ("software_throughput",       fig_software_throughput(df)),
        ("throughput_reconciliation", fig_throughput_reconciliation(df)),
        ("os_counters",               fig_os_counters(df)),
    ]
    # Per-module Gantt only for the modules worth the page weight.
    for module in big_mods:
        figs.append((f"gantt_module_{module}", fig_per_rank_gantt(df, module)))
    if dropped_mods:
        note = ("<p class='doc'>Per-module panels (critical path, Gantt) are "
                f"limited to the {len(big_mods)} module(s) that each take ≥10% of "
                f"run wallclock — modules {dropped_mods} are omitted there "
                "(they still appear in the cross-module summary panels).</p>")
    else:
        note = ""
    return _write_report(run_dir / "pf_profile_report.html",
                         f"MRmpi profile report — {run_dir}",
                         f"<p>Source: <code>{run_dir}</code></p>{note}", figs)


def render_compare(mr_dir: Path, org_dir: Path) -> Path:
    mr  = load_csv(mr_dir)
    org = load_csv(org_dir)
    mr["__src"] = "MR"
    org["__src"] = "org"
    combined = pd.concat([mr, org], ignore_index=True)
    roles = present_roles(combined)
    fig_phase = make_subplots(
        rows=2, cols=len(roles),
        subplot_titles=[f"{src} {role}"
                        for src in ("MR", "org") for role in roles],
    )
    shown = set()
    for row_idx, src in enumerate(("MR", "org"), start=1):
        sub_all = combined[combined["__src"] == src]
        for col_idx, role in enumerate(roles, start=1):
            sub = facet_df(sub_all, role)
            for phase_col, phase_label, phase_role in PHASE_COLS:
                if phase_role not in ROLE_OWNS[role] or phase_label in NON_ADDITIVE_PHASES:
                    continue
                grouped = sub.groupby("module")[phase_col].mean() / 1.0e6
                sl = phase_label not in shown
                shown.add(phase_label)
                fig_phase.add_trace(go.Bar(
                    x=grouped.index, y=grouped.values, name=f"{src}/{phase_label}",
                    legendgroup=phase_label,
                    showlegend=sl,
                ), row=row_idx, col=col_idx)
            fig_phase.update_xaxes(title_text="module", row=row_idx, col=col_idx)
            fig_phase.update_yaxes(title_text="time (s)", row=row_idx, col=col_idx)
    fig_phase.update_layout(barmode="stack", height=900,
                            title="Phase breakdown: MR vs org")

    matched_mr  = evaluate_decision(mr)
    matched_org = evaluate_decision(org)

    figs = [
        ("compare_headline",          fig_compare_headline(mr, org)),
        ("phase_breakdown",           fig_phase),
        ("mr_effectiveness",          fig_mr_effectiveness(mr)),
        ("critical_path",             fig_critical_path(mr)),
        ("wait_graph",                fig_wait_graph(mr)),
        ("decision_MR",               fig_decision_matrix(matched_mr)),
        ("decision_org",              fig_decision_matrix(matched_org)),
    ]
    return _write_report(mr_dir / "pf_profile_compare.html",
                         "MRmpi vs org compare",
                         f"<p>MR: <code>{mr_dir}</code><br>org: <code>{org_dir}</code></p>"
                         "<p class='doc'>Phase breakdown is MR (top row) vs org (bottom). "
                         "The critical-path and wait-graph panels are for the MR run; "
                         "run the single-run report on the org dir for its versions.</p>",
                         figs)


# AC.sMRflag values (ftypes.h): the per-module chain state stamped into the CSV.
SMRFLAG_LABEL = {0: "classic", 1: "MAPREDUCE", 2: "LAST(gather)", 4: "FIRST"}


def print_bypass_summary(df: pd.DataFrame) -> None:
    """One row per module of the master-bypass merger-tier metrics: which state
    the module ran in, how much the mergers distributed/partitioned, and how the
    master's merge split between a real chain-exit gather (MAS_GATHER) and a
    classic non-MR merge (MAS_FINAL_SORT). Makes the bypass directly legible:
    FIRST/MAPREDUCE modules should show ~0 MAS_FINAL_SORT (master off the path)."""
    if "nummergers" not in df.columns or df["nummergers"].max() <= 0:
        return
    print("\n=== master-bypass merger-tier per-module summary ===")
    print(f"{'mod':>4} {'state':>13} {'dist_terms':>12} {'mean_part_MB':>12} "
          f"{'MER_DIST_s':>10} {'MAS_GATHER_s':>12} {'MAS_FINAL_s':>11} {'MERGERDONE_s':>12}")
    for m, g in df.groupby("module"):
        flag = int(g["smrflag"].iloc[0]) if "smrflag" in g.columns else 0
        merg = g[g["role"] == "merger"]
        mas  = g[g["role"] == "master"]
        dist_terms = int(merg["mer_distribute_terms"].sum())
        part_mb = (merg["mer_partition_bytes"].mean() / 1e6) if len(merg) else 0.0
        dist_s  = (merg["t_mer_distribute_us"].mean() / 1e6) if len(merg) else 0.0
        gather_s = mas["t_mas_gather_us"].sum() / 1e6
        final_s  = mas["t_mas_final_sort_us"].sum() / 1e6
        done_s   = mas["t_mas_mergerdone_us"].sum() / 1e6
        if (flag == 0 and dist_terms == 0
                and gather_s < 0.05 and final_s < 0.05 and done_s < 0.05):
            continue   # nothing bypass-related happened in this module
        print(f"{m:>4} {SMRFLAG_LABEL.get(flag, str(flag)):>13} {dist_terms:>12} "
              f"{part_mb:>12.1f} {dist_s:>10.1f} {gather_s:>12.1f} {final_s:>11.1f} {done_s:>12.2f}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("dirs", nargs="+", type=Path,
                        help="One run dir for single-run report, two for MR-vs-org compare")
    args = parser.parse_args()
    if len(args.dirs) == 1:
        try:
            print_bypass_summary(load_csv(args.dirs[0]))
        except Exception as e:
            print(f"(bypass summary skipped: {e})")
        out = render_single(args.dirs[0])
    elif len(args.dirs) == 2:
        out = render_compare(args.dirs[0], args.dirs[1])
    else:
        parser.error("expected 1 or 2 run dirs")
    print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
