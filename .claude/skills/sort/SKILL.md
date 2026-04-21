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
