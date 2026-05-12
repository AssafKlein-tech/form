---
name: memory
description: Reference for FORM/TFORM/ParFORM memory model and allocations. Use when sizing form.set parameters, debugging OOM, deciding mpiprocs vs mem-per-rank tradeoffs, or discussing buffer-related parameters (largesize, smallsize, smallextension, scratchsize, sortiosize, filepatches, largepatches, MaxTermSize, WorkSpace, HideSize, PF_PACKSIZE). Also use when discussing PARALLELVARS, slavebuf, sbufs, AllocSort, or any allocation in setfile.c, parallel.c, mpi.c, structs.h.
---

FORM has three sources of large allocations: **AllocSort buffers** (sort.c via setfile.c), **scratch/hide files** (mmaped POBuffers), and **ParFORM communication buffers** (parallel.c, mpi.c). Memory consumption is dominated by per-rank replication under ParFORM — every MPI rank allocates its own full set, so node memory budget = `mpiprocs × per_rank`.

Key files: `sources/setfile.c` (form.set parsing + AllocSort), `sources/parallel.c` (PARALLELVARS, sbufs), `sources/mpi.c` (PF_packbuf), `sources/fsizes.h` (defaults), `sources/structs.h` (struct definitions).

## Per-process memory model

```
                 ┌──────────────────────────────────┐
                 │  AllocSort: lBuffer + sBuffer    │  ← largesize + smallextension
                 │  (combined alloc, setfile.c:974) │     (with hidden floor)
                 ├──────────────────────────────────┤
                 │  POBuffer (scratch sort file)    │  ← sortiosize
                 ├──────────────────────────────────┤
                 │  Compress buffer                 │  ← compresssize
                 ├──────────────────────────────────┤
                 │  WorkSpace (term assembly)       │  ← #: WorkSpace 20M
                 ├──────────────────────────────────┤
                 │  Hide file POBuffer              │  ← #: HideSize 10M
                 ├──────────────────────────────────┤
                 │  Scratch file POBuffer           │  ← AM.ScratSize (#: ScratchSize)
                 ├──────────────────────────────────┤
                 │  Compiler buffer (cbuf array)    │  ← grows with .frm complexity
                 ├──────────────────────────────────┤
                 │  ParFORM-only: per-dest sbufs[]  │  ← PF.numsbufs × bucket
                 │  ParFORM-only: slavebuf          │  ← AM.ScratSize × sizeof(WORD)
                 │  ParFORM-only: PF_packbuf        │  ← PF_PACKSIZE = 1600 (mpi.c:58)
                 └──────────────────────────────────┘
                                ↑
              every MPI rank duplicates the whole stack
```

## form.set knobs — what each one actually allocates

