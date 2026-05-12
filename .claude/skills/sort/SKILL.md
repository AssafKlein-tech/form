---
name: sort
description: Reference for FORM sort system — all four variants (regular, TFORM, ParFORM, MR ParFORM). Use when working with sort pipelines, sort buffer layout, PutOut routing, reducer/mapper roles, or any code in sort.c, parallel.c, mpi.c that touches sorting.
---

FORM sorts algebraic expressions into canonical order, combining equal terms. The sort system has four variants sharing the same core pipeline. Key files: `sources/sort.c`, `sources/parallel.c`, `sources/mpi.c`.

## Core Data Structure: `SORTING` (sources/structs.h)

`AT.SS` is the active sort; `AT.S0` = `AM.S0` is the ground-level (expression-level) instance.

```
sBuffer … sTop         small buffer — raw unsorted term storage
sPointer[]             pointer array: one entry per term in the small buffer
sTerms, PoinFill       count and current fill pointer for sPointer
lBuffer … lTop, lFill  large buffer — holds sorted patches
Patches[], lPatch      patch start pointers inside the large buffer
file                   sort file handle (.sor on disk)
fPatches[], fPatchN    file patch bookmarks
TermsLeft, GenTerms    statistics
```

`AR.sLevel` tracks nesting depth: 0 = expression level, higher = function args / `$`-vars. Each level has its own `SORTING*` in `AN.FunSorts[]`.

## Three-Tier Spill Pipeline (all variants share this)

