---
name: bracket
description: Reference for FORM brackets, keep brackets, and bracket-index (B+). Use when working with bracket statements, the HAAKJE marker, the deferred-bracket optimization, the bracket-index file, or any optimization that uses brackets to reduce computation or output size — including the MR mapper/reducer/master flow and the drain interaction with B+.
---

A **bracket** in FORM is a per-term boundary marker (the `HAAKJE` subterm) that splits a term into an "outside" and "inside" part. Brackets affect sort order, output grouping, and — combined with `keep brackets;` — enable an optimization that runs rules once per shared outside instead of once per term. They are a memory-level concept (the marker lives inside each term's bytes) — bracket-grouped output `outside*(inside_1 + inside_2 + ...)` is computed at **print time** by scanning consecutive same-bracket-part terms.

## Statements and what they set

| Statement | Sets | Effect |
|---|---|---|
| `bracket x,y,...;` / `b x;` | `AR.BracketOn = 1`, `AT.BrackBuf` = normalized selector | Generator calls `PutBracket` per term → inserts HAAKJE between outside (matching) and inside (rest) |
| `antibracket x;` | `AR.BracketOn = -1`, `AT.BrackBuf` | Same as `b`, but outside / inside swapped |
| `bracket+ x;` / `B+ x;` | also `AC.bracketindexflag = 1`, allocates `newbracketinfo` | Same as `b` plus builds a per-expression `.brak` index file for fast random-access to brackets |
| `keep brackets;` | `AC.ComDefer = 1` → `AR.DeferFlag = 1` at module entry | Generator defers to `Deferred()`: runs rules once per unique outside, splices each inside |

Bracket state is per-module and is saved/restored at module boundaries ([proces.c:460-461](../../../sources/proces.c#L460-L461), [parallel.c:2406-2413](../../../sources/parallel.c#L2406-L2413)). It does not leak across modules.

## HAAKJE marker — on-disk layout

`HAAKJE` is defined in [ftypes.h:361](../../../sources/ftypes.h#L361) as the constant `18` (`HAAKJE0 = 9` is reserved for compression internals).

When `AR.BracketOn != 0`, a term is laid out as:

```
[length] [ outside subterms ] [ HAAKJE, 3, level ] [ inside subterms ] [ coefficient ]
```

- `HAAKJE` (= 18) — subterm type code
- `3` — subterm length in WORDs (HAAKJE is always a 3-word subterm)
- `level` — nesting level (currently 0 in practice; reserved for multi-level brackets)
- Coefficient stays at the term tail, exactly as in a normal term

For antibrackets the layout is identical; `PutBracket` just swaps which side gets which subterms before reassembly ([execute.c:1140](../../../sources/execute.c#L1140)).

## Where the marker is inserted, consumed, and erased

**Inserted** in Generator, just before StoreTerm, when `AR.BracketOn` is set ([proces.c:3418-3439](../../../sources/proces.c#L3418-L3439)):

```c
if ( AR.sLevel <= 0 && AR.BracketOn ) {
    ...
    PutBracket(BHEAD term);   /* splits term, inserts HAAKJE marker */
    StoreTerm(BHEAD termout); /* term now carries HAAKJE in its bytes */
    return(ret);
}
```

`PutBracket` ([execute.c:1126](../../../sources/execute.c#L1126)) scans the input term against `AT.BrackBuf`, sorts matching subterms into the outside region (`t1`) and the rest into inside (`t2`), then assembles `[length][t1][HAAKJE,3,level][t2][coeff]`. Non-commuting functions to the left of bracketed functions follow the function. Vectors in dotproducts/tensors match flexibly.

**Consumed (or rather ignored)** in Normalize ([normal.c:509](../../../sources/normal.c#L509)):

```c
case HAAKJE :
    break;     /* skip — do not emit into rebuilt term */
```

When Normalize rewrites a term, it walks the subterms and does not copy HAAKJE into the output. So **HAAKJE is implicitly erased** the next time Normalize touches the term. There is no explicit "unbracket" step.

**Acts as a sort key** in `Compare1` ([sort.c:3115-3212](../../../sources/sort.c#L3115-L3212)). HAAKJE is treated as an early-sorting subterm: terms with HAAKJE at a given position sort before terms whose subterm at that position is anything else. Effect: same-bracket-part terms cluster contiguously in canonical sort order.

**Bypass for factorized expressions** at [proces.c:394](../../../sources/proces.c#L394):

```c
if ( ( e->vflags & ISFACTORIZED ) != 0 && term[1] == HAAKJE ) {
    StoreTerm(BHEAD term);   /* bypass Generator entirely; HAAKJE preserved */
}
```

Used to hold factors-as-brackets in factorized expressions. Doesn't apply to normal `b x;` usage.

## `keep brackets;` — the deferred-bracket optimization

### What it actually saves

Same-bracket terms remain N independent terms after `keep brackets`. The optimization saves **rule-application work**, not terms. If N input terms share an outside O, then:

- Without `keep brackets`: rules run N times, all producing the same outside transformation.
- With `keep brackets`: rules run **once** on the outside (via T_1), the rule result is reused, and each of the N inside parts is spliced onto that single result. Pattern-matching/normalization/substitution work runs once per unique outside.

The terms in the output sort are still N; they get emitted/stored/sorted individually. Only the rule loop above [proces.c:3378](../../../sources/proces.c#L3378) is amortized.

### Mechanism — `Deferred()`

1. Module entry: `AR.DeferFlag = AC.ComDefer = 1`.
2. Generator runs rules on T_1 normally. When `level > AR.Cnumlhs` (all rules done), dispatches to `Deferred()` ([proces.c:3378-3388](../../../sources/proces.c#L3378-L3388)):

```c
if ( level > AR.Cnumlhs ) {
    if ( AR.DeferFlag && AR.sLevel <= 0 ) {
        Deferred(BHEAD term, level);
        goto Return0;
    }
    ...
}
```

3. `Deferred()` ([proces.c:4920](../../../sources/proces.c#L4920)) finds HAAKJE in `AR.CompressBuffer` (T_1's original bytes), then loops:

```c
for(;;) {
    InsertTerm(BHEAD term, 0, AM.rbufnum, tstart, termout, 0);  /* splice */
    Generator(BHEAD termout, level);                            /* skip rules */
    retval = GetOneTerm(BHEAD AT.WorkPointer, AR.infile, &startposition, 0);
    if ( retval <= 0 ) break;
    /* check next term shares same outside; if not, break */
    ...
}
```

Each iteration reads the next input term, checks it shares T_1's outside (by comparing compressed deltas against `AR.CompressBuffer`), splices its inside onto the rule-applied `term`, and re-enters `Generator` at `level > Cnumlhs` — which skips all rule processing and goes straight to PutBracket + StoreTerm.

### When `keep brackets` is a win

Require: the previous module must have applied a `b` so the input terms carry HAAKJE; otherwise `Deferred()` finds no marker and falls through to a normal `Generator` call ([proces.c:4957-4963](../../../sources/proces.c#L4957-L4963)) — no error, no gain.

Optimization wins are proportional to **average terms per bracket**. Putting `b` on the symbols the next module's rules will touch (so rule-relevant work is on the outside, payload on the inside) maximizes sharing.

### Composing brackets across modules

A module can have **both** `keep brackets;` and `b y;`. They operate at different stages and don't conflict:

- `keep brackets;` controls how *input* terms are consumed (via DeferFlag → `Deferred()`).
- `b y;` controls how *output* terms are bracketed (via BracketOn + BrackBuf → `PutBracket` in Generator before StoreTerm).

Standard FORM idiom:

```
b x;                * module N-1: prepare for what N's rules touch
.sort

keep brackets;      * module N: amortize rule work over x-shared terms
id x = ...;
b y;                * re-bracket output for what N+1's rules touch
.sort

keep brackets;      * module N+1: amortize over y-shared terms
id y = ...;
b z;
.sort
```

Each module consumes the previous bracket structure to share rule work, then re-brackets for the next module. `PutBracket` re-scans the full algebraic term against the new spec — the old HAAKJE has been erased by Normalize during rule application, so there's no interference.

### MR + `keep brackets` + `toPolynomial`

If a `.frm` has `on mapreduce;` and uses `toPolynomial onlyfunctions`, add `off mapreduce;` (alongside `off parallel;`) before the procedure that calls toPolynomial — otherwise [parallel.c:1705](../../../sources/parallel.c#L1705) fires `ERROR: Calling Map Reduce without parallel`. See `Spin2_h5_45_mr.frm:152` for the canonical fix.

## Bracket-index (`B+`) — the index file

### Structure

```c
typedef struct BrAcKeTiNdEx {
    POSITION start;           /* master-side file position where bracket begins */
    POSITION next;            /* file position of next bracket */
    LONG    bracket;          /* offset into bracketbuffer */
    LONG    termsinbracket;
} BRACKETINDEX;

typedef struct BrAcKeTiNfO {
    BRACKETINDEX *indexbuffer;
    WORD         *bracketbuffer;     /* packed bracket-parts */
    LONG  bracketbuffersize, indexbuffersize, bracketfill, indexfill;
    WORD  SortType;
} BRACKETINFO;
```

Defined in [structs.h:311-330](../../../sources/structs.h#L311-L330). One `BRACKETINFO` per expression, hung off `Expressions[expr].newbracketinfo`. Allocated only by `B+`, not by plain `b`.

### Built by

`PutBracketInIndex()` ([index.c:331](../../../sources/index.c#L331)). Called per emitted term during `PutOut` when `dobracketindex` is set (see below). Internally stateful: if the new term's bracket-part matches the current open entry, just increments `termsinbracket`; otherwise opens a new entry. So same-bracket calls collapse correctly without explicit tracking by the caller.

### Read by

`FindBracket()` ([index.c:65](../../../sources/index.c#L65)). Binary-searches `indexbuffer` for a given bracket-part. If the index buffer overflows `MaxBracketBufferSize`, some entries are skipped ([index.c:441-522](../../../sources/index.c#L441-L522)); sequential scan from the last indexed bracket fills the gap.

### When is `dobracketindex` on?

In sort.c (1576, 1876, 2241) and parallel.c (1310):

```c
int dobracketindex = ( AR.sLevel <= 0
                  && Expressions[AR.CurExpr].newbracketinfo
                  && ( fout == AR.outfile || fout == AR.hidefile ) ) ? 1 : 0;
```

`newbracketinfo` non-null ⟺ `B+` was used. Plain `b` → `dobracketindex = 0` always.

## MR flow with brackets — full picture

### Plain `b x;` (no `+`)

1. **Mapper**: Generator calls `PutBracket` per term → HAAKJE inserted before StoreTerm.
2. **Mapper hash route** ([sort.c:1897-1927](../../../sources/sort.c#L1897-L1927)): hash on first K words (`AM.MR.HashPrefixWords`), or whole symbolic part if K=0. **No bracket-aware routing.**
3. **Reducer**: sorts via Compare1 (HAAKJE participates as a special early-sorting subterm) → same-bracket terms cluster contiguously. Coefficient-adds identical terms. Delta-compresses and forwards.
4. **Master**: loser-tree merge across reducer streams. Writes terms in canonical order. **No bracket-specific master code** — drain runs as usual, brackets ride in the term bytes.
5. **Print time**: `print;` scans the output, groups contiguous same-bracket-part terms as `(...)`.

### `B+ x;` (indexed)

1-3. Same as plain `b`.
4. **Master**: `dobracketindex = 1` ⇒
   - Bracket index file built per-term via `PutBracketInIndex` inside `PutOut`.
   - **Drain gated off** at [parallel.c:1368](../../../sources/parallel.c#L1368) (`!dobracketindex`): per-term `PutOut` runs instead of bulk-emit.
5. **Print time**: `FindBracket()` uses the index for fast lookup. Sequential fallback still works if the index is partial.

### Bracket fragmentation across reducers — Case A vs Case B

Routing is by first K words of the term (the "K-prefix"). The relationship between K-prefix and bracket-part (positions 1..HAAKJE-position) determines whether a single bracket can span reducers:

| | HAAKJE position P vs K | K-prefix relation to bracket-part | Single bracket span |
|---|---|---|---|
| Case A | P ≤ K | bracket-part ⊆ K-prefix | **Can fragment across reducers** (same bracket, different inside-content-within-K → different K-prefix → different reducer). Master's loser tree handles re-merging — same-bracket terms come out contiguous in canonical order. |
| Case B | P > K | K-prefix ⊆ bracket-part | **Always co-located in one reducer** (same bracket-part ⇒ same K-prefix ⇒ same reducer). Different brackets can also be in same reducer if their K-prefixes happen to match. |

This matters for the master-side drain. See "drain interaction" below.

### Drain interaction with B+

The drain bulk-emit optimization (`pf_emit_compressed_bulk`, [parallel.c:750](../../../sources/parallel.c#L750)) bulk-memcpies a run of compressed terms from one reducer's buffer while passing T_i as a bracket proxy to `PutBracketInIndex` ([parallel.c:828-843](../../../sources/parallel.c#L828-L843)). The proxy is correct only if every drained term shares T_i's bracket-part.

- **Plain `b` workloads**: `dobracketindex = 0` → drain runs unconditionally. HAAKJE bytes ride through the memcpy. Fine.
- **`B+` workloads**: `dobracketindex = 1` → drain currently **gated off** ([parallel.c:1368](../../../sources/parallel.c#L1368)). Reason: the drain's `share > K` trigger does not reliably stop at bracket transitions when HAAKJE > K (Case B). The proxy would be wrong → different brackets collapsed into one index entry.

**Possible optimizations for B+ + drain** (not implemented):

1. **Master-side**: change drain trigger from `share > K` to `share > max(K, P_i − 1)` where P_i = HAAKJE position in T_i. Provably correct in both cases.
2. **Reducer-side**: at bracket transitions in the reducer's emit stream, cap compression `share` to ≤ K so the existing `share > K` trigger stops there naturally.

Either fix would unlock the drainfix6-class speedup (~26% on Spin-shape workloads) for `B+` expressions. Skip until profile evidence shows the per-term `PutBracketInIndex` cost dominates a real B+ workload.

## Optimizing FRM scripts with brackets

### Tuning what's outside vs inside

Rule of thumb: put on the outside what the **next** module's rules will touch. The inside is "payload" that rides through unchanged. Reasons:

- **Compute (with `keep brackets`)**: rule application is amortized over all same-outside terms.
- **Sort (always)**: same-outside terms cluster contiguously in canonical order. Coefficient-adding happens within identical terms; clustering doesn't accelerate it directly, but it improves delta compression (consecutive same-outside terms share a long prefix → high `share`, small compressed tail).
- **Disk space**: in MR mode the wire format between reducer and master is delta-compressed; clustering same-outside terms cuts the per-term tail size to inside + coefficient only.

If the bracket has **one term per bracket on average**, `keep brackets` saves nothing and the clustering benefit is gone. If brackets are very large (thousands of inside terms each), both are big wins.

### Pipeline pattern

Always pair `b <relevant for next module>;` at the end of module N with `keep brackets;` (and a fresh `b` for module N+1) at the start of module N+1. This is the standard chain.

### When `B+` is worth it

`B+` adds a per-term `PutBracketInIndex` call in the master output path, costs memory for the index, and **disables the drain optimization in MR mode**. So:

- Use `B+` only if the workload **reads back brackets** later (via `FindBracket`-using code paths — `KeepBrackets`, `Collect`, `ToPolynomial`, etc., specifically when they need random-access lookup).
- For a workload that just emits to disk and never reads brackets back, plain `b` is strictly better in MR mode.
- For workloads that need the index AND have large enough bracket-counts that the drain win would matter, consider implementing one of the drain-trigger fixes described above. Until then, accept the per-term path cost.

### Compute vs space tradeoff

| Goal | Bracket strategy |
|---|---|
| Minimize compute (rule work) | `b <rule-touched symbols>;` + `keep brackets;` in each module |
| Minimize disk/network (compression) | `b <symbols with high prefix sharing across terms>;` — same effect as above; clustering improves delta compression |
| Need random-access bracket lookup | `B+` — accepts the master per-term overhead in exchange |
| MR drain throughput (most workloads) | Plain `b`, no `+`. Avoid `B+` unless you need it. |

### Common pitfall: `keep brackets` without `b` in previous module

If the previous module had no `b`, terms on disk carry no HAAKJE. `keep brackets;` then has no effect (graceful fall-through at [proces.c:4957](../../../sources/proces.c#L4957)). Always pair `b` (previous module) with `keep brackets` (current module).

### Common pitfall: `b` interactions with `Collect`, `Multiply`, `ToPolynomial`

Some statements force the module non-parallel and may also strip brackets via Normalize. If a module uses these alongside `b` / `keep brackets`, verify the interaction in a small test (diff `on/off mapreduce` outputs). For `toPolynomial onlyfunctions` specifically: add `off mapreduce;` + `off parallel;` before the procedure that calls it (see "MR + keep brackets + toPolynomial" above).

### Common pitfall: bracketing on a variable that doesn't exist yet (READ THE BLOCK FIRST)

**Before placing or judging a `b`, read the actual `.frm` block and trace where each candidate bracket variable is first introduced.** A bracket on a function that is not yet present in the terms at that point is a **no-op** — it brackets on nothing and changes neither storage nor sort order. This is easy to miss when reasoning from memory instead of the code.

Worked example (the `split_id.py` pattern — `scripts/split_id.py`, Spin/dRGT `*_split4096*.frm`): the giant `id diags<N> = …` is split into **two steps** —

```
id diags45 = prefactor*(dgs..V10c0+...+V10c7)*(...V11..)*(...V12..)*(...V13..);  * step 1: 8^4 = 4096 chunk-SYMBOL terms
b ...;
.sort                                          * <-- bracket applied HERE
id dgs45V10c0 = <poly with gi,dotp,Mom>;       * step 2: expand each chunk symbol
...                                            *         this is what INTRODUCES gi/dotp/Mom
.sort:Diagram Loaded;                          * the heavy sort
```

At the step-1 `.sort`, the terms contain only the `dgs45V*` chunk symbols (+ the prefactor's `prop`/`Etens`). `gi/deltaF/Mom/dotp` **do not exist until step 2 expands the chunks.** So `B gi,deltaF,Mom,dotp,prop` *before* step 2 brackets on almost nothing (measured **0.00005 % no-op** on completed Spin — [[project_drgt_bracket_storage]]).

The fixes (both follow "bracket the outside = what the next rules touch"):
- **To make `keep brackets` valid on the step-2 expansion:** bracket on the **`dgs45V*` chunk symbols** — those are exactly what the `id dgs45V*=…` rules match, so they land in the bracket *outside* and the rules act on them. The heavy `Etens×prop` prefactor goes *inside* and is spliced, not reprocessed → "substitution on the lesser term". Bracketing on `gi,…` instead puts the chunk symbols *inside*, and `keep brackets` then **skips** the expansion rules for terms 2…N (leaving raw `dgs45V*` in the output — a correctness failure, not just a no-op).
- **To bracket the heavy `.sort:Diagram Loaded` itself:** put `B gi,deltaF,Mom,dotp,prop` *after* step 2, where those functions now exist.

General rule: never conclude a bracket's storage/timing effect from a **killed/walltimed** run (byte figures shift with term spread); only measure on completed runs. See `feedback_read_before_theorizing` and `feedback_verify_with_flag_set` in auto-memory.

## Quick file map

| File | Role |
|---|---|
| [ftypes.h:361](../../../sources/ftypes.h#L361) | `HAAKJE = 18` (subterm type code) |
| [structs.h:311-330](../../../sources/structs.h#L311-L330) | `BRACKETINDEX`, `BRACKETINFO` |
| [compcomm.c:3779](../../../sources/compcomm.c#L3779) | `DoBrackets`, `CoBracket`, `CoAntiBracket` — statement compilation |
| [compcomm.c:997-1002](../../../sources/compcomm.c#L997-L1002) | `CoKeep` — sets `AC.ComDefer = 1` |
| [execute.c:1126](../../../sources/execute.c#L1126) | `PutBracket` — splits term, inserts HAAKJE |
| [proces.c:243](../../../sources/proces.c#L243) | `AR.DeferFlag = AC.ComDefer` at module entry |
| [proces.c:394](../../../sources/proces.c#L394) | ISFACTORIZED HAAKJE bypass |
| [proces.c:3378-3388](../../../sources/proces.c#L3378-L3388) | Generator dispatch to `Deferred()` |
| [proces.c:3418-3439](../../../sources/proces.c#L3418-L3439) | `PutBracket` call in Generator pipeline |
| [proces.c:4920](../../../sources/proces.c#L4920) | `Deferred` — keep-brackets loop |
| [normal.c:509](../../../sources/normal.c#L509) | HAAKJE skipped (implicitly erased) |
| [sort.c:3115-3212](../../../sources/sort.c#L3115-L3212) | `Compare1` — HAAKJE as early-sorting subterm |
| [index.c:331](../../../sources/index.c#L331) | `PutBracketInIndex` — index build |
| [index.c:65](../../../sources/index.c#L65) | `FindBracket` — index lookup |
| [parallel.c:1310](../../../sources/parallel.c#L1310) | `dobracketindex` setup |
| [parallel.c:1368](../../../sources/parallel.c#L1368) | drain gate (`!dobracketindex`) |
| [parallel.c:828-843](../../../sources/parallel.c#L828-L843) | `pf_emit_compressed_bulk` per-term `PutBracketInIndex` proxy walk |
| [parallel.c:2279](../../../sources/parallel.c#L2279) | Master clears `AR.DeferFlag` (workers run keep-brackets, not master) |
