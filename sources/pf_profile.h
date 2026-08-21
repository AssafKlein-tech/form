#ifndef __PF_PROFILE_H__
#define __PF_PROFILE_H__

/** @file pf_profile.h
 *
 *  MRmpi per-phase profiling instrumentation.
 *
 *  Active only when compiled with -DPF_PROFILE (see --enable-mr-profile).
 *  In production builds without PF_PROFILE all macros expand to ((void)0)
 *  and the translation unit pf_profile.c compiles to nothing, so there is
 *  zero runtime overhead.
 */
/* #[ License : */
/*
 *   Copyright (C) 1984-2026 J.A.M. Vermaseren
 *   When using this file you are requested to refer to the publication
 *   J.A.M.Vermaseren "New features of FORM" math-ph/0010025
 *   This is considered a matter of courtesy as the development was paid
 *   for by FOM the Dutch physics granting agency and we would like to
 *   be able to track its scientific use to convince FOM of its value
 *   for the community.
 *
 *   This file is part of FORM.
 *
 *   FORM is free software: you can redistribute it and/or modify it under the
 *   terms of the GNU General Public License as published by the Free Software
 *   Foundation, either version 3 of the License, or (at your option) any later
 *   version.
 *
 *   FORM is distributed in the hope that it will be useful, but WITHOUT ANY
 *   WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *   FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *   details.
 *
 *   You should have received a copy of the GNU General Public License along
 *   with FORM.  If not, see <http://www.gnu.org/licenses/>.
 */
/* #] License : */

#ifdef PF_PROFILE

#include <stdio.h>
#include <mpi.h>
#include "ftypes.h"

/* Phase IDs. Each phase maps to a slot in pf_phase_us[]. The 9 timed phases
   cover all instrumented call sites; the 2 derived phases (MAP_HASH_PACK,
   RED_STORE) are computed by the visualization script via subtraction. */
