
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
| **Fan-in (real form)** | — | The genuine fan-in limit is **buffer granularity**: each recv buffer = `arena/(fan-in·numrbufs)` must stay ≥ `2·MaxTer` (hard) and ≥ the UCX rendezvous threshold `256 KB` (soft — below it, transfers go eager and throughput drops). With `arena≈3.6 GB`, that soft cap is in the **thousands** — not binding for any realistic `W`. |
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
| **`smallsize`** | **Decouple: mapper smaller, reducer keep/grow** (current single value 100 MB) | Mapper's local sort is the §6 DRAM-latency hazard — a smaller mapper-side `smallsize` (toward L3 ≈ tens of MB) makes it cache-resident; reducer's use is different (the patch arena) and has slack | **Hypothesis — needs per-role code change** (§11) |
| **`scratchsize`** | **400 MB** | Inherited; not the lever | Low priority |
| **`sortiosize`** | **4 MB** | Inherited; not the lever | Low priority |
| **`processbucketsize`** | **50–100** | Smaller bucket → finer mapper load balance; 2026-05-22 used 50 | Medium |
| **`largesize`** | **Decouple: mapper smaller (~512 MB–1 GB), reducer larger (~3–4 GB)** (current single value 2 GB) | Asymmetric work: **bigger on the mapper hurts twice** — (a) local sort spans a >L3 region ⇒ DRAM-latency compares on the critical path (§6), (b) RAM not spent on more mapper ranks. **Bigger on the reducer helps** — more sorted runs co-resident ⇒ more combining (incl. cross-mapper, which only the reducer sees) ⇒ fewer/larger patches ⇒ less EndSort work. The reducer's cache + RAM cost lands on its 42–100 % idle budget; ~17 % of ranks, so cluster RAM swing is net positive | **Hypothesis — needs per-role code change** (§11) |
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
5. **Per-role buffer decoupling** (§11) — the most attractive untested lever: shrink
   the mapper-side `largesize`/`smallsize` (cache-resident local sort + freed RAM for
   more mappers) while growing the reducer-side (more combining, paid from the
   reducer's idle budget). Needs a small `setfile.c` / `AllocSort` change so values
   can be chosen per role.

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
6. **Per-role buffer decoupling — joint Spin sweep + small code change.** Currently
   `form.set` ties `largesize` and `smallsize` to a single value for every rank, but
   mapper and reducer use them very differently (§10 coupling). The hypothesis: on the
   mapper, shrink both to ~L3-sized regions to make the local sort cache-resident
   (§6) and free RAM for more mappers; on the reducer, grow `largesize` (~3–4 GB) so
   lBuffer holds more sorted runs and combines more before extracting patches — paid
   for out of the reducer's 42–100 % idle budget. Test plan: (a) small per-role
   plumbing in `setfile.c` / `AllocSort` so the values can be set per role; (b) joint
   Spin sweep of mapper-`{largesize, smallsize}` × reducer-`largesize` while pushing
   workers/node to absorb the freed RAM; (c) per-role reducer lBuffer high-water-mark
   instrumentation to confirm the reducer-side growth is genuinely used.
