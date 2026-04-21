# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

FORM is a Symbolic Manipulation System for high-energy physics. It reads symbolic expressions from files and performs algebraic transformations.

This branch (`MRmpi`) is a research fork of ParFORM adding an optional **Map-Reduce parallel sort** mode. Goals vs standard ParFORM:
- **Less disk I/O** — reducers deduplicate terms before they reach the master, so no duplicate terms are written to disk across workers.
- **Faster mappers** — mappers only generate and hash-route terms; they do not sort or write to disk themselves.
- **Smaller master merge tree** — master receives from reducers only, not from all workers.

The mode is enabled per-module with `on mapreduce;` in a `.frm` script and activated at runtime with `-r<N>` (N = percentage of workers to assign as reducers). When disabled the code falls back to standard ParFORM behavior.

## Build Commands

```bash
# First-time setup
autoreconf -i
./configure --enable-parform   # MRmpi requires parform

# Build parform
make -C sources parform

# Build all enabled targets
make
```

**Configure flags of note:**
- `--enable-parform` — build `parform` (MPI) — required for MRmpi
- `--enable-debug` — build debug variant `parvorm`
- `--with-zstd` — zstd compression
- `--enable-coverage`, `--enable-profile` — coverage/profiling builds

## Running Tests

```bash
# Full test suite via make
make check

# Quick MRmpi smoke tests (9 MPI processes, uses hostfile)
cd tests/simple_tests && bash runall.sh

# Single MRmpi run
mpirun -np 9 parform small_test_red.frm   # uses 'on mapreduce;' inside the .frm
```

`tests/simple_tests/` contains hand-crafted `.frm` scripts for MRmpi. `tests/iostat/` contains I/O measurement scripts.

## Architecture

### Expression Pipeline

1. **Parsing** — `compiler.c` compiles the FORM language; `pre.c` handles the preprocessor; `token.c` tokenizes input.
2. **Storage** — `store.c` persists expressions to disk.
3. **Pattern matching / Transformation** — `pattern.c`, `findpat.c`, `execute.c`, `transform.c`.
4. **Sorting** — `sort.c` is performance-critical; canonical ordering is central to the system.
5. **Normalization** — `normal.c` + `comexpr.c`; `compcomm.c` handles common sub-expression collection (largest file, ~200 KB).

Core structs are in `sources/structs.h` (~3000+ lines). Key type aliases (`WORD`, `LONG`, `ULONG`) are in `sources/ftypes.h`.

### MRmpi Map-Reduce Extension

The map-reduce mode is controlled by three flags in `AC` (`structs.h`):
- `MRflag` — global setting (`on/off mapreduce;` in FORM script, parsed in `compcomm.c`)
- `mMRflag` — per-module effective flag
- `sMRflag` — state machine: `NO_MAPREDUCE` / `MAPREDUCE_FIRST` / `MAPREDUCE` / `MAPREDUCE_LAST` (constants in `ftypes.h`)



Key per-role code paths:
- **Mapper (`sort.c` `PF_LowMRsort`)** — terms are hashed and routed to a reducer via `PF_WISendSbuf` instead of being written to the local sort file. Each mapper holds a per-reducer send buffer (`PF.sbufs[dest]`).
- **Reducer (`parallel.c` `PF_StoreBuffer` / `PF_ReducerInit`)** — receives terms from all mappers, stores patches to its local sort buffer, runs a standard merge sort, then forwards the sorted stream to the master.
- **Master** — runs the merge tree as usual but receives only from reducers, shrinking the fan-in.

New MPI message tags (`parallel.h`):
- `PF_SHUFFLE_MSGTAG` (110) — mapper → reducer: term data
- `PF_ENDSHUFFLE_MSGTAG` (111) — mapper → reducer: end of data
- `PF_ENDSHUFFLEALL_MSGTAG` (112) — signals all shuffling is complete

`-r<N>` CLI flag (parsed in `startup.c`) sets `AM.ReducerPer`.

### Parallel Processing Files

- `mpi.c` — MPI communication; `PF_WISendSbuf` routes sends to reducer or master
- `parallel.c` + `parallel.h` — full MRmpi role dispatch loop; `PARALLELVARS` struct holds `nummappers`, `numreducers`, `sbufs`

## Key Files Quick Reference

| File | Role |
|------|------|
| `sources/structs.h` | All core data structures (includes `MRflag`, `ReducerPer`) |
| `sources/ftypes.h` | FORM type aliases + MRmpi state flag constants |
| `sources/startup.c` | `main()`, command-line parsing, `-r<N>` flag |
| `sources/compcomm.c` | `on mapreduce;` keyword handling; per-module flag logic |
| `sources/execute.c` | `sMRflag` state machine transitions between modules |
| `sources/sort.c` | Sorting; `PF_LowMRsort()` decides mapper vs normal path |
| `sources/parallel.c` | Master/mapper/reducer dispatch; `PF_StoreBuffer`, `PF_ReducerInit` |
| `sources/parallel.h` | `PARALLELVARS` struct; new MPI tags |
| `sources/mpi.c` | `PF_WISendSbuf` — routes sends to reducer or master |
| `sources/form3.h` | Master include, platform abstractions |
| `tests/simple_tests/` | MRmpi test scripts |
| `configure.ac` | Autoconf build configuration |