enum {
	PF_PHASE_MAP_GENERATOR = 0,    /* mapper: Generator() loop */
	PF_PHASE_MAP_ENDSORT_TOTAL,    /* mapper: total wall time of EndSort */
	PF_PHASE_MAP_SEND_WAIT,        /* mapper: MPI_Wait inside PF_WISendSbuf */
	PF_PHASE_MAP_SEND_MPI,         /* mapper: MPI_Isend post */
	PF_PHASE_MAP_GETTERM_WAIT,     /* mapper: blocked in PF_RecvWbuf waiting for master to dispatch terms (pair to MAS_DISTRIBUTE_WAIT) */
	/* Mapper sub-phases inside the per-term hot path. Splitting the previously
	   uncounted "hash+pack" gap (see plan i-want-to-attack-dazzling-gizmo.md)
	   into its constituent slices so we can attribute the 14% directly instead
	   of by subtraction. */
	PF_PHASE_MAP_SMALL_FLUSH_TOTAL,/* mapper: total wall time of small-buffer flush at sort.c:5024 (SplitMerge+ComPress+PutOut loop) */
	PF_PHASE_MAP_SPLITMERGE,       /* mapper: SplitMerge call inside the small-buffer flush (sort.c:5034) */
	PF_PHASE_MAP_COMPRESS_BATCH,   /* mapper: ComPress call inside the small-buffer flush (sort.c:958) -- the first (potentially redundant) compression pass */
	PF_PHASE_MAP_HASH_ROUTE,       /* mapper: hash + dst calculation per term in PutOut (sort.c:1892-1927) */
	PF_PHASE_MAP_DELTA_COMPRESS,   /* mapper: per-reducer delta compress block in PutOut (sort.c:1940-2020) */
	PF_PHASE_MAP_SBUF_COPY,        /* mapper: NCOPY of the compressed term into the per-reducer sendbuf (sort.c:2089-2099) */
	/* Generator sub-phases. Wrap per-term inner work in proces.c Generator
	   so MAP_GENERATOR can be decomposed into Normalize / TestSub / PrepPoly
	   / StoreTerm / (derived) GEN_OTHER. */
	PF_PHASE_MAP_TESTSUB,          /* mapper: TestSub call in Generator (proces.c) */
	PF_PHASE_MAP_NORMALIZE,        /* mapper: Normalize call in Generator (normal.c:193 callee, wrapped at proces.c call site) */
	PF_PHASE_MAP_PREPPOLY,         /* mapper: PrepPoly call in Generator (proces.c:3361) -- should be ~0 in MR modules */
	PF_PHASE_MAP_STORETERM,        /* mapper: StoreTerm call in Generator -- small-buffer write cost */
	/* Phase-2 Generator sub-phases: drilling into GEN_OTHER (74% of mapper
	   time on Spin per mapperprof CSV). All five experimental module-splits
	   ended within noise band, confirming the bottleneck is per-term work
	   inside Generator()'s level-dispatch loop, not module boundaries.
	   Wrapping the heaviest calls inside that loop so the next profile run
	   can attribute the GEN_OTHER residual to specific FORM primitives. */
	PF_PHASE_MAP_TESTMATCH,        /* mapper: TestMatch call inside Generator's do-while (proces.c:4049) -- per-rule pattern scan, likely the dominant slice of GEN_OTHER */
	PF_PHASE_MAP_TESTSUB_POSTMATCH,/* mapper: TestSub call after TestMatch returned non-zero (proces.c:4051) -- different from MAP_TESTSUB which wraps the entry-side call */
	PF_PHASE_MAP_POLYFUNMUL,       /* mapper: PolyFunMul calls in Generator (proces.c:3304,3331,3337,3345) -- polynomial multiplication in PolyNormFlag handling */
	PF_PHASE_MAP_TAKEIDFUNCTION,   /* mapper: TakeIDfunction call in Generator (proces.c:3353) -- when idfunctionflag is set after Normalize */
	PF_PHASE_MAP_PUTBRACKET,       /* mapper: PutBracket call in Generator (proces.c:3407) -- bracket emission before StoreTerm */
	PF_PHASE_RED_RECV_WAIT,        /* reducer: PF_WaitAnyRbuf in PF_StoreBuffer */
	PF_PHASE_RED_STORE_TOTAL,      /* reducer: total wall time of one PF_StoreBuffer pass (includes recv wait + memcpy + maybe MergePatches) -- baseline for 1b which would add SplitMerge here */
	PF_PHASE_RED_BUFFER_COPY,      /* reducer: term-by-term memcpy of an arrived buffer into the sort patch */
	PF_PHASE_RED_MERGE_PATCHES,    /* reducer: MergePatches calls */
	PF_PHASE_RED_FINAL_SORT,       /* reducer: EndSort in PF_ForwardTermsToMaster */
	PF_PHASE_RED_FORWARD_WAIT,     /* reducer: MPI_Wait when sending to master */
	PF_PHASE_RED_FORWARD_MPI,      /* reducer: MPI_Isend to master */
	PF_PHASE_MAS_DISTRIBUTE,       /* master: term-distribution loop */
	PF_PHASE_MAS_DISTRIBUTE_WAIT,  /* master: PF_Wait4Slave */
	PF_PHASE_MAS_FINAL_SORT,       /* master: EndSort merge tree (includes MAS_MERGE_RECV_WAIT) */
	PF_PHASE_MAS_COLLECT,          /* master: end-of-module PF_LongSingleReceive stats loop */
	PF_PHASE_MAS_MERGE_RECV_WAIT,  /* master: MPI_Wait inside PF_PutIn -- blocked mid-merge for a child's (reducer's) next sorted chunk. Sub-component of MAS_FINAL_SORT, not additive with it. */
	/* Merger phases. Mapper-merger ranks (1..nummergers) only; zero on every
	   other rank and on non-merger runs. The merger reuses the master's
	   loser-tree merge body, so MER_MERGE is its analogue of MAS_FINAL_SORT
	   and MER_RECV_WAIT its analogue of MAS_MERGE_RECV_WAIT -- the runtime
	   PF.in_merger_phase flag picks MER_* vs MAS_* at the shared call sites. */
	PF_PHASE_MER_MERGE,            /* merger: total wall time of PF_MergerLoop's EndSort over the leaf-reducer streams */
	PF_PHASE_MER_RECV_WAIT,        /* merger: MPI_Wait inside PF_PutIn -- blocked mid-merge for a leaf reducer's next sorted chunk. Sub-component of MER_MERGE, not additive with it. */
	PF_PHASE_MER_FORWARD_WAIT,     /* merger: MPI_Wait when forwarding the merged stream to master */
	PF_PHASE_MER_FORWARD_MPI,      /* merger: MPI_Isend forwarding the merged stream to master */
	/* Master-bypass merger-tier phases (the partitioned-redistribution feature).
	   MER_DISTRIBUTE is the merger's analogue of MAS_DISTRIBUTE -- when a module is
	   input-partitioned the merger (not the master) hands node-local buckets out of
	   its merger_infile; MER_DISTRIBUTE_WAIT is its PF_Receive(READY) wait, the
	   analogue of MAS_DISTRIBUTE_WAIT. The GATHER phases isolate the chain-exit
	   re-globalization (MAPREDUCE_LAST / `.sort(gather)`) from the per-module merge:
	   MAS_GATHER is the master merging the G merger streams into the global scratch
	   (vs MAS_FINAL_SORT = a classic non-MR master merge), MER_GATHER is the merger
	   streaming its file up in pf_merger_gather_to_master. MAS_MERGERDONE is the
	   master's ENTIRE EndSort on a bypassed (output-partitioned) module --
	   WaitAllSlaves + PF_MERGERDONE collect + prototype flush, with NO merge -- so
	   MAS_FINAL_SORT is 0 there, which is the whole point of the bypass. */
	PF_PHASE_MER_DISTRIBUTE,       /* merger: pf_distribute_terms loop in PF_MergerDistribute (node-local input handoff) */
	PF_PHASE_MER_DISTRIBUTE_WAIT,  /* merger: PF_Receive(PF_READY) wait for a node-local mapper inside PF_MergerDistribute */
	PF_PHASE_MAS_GATHER,           /* master: EndSort merge of the G merger streams at the chain-exit gather (MAPREDUCE_LAST) */
	PF_PHASE_MER_GATHER,           /* merger: pf_merger_gather_to_master -- read merger_infile + stream to master at the gather */
	PF_PHASE_MAS_MERGERDONE,       /* master: whole EndSort on a bypassed (output-partitioned) module -- WaitAllSlaves + MERGERDONE collect + prototype flush, NO merge */
	PF_PHASE_COUNT
};

