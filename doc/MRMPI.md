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

## Large `id` substitutions

A single `id` rule with very large right-hand sides (products of many-summand
vertex factors) can blow up term generation before the sort ever runs. The
helper `scripts/split_id.py` rewrites such a rule into `K` parallel chunks that
generate independently. See the script's header for usage.
