
# MRmpi merge-tree scaling analysis — theoretical model

*Analytical study, calibrated from the `pf_profile.csv` files in `tmp/` — the 34-run
historical archive plus the **2026-05-22 merger sweep** (5 runs: G=0 / G=4 / node-local
at two reducer fractions) that measured the merge tree directly. Companion to the
merge-tree implementation effort.*

Objective: as the Spin-class workload scales up (more terms, more data, more ranks),
decide **how many reducers `R`**, **what merger / master fan-ins `W_m` / `W_M`**, and
**what tree depth `L`** keep the run **compute-bound** — i.e. CPU-limited, not stalled
on disk, memory, or network.

---

## 0. Executive summary

Calibration of the real profiles **overturns three assumptions** the planning phase
started from, and reframes the merge tree as a *scale-forward* investment rather than a
current-scale win:

1. **The run is already compute-bound — on mapper term generation, not on the sort.**
   In the drainfix6 6n×28 baseline (7,016 s), the three dominant modules (9, 10, 11 =
   99.4 % of wallclock) each have their critical path set by the **slowest mapper's
   `Generator` loop**. Reducers sit **42–100 % idle** (`recv_wait`); the master sits
   **70 % idle** (`distribute_wait`).

2. **Disk is not a wall — but the reducer is disk-backed, not streaming.** The reducer
   stores the terms it receives from mappers into **patches on local disk** (`/gtmp`)
   and merges all of them in its `EndSort`; the patches sit on disk until that merge
   consumes them. The plan's `R ≥ T/C_buf` rule mis-modelled this: `C_buf` (the
   in-memory sort buffer) is the patch *chunk size*, not a spill threshold the slice
   must exceed. Measured — dominant module — the reducers wrote **~1.6 GB each (≈89
   patches)** while the **master wrote 273 GB** (the final output): the master, not
   the reducers, is the dominant disk writer. Per-reducer patch volume scales with the
   received slice and shrinks with `R`; it binds only when it approaches local `/gtmp`
   capacity — far off today.

3. **Network is nowhere near a wall, and per-rank memory barely moves with `R`.**
   Cross-node traffic runs at **~1.4 % of NIC capacity**. Receive buffers *subdivide a
   fixed arena*, so master/reducer memory is roughly **constant in fan-in**, not `∝ R`.

4. **The merger tier, now measured with a proper A/B sweep, gives no clear win at
   current scale.** The 2026-05-22 sweep (4 nodes × 240 ranks, K=128) ran G=0 vs G=4
   vs node-local at two reducer fractions. At `r17` (43 reducers): 5,613 s / 5,465 s /
   5,443 s — a ≤3 % spread, within Zeus run-to-run variance. At `r30` (74 reducers):
   6,350 s vs 6,358 s — flat. **Node-local and strided are indistinguishable**
   (5,443 vs 5,465 s). The merger nodes were profiled for the first time and spend
   ~35–50 % of their time *forward-wait-blocked on the master* — the tree parallelises
   the merge, then queues at the serial master.

5. **The K-prefix drain already does most of what the merge tree was meant to do.**
   It cuts master `final_sort` **7.4×** (9,256 s → 1,251 s at `R=24`) and the run
   **2.1×**. With the drain active the master is already near `Θ(D)` — the irreducible
   cost of writing `D` output bytes.

6. **The merge's compute is already cache-resident; the binding cap on fan-in is the
   prefetcher.** Loser tree + K-prefix compare frontier total tens of KB (L1/L2 at any
   sane `W`); the hardware prefetcher's ~8–16 streams/core sets a cache-derived cap
   **`W ≈ 8–12`** (Section 6), and the **drain doubles as the merge's cache-optimality
   mechanism** (sequential memcpy). The one DRAM-latency hazard is the mapper local
   sort's random access into the 100–600 MB `smallsize` buffer — currently in
   generation's shadow, but the structural risk to watch.

7. **Per-role `largesize` decoupling pays off — measured −14.1 % on Spin (2026-05-28).**
   The §10 hypothesis (mapper smaller / reducer larger) was A/B-tested on
   `zeus_combined_q` 8 × 30 × 150 GB: bufdecouple (mapper `largesize 1 GB`,
   `smallsize 50 MB`, `reducerlargesize 5 GB`) ran in **8,680 s** vs baseline
   (2 GB everywhere) **10,098 s** — Δ = −1,418 s. **91 % of the saving is module
   11 alone (−31 %)**, where the bigger reducer absorbed the mapper stream
   faster and **mapper `send_wait` dropped 75 %** (388 → 95 s). Modules 10 and
   12 saw essentially no change — module 10 was Generator-bound (`send_wait`
   only 87 s in baseline) and module 12 was already at `send_wait = 0`. The
   lever helps wherever the profile shows real mapper send-wait stall; outside
   those modules it's neutral. See [[project_bufdecouple_result]].