/* OS counter slots (filled from /proc/self/io and getrusage at module
   boundaries). Values are end-minus-start diffs. */
enum {
	PF_OS_IO_RCHAR = 0,
	PF_OS_IO_WCHAR,
	PF_OS_IO_READ_BYTES,
	PF_OS_IO_WRITE_BYTES,
	PF_OS_IO_SYSCR,
	PF_OS_IO_SYSCW,
	PF_OS_RU_MAXRSS,
	PF_OS_RU_MINFLT,
	PF_OS_RU_MAJFLT,
	PF_OS_RU_NVCSW,
	PF_OS_RU_NIVCSW,
	PF_OS_NODE_DISK_TIME_IN_IO_MS, /* node-leader only; -1 on non-leader */
	PF_OS_NODE_WALLCLOCK_US,       /* node-leader only; -1 on non-leader */
	PF_OS_NODE_NIC_XMIT_BYTES,     /* node-leader only; -1 on non-leader / wrap */
	PF_OS_NODE_NIC_RCV_BYTES,      /* node-leader only; -1 on non-leader / wrap */
	PF_OS_COUNT
};

/* Extra per-rank counters tracked outside the phase timers. */
enum {
	PF_EX_BYTES_SENT = 0,        /* mapper: total bytes shipped to reducers */
	PF_EX_BYTES_TO_MASTER,       /* reducer: total bytes shipped to master */
	PF_EX_TERMS_SENT,            /* reserved (filled from existing PF_linterms in CSV) */
	PF_EX_PATCHES_BUILT,         /* reducer: total MergePatches calls */
	PF_EX_WALLCLOCK_US,          /* per-rank wallclock for the module */
	PF_EX_BUFFERS_RECEIVED,      /* reducer: chunks consumed in PF_StoreBuffer (1 per mapper-side send) */
	PF_EX_MERGE_LBUFFER_FULL,    /* reducer: MergePatches firings caused by large buffer running out of room */
	PF_EX_MERGE_MAX_PATCHES,     /* reducer: MergePatches firings caused by lPatch >= MaxPatches */
	PF_EX_BYTES_MER_TO_MASTER,   /* merger: total bytes forwarded to master */
	/* Counters added for the mapper-attack instrumentation plan
	   (i-want-to-attack-dazzling-gizmo.md). Each tests a specific candidate
	   optimization's premise (e.g. how often Normalize would short-circuit). */
	PF_EX_MAP_SBUF_FLUSHES,      /* mapper: PF_WISendSbuf calls -- average payload = BYTES_SENT/this */
	PF_EX_MAP_BYTES_PRECOMPRESS, /* mapper: sum raw term bytes entering per-reducer delta compress (sort.c:1946) -- compression ratio numerator */
	PF_EX_MAP_BYTES_POSTCOMPRESS,/* mapper: sum compressed bytes emitted into per-reducer CompressBuffers -- compression ratio denominator */
	PF_EX_MAP_BYTES_SHUFFLED,    /* mapper: sum of MPI_Isend size in PF_ISendSbuf (mpi.c:445) -- bandwidth sanity check */
	PF_EX_MAP_TERMS_IN,          /* mapper: terms entering Generator -- denominator for the fractional counters below */
	PF_EX_MAP_NORM_CLEAN_IN,     /* mapper: terms arriving at Normalize with the dirty flag clear -- candidate 2b ceiling */
	PF_EX_MAP_NORM_CHANGED,      /* mapper: Normalize returns with the term modified -- candidate 2b realised benefit */
	PF_EX_MAP_TESTSUB_PREV_RULE_HIT, /* mapper: TestSub picked the same rule as the previous term -- candidate 2c LRU-1 value */
	PF_EX_MAP_TESTSUB_NO_MATCH,  /* mapper: TestSub returned 0 (no match) -- candidate 2c pure-overhead path */
	PF_EX_RED_BYTES_RECEIVED,    /* reducer: bytes consumed in PF_StoreBuffer -- per-link bandwidth */
	PF_EX_MER_DISTRIBUTE_TERMS,  /* merger: terms handed to node-local mappers from merger_infile (PF_MergerDistribute ninterms) */
	PF_EX_MER_PARTITION_BYTES,   /* merger: byte size of this module's partitioned output written to merger_outfile (the .sc-style scratch the next module distributes) */
	PF_EX_COUNT
};