### Small buffer → patch in large buffer
`StoreTerm()` ([sort.c:4829](sources/sort.c#L4829)) appends the term to `sBuffer` and records a pointer. When full (`sTerms >= TermsInSmall` or `sFill + *term >= sTop`):
1. `SplitMerge(BHEAD sPointer, sTerms)` — in-place Timsort of the pointer array
2. `ComPress(ss, &RetCode)` — walks sorted pointers, sums coefficients of equal terms
3. Compressed sorted block → appended as a new patch to the large buffer (`Patches[lPatch++]`)

### Large buffer → sort file
When `lPatch >= MaxPatches` or large buffer would overflow:
- `MergePatches(1)` — k-way merge of all large-buffer patches using **tree of losers** (Knuth vol. 3) → written to `S->file`
- Large buffer reset

### EndSort final merge ([sort.c:888](sources/sort.c#L888))
1. `SplitMerge` + `ComPress` on any leftover small-buffer terms
2. If no large/file patches: write directly to `AR.outfile` (fast path)
3. Otherwise: `MergePatches(0)` — merges all large-buffer patches + all file patches → calls `PutOut()` per term → `AR.outfile`

**Comparison:** `Compare1()` uses `GETSTOP` to find the coefficient boundary, then compares `[term+1, stopper)` lexicographically. Equal symbolic parts → `AddCoef()` sums coefficients.

---

## Variant 1: Regular FORM

**Driver:** `Processor()` in `proces.c`:
```c
NewSort(BHEAD0);
while (GetTerm(...)) {
    Generator(BHEAD term, 0);  // produces result terms → StoreTerm()
}
EndSort(BHEAD AM.S0->sBuffer, 0);
```

No MPI or threading. Single small/large/file pipeline. Output lands in `AR.outfile`.

---

## Variant 2: TFORM (threaded)

Each worker thread has its own `SORTING` struct (allocated by `InitializeOneThread()` → `AllocSort()`; buffer sizes ≈ 1/N of single-thread sizes).

Workers run the identical three-tier pipeline independently. Final output: instead of `PutOut()` → `AR.outfile`, workers call `PutToMaster(BHEAD t)` when `AS.MasterSort && fout == AR.outfile`. The master thread collects per-term and writes the inter-thread merged stream.

---

## Variant 3: ParFORM (MPI, non-MR)

Active when `AC.partodoflag > 0 && AC.mparallelflag == PARALLELFLAG` and `AC.sMRflag == NO_MAPREDUCE`.

**Roles:** rank 0 = master; ranks 1..numtasks-1 = slaves.

**Master** distributes term buckets to all slaves via `PF.sbufs[0]` + `PF_ISendSbuf(slave, PF_TERM_MSGTAG)`, then calls `EndSort()` → `PF_EndSort()` → `PF_WaitAllSlaves()` to collect sorted slave output and do a final merge into `AR.outfile`.

**Slave** receives terms via `PF_GetTerm()`, runs `Generator()` → `StoreTerm()` → local three-tier pipeline. In `PutOut()` when `PF.me != MASTER && AR.sLevel <= 0 && fi == AR.outfile && PF.parallel`:
- `fi->POfill` is aliased into `PF.sbufs[MASTER]->buff` — term is written directly into the send buffer
- When send buffer fills: `PF_WISendSbuf(PF_BUFFER_MSGTAG, MASTER)` to flush

Data flow: `infile → master → buckets → slave[k] → Generator → sort → send to master → master final merge → AR.outfile`

---

## Variant 4: MR ParFORM (MapReduce)

Active when `AC.sMRflag != NO_MAPREDUCE` (user puts `on mapreduce;` in the FORM script for the module). Enabled at runtime with `-r<N>` (N = % of workers as reducers).

### Role assignment ([parallel.c:1657](sources/parallel.c#L1657))
```c
PF.numreducers = (PF.numtasks - 1) * AM.ReducerPer / 100;
if (PF.numreducers < 2) PF.numreducers = 2;
PF.nummappers = PF.numtasks - PF.numreducers;
// rank 0              → master  (ROLE_MAPPER by rank check)
// ranks [1..m-1]      → mapper workers
// ranks [m..N-1]      → reducer workers
role = (PF.me < PF.nummappers) ? ROLE_MAPPER : ROLE_REDUCER;
```

`PF_LowMRsort()` ([sort.c:101](sources/sort.c#L101)) — inline predicate true for mapper workers at ground level:
```c
return (PF.me < PF.nummappers && PF.me != MASTER
     && AR.sLevel <= 0 && PF.parallel
     && PF.exprtodo < 0 && AC.sMRflag != NO_MAPREDUCE);
```

### Master
Distributes term buckets only to mapper workers (ranks 1..nummappers-1). After all terms sent, `EndSort()` → `PF_EndSort()` waits for `PF_ENDSORT_MSGTAG` from ALL workers, then does a final k-way merge of the reducer output streams into `AR.outfile`.

### Mapper workers — routing inside `PutOut()` ([sort.c:1738](sources/sort.c#L1738))

Mappers run the full three-tier local sort first (`Generator` → `StoreTerm` → small/large/file pipeline). During `EndSort()`'s final merge, `PutOut()` is called for each merged term and `lowmr_sort = PF_LowMRsort()` is true:

**Step 1 — Hash symbolic part and pick reducer (line 1758):**
```c
WORD *start = term + 1;
WORD *end   = (term + *term) - ABS((term + *term)[-1]);  // same as GETSTOP
term_hash = hash_list_avx512(start, end - start);        // or AVX2 / scalar
dst = term_hash % PF.numreducers + PF.nummappers;
r = rr = AR.CompressPointers[dst];  // per-reducer delta-compression context
```
Coefficient is excluded from the hash — terms that differ only by coefficient get the same `dst` and will be summed at the reducer.

**Step 2 — Delta-compress using per-reducer context (line 1786):**
`AR.CompressBuffers[dst]` holds the previous term sent to reducer `dst`. Compression delta is relative to that reducer's last term, not the overall last term.

**Step 3 — Redirect output into per-reducer send buffer (line 1920):**
```c
PF_BUFFER *sbuf = PF.sbufs[dst];
fi->POfill = sbuf->fill[sbuf->active];   // alias file handle into send buffer
fi->POstop = sbuf->stop[sbuf->active];
if (fi->POfill + i >= fi->POstop) {
    PF_WISendSbuf(PF_BUFFER_MSGTAG, dst); // flush full buffer to reducer
    // reset fill to buffer start
}
```

**Step 4 — Write term word-by-word (line 1934):**
Inner loop writes into `fi->POfill` (now inside `sbuf->buff[active]`). Mid-loop overflow → same flush.

**Step 5 — Persist fill pointer (line 2028):**
```c
sbuf->fill[sbuf->active] = sbuf->full[sbuf->active] = p;
```

**FlushOut() at EndSort termination (line 2056):**
Loops over all reducers, sends remaining partial buffer (`PF_ENDBUFFER_MSGTAG`), then `PF_ENDSHUFFLE_MSGTAG` per reducer. Also sends `PF_BUFFER_MSGTAG` to master to signal mapper done.

### Reducer workers

**Init — `PF_ReducerInit()` ([parallel.c:2184](sources/parallel.c#L2184)):**
Allocates `PF.rbufs[src]` for each mapper (double-buffered). Posts initial `MPI_Irecv` for every mapper × buffer slot. Builds `PF.dispatch.reqs[]` flat view for `MPI_Waitany`.

**Receive loop — `PF_StoreBuffer()` ([parallel.c:630](sources/parallel.c#L630)):**
```
PF_WaitAnyRbuf()          // MPI_Waitany over all outstanding receives
  PF_SHUFFLE_MSGTAG      → copy received buffer directly into large buffer as a patch
                            post next MPI_Irecv for double-buffer slot
                            if large buffer full → MergePatches(1) → sort file
  PF_ENDSHUFFLE_MSGTAG   → one mapper done
  PF_ENDSHUFFLEALL_MSGTAG→ all mappers done, return
```
Note: received buffers go **directly into the large buffer**, bypassing the small buffer entirely.

**Sort and forward — `PF_ForwardTermsToMaster()` ([parallel.c:2235](sources/parallel.c#L2235)):**
```c
PF_StoreBuffer();                         // receive all mapper data
EndSort(BHEAD AM.S0->sBuffer, 0);        // merge patches → PutOut → send to master
```
In `PutOut()` for a reducer, `PF_LowMRsort()` is **false** (reducer rank >= nummappers), so the non-MR slave path applies: output goes to `PF.sbufs[MASTER]` → forwarded to master.

### When the reducer fires terms to the master — and why you can't make it earlier

A recurring "optimization" idea is: the master sits idle in `MAS_FINAL_SORT` (= `PF_EndSort` on rank 0) waiting on the slowest reducer; can we make reducers start feeding it sooner — e.g. shrink `largesize` early so the reducer flushes faster, or start the `sbufs[MASTER]` flush before the buffer is full and ramp it up (mirroring the master's slow-startup bucket ramp at [parallel.c:1786](sources/parallel.c#L1786))? **No — and the reasons are structural, not missing optimizations.**

1. **The reducer sends term data to the master in exactly ONE place: `EndSort`, after `PF_StoreBuffer` returns.** `PF_ForwardTermsToMaster` is literally `PF_StoreBuffer(); EndSort(...)`. `PF_StoreBuffer` only returns on `PF_ENDSHUFFLEALL_MSGTAG` (all mappers done). During the receive loop, the *only* thing a full large buffer triggers is `MergePatches(1)` → the reducer's **own local `.sor` file** — never the master. So shrinking `largesize` does not "feed the master sooner"; it just spills more, smaller patches to local disk, which makes `EndSort`'s later `MergePatches(0)` a *wider* k-way merge (more tree-of-losers inputs ≈ more comparisons/term) and adds patch-file read I/O. Strictly the wrong direction.

2. **The master-distribute slow-startup ramp works because distribution is order-free; the reducer→master path is order-bound.** A mapper bucket can be any arbitrary subset of terms, so a tiny first bucket is a valid hand-off — the ramp at [parallel.c:1786](sources/parallel.c#L1786) (`maxinterms` starts at `ProcessBucketSize/100`, doubles every `nummappers-2` buckets) just gets every mapper working ASAP. The reducer's output to the master *must be in canonical sorted order* — it can only hand over a sorted **prefix** ("all terms ≤ K"), and it cannot know it holds the complete prefix below any K until it has seen *all* its input, because a smaller term could still arrive from any mapper. That is why `EndSort` runs *after* the receive loop, not interleaved with it. The ramp cannot transfer because the property it exploits (order doesn't matter) is exactly what the reducer→master path lacks.

3. **Soft-flushing `sbufs[MASTER]` early would be cheap and safe — but buys ~sub-second per reducer, against a multi-thousand-second phase.** Mechanically easy (the flush logic exists; add a small-threshold trigger for the first few flushes, then disable). But: (a) the master cannot emit *any* output until it has the head term of *every* reducer stream, so the binding delay is the **slowest** reducer's time-to-first-chunk, not the average — early-flushing the fast reducers doesn't help; (b) the slow reducer's "warm-up" is dominated by `MergePatches(0)` opening all its file patches and reading the first block of each (scales with patch count — ~hundreds of ms to seconds at ~200 patches), which an output-side flush can't touch; (c) steady-state throughput is `min` over reducers of each one's merge output rate, again unaffected. Flushing *always* small (not just first-few) also costs more MPI messages → more rendezvous CTS handshakes / eager-descriptor pool pressure (the `mm_recv_desc`/`rc_recv_desc` OOM that `UCX_RNDV_THRESH=256k` exists to dodge) + more `MPI_Waitany` work on the master, which *is* the bottleneck rank.

**The real levers for "feed the master faster"** are therefore (a) **fewer patches per reducer** → faster `MergePatches(0)` setup *and* faster steady-state merge *and* reducer starts forwarding sooner: bigger `largesize`, and `filepatches` ≥ patch count so the `EndSort` merge is single-pass (default 128 can be exceeded — e.g. ~200 patches/reducer at low reducer counts on the Spin workload → a 2-pass merge); and (b) **hierarchical reducer→master merge** — group-leaders run their group's `EndSort` in parallel, master merges G≈4 fully-sorted streams, and a slow reducer is absorbed inside its group's parallel merge instead of directly stalling the master. See `project_buffer_size_finding.md` and `project_spin_4n128_optimum.md`.

> Note `MAS_FINAL_SORT` (and the `RED_*` timers) are **wall-clock phase timers**, not CPU-busy timers — they include the time the master/reducer is blocked in `MPI_Recv` waiting for the next chunk. So a 15-way master merge reading `MAS_FINAL_SORT` *larger* than a 12-way one is not "a wider merge is slower" (it isn't — log₂15 > log₂12 in compare count, so 12-way is cheaper *compute*); it's that with 12 reducers each carries more data → more patches → slower `EndSort` → streams arrive late and lumpy → the master spends more wall time blocked on the laggard. The reducer-count optimum is where (reducer-`EndSort`-speed) × (stream evenness) × (master fan-in cost) is jointly minimized — a system balance point, not a merge-algorithm property — which is also why a bigger `largesize` (fewer patches per reducer at low reducer count) can shift that optimum.

### MPI message tags (parallel.h)
| Tag | Value | Meaning |
|-----|-------|---------|
| `PF_SHUFFLE_MSGTAG` | 110 | mapper → reducer: partial term buffer |
| `PF_ENDSHUFFLE_MSGTAG` | 111 | mapper → reducer: this mapper is done |
| `PF_ENDSHUFFLEALL_MSGTAG` | 112 | sentinel: all mappers done |

### Why it's faster than plain ParFORM
- Mappers sort locally first — the data sent to reducers is already sorted, so reducers only need k-way merge
- Hash invariant: terms with the same symbolic structure always reach the same reducer → reducer sums their coefficients immediately, **zero duplicates reach the master**
- Master fan-in = numreducers < (numtasks − 1)
- No duplicate terms written to sort files across mappers for the same symbolic expression

### Mappers do NOT write term data to disk in MR mode

This is the single most important non-obvious thing about the MR path. `PF_LowMRsort()` is checked inside both `PutOut()` AND `FlushOut()`, and **both have early-returns that redirect the disk-write path to `PF_WISendSbuf()`**. There is no `WriteFile(S->file, ...)` of term data on a mapper in MR mode.

| Site | What happens when `lowmr_sort=true` |
|---|---|
| `PutOut` per-term, [sort.c:1921-1933](sources/sort.c#L1921) | term destined for `dst`'s reducer; alias `fi->POfill` into `sbuf->fill[active]`; if would overflow, `PF_WISendSbuf(PF_BUFFER_MSGTAG, dst)` |
| `PutOut` mid-write spill, [sort.c:1939-1945](sources/sort.c#L1939) | when `p >= fi->POstop` mid-loop, `PF_WISendSbuf` to `dst` instead of falling through to `CreateFile`/`WriteFile` |
| `FlushOut`, [sort.c:2068-2105](sources/sort.c#L2068) | early-return at line 2104 — loops over reducers, `PF_WISendSbuf` per destination, then `return(0)` *before* the `WriteFile` at 2147-2148 |
| `MergePatches(par=1)` from EndSort | calls `FlushOut` for actual writes, so all data goes to MPI per above. The function does call `CreateFile(fout->name)` at [sort.c:4051](sources/sort.c#L4051) unconditionally — that creates an **empty** file on `/gtmp` which is `close()`d and `remove()`d at [sort.c:1339-1346](sources/sort.c#L1339) at end of EndSort. Cost: microseconds; no data ever written into it. |

So the mapper-side flow is: `Generator` → `StoreTerm` → `sBuffer` → `SplitMerge`+`ComPress` → `lBuffer` patches → `MergePatches(1)`/`MergePatches(2)`/direct, all leading to `PutOut`+`FlushOut` in EndSort which **stream straight to reducers via MPI**. The "less disk I/O" tagline is fully realized in the term-data path; only the residual empty-`creat()` syscall remains.

When reading the EndSort flow ([sort.c:888-1340](sources/sort.c#L888)), don't be misled by `MergePatches(par=1)` — `par=1` means "write to S->file" in *regular* FORM, but in MR mode the leaf `FlushOut` redirects, so the same `par=1` API produces MPI traffic instead.

---

## Cyclic send/receive buffers and tuning

### Send buffers (mapper side) — `PF.sbufs[k]`

One per destination (reducer rank or master rank). Each holds `PF.numsbufs` slots cycled via `sbuf->active`. While slot `active` is being filled (write path in `PutOut`), prior slots may still be in flight as `MPI_Isend` requests; `PF_WISendSbuf` waits on `request[active]` before reusing it.

- Default `numsbufs = 2`, env `PF_SBUFS`, clamped to [1, 10] at [parallel.c:2419](sources/parallel.c#L2419).

### Receive buffers (reducer side) — `PF.rbufs[src]`

One per source mapper. Each holds `PF.numrbufs` slots cycled via `rbuf->active`. Used by `PF_StoreBuffer` ([parallel.c:630](sources/parallel.c#L630)) — incoming `PF_SHUFFLE_MSGTAG` lands into the active slot, the slot is drained directly into the reducer's large buffer as a new patch, and the *next* slot's IRecv is posted then.

- Default `numrbufs = 2`, env `PF_RBUFS`, **clamped to [1, 2]** at [parallel.c:2425](sources/parallel.c#L2425) (since 2026-05-05; was 4).
- **Init only pre-posts ONE IRecv per source** (the `active` slot) at [parallel.c:2210-2218](sources/parallel.c#L2210). Other slots' `MPI_Request` stays `MPI_REQUEST_NULL` until the active message is consumed and the next IRecv is posted. So even with `numrbufs = 2`, a single (mapper, reducer) pair has at most one in-flight receive at a time — additional slots only help by overlapping reducer-side processing with the next IRecv post, not by buffering multiple in-flight messages from the same source.
- **Why the cap is 2**: `numrbufs ≥ 3` had a latent bug — `PF_InitTree:424` only IRecv'd the active slot, while `PF_PutIn:589`'s `newterms` branch waited on slot `next` without arming it; the cycle 0→1→2→0 hit unarmed slot 2 on the 2nd wrap and `MPI_Get_count` read uninitialized `type[2]` → `MPI_ERR_TYPE`. Even fixed, depth >2 gives no measurable speedup at the chunk sizes typical for this workload (rendezvous protocol on multi-MB chunks, bandwidth-limited TCP; receiver memcpy is much faster than network so concurrent CTS handshakes share the same link without speedup). Production runs show near-zero `Wait time for Reducers` per mapper. Reopen only if `PF_SBUFS` rises >2 or interconnect changes (RDMA). Deferred design at [/home/assafklein/.claude/plans/eager-cuddling-wreath.md](../../../../.claude/plans/eager-cuddling-wreath.md).

### Per-term invariant in PF_StoreBuffer

While walking the received buffer, each term-start pointer `sss` is validated at [parallel.c:707](sources/parallel.c#L707):
```c
if ( sss > rbuf->full[a] || sss <= rbuf->fill[a] ) { /* error */ }
```
Where `rbuf->fill[a]` = patch start written into the large buffer, `rbuf->full[a]` = patch end. Useful invariant when debugging garbled wire data.

### `msg_size` invariant on the send side

`PF_ISendSbuf` ([mpi.c:293](sources/mpi.c#L293)) computes `LONG msg_size = s->fill[a] - s->buff[a]` and aborts if negative. Seeing `PF_ISendSbuf: invalid msg_size -1` means `fill` was set below `buff` — typically a bug in the writer that drives `PutOut`'s spill path or in `FlushOut`'s reset of `fill` to `buff` before a partial flush.

### `FlushOut(patch=1)` behavior

[sort.c:2058](sources/sort.c#L2058), called from `MergePatches(1)` on a mapper. Loops over **every** reducer and emits a `PF_BUFFER_MSGTAG`. The empty-buffer optimization landed in commit `dcef34c`: the gating condition is now `sbuf->fill[active] >= sbuf->stop[active] || (patch && nonempty)` — empty per-reducer slots are skipped on `patch=1` to avoid pure-overhead Isends under hash skew. The `!patch` branch (called from EndSort termination) still writes a 0 terminator and emits `PF_ENDBUFFER_MSGTAG` for end-of-stream signalling unconditionally — that's required.

---

## Debug build & smoke test

```bash
# Debug build of parform (parvorm rule is broken — see CLAUDE.md):
rm -f sources/parform-*.o sources/parform
make -C sources parform CFLAGS="-g -O0"

# 9-process smoke test:
cd tests/simple_tests && bash runall.sh
```

Smoke tests in [tests/simple_tests/](tests/simple_tests/) expect identical output between MR and non-MR runs — diff against a reference run with `off mapreduce;` to verify correctness after any sort.c / parallel.c / mpi.c change.