8. **Confirmed at ~10× scale, and the partition form now measured (2026-06).**
   dRGT_h3_9 (~40× Spin's term volume) profiled at 6n×64 is **generation-bound
   exactly as §3 predicts** — module 9: mapper `Generator` **27.4 h**, reducers idle
   **24.76 h**, sort only **2.5 h**, master idle in `mergerdone` the **full 30 h**.
   The **partition / master-bypass form** (§13 — mergers write node-local and feed
   the next module's mappers directly) takes the master off the per-module path and
   carried the **first complete dRGT run** (job `4331798`, 8n×64, 52 h, output sha
   `2e80d362`) where the master-funnelled path timed out at 72 h on 6n. zstd held the
   distributed scratch at **173 TB→3.9 TB (44×)**. The dominant lever is still
   **mapper count** (the +29 % mappers from 6→8 nodes drove the wallclock); partition
   is the *enabler* that kept the master and its `/gtmp` from becoming the new wall.
   See §12.4 and §13.

**Bottom line.** The merge tree is worth building, but as **insurance for the large-`R`
regime** (memory/Irecv-count safety + parallelising the non-drained sift), not as a
near-term speed-up. The dominant lever for "scale up and stay compute-bound" is
**mapper `Generator` parallelism** plus **keeping the drain effective**. Section 10
gives the concrete cluster-configuration recommendations.

---

## 1. Method and the cross-version caveat

The profile CSVs span many builds of `parform`. They split into cohorts by the run-dir
name, cross-checked against `git log` and the `project_binary_state` memory:

| Cohort | Runs | Use |
|---|---|---|
| **drainfix6** (commit 3dce3f6) | 5 (`6n_r9`, `6n_29r_r13`, `…_K192`, `…_nopfx`, `4n_44r`) | **code-sensitive constants** — the lineage the tree extends |
| **local-merger sweep** (2026-05-22, merger + nodelocal build) | 5 — `4n_60c_r17/r30` × `g0/g4/nl` | **direct merge-tree A/B; first merger-phase profiling** |
| merger-early (7d62242 + uncommitted) | `merge4`, `exp_mergewait/{r12,r25}` | merger cross-check (no merger-phase columns) |
| drainfix1–5, drain-era, pfx-routing, pre-pfx | 24 | workload-intrinsic constants only; **trends, not absolutes** |

Two practical consequences of the version skew:

- **The CSV schema itself changed** (75 → 78 → 92 cols). The 78-col jump (commit
  7d69131) added `MAS_MERGE_RECV_WAIT`; the **92-col jump** added the `merger` role and
  `t_mer_merge / _recv_wait / _forward_wait` phases. Column presence dates a file.
- **Merger COMPUTE is now measured.** The 2026-05-22 sweep uses the 92-col schema, so
  the merger row of the cost model (Section 4) and the merge-tree verdict (Section 7)
  rest on direct measurement — no longer inferred from reducer/master rates.

**Calibration rule applied throughout:** workload-intrinsic quantities (term counts,
byte volumes, dedup ratio) are read from any cohort and are stable; code-sensitive
quantities (phase times, throughputs) are taken **only from drainfix6**.

---

## 2. The workload, calibrated (`Spin2_h5_45_mr.frm`)

82 modules; **three dominate**, the rest are noise:

| Module | Wallclock | % run | Shuffle `T` | To-master `D` | shrink `σ=D/T` | Character |
|---|---|---|---|---|---|---|
| 9 | 3,149 s | 44.9 % | **2,657 GB** | 274 GB | **0.103** | big-data sort; ~10× shrink |
| 10 | 2,350 s | 33.5 % | 169 GB | 158 GB | **0.938** | low-data; terms ~all distinct |
| 11 | 1,473 s | 21.0 % | ~0 | ~0 | — | pure transform; **no sortable output** |
| other 79 | ~43 s | 0.6 % | — | — | — | negligible |

Workload-intrinsic constants (version-robust — identical across all cohorts):

- Module 9 emits exactly **4.078 M terms** (deterministic).
- **The shrink factor `σ` is module-dependent**, `σ ∈ [0.10, ~1.0]`. Module 9 shrinks
  ~10×; module 10 essentially not at all. *Whether `σ` is like-term coefficient summing
  or delta-compression gain from terms becoming adjacent at the reducer is not resolved
  by the profile data — and the two project notes (`CLAUDE.md` vs the
  `feedback_mr_no_reducer_dedup` memory) disagree. The model treats `σ` as a measured,
  module-dependent workload constant and does not depend on the mechanism.*

Implication for the merge tree: it relays `D = σ·T` bytes. For module 9 that is small
(274 GB); for module 10 it is ~all of `T`. **A deeper tree relays more total bytes
(`L·D`)** — cheap when `σ` is small, not free when `σ ≈ 1`.

---

## 3. Where the time actually goes (drainfix6, 6n×28, R=16)

Per-module critical path, calibrated:

| | Module 9 | Module 10 | Module 11 |
|---|---|---|---|
| wallclock | 3,149 s | 2,350 s | 1,473 s |
| mapper `Generator` (mean) | **2,226 s** | **1,580 s** | **1,473 s** |
| reducer `recv_wait` (idle) | 1,339 s (42 %) | 1,691 s (72 %) | 1,473 s (100 %) |
| reducer merge (`merge_patches`+`final_sort`) | 1,774 s | 658 s | 0 |
| master `final_sort` | 951 s | 770 s | 0 |
| master `distribute_wait` (idle) | 2,196 s (70 %) | — | — |

Reading: **every heavy module's wallclock is the slowest mapper's `Generator` time plus
a merge tail.** The sort pipeline runs in the shadow of generation — reducers and the
master are mostly idle. `map_getterm_wait ≈ 0`, so the master's dispatch is *not*
starving mappers; the master is simply idle waiting for mappers to finish (per the
`feedback_profile_metric_framing` memory, `distribute_wait` is master-idle, not a
workload stall).

The **merge tail** (master `final_sort`, the part the merge tree targets) is ~900 s on
module 9 and ~770 s on module 11 — call it **~25 % of the run, ~29 % of module 9's
critical path**. That is the entire budget the merge tree competes for, and the drain
has already taken a 7× bite out of it (Section 7).

*Confirmed at ~10× scale (dRGT 6n×64, 2026-06; §12.4).* The same structure holds far
above Spin: module 9's **30 h** wall is mapper `Generator` **27.4 h** with the reducers
**82 % idle** (`recv_wait` 24.76 h, sort only 2.5 h) and the master **100 % idle**
(`mergerdone` — the partition form, §13, keeps it off the per-module path). "Heavy
module = slowest mapper's `Generator` + a merge tail in its shadow" is not a small-`g`
artifact; it is the workload's intrinsic shape, and it sharpens at scale (the merge
tail is a *smaller* fraction of a bigger generation cost).

---

## 4. The cost model

Parameters: `M` mappers, `R` reducers, `W_m` merger fan-in, `W_M` master fan-in,
`L = ceil(log_{W_m}(R/W_M)) + 1` depth, `n[k]` nodes at level `k`, `Q` physical nodes.
Workload per module: `T` shuffle bytes, `D = σ·T` to-master bytes, `W_gen` total
`Generator` CPU-work. A loser-tree merge of `k` streams over `B` bytes costs
`Θ(B·log k)`; a K-prefix-drained bucket costs `Θ(B)` (bulk memcpy).

| Role | COMPUTE | MEMORY | DISK | NETWORK (cross-node \| intra-node) |
|---|---|---|---|---|
| Mapper ×M | `Θ(W_gen/M)` generate + local sort — **the dominant term** | ~4 GB measured; `R` send-bufs subdivide a fixed arena | ~0 (MR mappers stream) | sends `T/M`; aggregate `≈ T·(Q−1)/Q` cross-node |
| Reducer ×R | `Θ((T/R)·log M)` — `EndSort` merges all patches; runs in generation's shadow | ~9 GB measured; `M` recv-bufs subdivide a fixed arena → **~flat in M** | stores received terms as **patches on local disk**, merged in `EndSort`; **measured ~1.6 GB/reducer (≈89 patches)** — scales with the slice, shrinks with `R` | recv `T/R`; send `D/R` up |
| Merger lvl k ×n[k] | `Θ((D/n[k])·log W_m)`; drain → `Θ(D/n[k])` — **measured ~130 MB/s/merger** (533 s for ~68 GB, 2026-05-22) plus a large `forward_wait` queued on the master | `W_m` recv-bufs subdivide a fixed arena; ~5 GB measured | ~0 (streams upward) | relays `D` — mergers do **not** shrink it (277→274 GB measured); tree total `L·D`, of which `(L−p)·D` cross-node |
| Master | `Θ(D·log W_M)`; **with drain ≈ `Θ(D)`** (≈500 MB/s memcpy, measured) | ~3.4 GB measured; `W_M` recv-bufs subdivide a fixed arena | writes final `D` | recv `D` from `≤ W_M` streams |

Calibrated rates (drainfix6, module 9): master `final_sort` CPU ≈ 544 s for `D`=274 GB
⇒ **~500 MB/s** (memcpy-bound, drain active). Reducer merge ≈ 274 GB out / 1,774 s ⇒
**~155 MB/s**. Mapper `Generator` is the workload's own expansion rate and is the term
that must be parallelised.

**Memory-hierarchy annotation (see §6).** Per role, what's cache-resident vs streaming:
- **Mapper** — per-term `Generator` scratch fits L2; the local sort's random
  pointer-derefs into the 100–600 MB `smallsize` buffer are the one DRAM-latency
  hazard. Send-buffer write frontiers (`R·64 B`) stay cache-resident.
- **Reducer** — patch I/O is sequential (page-cache + prefetcher); the `EndSort`
  loser-tree merge of `M` mapper streams is large-`W` but runs in generation's shadow.
- **Merger / Master** — loser tree (~`64W` B) + K-capped compare frontier (~`4·W·K` B)
  are L1/L2-resident; the `W` input streams are sequential and **prefetcher-tracked
  only up to ~8–16 streams/core** — this fixes the cache-derived cap `W ≈ 8–12`. The
  drain is pure sequential memcpy — cache-optimal.

---

## 5. Resource walls — what calibration confirmed and refuted

| Wall | Plan hypothesis | Calibrated verdict |
|---|---|---|
| **Disk** | reducer spills when `T/R > C_buf` ⇒ `R ≥ T/C_buf` | **Mis-modelled.** The reducer stores received terms as **patches on local disk** and merges them in `EndSort` — `C_buf` is the patch *chunk size*, not a spill threshold. Measured ~1.6 GB/reducer (≈89 patches); the **master** writes the bulk (273 GB final output). Per-reducer patch volume scales with the received slice and shrinks with `R`; binds only near local `/gtmp` capacity — far off today. |
| **Mapper memory** | send-buffer memory `∝ R` ⇒ `R ≤ R_max` | **Refuted as stated.** The `R` send-buffers subdivide a fixed arena; total memory ≈ constant in `R`. What grows is *per-buffer smallness*. |
| **Merger/master memory** | recv-buffer memory `∝ W` | **Refuted as stated.** Same fixed-arena subdivision (`PF_InitTree`: `size = (sTop2−lBuffer−1)/(numtasks−1)`, floored at `2·MaxTer`). Memory ≈ flat in fan-in. |
| **Fan-in (real form)** | — | The receive-buffer size formula is `arena/(PF.numtasks − 1)` (parallel.c:473) — **divided by the global rank count, not the local fan-in** — so per-source size is invariant of `W`. The genuine fan-in limit is therefore the prefetcher (§6, `W ≈ 8–12`) and the loser-tree management overhead (more later at `R ≳ 100`, §7), not buffer-granularity-per-fan-in as previously claimed. Per-source buffer = arena / (PF.numtasks − 1) must still stay ≥ `2·MaxTer` (hard) and ≥ `256 KB` (UCX rendezvous soft), but that is a rank-density constraint on `PF.numtasks`, not a fan-in constraint on `W`. |
| **Network** | `X_cross/wallclock → Q·B_nic` | **Confirmed as a wall, but ~70× away.** Measured ~175 MB/s/node vs ~12,375 MB/s NIC = **1.4 %**. |
| **Master compute** | tree caps `Θ(D·log R)` → `Θ(D·log W_M)` | **Confirmed — but the drain already collapsed `log R`.** Without the drain master `final_sort` is `Θ(D·log R)` (nopfx: 9,256 s); with it ≈ `Θ(D)` (1,251 s). The tree's remaining job is the *non-drained* sift and the Irecv-count, not `log R` on the bulk. |
| **Per-node RAM** | (not in plan) | Aggregate RSS vs node RAM. 2026-05-22 sustained **60 ranks/node** at 5–7 GB peak RSS on 200 GB nodes; lifetime-peak sums overcount (peaks not coincident across modules), so real headroom exceeds `ranks × peak`. **Still the closest non-compute wall** — watch it as `form.set` buffers or ranks/node grow. |

**The compute-bound condition, restated from the data:** the run stays compute-bound as
long as (a) `R` keeps each reducer's patch set and merge bounded (per-reducer disk
within local `/gtmp`) while staying small enough that the master is not swamped by
Irecv management; (b) ranks/node keeps aggregate RSS under node RAM; (c) the drain
stays effective so the master is `Θ(D)`. All three hold comfortably today — **the binding resource is mapper
`Generator` CPU.**

