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
	PF_PHASE_RED_RECV_WAIT,        /* reducer: PF_WaitAnyRbuf in PF_StoreBuffer */
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
   Timer macros. PF_TIMER_BEGIN(P) introduces a local _pf_t_##P at the call
   site and (on first entry within a module) records the phase's relative
   start time in pf_phase_first_us[P]. PF_TIMER_END(P) accumulates duration
   and updates pf_phase_last_us[P]. MPI_Wtime() ~30 ns on Linux.

   IMPORTANT: PF_TIMER_BEGIN expands to a declaration + a conditional update;
   it must appear at top-level inside a brace-delimited block, never as the
   body of an unbraced if/else/for. All current call sites comply.
*/
#define PF_TIMER_BEGIN(P) \
	double _pf_t_##P = MPI_Wtime(); \
	if (pf_phase_first_us[PF_PHASE_##P] < 0) \
		pf_phase_first_us[PF_PHASE_##P] = (LONG)((_pf_t_##P - pf_module_t0) * 1.0e6)
#define PF_TIMER_END(P) \
	do { \
		double _pf_now_##P = MPI_Wtime(); \
		pf_phase_us[PF_PHASE_##P] += (LONG)((_pf_now_##P - _pf_t_##P) * 1.0e6); \
		pf_phase_last_us[PF_PHASE_##P] = (LONG)((_pf_now_##P - pf_module_t0) * 1.0e6); \
	} while (0)
/* Runtime-indexed timer: phase ID resolved at runtime (e.g. PF_ISendSbuf
   chooses MAP_SEND_* vs RED_FORWARD_* based on caller's role). Both first
   and last are attributed at END time since the index isn't known at BEGIN. */
#define PF_TIMER_BEGIN_RT(name) double _pf_t_rt_##name = MPI_Wtime()
#define PF_TIMER_END_RT(name, idx) \
	do { \
		if ((idx) >= 0) { \
			double _pf_now_rt_##name = MPI_Wtime(); \
			pf_phase_us[(idx)] += (LONG)((_pf_now_rt_##name - _pf_t_rt_##name) * 1.0e6); \
			if (pf_phase_first_us[(idx)] < 0) \
				pf_phase_first_us[(idx)] = (LONG)((_pf_t_rt_##name - pf_module_t0) * 1.0e6); \
			pf_phase_last_us[(idx)] = (LONG)((_pf_now_rt_##name - pf_module_t0) * 1.0e6); \
		} \
	} while (0)
#define PF_TIMER_ADD_BYTES(idx, n) (pf_extras[(idx)] += (LONG)(n))
#define PF_TIMER_INC(idx) (pf_extras[(idx)]++)

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
#define PF_TIMER_ADD_BYTES(idx, n) ((void)0)
#define PF_TIMER_INC(idx) ((void)0)

#endif /* PF_PROFILE */

#endif /* __PF_PROFILE_H__ */