/* OS-counter snapshot at a point in time. */
typedef struct {
	LONG io_rchar;
	LONG io_wchar;
	LONG io_read_bytes;
	LONG io_write_bytes;
	LONG io_syscr;
	LONG io_syscw;
	LONG ru_maxrss;
	LONG ru_minflt;
	LONG ru_majflt;
	LONG ru_nvcsw;
	LONG ru_nivcsw;
	LONG node_disk_time_in_io_ms; /* -1 on non-leader */
	LONG node_wallclock_us;       /* -1 on non-leader */
	LONG node_nic_xmit_bytes;     /* -1 on non-leader; IB sysfs counter * 4 */
	LONG node_nic_rcv_bytes;      /* -1 on non-leader; IB sysfs counter * 4 */
} PF_OSCounters;

/* Per-rank profile slots (sent via PF_LongSinglePack; received and stored
   in pf_profile_stats[rank] on the master). phase_first_us / phase_last_us
   are wall-clock offsets from the module start in microseconds (-1 if the
   phase never executed on that rank for that module); they drive the Gantt
   panel in the visualization. */
typedef struct {
	LONG phase_us[PF_PHASE_COUNT];
	LONG phase_first_us[PF_PHASE_COUNT];
	LONG phase_last_us[PF_PHASE_COUNT];
	LONG os_diff[PF_OS_COUNT];
	LONG extras[PF_EX_COUNT];
} PF_ProfileSlot;