---

## 6. Cache & the memory bus

A run streaming TBs through a 27 MB L3 cannot be "DRAM-free" — that is physically
impossible. The achievable goal is to split each stage into a small **random-access hot
set** (must be cache-resident) and a large **sequentially-streamed set** (DRAM is fine
there — bandwidth-bound, the hardware prefetcher hides the latency). DRAM is harmful
only for the hot set, where each miss is an ~80–100 ns core stall. **Cache-aware =**
keep the hot set ≤ cache, keep everything else strictly sequential.

### Cache hierarchy (Zeus, two node classes)

| Class | CPU | Cores/node (logical) | L2/core (private) | L3/socket |
|---|---|---|---|---|
| Small (`ncpus=80` — 2026-05-22 sweep) | Xeon Gold 6230 Cascade Lake-SP | 40 (80 HT) | **1 MB** | **27.5 MB** |
| Large (`ncpus=128` — production Spin) | Ice Lake-SP class (Platinum 8358) | 64 (128 HT) | **1.25 MB** | **48 MB** |

DRAM: small class 6×DDR4-2933 ≈ 140 GB/s/socket (≈280 GB/s/node). With 60 ranks/node
that's ~4.7 GB/s per-rank DRAM bandwidth — vs the drain memcpy at ~0.5 GB/s — so
**DRAM is latency-bound, not bandwidth-bound** at current scale. The reliable per-rank
cache budget at 1.5 ranks/physical core is **~0.5–1 MB** (L2 contended + an L3 slice).
A "1 rank per physical core" deployment gives each rank a full private 1 MB L2 — a
cache-aware option (see §10).

### Merge stage — the compute is *already* cache-resident

- **Loser tree:** `2W−1` NODEs × 32 B ≈ **64W bytes**. `W=32` → 2 KB; `W=256` → 16 KB.
  Fits L1 trivially for any realistic fan-in.
- **Compare frontier** (`CompareTerms` with K-prefix cap, `parallel.c:1304-1311`): the
  `W` term-heads of `K` WORDs each ≈ `4·W·K` bytes. `W=16, K=128` → 16 KB. L1/L2-
  resident.
- **The streaming set** is the `W` input buffers' bulk bytes — `W` concurrent
  sequential consumers + 1 output stream + drain scratch. The hardware prefetcher
  tracks roughly **8–16 streams per core**; beyond that the merge falls off the
  prefetcher and pays full DRAM latency per fetch. **That is the cache-derived cap on
  fan-in: `W ≈ 8–12`** — it tightens the doc's `W_m ∈ [8,16]` (Section 5 buffer-
  granularity range) toward the low end with a hardware reason that *binds before*
  buffer granularity does.
- **The drain is the merge's cache-optimality mechanism.** A drained run is one
  sequential memcpy — prefetcher-perfect, no random access. A sifted term, by
  contrast, is touched ~`log W` times along the loser-tree path. Keeping the drain
  hit-rate high is *the* cache strategy for the merge.
- `K` is **not** cache-constrained at sane values (≤ 4 KB/term-head). Tune `K` freely
  for drain hit-rate.

### Generation stage — per-term work cache-resident; the local sort is the hazard

- **`Generator`** (`proces.c:3259+`) processes one term at a time. The active region
  of the 160 MB `WorkSpace` arena is one term's expansion — typically ≤ tens of KB
  ≤ L2. Per-term pattern-match/normalise stays cache-local **as long as the typical
  term ≤ L2**; near-`MaxTer` (160 KB) terms blow L2 — workload-dependent.
- **The hazard is mapper-specific.** The mapper local sort sorts a pointer array
  `sPointer` whose comparisons dereference terms scattered across the **100–600 MB
  `smallsize` buffer** (`SORTING` struct, `structs.h:1115`). The buffer is ≫ L3, so
  every compare-deref risks a DRAM-latency miss — **and this happens on the rank that
  is *on the critical path***. The reducer uses its `smallsize` buffer differently
  (patch arena + merge workspace, with much more sequential access), and runs with
  42–100 % idle, so the same buffer size on the reducer side does not pay the same
  latency cost. This asymmetry is what motivates the per-role buffer decoupling
  recommended in §10.
  - **Knob-level mitigation:** cache-sized sort chunks — make each in-memory sort run
    fit L2/L3 before flushing a patch, then merge patches sequentially. FORM already
    chunks into patches; the lever is `termsinsmall` / `smallsize`.
  - **Deferred code idea:** explicit cache-blocking of the in-memory sort
    (sort-then-merge of L3-sized blocks). Out of scope for this analytical doc.
- **Hash routing** touches only the first `K` WORDs (sequential, small) — cache-fine.
  The `R` send-buffer write frontiers are `R·64 B` ≤ 5 KB even at `R=74` — cache-fine.

### Cross-cutting: latency, not bandwidth (at current ranks/node)

DRAM bandwidth has ~10× headroom per rank at 60 ranks/node, so cache-awareness here =
**kill random access**, not minimise bytes. Bandwidth contention only becomes the
binding mode at much higher ranks/node — note the crossover for very dense deployments
(>~120 ranks/node) but it does not bind today.

### What this implies for the knobs

- `W_m`, `W_M` should sit at the **low end** of the buffer-granularity range —
  `~8–12` — driven by the prefetcher cap, not memory.
- **Buffer sizes want to be decoupled per role.** On the mapper, `largesize` and
  `smallsize` should be **small** (~L3-sized for `smallsize`, ~512 MB–1 GB for
  `largesize`) so the local sort stays cache-resident and frees RAM for more mapper
  ranks. On the reducer (idle ≥ 42 %), `largesize` can be **larger** (~3–4 GB) to hold
  more sorted runs and improve cross-mapper combining before patch extraction. See §10
  for the joint recommendation and §11 for the implementation note.