Values in form.set are **bytes**, not WORDs (setfile.c stores them as bytes; AllocSort divides by `sizeof(WORD)=4` to get WORD counts). Defaults in [fsizes.h:107-119](../../../sources/fsizes.h#L107).

| Parameter | Default | Allocates | Where (file:line) |
|---|---|---|---|
| `largesize` | 800 MB | `lBuffer` portion (large sort buffer) | [setfile.c:974](../../../sources/setfile.c#L974) |
| `smallsize` | 150 MB | `sBuffer` (small sort buffer, top half) | [setfile.c:944](../../../sources/setfile.c#L944) |
| `smallextension` | (auto = 1.5×smallsize) | `sBuffer` extension; co-allocated with `lBuffer` | [setfile.c:974](../../../sources/setfile.c#L974) |
| `scratchsize` | 500 MB | scratch-file POBuffer per rank (`AR.infile->PObuffer`); also `PF.slavebuf` on slaves | [execute.c:753](../../../sources/execute.c#L753) |
| `sortiosize` | 200 KB | `sort->file.PObuffer` (sort-file IO block) | [setfile.c:983](../../../sources/setfile.c#L983) |
| `compresssize` | (small) | scratch-file compression buffer | — |
| `termsinsmall` | (auto) | `sort->sPointer` array (8 bytes × `2×termsinsmall`) | [setfile.c:952](../../../sources/setfile.c#L952) |
| `largepatches` | 256 | `sort->Patches[]` & friends (small) | [fsizes.h:131](../../../sources/fsizes.h#L131) |
| `filepatches` | 256 | `sort->fPatches[]`; **also drives the sort buffer floor** ↓ | [fsizes.h:132](../../../sources/fsizes.h#L132) |

**`#: ScratchSize` in the .frm overrides `scratchsize` from form.set** (and overrides default), per [setfile.c:513](../../../sources/setfile.c#L513). Other directives:

- `#: MaxTermSize` (default ~20K) → `AM.MaxTer` in bytes — also enters the floor formula
- `#: WorkSpace` (default ~40M) → `AT.WorkSpace`, allocated per worker thread / per process; bounds how big a single term can grow during pattern matching
- `#: HideSize` (default 50M) → hide-file POBuffer
- `#: ProcessBucketSize` (default 1000 terms) → master→worker bucket size

## The hidden floor: why your largesize gets silently raised

[setfile.c:911-918](../../../sources/setfile.c#L911) enforces:

```
LargeSize + SmallEsize  ≥  filepatches × ((sortiosize/4 + COMPINC) × 4 + 2 × MaxTer)
```

with `COMPINC = 2` (fsizes.h:147), `MaxTer = MaxTermSize × 4` bytes. If your requested `largesize+smallext` is below this floor, **`largesize` is silently raised** (no warning unless DEBUGGING build).

### Worked example — why parform with a tform-tuned form.set OOMs at startup

tform-generated form.set typical: `sortiosize 96300812` (96 MB), `filepatches 256` (default), `MaxTermSize 300000` words (1.2 MB):

```
floor = 256 × ((96300812/4 + 2)×4 + 2×1200000)
      = 256 × (96300820 + 2400000)
      = 25,267,409,920 bytes  ≈ 25.27 GB
```

**That's the floor.** Even setting `largesize 1000000000` (1 GB) gets bumped to ~25 GB. With 60 ranks: 60 × 25 GB = **1.5 TB**, instant OOM. Error in .o:
```
Attempted to allocate 25267409920 bytes — allocating AllocSort: lBuffer+sBuffer
```

To honor a small `largesize`, **also lower `sortiosize` and `filepatches`** — they multiply into the floor. Reasonable parform values:

```
sortiosize       1000000      # 1 MB IO block
filepatches      32           # default 256 was overkill
largepatches     32
```

New floor: `32 × ((1000000/4+2)×4 + 2×1200000) = 32 × 3,400,008 = 109 MB` ✓ trivial.

## ParFORM-specific allocations

### `PF.slavebuf` ([execute.c:753](../../../sources/execute.c#L753))

```c
PF.slavebuf.PObuffer = Malloc1(AM.ScratSize * sizeof(WORD), "PF inbuf");
```

Allocated on each slave when `AC.RhsExprInModuleFlag` is set (any module that reads RHS expressions). `AM.ScratSize` is the *number of WORDs* (after division by `sizeof(WORD)`), so the malloc is `AM.ScratSize × 4` bytes — matches user-given `scratchsize` directly.

### `PF.sbufs[]` ([parallel.c:1969 region, allocateSbuf](../../../sources/parallel.c#L1969))

Each mapper holds a per-destination cyclic send buffer. Master-side: `min(LARGEBUFFER/numtasks, AM.ScratSize-1)` per slot. Worker-side: `(sTop2 - lBuffer - 1) / (numtasks-1) - (MaxTer/sizeof(WORD)+2)` per slot. **Knobs:**

- `PF_SBUFS` env (default 2, cap 10) — slots per mapper destination. Bumping to 3 hides one `Isend` latency, cost is one extra slot per dest.
- `PF_RBUFS` env (default 2, **cap 2 since 2026-05-05**) — slots per reducer source. Cap was 4; clamped down to 2 in [parallel.c:2425](../../../sources/parallel.c#L2425) because (a) `numrbufs > 2` was buggy: `PF_InitTree:424` only armed the active slot and `PF_PutIn:589`'s newterms wait on slot `next` reached unarmed slot 2 on the 2nd wrap → `MPI_Get_count` on uninit `type[2]` → `MPI_ERR_TYPE`. (b) Even if pre-posting were fixed, depth >2 buys nothing: chunks are ~17 MB rendezvous (above 1 MB eager limit), receiver memcpy ≥10× faster than network, so concurrent CTS handshakes share the same TCP link without speedup. Production v3 instrumentation showed 0 mapper stall on 1-node and ≤0.5% on 4-node. The depth-1-effective queue is correct for this workload. Multi-deep IRecv design deferred at [/home/assafklein/.claude/plans/eager-cuddling-wreath.md](../../../../.claude/plans/eager-cuddling-wreath.md); reopen only if PF_SBUFS rises >2 or interconnect changes (RDMA).

### `PF_packbuf` ([mpi.c:58](../../../sources/mpi.c#L58))

```c
#define PF_PACKSIZE 1600
```

**Tiny by design.** Used for `PF_Pack`/`MPI_Pack` paths: dollarvar broadcasts, exprflag broadcasts, compiler-buffer broadcasts (`PF_BroadcastCBuf`). The `PF_LongMulti*` API chunks larger payloads but per the comment at [mpi.c:1100-1103](../../../sources/mpi.c#L1100), the chained path is itself capped near 320 KB and **`MPI_ERR_TRUNCATE` at end-of-run is the symptom of overflow** — usually triggered by `toPolynomial` (sets `TOPOLYNOMIALFLAG` → calls `PF_BroadcastCBuf` from [execute.c:894](../../../sources/execute.c#L894)).

Bump only when chasing this specific failure; rebuild required.

## form.set search order — the silent-override gotcha

[tools.c:580-588](../../../sources/tools.c#L580) opens `form.set` from CWD **before** consulting `AM.SetupFile` (the `-S` flag) or `AM.SetupDir` (the `-s` flag). Search order:

1. `./form.set` in CWD ← wins if exists
2. `AM.SetupFile` (`-S file`)
3. `AM.SetupDir/form.set` (`-s dir`)
4. `setupfilename` (compiled-in)
5. `$FORMSETUP`

**Practical implication for parform:** if you `cd` into a directory containing a tform-tuned `form.set`, it's used regardless of `-S`. Either rename the tform file aside (`form.set.tform`) or write your parform values directly into the CWD `form.set`.

The user's run dir at [runs/Adquanta/](../../../runs/Adquanta/) keeps the parform values in `form.set` and `form.set.tform` as a sibling for the tform path.

## Per-node sizing recipe (ParFORM)

Goal: pick `mpiprocs` and form.set values so `mpiprocs × per_rank_peak ≤ node_memory − headroom`.

1. **Measure per-rank peak** in the heavy phase. Use the memory logger pattern from [form_spin_test.pbs](../../../runs/Adquanta/form_spin_test.pbs) — `free -g` + `ps -eo pid,rss,comm | awk '/parform/'` every 30s. Peak rss × ranks tells you what you actually need.
2. **Compute the AllocSort floor** for your candidate form.set (formula above). The floor MUST fit your per-rank budget; if not, lower `sortiosize` and `filepatches`.
3. **Subtract scratch & hide**: scratchsize and hidesize are extra (separate POBuffers) on top of largesize+smallext. Budget ~1.5× the AllocSort sum to cover them.
4. **Master needs more headroom than workers**: master holds the gathered output (~bytes-of-final-expression). On asymmetric cluster layouts (`select=1:..mem=Xgb+N:..mem=Ygb`), put the master node first with bigger `mem`.
5. **Add `ulimit -v <bytes>` in the .pbs** so a single runaway rank dies cleanly with ENOMEM rather than dragging the node into thrash.

### Reference: what the Spin run actually used

After form.set v3 (largesize=1G, smallext=400M, sortiosize=1M, filepatches=32):
- 60 ranks on 1 TB node, peak node memory **5 GB used**, master rss ~1.2 GB
- Compare v0 (tform form.set, sortiosize 96M, filepatches default 256): floor 25 GB/rank → 1.5 TB → instant OOM
- Compare v1/v2 (largesize lowered but sortiosize/filepatches unchanged): floor still 25 GB/rank → same OOM, no improvement

**Pre-MR-active "sweet spot" claim was based on a serial bench.** The original `bench_compress_mr.frm` has `off parallel;` (line 121) before the heavy `.sort:Diagram Loaded` — heavy sort runs serial on the master. Buffer tunings on that bench (which made v3 the "sweet spot") were tuning a master-side serial sort, not MR. **MR-active sweeps tell a different story:**

- **Asymmetric `lp >> fp` wins big.** On MR-active heavy bench (`bench_heavy.frm`), `fp=128 / lp=1024 / largesize=1G / smallext=400M` beats v3 by ~24%. Going further to `lp=512` or `lp=256` is worse; `lp=1024` is the sweet spot at fp=128.
- **`smallextension=1.5G` beats `smallext=400M` by another ~17%** on the same bench (lp128_2G base). Combined with lp=1024 → expected ~35% gain over v3 (Phase B in flight will validate).
- **`smallsize=100M` is the sweet spot.** 50–400M all within 3%; 10M is +80% slower (tier-1 thrashes), 800M is +17% slower.
- **`largesize=1G` ≈ `2G` for lp=1024** (within 5% on Phase A1.5 fill-in sweep). For lp=256, `2G > 3G` because at 3G we hit memory pressure on some node groups (cross-job variance).
- **Compression is wash at lp128_2G but critical at lp128_3G** (+41% slower without it on the regressed-memory config).

**Lesson:** more memory IS faster *when buffers actually get used*. The earlier "more memory ≠ faster" conclusion came from a serial bench where the working set fit in 1G regardless of what you allocated. On the MR path with multi-sort workloads (Spin's `expandmomenta`-class modules), per-rank intermediate state grows large and bigger smallext buys real merging headroom.

**Bench correctness traps:**
1. `bench_compress_mr.frm` has `off parallel;` before the heavy sort — heavy sort runs **serial** on master, not MR. Tuning on this bench measures master-side single-thread sort behavior, not MR. Use `bench_compress_mr_keepmr.frm` (parallel kept on) or `bench_heavy.frm` (= keepmr + `#call dummyindices`).
2. The first heavy sort in the keepmr/heavy bench has a **serial term-generation pre-cursor** (~13% of wallclock) — only one rank actually generates the 4M output terms because there's only one input expression. Subsequent sorts redistribute terms across all ranks and are truly parallel. Full Spin's heavy modules don't have this artifact (terms are pre-distributed from earlier modules).

## Diagnostic recipes

### "Attempted to allocate N bytes — allocating scratchsize"

Per-rank `AM.ScratSize × sizeof(WORD)` (or `#: ScratchSize`) × mpiprocs > node mem. Either lower `scratchsize` in form.set or remove `#: ScratchSize` from the .frm. Common cause: `#: ScratchSize 100M` in a .frm that runs under both tform and parform.

### "Attempted to allocate N bytes — allocating AllocSort: lBuffer+sBuffer"

The hidden floor. Decode `N`:
```
N = filepatches × ((sortiosize/4 + 2)×4 + 2×MaxTermSize×4)
```
Lower `sortiosize` and `filepatches` first; only then `largesize`/`smallext` (which can be below floor without effect).

### "MPI_ERR_TRUNCATE in MPI_Pack"

**FIXED in commit `adeb92f`** — `PF_LongMultiBroadcast` now uses a single growable buffer with an INT size header, so any-size broadcasts work. Previously: PF_PACKSIZE=1600 chained-chunk path silently truncated near 320 KB total payload.

Historical context (kept for understanding pre-`adeb92f` behavior): the bug was usually `toPolynomial onlyfunctions`-triggered, since it sets `TOPOLYNOMIALFLAG` → calls `PF_BroadcastCBuf` from [execute.c:894](../../../sources/execute.c#L894). `off parallel;` does NOT prevent the broadcast — it runs unconditionally inside `#ifdef WITHMPI` at [execute.c:885-902](../../../sources/execute.c#L885).

### "ERROR: Calling Map Reduce without parallel"

[parallel.c:1705](../../../sources/parallel.c#L1705) fires when `AC.mparallelflag != PARALLELFLAG && sMRflag != NO_MAPREDUCE`. **`toPolynomial onlyfunctions` internally forces non-parallel for its module** (its poly arithmetic isn't parallel-safe), so any module that calls toPolynomial in an MR-mode .frm trips this guard. Fix: add `off mapreduce;` next to `off parallel;` before the procedure call. This is what `runs/Adquanta/Spin2_h5_45_mr.frm` line 152 does.

### Run starts, gets through several modules, then SIGKILL on rank 0

PBS OOM-kill. Check `qstat -fx <jobid> | grep resources_used` — if `mem` >> requested, you exceeded `mem=` and PBS killed you. Per-rank growth between sorts is normal on heavy `id` substitutions; the node-wide free(1) trace from the memory logger shows whether memory grows monotonically (legitimate workload) or jumps (one rank balloons).

### Run hangs with no output

Different from above. Check the .e for MPI errors. If clean and it's just stuck, attach gdb to rank 0 and a worker — usually it's a `repeat;` block doing O(N²) work, not a deadlock. Add per-procedure timing markers (`#message <name>`) before the candidate procedures to localize.

## Defaults summary (64-bit, no pthreads — i.e. parform)

From [fsizes.h:107-119](../../../sources/fsizes.h#L107):
```
SMALLBUFFER   150 MB    (small sort buffer)
SMALLOVERFLOW 300 MB    (small extension)
LARGEBUFFER   800 MB    (large sort buffer)
SCRATCHSIZE   500 MB    (scratch POBuffer)
SORTIOSIZE    200 KB    (sort-file IO block)
MAXPATCHES    256       (large-patch count)
MAXFPATCHES   256       (file-patch count) ← ALSO drives the floor
COMPINC       2
DEFAULTPROCESSBUCKETSIZE 1000 terms
```

For pthreads (TFORM): `SMALLBUFFER` 300M, `LARGEBUFFER` 1.5G — bigger because threads share. **Multiply nothing by `numthreads`** under TFORM. **Multiply everything by `numranks`** under ParFORM.

## Code anchors

- [setfile.c:865-1008](../../../sources/setfile.c#L865) — `AllocSort` (the actual mallocs)
- [setfile.c:911-918](../../../sources/setfile.c#L911) — the hidden floor
- [setfile.c:944-946](../../../sources/setfile.c#L944) — bytes→WORDs conversion (`/sizeof(WORD)`)
- [setfile.c:559-578](../../../sources/setfile.c#L559) — top-level form.set → AllocSort wiring
- [tools.c:576-618](../../../sources/tools.c#L576) — `LocateFile`, the form.set search order
- [execute.c:753-757](../../../sources/execute.c#L753) — `PF.slavebuf` malloc
- [execute.c:885-902](../../../sources/execute.c#L885) — end-of-module PF broadcasts
- [parallel.c:2389-2425](../../../sources/parallel.c#L2389) — PF env-var parsing (`PF_SBUFS`, `PF_RBUFS`)
- [parallel.c:2267-2326](../../../sources/parallel.c#L2267) — `PF_allocateSbuf` (master vs worker sizing)
- [mpi.c:58](../../../sources/mpi.c#L58) — `PF_PACKSIZE` (the 1600-byte cap)
- [mpi.c:1080-1103](../../../sources/mpi.c#L1080) — flaw description for `PF_LongMultiBroadcast`
- [fsizes.h:107-165](../../../sources/fsizes.h#L107) — all default sizes
- [structs.h:1427](../../../sources/structs.h#L1427) — `M_const`/`AM` memory fields