/* Globals: filled per-module by each rank, then packed and sent to master. */
extern LONG pf_phase_us[PF_PHASE_COUNT];
extern LONG pf_phase_first_us[PF_PHASE_COUNT];
extern LONG pf_phase_last_us[PF_PHASE_COUNT];
extern LONG pf_os_diff[PF_OS_COUNT];
extern LONG pf_extras[PF_EX_COUNT];

/* Module-start MPI_Wtime() reference; first/last timestamps are computed
   relative to this. Set by parallel.c right after pf_profile_reset_module()
   and the start-of-module OS snapshot. */
extern double pf_module_t0;

/* Per-module chain state (AC.sMRflag) of the module being profiled, stamped by
   PF_Processor so the master CSV/viz can label each module
   NO_MAPREDUCE(0)/MAPREDUCE(1)/MAPREDUCE_LAST(2)/MAPREDUCE_FIRST(4) -- i.e.
   distinguish a bypassed (FIRST/MAPREDUCE) module from a gather (LAST) or a
   classic non-MR (0) one, which a raw MAS_FINAL_SORT total cannot. */
extern int pf_module_smrflag;

/* Master-only: per-rank receive buffer, allocated lazily on first dump. */
extern PF_ProfileSlot *pf_profile_stats;

/* Compare1 K-prefix first-diff histogram. Only the master writes this, and
   only when sort.c's pf_compare_kcap>0 (master's MR k-way merge with the
   hash-prefix-routing invariant). Bins 0..PF_COMPARE1_HIST_K-1 count the WORD
   position of the first divergence in the K-prefix scan; bin
   PF_COMPARE1_HIST_K counts the "no diff within K" case (terms agreed on the
   full K-prefix). Used to decide whether SIMD on the prefix branch is worth
   the engineering cost. Linear bins because K is small (default 128). */
#define PF_COMPARE1_HIST_K 128
#define PF_COMPARE1_HIST_BINS (PF_COMPARE1_HIST_K + 1)
extern LONG pf_compare1_diff_hist[PF_COMPARE1_HIST_BINS];