- `K` is free to tune for drain hit-rate (cache-cheap at all sane values).
- Consider **1 rank/physical core** as a cache-aware deployment option (each rank
  gets a full private 1 MB L2); the alternative (HT-filled 1.5–2 ranks/core) trades
  cache for `Generator` throughput. See §10.

---

## 7. The merge tree: cost, benefit, and the crossover scale

**What the drain already did.** At `R=24`, the K-prefix drain takes master `final_sort`
from 9,256 s (nopfx) to 1,251 s — a 7.4× cut — and the run from 14,690 s to 6,641 s.
With the drain the master is `Θ(D)`: it must stream and write `D` bytes, and the drain
makes that nearly pure memcpy. **`Θ(D)` is irreducible — no merge tree beats it**,
because every output byte passes through the master on its way to the output file.

**What the merge tree can still add:**

1. **Parallelise the non-drained sift.** The fraction of terms the drain cannot
   bulk-copy still sifts through a loser tree of depth `log(fan-in)`. A merger tier
   moves that work off the master onto `n[1]` mergers running concurrently.
2. **Bound the master's Irecv count and loser-tree management** at `W_M` instead of
   `R`. Not a memory wall (Section 5), but `R` concurrent rendezvous handshakes and an
   `R`-leaf loser tree are real per-emit overhead once `R` reaches the hundreds.
3. **Drain at every level** (the working tree already enables this — `drain_active`
   has no role gate). A drained K-prefix bucket is bulk-copied at *each* hop; correct
   at any depth because the routing invariant (a K-prefix → one reducer) is
   depth-invariant. Cost stays `Θ(D/n[k])` per node, `Θ(L·D)` total memcpy distributed
   across the tree — the master still does only `Θ(D)`.
