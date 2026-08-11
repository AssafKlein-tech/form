# Map-Reduce Parallel Sort (MRmpi) for ParFORM

This build of FORM/ParFORM adds an optional **map-reduce parallel sort** mode on
top of the standard ParFORM (`parform`, MPI) engine, for large workloads where
the standard master-merge sort becomes I/O- or fan-in-bound.

When the mode is **not** enabled the engine behaves exactly as stock ParFORM:
the map-reduce code paths are inert and the output is byte-for-byte identical.

## What it does

In standard ParFORM every worker sorts its terms locally and streams them to a
single master that merges all worker streams. In map-reduce mode the workers are
split into two roles:

- **Mappers** generate terms and hash-route them to reducers. They do not sort
  or write sort files themselves.
- **Reducers** receive terms from all mappers, merge them, and forward a single
  sorted stream upstream.

This cuts disk I/O (terms are combined before they reach disk), speeds up the
mappers (no local sort), and shrinks the master's merge fan-in.

An optional **merger tier** (`PF_MERGERS`) inserts one node-local merge stage
between reducers and the master, keeping reducer→merger traffic on-node.

## Building

```sh
autoreconf -i
./configure --enable-parform            # MPI engine, required for MRmpi
make -C sources parform
```

To enable zstd compression of sort/scratch files (recommended — see
"Compression"), initialise the bundled zstd submodule and configure with
`--with-zstd`:

```sh
git submodule update --init extern/zstd
./configure --enable-parform --with-zstd
make -C sources parform
```

`--enable-mr-profile` additionally builds an optional per-phase profiler (writes
`pf_profile.csv` with per-module, per-rank timings). It is gated behind a
compile flag and adds zero overhead to builds without it.

## Enabling the mode

Two things are required:

1. Turn the mode on for the heavy modules in the FORM script:

   ```
   on mapreduce;
   ... heavy modules ...
   off mapreduce;
   ```

2. At launch, assign a percentage of the MPI ranks as reducers with `-r<N>`
   (`N` = percent of workers to make reducers):

   ```sh
   mpirun -np 64 parform -r12 myjob.frm
   ```

   Without `-r<N>` the percentage comes from the `reducerpercent` setup-file
   parameter, whose default is **12**. Values outside `0 <= N < 50` are clamped
   to 50 — a run cannot have more reducers than mappers.

Reducers are the highest-numbered ranks. On a **multi-node** run pass
`--map-by node` to `mpirun` so ranks — and therefore reducers — spread evenly
across nodes instead of concentrating on the last one.

### `toPolynomial` caveat

If a module uses `toPolynomial onlyfunctions`, add `off mapreduce;` (alongside
`off parallel;`) before the procedure that calls it. `toPolynomial` forces the
module non-parallel for its polynomial arithmetic, and leaving the map-reduce
flag set trips an internal guard.

## Compression

Sort files and the master's expression scratch can be compressed on disk:

- Build with `--with-zstd` to route FORM's compression through zstd globally
  (reducer sort files included).
- Set `PF_SCRATCH_COMPRESS=<1-9>` to enable block compression of the master's
  expression scratch (`.sc0`). With a zstd build, level 1 is the recommended
  setting: a large space saving at essentially no time cost. Unset = off (a
  provable no-op).

## Runtime tuning

All knobs are environment variables, read on the master and broadcast to the
workers.

| Variable | Default | Cap | Purpose |
|---|---|---|---|
| `PF_SBUFS` | 2 | 10 | Send-buffer slots per (mapper, destination). |
| `PF_RBUFS` | 2 | 2 | Receive-buffer slots per (reducer, source mapper). |
| `PF_MERGERS` | 0 (off) | — | Any value > 0 enables the node-local merger tier. |
| `PF_STEAL` | 0 (off) | — | Work-stealing load balancer (requires the merger tier). |
| `PF_HASH_PREFIX_WORDS` | 0 | — | Prefix-word count for hash routing / master drain. |
| `PF_SCRATCH_COMPRESS` | 0 (off) | 9 | Compression level for master `.sc0` block compression. |
| `PF_STATS` | 10 | — | Statistics reporting interval. |
| `PF_LOG` | 0 | — | Logging verbosity. |
| `PF_BC_STATS` | unset | — | If set, per-module scratch compression ratios are appended to this file. |

## Verifying correctness

The mode is designed to produce identical results to the standard engine. To
verify on any script:

1. Make two versions of the script — one with `on mapreduce;` and one with
   `off mapreduce;` on the heavy modules.
2. Run the map-reduce version with `-r<N>` and the standard version without.
3. `diff` the final FORM output: it must be byte-identical.

## Large `id` substitutions — `scripts/split_id.py`

### When to use it

A single `id` rule whose right-hand side is a product of several many-summand
vertex factors can blow up term generation long before the sort ever runs. A
3-vertex rule with 399 summands per vertex expands to `399^3` products per input
term; at ~1300 input terms that is ~83 billion generated terms.

Worse, such rules are usually written inside an `off parallel;` block, so the
whole expansion runs **serially on the master** — no mapper ever sees it, and
map-reduce mode cannot help. The symptom is a job that sits on one module for
hours with `Terms active` climbing steadily and no `.sort` in sight.

Use the splitter when **all** of these hold:

- One `id diags<N> = ...;` rule dominates the runtime.
- Its right-hand side is a product of parenthesised factors with many top-level
  summands (the script's threshold is 10).
- The rule sits under `off parallel;`.

Do **not** use it for rules that are already cheap, or whose blow-up comes from
the number of input terms rather than the size of the right-hand side — the
splitter reduces peak term count per module, not total work.

### Where to use it

Run it on the `.frm` script **before** submitting the job; it is a source-to-
source rewrite, not a runtime option. It is orthogonal to `on mapreduce;` — the
split makes the module parallelisable, and map-reduce mode then handles the
resulting sorts. Apply it to the map-reduce variant of the script.

### How to use it

```sh
python3 scripts/split_id.py INPUT.frm OUTPUT.frm K_target
```

`K_target` is the desired number of chunks. The script finds the `id diags<N>`
rule, identifies the `V` vertex factors, and gives each one
`N = round(K_target ** (1/V))` chunks (capped at that vertex's summand count),
so the realised `K` is the product of the per-vertex `N`s and only approximates
`K_target`. It prints the factor analysis and the actual `K` it produced.

The rewrite is a two-step substitution: the original rule becomes a product of
sums of fresh opaque chunk symbols (`dgs<N>V<factor>c<chunk>`), then a `.sort`,
then one `id` per chunk symbol expanding it to its share of the summands.
Substituting the chunk symbols back reproduces the original product exactly, so
the rewrite is algebraically equivalent by construction. The script also flips
the nearest preceding `off parallel;` to `on parallel;` so the chunk
substitutions run distributed.

Because the chunk symbols only exist between the two steps, a `b`/bracket
statement aimed at this block must bracket on the `dgs*` chunk symbols — the
functions the second step creates do not exist yet at the first `.sort`.

### Validating a split

The rewrite is meant to be output-preserving, so validate it before trusting a
new workload:

1. Pick a script that already has a known-good baseline output.
2. Split it with the same `K_target` you intend to use.
3. Run both and confirm the final output is byte-identical (compare `sha256sum`
   of the `.out` files).

Only then apply the splitter to the workload that has no baseline yet.