/*
   Timer macros. PF_TIMER_BEGIN(P) introduces a local pf_t_##P at the call
   site and (on first entry within a module) records the phase's relative
   start time in pf_phase_first_us[P]. PF_TIMER_END(P) accumulates duration
   and updates pf_phase_last_us[P]. MPI_Wtime() ~30 ns on Linux.

   IMPORTANT: PF_TIMER_BEGIN expands to a declaration + a conditional update;
   it must appear at top-level inside a brace-delimited block, never as the
   body of an unbraced if/else/for. All current call sites comply.
*/
#define PF_TIMER_BEGIN(P) \
	double pf_t_##P = MPI_Wtime(); \
	if (pf_phase_first_us[PF_PHASE_##P] < 0) \
		pf_phase_first_us[PF_PHASE_##P] = (LONG)((pf_t_##P - pf_module_t0) * 1.0e6)
#define PF_TIMER_END(P) \
	do { \
		double pf_now_##P = MPI_Wtime(); \
		pf_phase_us[PF_PHASE_##P] += (LONG)((pf_now_##P - pf_t_##P) * 1.0e6); \
		pf_phase_last_us[PF_PHASE_##P] = (LONG)((pf_now_##P - pf_module_t0) * 1.0e6); \
	} while (0)
/* Runtime-indexed timer: phase ID resolved at runtime (e.g. PF_ISendSbuf
   chooses MAP_SEND_* vs RED_FORWARD_* based on caller's role). Both first
   and last are attributed at END time since the index isn't known at BEGIN. */
#define PF_TIMER_BEGIN_RT(name) double pf_t_rt_##name = MPI_Wtime()
#define PF_TIMER_END_RT(name, idx) \
	do { \
		if ((idx) >= 0) { \
			double pf_now_rt_##name = MPI_Wtime(); \
			pf_phase_us[(idx)] += (LONG)((pf_now_rt_##name - pf_t_rt_##name) * 1.0e6); \
			if (pf_phase_first_us[(idx)] < 0) \
				pf_phase_first_us[(idx)] = (LONG)((pf_t_rt_##name - pf_module_t0) * 1.0e6); \
			pf_phase_last_us[(idx)] = (LONG)((pf_now_rt_##name - pf_module_t0) * 1.0e6); \
		} \
	} while (0)
/* Elapsed-wait accumulator (tools.c TimeElapsed). Profile-only, like the
   timers above: the accumulated total is reported through the per-module
   statistics and has no consumer in a production build.
   PF_TIME_ELAPSED is the statement form (reset/start/stop);
   PF_TIME_ELAPSED_GET is the value form (read the accumulated total). */
#define PF_TIME_ELAPSED(par) ((void)TimeElapsed(par))
#define PF_TIME_ELAPSED_GET() TimeElapsed(TIMEGET)
#define PF_TIMER_ADD_BYTES(idx, n) (pf_extras[(idx)] += (LONG)(n))
#define PF_TIMER_INC(idx) (pf_extras[(idx)]++)
#define PF_TIMER_ADD_COUNT(idx, n) (pf_extras[(idx)] += (LONG)(n))
/* Conditional timer: same shape as PF_TIMER_BEGIN/END but only accumulates
   when `cond` is true at BEGIN time. The local `pf_t_##P` carries the gate
   (set to -1.0 when disabled). Lets us write straight-line code instead of
   duplicating bodies for MR-only call sites in shared functions like PutOut. */
#define PF_TIMER_BEGIN_IF(P, cond) \
	double pf_t_##P = (cond) ? MPI_Wtime() : -1.0; \
	if (pf_t_##P >= 0.0 && pf_phase_first_us[PF_PHASE_##P] < 0) \
		pf_phase_first_us[PF_PHASE_##P] = (LONG)((pf_t_##P - pf_module_t0) * 1.0e6)
#define PF_TIMER_END_IF(P) \
	do { \
		if (pf_t_##P >= 0.0) { \
			double pf_now_##P = MPI_Wtime(); \
			pf_phase_us[PF_PHASE_##P] += (LONG)((pf_now_##P - pf_t_##P) * 1.0e6); \
			pf_phase_last_us[PF_PHASE_##P] = (LONG)((pf_now_##P - pf_module_t0) * 1.0e6); \
		} \
	} while (0)

/* API. */
void pf_profile_reset_module(void);
void pf_profile_snapshot_os(PF_OSCounters *out);
void pf_profile_diff_os(const PF_OSCounters *start, const PF_OSCounters *end, LONG *out_diff);
int  pf_profile_per_node_init(void);
int  pf_profile_is_node_leader(void);
void pf_profile_alloc_master(int numtasks);
void pf_profile_dump_master_csv(int module_num, const char *expr_name,
                                int nummappers, int numreducers,
                                int numtasks);

#else /* !PF_PROFILE */

#define PF_TIMER_BEGIN(P) ((void)0)
#define PF_TIMER_END(P) ((void)0)
#define PF_TIMER_BEGIN_RT(name) ((void)0)
#define PF_TIMER_END_RT(name, idx) ((void)0)
#define PF_TIME_ELAPSED(par) ((void)0)
#define PF_TIME_ELAPSED_GET() ((LONG)0)
#define PF_TIMER_ADD_BYTES(idx, n) ((void)0)
#define PF_TIMER_INC(idx) ((void)0)
#define PF_TIMER_ADD_COUNT(idx, n) ((void)0)
#define PF_TIMER_BEGIN_IF(P, cond) ((void)0)
#define PF_TIMER_END_IF(P) ((void)0)

#endif /* PF_PROFILE */

#endif /* __PF_PROFILE_H__ */