4. ~~**Grow the master's per-source receive buffer from `largebuf/R` to `largebuf/G`**.~~
   *(Retracted 2026-05-23 after re-reading the code.)* The earlier claim that the
   merger tier grows per-source buffer size is wrong by the code. `PF_InitTree`
   ([parallel.c:473](../../sources/parallel.c#L473)), `PF_ReducerInit`
   ([parallel.c:2812](../../sources/parallel.c#L2812)), and `PF_allocateSbuf`
   ([parallel.c:3046](../../sources/parallel.c#L3046)) all compute
   `size = (sTop2 − lBuffer − 1) / (PF.numtasks − 1)` — **divided by the global
   MPI rank count (`PF.numtasks`), not by the local loser-tree fan-in.**
   `PF.numtasks` is set once at `PF_LibInit` ([mpi.c:193](../../sources/mpi.c#L193))
   and never reassigned, so the merger tier (which lowers the *local* `numtasks`
   from `numreducers + 1` to `nummergers + 1`) **does not change per-source
   buffer size**. What it does change is *how many* `rbuf` slots point into
   `lBuffer` (lines 484–492: the loop runs to the local `numtasks`); the rest of
   `lBuffer` arena is unused. For a dRGT-style 5n × 64 × -r17 run with G = 5:
   per-source buffer ≈ 3.5 GB / 319 ≈ 11 MB regardless of merger config — not
   400 MB. The genuine merger-tier benefits remain points 1–3 above (parallelised
   sift, lower Irecv count, drain at every level). If per-source buffer size *is*
   the binding factor on some future workload, the lever is bigger `largesize` /
   `smallextension`, not the merger tier. (A code change to swap `PF.numtasks − 1`
   for the local `numtasks − 1` at the three sites above would make the merger
   tier behave as the earlier claim assumed; flagged in §11.)

**What the merge tree cannot do:** speed up generation (modules 9/10/11 are
generation-bound); shrink `D`; help module 11 at all (no sortable output); or help the
reducer→mapper fan-in (the tree sits *above* the reducers).

**Measured reality check (2026-05-22 sweep).** A proper A/B — G=0 vs G=4 vs node-local,
4 nodes × 240 ranks, K=128 — confirms the tree is **not a current-scale win**:

| Config | `r17` (43 reducers, 197 mappers) | `r30` (74 reducers, 166 mappers) |
|---|---|---|
| G=0 (no mergers) | 5,613 s | 6,350 s |
| G=4 strided | 5,465 s (−2.6 %) | 6,358 s (+0.1 %) |
| G=4 node-local | 5,443 s (−3.0 %) | — |

The ≤3 % at `r17` is within Zeus run-to-run variance; `r30` is flat. **Why so little:**
the profiled merger nodes spend ~530 s merging but ~290 s (`r17`) to ~548 s (`r30`)
*forward-wait-blocked on the master* — the four mergers parallelise the merge work and
then **queue at the single master**. The master's `Θ(D)` sink is serial; a merge tree
relocates the merge but cannot remove that queue. Master `final_sort` barely moved
(`r17`: 1,248 → 1,171 s; `r30`: 1,258 → 1,260 s). Separately, `r17` beat `r30` by
**12 %** (5,613 vs 6,350 s) — the extra reducers stole mapper ranks from the
generation-bound critical path (Section 10).

**Crossover scale.** The tree starts paying off when the master's *non-`Θ(D)`* overhead
— the non-drained sift `Θ(D_nd·log R)` plus `R`-way Irecv management — becomes a
visible slice of the merge tail. The 2026-05-22 sweep gives measured points at `R=43`
and `R=74` — **both ≤3 %**, so the crossover is *not yet reached at R=74*:

- `R ≤ ~75`: direct reducer→master is competitive — the tree buys ≤3 %; keep `L = 1`
  unless the large-`R` safeguards (master Irecv-count, non-drained sift) are needed.
- `R ≳ 100–150`: a **single merger layer** (`L = 2`) should earn a clearer margin.
  This is now an **extrapolation past the measured `R=74` point**, not a confident
  number — the merge tail must be both large *and* sift-bound, and with the drain
  active it is mostly `Θ(D)` memcpy that no tree beats.
- `R ≳ 1000`: a **second merger layer** (`L = 3`). Not reachable by this workload.

---

## 8. Placement — strided vs node-local

`X_cross` (cross-node bytes) `= T·(Q−1)/Q + (L−p)·D`, with `p=1` for node-local level-1
placement, `p=0` for strided. Node-local moves exactly one relay level (`D` bytes) from
NIC to intra-node shared memory.

**Calibrated verdict: measured — node-local = strided, ~0 s.** The 2026-05-22 sweep ran
the identical `r17` config strided (G=4) and node-local: **5,465 s vs 5,443 s — a 0.4 %
difference, indistinguishable, exactly as the model predicted.** Network runs at 1.4 %
of NIC, so moving one relay level (`D` ≈ 274 GB) from NIC to shared memory saves
`ΔT_net ≈ 3 s` — and only *if* that leg were on the critical path, which it is not
(reducers forward while idle 42 % of the time). Node-local placement becomes relevant
only at **~30–50× the current data rate**, or if ranks/node rises enough to make
per-node NIC contention real. **Recommendation:** keep node-local level-1 placement —
it is the right structure and costs nothing — but treat it as correctness/defensive
infrastructure, not a wallclock lever at current or near scale.

---

## 9. Scaling envelope — staying compute-bound

As the workload grows by a factor `g` (`T, D, W_gen → g×`) with the rank budget grown
to match:

| Resource | Scales as | Keep-flat rule | First to bind? |
|---|---|---|---|
| Mapper `Generator` | `W_gen/M` | `M ∝ g` | **The intended bottleneck — keep it the bottleneck.** |
| Reducer merge | `(T/R)·log M` | `R ∝ g`, watch `log M` | Soft — runs in generation's shadow until `R` lags. |
| Reducer patch disk | received slice / `R` | `R ∝ g` keeps per-reducer patches small | Only near local `/gtmp` capacity — far off. |
| Master output disk | `Θ(D) = Θ(g·D₀)` written to `/gtmp` | local scratch must hold `D`; ~290 MB/s, within disk BW | Capacity, not bandwidth — watch `/gtmp` free space at large `g`. |
| Master `final_sort` | `Θ(D) = Θ(g·D₀)` with drain | irreducible; parallelise sift via tree | Grows linearly with `g`; overtakes the gen tail at `g ≈ 4–6×`. |
| Per-node RAM | `ranks/node × per-rank RSS` | watch aggregate vs node RAM | **Closest non-compute wall — with slack.** 2026-05-22 sustained 60 ranks/node at 5–7 GB peak RSS on 200 GB nodes; peak sums overcount. |
| Network | `X_cross/wallclock` | — | ~70× headroom; effectively never for this workload. |

The run stays compute-bound across a wide range of `g` provided `M` and `R` scale with
`g` and ranks/node stays bounded. The one term that grows *and* lands on the critical
path is the master merge tail `Θ(g·D₀)` — which the merge tree parallelises (sift) but
cannot drive below `Θ(D)`.

*Partition update (2026-06; §13).* With the master-bypass form the master's per-module
`final_sort` row above **no longer accumulates**: the `Θ(D)` master pass happens once
at the chain-end gather, not per module, so the `master_total` is `Θ(D_final)` instead
of `Σ_m Θ(g·D₀)` and never "overtakes the gen tail" at large `g`. The dRGT run
(93 modules, master measured idle mid-chain) confirms it. So the `g ≈ 4–6×`
master-merge crossover in the table applies **only to the relay / non-partition
forms**; under partition the binding term stays mapper `Generator` as `g` grows — and
the second growing term, the master's *single-node* output scratch, is replaced by `G`
node-local compressed files (the dRGT `/gtmp` fix, §13.4).

---

## 10. Recommended cluster configuration

Workers per node, reducer fraction, and buffer sizes are **one coupled decision**. The
run is mapper-`Generator`-bound (Section 3); everything below is set to
**maximise mapper ranks while keeping the sort in generation's shadow**.

### Settings (recommended · reason · confidence)

| Knob | Recommended | Reason | Confidence |
|---|---|---|---|
| **Workers / node** | Fill toward the logical-CPU count — **~60–80 small class** (40 phys + HT), **~100–128 large class** (64 phys + HT) | Generation-bound ⇒ more mappers = faster; `Generator` is branchy → HT helps; *concurrent* RSS ≈ 3 GB/rank (the 60/node sweep held inside a 200 GB request), so memory is **not** the cap | High direction; 60/node measured-good, full HT (80) untested |
| **Reducer fraction `R / P`** | **~15–18 %** (`-r15`–`-r18`) | Measured 2026-05-22: `r17` (43/197) beat `r30` (74/166) by **12 %**; reducers run 42–100 % idle | High — direct A/B |
| **`largepatches`** | **1024** | Phase-B sweep: `fp=128/lp=1024` beats v3 by ~24 % | High — measured |
| **`filepatches`** | **128** | Same sweep | High — measured |
| **`smallextension`** | **1.5 GB** | Phase-B sweep: `sext=1.5G` adds ~17 % | High — measured |
| **`smallsize`** | **Shrink globally toward L3** (no per-role key) | Mapper's local sort is the §6 DRAM-latency hazard — a smaller `smallsize` (toward L3 ≈ tens of MB) makes it cache-resident. **Not decoupled per role**: the reducer never writes `sBuffer` in MR mode (`PF_StoreBuffer` memcpy's straight to lBuffer; `EndSort` enters with `sTerms = 0`), so a smaller `smallsize` doesn't hurt it. The `setfile.c:922` floor `16·MaxTer` ≈ 19 MB clamps any too-small value silently. | **Hypothesis — global shrink in flight 2026-05-23** |
| **`scratchsize`** | **400 MB** | Inherited; not the lever | Low priority |
| **`sortiosize`** | **4 MB** | Inherited; not the lever | Low priority |
| **`processbucketsize`** | **50–100** | Smaller bucket → finer mapper load balance; 2026-05-22 used 50 | Medium |
| **`largesize`** | **Decouple: mapper ~1 GB, reducer ~5 GB** — via the `reducerlargesize` form.set key (per-role plumbing landed 2026-05-23). Only `largesize` is decoupled; `smallsize` / `smallextension` are not — the reducer never writes its sBuffer and the shuffle arena is anchored at the mapper values | Asymmetric work: bigger on the mapper hurts twice (cache + RAM); bigger on the reducer absorbs the mapper stream faster ⇒ less `send_wait` stall. **Measured 2026-05-28: this exact setting cut Spin wallclock 14.1 % on `zeus_combined_q` 8 × 30 × 150 GB layout, with 91 % of the win on module 11 alone (mapper `send_wait` −75 %, reducer `recv_wait` −33 %)** | **Confirmed — first clean A/B on combined queue (§0 point 7, [[project_bufdecouple_result]])** |
| **Master fan-in `W_M`** | **`8–16`** (low end of `[16, 32]`) | Prefetcher cap (§6) + shallow loser tree | Model-derived |
| **Merger fan-in `W_m`** | **`8–12`** | Prefetcher stream cap (§6); recv buffers stay above the 256 KB rendezvous threshold | Model-derived |
| **Tree depth `L`** | **`L = 1`** while `R ≲ ~75`; `L = 2` only at `R ≳ 100–150`; `L = 3` only beyond `R ≳ 1000` | Measured at `R=43, 74`: tree ≤ 3 %; the crossover is past `R=74` | Measured + extrapolation |
| **Placement** | **Node-local level-1**; strided above | Measured 2026-05-22: node-local = strided to 0.4 % — keep for correct structure, not speed | High — measured |
| **K-prefix `K`** | **128** (current); tune for drain hit-rate | Drain is the merge's cache-optimality mechanism (§6); `K=128 ≈ K=192` empirically | High — measured |
| **Drain** | **Enabled at every tree level** | Correct at any depth; carries the master to `Θ(D)` | Confirmed |
| **Ranks per physical core** | **1.5–2 (HT-filled)** for throughput; **1/core** is the cache-aware alternative | Generation-bound + HT-helps → throughput choice; if the local-sort LLC-miss rate (§11) turns out to dominate, switch to 1/core | Model-derived |

### The coupling — why the three big knobs go together

The run is mapper-`Generator`-bound, and the reducer carries hours of idle slack. Every
rank spent as a reducer, every GB of RAM spent on mapper-side buffers, is a mapper rank
not running. So:

- **Workers/node** → push to the memory / logical-CPU cap; the generation critical
  path scales with `1/M`.
- **Reducer count** → just enough to keep the sort in generation's shadow. Reducers
  are already 42–100 % idle at `~17 %` of ranks, so "just enough" is small.
- **Buffer sizes — decouple per role.** A bigger buffer **on the mapper hurts twice**:
  (a) the local sort spans a >L3 region, so each compare risks a DRAM-latency miss
  (§6) — directly slowing the critical path; (b) the RAM isn't available for more
  mapper ranks, which is the binding lever. A bigger buffer **on the reducer helps**:
  more sorted runs co-resident ⇒ more cross-mapper combining (only the reducer sees
  all copies of a given K-prefix term) ⇒ fewer & larger patches ⇒ less EndSort work,
  paid for out of the reducer's idle budget. ~17 % of ranks are reducers, so growing
  *their* RSS while shrinking *the mapper's* nets positive on cluster RAM.

The cache analysis (§6) supplies the rest: keep fan-ins at the low end (~8–12) so the
merge stays prefetcher-tracked. The local-sort DRAM-latency hazard is **mapper-
specific** and is exactly what motivates the asymmetric tune.

### Priority ordering for "scale up and stay compute-bound"

1. **Mapper `Generator` parallelism** — add mappers; this is the real bottleneck.
2. Keep the **K-prefix drain effective at scale** — the master's cache-optimality
   mechanism (§6).
3. Keep **aggregate RSS under node RAM** (60 ranks/node sustained on 200 GB; lifetime-
   peak sums overcount, so dense HT deployments are worth trying).
4. The **merge tree** — build it now as depth-general infrastructure, but measured
   ≤ 3 % at `R ≤ 74`: a `g ≳ 4` / `R ≳ 100` optimisation, not a `g = 1` one.
5. **Per-role `largesize` decoupling** — *now measured (2026-05-28).* mapper
   `largesize 1 GB / smallsize 50 MB` + `reducerlargesize 5 GB` beat the
   single-value 2 GB baseline by **−14.1 %** on Spin (8 × 30 × 150 GB
   `zeus_combined_q`). 91 % of the win is module 11 (the no-sortable-output
   transform module where mappers were piling on the reducer): mapper
   `send_wait` −75 %, reducer `recv_wait` −33 %. Set this on any heavy
   workload whose profile shows non-trivial mapper `send_wait`; neutral
   elsewhere.

**dRGT confirmation (2026-06; §12.4).** At ~10× Spin's scale the ordering holds and
sharpens: dRGT's heavy modules are generation-bound with reducers **82 % idle** at
both `-r9` and `-r12`, so the reducer-fraction recommendation pushes *lower* — a very
large generation-bound workload tolerates **`R/P ≤ 12 %`**, spending the freed ranks on
mappers (the 6n→8n mapper increase, not any sort-side knob, is what completed the run).
Two table rows are explicitly **not** the lever for such a workload: the merge tree
(item 4 — the partition form already takes the master off the path) and
`reducerlargesize` (the dRGT run carried 10 GB with no isolable benefit). Everything
else in the table is workload-neutral and stands. The one new must-have for dRGT-class
runs is the **partition form + zstd** (§13): it is what keeps the master and its
`/gtmp` off the wall list as `M`, `R`, and the module count all grow.

---

## 11. Open questions / what to measure next

1. **Merger nodes are now profiled** *(closed 2026-05-22)*. The 92-col schema confirms
   merger COMPUTE ≈ 130 MB/s/merger and shows the dominant merger phase is
   *forward-wait on the master* (~290–548 s) — the master is the serial sink. Still
   unmeasured: a **multi-layer** tree (`L ≥ 3`) and the **drain-at-merger** hit-rate
   per level.
2. **The shrink mechanism (`σ`).** Resolve whether the ~10× module-9 shrink is
   like-term summing or delta-compression adjacency — it determines whether a *deeper*
   tree shrinks `D` further (compression) or just relays it (summing already done).
3. **`B_shm` is uncalibrated.** Node-local's `ΔT_net` uses an assumed `β ≈ 6`; an
   intra-node `osu_bw` probe would pin it. Low priority — network is 70× from binding.
4. **The crossover is now bracketed, not pinned.** `R=43` and `R=74` both measured
   ≤3 %, so the first clear merger win needs `R ≳ 100–150` — still unmeasured. An
   `L=2` run at `R ≈ 120` would confirm it.
5. **Cache empirics, unmeasured.** Confirm the Zeus CPU's hardware prefetcher stream
   count and the mapper local-sort LLC-miss rate with
   `perf stat -e LLC-load-misses,l2_rqsts.*` to validate the `W ≈ 8–12` cap (§6) and
   quantify the local-sort DRAM-latency hazard.
6. **Per-role lBuffer decoupling — closed 2026-05-28.** Code (single
   `reducerlargesize` form.set key in `setfile.c::RecalcSetups`) and matched-
   queue A/B both landed. Result: mapper `1 GB / 50 MB` + `reducerlargesize
   5 GB` beats `2 GB everywhere` by **−14.1 %** on Spin (8 × 30 × 150 GB,
   `zeus_combined_q`). 91 % of the win is module 11 (no-sortable-output
   transform), driven by mapper `send_wait −75 %`. The two siblings tried
   initially (`reducersmallsize`, `reducersmallextension`) were dropped on a
   code re-read: reducer never writes `sBuffer` (`PF_StoreBuffer` memcpy's
   straight to `lBuffer`; reducer-`EndSort` enters with `sTerms = 0`) and the
   shuffle arena is anchored at the mapper values via
   `PF.shuffle_arena_words`, so they would be RAM waste. Two follow-ups
   remain open: (a) sweep `reducerlargesize` across {3, 4, 5, 7} GB to
   bracket the optimum; (b) per-role reducer lBuffer high-water-mark
   instrumentation to confirm the 5 GB allocation is genuinely *used* on
   module 11. See [[project_bufdecouple_result]].

7. **Shuffle-pair bug from per-role decoupling — resolved with `PF.shuffle_arena_words`.**
   *(Discovered + fixed 2026-05-23 while bringing up the per-role decoupling A/B.)*
   The first `parform.bufdecouple` binary divided by the *local* arena at each
   formula site — so a reducer with `reducerlargesize=6 GB` sent shuffle slots
   sized to its own arena (~32 MB) into a master/merger expecting mapper-arena
   sized slots (~10 MB), producing certain `MPI_ERR_TRUNCATE` on the first
   reducer→master flush. **The fix** captures the mapper-arena once in
   `setfile.c::RecalcSetups` *before* the `reducer*` override into a new global
   `PF.shuffle_arena_words` ([parallel.h:206](../../sources/parallel.h#L206)),
   and the three formula sites (`PF_InitTree:473`, `PF_ReducerInit:2812`,
   `PF_allocateSbuf:3050`) all use it. Result: every rank computes the same
   slot size regardless of role; the reducer's grown `lBuffer` is now used
   exclusively for patch accumulation in `PF_StoreBuffer` (the actual
   decoupling payoff). The alternative — switching the divisor to the local
   `numtasks` to grow per-source buffer by `R/G` — was rejected because it
   would re-introduce the merger inheritance problem (mergers carry mapper-
   phase allocation; they can't hold reducer-sized chunks). If shuffle
   throughput later turns out message-rate-bound, a *separate* dedicated
   shuffle arena (sized independently of the sort buffer) is the cleaner path.

### What the 2026-06 dRGT runs answer (update 2026-06-21)

The first full-scale dRGT runs — `4310854` (6n×64 -r9, 72 h, profiled) and the
**completed** `4331798` (8n×64 -r12 `reducerlargesize 10 G`, 52 h, §12.4) — close or
sharpen several of the questions above:

- **(1) Merger profiling / multi-layer.** *Sharpened.* The **partition form** (§13)
  changes the merger from a relay into a redistributor, which **removes the
  ~290–548 s `forward_wait` on the serial master** that capped the 2026-05-22 relay
  tier — there is no per-module master sink to queue on (master measured **30 h idle**
  in `mergerdone`). A multi-layer tree (`L ≥ 3`) and per-level drain hit-rate are
  still unmeasured, but the relay tier's binding constraint is structurally gone.
- **(2) The shrink mechanism `σ`.** *New extreme data point; mechanism still open.*
  dRGT collapses **173 TB** of logical shuffle to a **one-term** final answer
  (`-1/311040·esfull(1)`), so its heavy modules are dominated by like-term
  summing/cancellation (`σ ≪ 1`). Whether the per-module collapse is coefficient
  summing or delta-adjacency is still not isolated by the profile — **still open**.
- **(3) `B_shm` uncalibrated.** *Relevance raised.* The partition form's
  merger→mapper redistribution is **intra-node shm on the inter-module critical
  path** now (not just the relay leg), so `B_shm` matters more — still uncalibrated,
  and network is still ~70× from binding, but an intra-node `osu_bw` probe is now
  worth the few minutes.
- **(4) The crossover scale.** *Reframed.* dRGT ran `R = 34` (-r9) and `R = 61`
  (-r12) — still below the `R ≳ 100–150` bracket, so the relay-tier crossover is
  still unmeasured. But under the partition form the merger is **structurally
  required** for master-bypass, not an optional tier with a crossover, so the
  question now applies only to non-partition runs and is **lower priority**.
- **(5) Cache empirics.** *Still open* — `perf`/VTune access remains blocked
  cluster-wide (no driver, paranoid=2; VTune-setup memory). The `W ≈ 8–12` cap and
  the mapper local-sort LLC-miss rate are still unconfirmed by hardware counters.
- **(6) `reducerlargesize` sweep.** *Partly touched, not settled.* The completing run
  used `reducerlargesize = 10 GB` — past the {3,4,5} GB sweep — but its effect is
  **unisolated** (8 nodes, -r12 and 10 GB changed together) and the generation-bound
  profile predicts ≈ 0 reducer-side benefit (reducers idle 82 % on the heavy module).
  So 10 GB is neither confirmed nor refuted; the clean same-layout sweep and the
  high-water-mark instrumentation (6b) are **still open**, and the dRGT evidence says
  the reducer buffer is *not* the dRGT lever regardless.

Net for the §10 priority ordering: the dRGT data **reinforces item 1** (mapper
parallelism is the lever, now confirmed at ~10× scale) and **demotes the merge-tree
and reducer-buffer items further** for generation-bound workloads — while elevating
the partition form from "optional tier" to **the structural enabler of scale-out**
(§13.5).

---

## 12. The dRGT_h3_9 calibration — a new regime past Spin

*Appended 2026-05-23. Spin (`g=1` per §2) is generation-bound and the MR
pipeline downstream of the diagram-load handles it comfortably. dRGT_h3_9
is the first workload that **breaks an assumption baked into §3–§10**:
that the diagram-load itself is in the noise. It isn't, structurally.*

### What changes at dRGT scale

The §2 workload model implicitly assumes the diagram-load is a small fixed
overhead (Spin: ~125 s on the master, producing 4,078,125 terms — exactly
`25 × 25 × 25 × 261` = vertex combinatorics, zero merging). dRGT keeps the
same script structure but the id substitution itself is on a different
scale:

| | Spin (`diags45`) | dRGT (`diags9`) |
|---|---|---|
| Top-level factors | 14 | 10 |
| Vertex factors | 4, summands `[25, 25, 25, 261]` | 3, **summands `[399, 399, 399]`** |
| Propagator factors | single-term (`I*prop(...)`) | **rich inner sums (~11 terms each)** |
| Pure vertex combinatorics | `25 × 25 × 25 × 261 = 4.08 M` | `399³ × ~11³ ≈ 83 B` |
| Diagram-load wallclock | ~125 s | **walltimes serially** (8h+ killed in `4075662`) |
| Diagram-load wraps in | `off parallel;` | `off parallel;` |

**The bottleneck migrates.** Spin's heavy modules are 9/10/11 (per §3),
i.e. inside the doall, and the diagram-load is in the 0.6 % "other"
bucket. dRGT *cannot reach* those modules in the bare MR variant — the
serial diagram-load on the master consumes the whole budget.

### The structural fix: split the id

`scripts/split_id.py` rewrites a giant `id diags<N>` substitution as a
sum of `K` opaque-symbol chunks + `K` parallel substitutions inside a
single `on parallel;` module. Algebraic equivalence by construction
(opaque symbols substituted back rewrite the same product). The
optimal-K math, from a two-term cost model:

```
T(K) ≈ T_props · K  +  V_total / min(K, M)
       └ serial ┘     └─── parallel ───┘
```

- `T_props` = work per opaque-symbol-triple in the first id (calibrated
  for dRGT as `prop_distribution³ ≈ 1300`, from the first split run's
  module-3 output exactly equal to `4096 × 1300 = 5,324,800`).
- `V_total` = raw module-4 expansion count (~83 B for dRGT).
- `M` = mapper rank count (~265 at 5n × 64 × -r17).

`K_opt = √(V_total/T_props) ≈ 8000` for dRGT, but anywhere in
`[256, 8000]` is essentially indistinguishable in total time and **all
mapper-bound at `V_total/M`** beyond `K = M`. Catastrophic K (e.g.
399³ ≈ 64 M, one chunk per summand): first-id serial regrows to
`V_total · T_props` and recreates the original wall.

### Splitter validation: zero merging in the first split id (dRGT)

The first dRGT split run (`4075681`, in flight as of this addendum)
emits `expr1 Terms in output = 5,324,800` at the end of module 3 —
exactly `4096 × 1300`. **Zero merging at this stage**, confirming
splitter correctness structurally (each opaque-symbol triple stays
distinct, by construction). Whether dRGT's *real* `.sort:Diagram Loaded`
(module 4 in the split variant — equivalent to Spin's full diagram-load)
will or won't merge depends on the vertex's within-summand scalar-only
parallelism, which is unmeasured.

### Why the §5 walls don't move (dRGT)

The §5 wall enumeration was per-rank; the dRGT diagram-load violates
none of those resource walls. It violates a wall §5 didn't enumerate:
**single-rank serial-CPU throughput on a script-level `off parallel;`
section**. The fix isn't a per-rank or per-tier knob — it's a
script-level rewrite (the split). Updated wall taxonomy:

| Wall class | §5 enumerated? | Lever |
|---|---|---|
| Per-rank disk / memory / NIC | yes | layout, form.set, merger tier |
| Master loser-tree fan-in | yes | drain (K-prefix), merger tier |
| Master memcpy throughput `Θ(D)` | yes | drain (already irreducible) |
| **Serial off-parallel module wallclock** | **no — Spin was small enough not to notice** | **script-level split (the splitter)** |

### Per-role/per-script implication

§5's "compute-bound condition" needs an additional clause:

> **(d) Every `off parallel;` module has either (i) trivial work — say
> ≤ a few minutes serial — *or* a structural rewrite (e.g. id split via
> `scripts/split_id.py`) that distributes the work into a parallel
> module.**

For Spin this clause is satisfied trivially (every off-parallel section
is small). For dRGT and any workload with large id substitutions, the
clause requires action — generated via the splitter, then validated
byte-identical against a small-scale baseline (or the Spin oracle).

### dRGT at full scale — profiled, then completed (update 2026-06)

The split + partition + zstd stack reached the **first complete dRGT_h3_9 result**.
Two production runs settle the regime:

- **`4310854`** (6n×64, -r9, K=192, partition form, drain-fixed binary `c2e6129`),
  72 h cap: ran clean the full 72 h (drain fix held — no crash) and **timed out** in
  module 12 (`dummyindices`) after 10 modules. Per-module master wall: m8 = 17.8 h,
  **m9 = 30.0 h**, m10 = 11.5 h — the heavy three are 82 % of the run. The §3 / §0-pt-8
  generation-bound profile is from this run. zstd 172.5 TB → 3.71 TB (46.5×).
- **`4331798`** (8n×64, -r12, K=192, `reducerlargesize 10 G`): **`Exit_status=0`,
  52 h 29 m**, `h_h_h.out` sha `2e80d362…`, final
  `amplhhhparts9 = -1/311040·esfull(1)` — the **whole ~93-module pipeline**. zstd
  173.2 TB → 3.94 TB (44×). This is now the dRGT baseline (no non-MR oracle yet;
  first complete run = baseline).

**Corrected diagnosis — supersedes the "sort-bound / need more reducers" inference.**
The 24 h killed run (`4285232`) inferred a *sort* wall from stalled `Terms in process`
stats; the direct profile shows the opposite. The heavy modules are
**generation-bound** (module 9: mapper `Generator` 27.4 h; reducers idle 24.76 h, sort
only 2.5 h; master idle in `mergerdone`). The lever is therefore **more mappers, a
*lower* `-r`** — not more reducers, not a bigger `reducerlargesize`. The 6n→8n jump
(+29 % mappers) is what turned the 72 h timeout into a 52 h completion; `-r12` and the
10 GB reducer buffer did not target the bottleneck and are not cleanly isolated. **The
partition form (§13) is the structural enabler**: it kept the master idle (off the
per-module path) and distributed the 173 TB scratch across the merger nodes, so
neither the master nor its single-node `/gtmp` became the new wall as the run scaled
to 8 nodes and the full module chain. See [[project_drgt_workload]].

### Connection to §10 / §11

§10's settings table is unchanged for dRGT — the merger tier, K-prefix,
form.set buffers, layout, all apply the same way once the diagram-load
is unblocked. Only the **first** entry of §10's priority ordering
shifts: for workloads where the bare MR variant has a serial-id wall,
"split the bare id" precedes "add mappers" because adding mappers does
nothing until the work is parallelizable.

Most of §11's open questions are now **partly answered** by the full-scale dRGT runs
— see the "2026-06 dRGT runs answer" update at the end of §11. The splitter also
introduces a measurement target still open: the **per-vertex scalar-coef merger
fraction**
(do dRGT's 399 summands per vertex group into fewer canonical tensor
patterns?). The cheapest probe is a `B gi, deltaF, Mom, dotp, prop;
.sort;` inserted before `.sort:Diagram Loaded;` in a split variant —
if the post-bracket term count drops materially vs without bracket, the
vertex has merger fraction worth exploiting; if not, the raw upper
bound (~83 B) is the real count. Not yet measured; bracket+MR broadcast
of `AR.BracketOn` in ParFORM is unconfirmed in the code.

---

## 13. Cost analysis across the FORM sort lineage

*Appended 2026-06-16, after the first complete dRGT run (job `4331798`, §12).
Sections 4 and 7 model the merger as a tier that **relays to the master**; the
**partitioned** form (master-bypass, committed `cfe4dc5` — mergers write
node-local files and redistribute the next module's input directly to node-local
mappers) is a different cost structure that takes the master off the per-module
critical path entirely. This section places all five variants on one axis and
gives the per-module → per-chain master-cost progression. It supersedes the
"merger relays `D` to master" picture of §4/§7 for mid-chain modules.*

The five variants are an evolution in which each removes the dominant cost of the
previous one:

| Variant | What it adds | The cost it removes |
|---|---|---|
| **TFORM** | shared-memory thread parallelism | serial single-thread generation |
| **ParFORM** | multi-node MPI | the single-node ceiling |
| **MR-ParFORM** | hash-route + reducer dedup | master gets the full undeduped stream; slaves write their whole sort to disk; master fan-in `P−1` |
| **MR + prefix drain** | K-prefix bulk-copy in the master merge | the master's `Θ(D·log R)` sift → `Θ(D)` memcpy |
| **MR + partition** | mergers write node-local + feed the next module directly | the master's per-module `Θ(D)` sink **and** its single-node global scratch |

### 13.1 Notation (extends §4)

`P` total ranks, `Q` nodes, `M` mappers, `R` reducers, `G` mergers (= reducer-
bearing nodes, §7). Per module: `T` shuffle bytes, `D = σ·T` output bytes
(`σ ∈ [0.1, 1]`, §2), `W_gen` generation CPU. A run is a **chain of `m` modules**,
so the inter-module expression is handed off `m−1` times. For ParFORM only:
`T'` = the slaves' locally-combined total, `D ≤ T' ≤ T` (each slave dedups within
its own share; cross-slave like terms combine only at the master merge).

### 13.2 The comparison

| Variant | Scope | Mapper/slave disk write | Dedup at | Master fan-in | Master compute / module | Inter-module hand-off | Binding wall |
|---|---|---|---|---|---|---|---|
| **TFORM** | 1 node, threads | spill only (if > buffers) | master thread merge | `W` threads (shm) | `Θ(T·log W)` | in-process RAM | **single node** (cores + RAM) |
| **ParFORM** | `Q` nodes, MPI | **full local sort → `≈T` to disk** | master merge (cross-slave) | **`P−1`** (all slaves) | `Θ(T'·log(P−1))` | master global scratch (re-read) | master merge of `T'` + `P−1` fan-in + slave disk |
| **MR-ParFORM** | `Q` nodes | **mappers 0** (stream); reducers transient patches `T/R` | **reducers** (before master) | **`R`** | `Θ(D·log R)` | master global scratch | master `Θ(D·log R)` + `R`-way Irecv mgmt |
| **MR + drain** | `Q` nodes | same as MR | reducers | `R` (`G` with relay-merger tier) | **`Θ(D)`** (memcpy; drain kills `log R`) | master global scratch | accumulated `Θ(m·D)` + **master-node `/gtmp`** + mapper gen |
| **MR + partition** | `Q` nodes | mappers 0; **mergers node-local `D/G`, compressed, persisted** | reducers | **0 mid-chain; `G` at gather** | **`Θ(1)` mid-chain; `Θ(D)` once at gather** | **node-local** (merger→mapper, intra-node shm) | **mapper generation** (master + master-disk off the equation) |

### 13.3 Per-module → per-chain: the master's total cost

The first four variants funnel every inter-module hand-off **through the master**;
the partition form funnels it **once**:

```
TFORM         master_total = Σ_m Θ(T·log W)        (in-process, single node)
ParFORM       master_total = Σ_m Θ(T'·log(P−1))    full fan-in, undeduped
MR            master_total = Σ_m Θ(D·log R)        dedup'd, fan-in R
MR + drain    master_total = Σ_m Θ(D)   = Θ(m·D̄)   memcpy, log R gone
MR + partition master_total = Θ(D_final)            ONE gather — independent of m
```

The drain makes each per-module master pass irreducible — `Θ(D)`, because every
output byte must pass through the master to the output file. **The partition form
breaks that frame:** mid-chain modules keep the expression hash-partitioned across
the `G` mergers, and the mergers feed the next module's mappers directly
(intra-node), so the master is bypassed on **both** input and output. The `Θ(D)`
master pass happens **only at the chain end** (the `MAPREDUCE_LAST` gather), not `m`
times. The same hash invariant that lets the reducers dedup (a K-prefix → one
reducer/merger, §2/§4) is what makes the partitioned files collectively complete and
re-distributable without a global re-merge between modules.

### 13.4 Calibration — what each step is measured to buy

- **ParFORM → MR** (the three documented wins): mappers stop writing their full
  local sort (`≈T` of write I/O removed); dedup moves to the reducers so the master
  receives `D` not `T'`; master fan-in shrinks `P−1 → R`. [CLAUDE.md goals;
  sort-skill "Why it's faster than plain ParFORM": `fan-in = numreducers < numtasks−1`.]
- **MR → MR + drain** (§7, drainfix6, `R=24`): master `final_sort` `9,256 → 1,251 s`
  (**7.4×**); whole run `14,690 → 6,641 s` (**2.1×**). The master reaches `Θ(D)`.
- **MR + drain → MR + partition** (dRGT, §12; profile of `4310854` + completion of
  `4331798`):
  - Master **off the per-module critical path** — measured: ~30 h of module 9 spent
    in `mergerdone` *idle-poll*, `≈0` in sort/distribute. Confirms the `Θ(1)`
    mid-chain master cost empirically.
  - Scratch **distributed across the `G` merger nodes** and compressed —
    `173 TB logical → 3.9 TB physical (44×)`, written node-local rather than to one
    master `.sc0`. This removes the **master-node `/gtmp` ENOSPC wall** a single-node
    global scratch would hit at this scale.
  - Net: the **first complete dRGT result** (52 h, `Exit_status=0`), where the same
    workload via a master-funnelled path (`4310854`, MR + drain) timed out at 72 h on
    10 of ~93 modules.

### 13.5 The honest caveat — what partition does *not* buy

For a **generation-bound** workload (Spin and dRGT both — §3, §12), the heavy
modules' wallclock is set by the slowest mapper's `Generator`, not by the master. So
the partition form's mid-chain wallclock saving on those modules is ≈ 0 — the master
was already idle in the merge tail's shadow (the §7 measured ≤ 3 % merger-tier result
is the same observation from the relay side). **The partition form's value is
structural, not a per-module speed-up:**

1. It removes the master as a **scaling** bottleneck — `master_total` stops growing
   with `m` and `R`, so the run stays mapper-bound as it scales out.
2. It **distributes the scratch**, removing the single-node disk-capacity wall.
3. It keeps the inter-module hand-off **node-local** (intra-node shm), off the NIC.

So the lineage's first-order wallclock lever remains **mapper `Generator`
parallelism** (§10). Partition is what lets you *add those mappers* — more nodes,
more modules, larger `R` — without the master or its disk becoming the new wall.
That is exactly what carried dRGT across the finish line at 8 nodes where 6 timed out
(§12): the +29 % mapper count was the wallclock driver, and the master-bypass was the
**enabler** that kept the master and `/gtmp` from capping the larger run.
