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
	PF_PHASE_RED_RECV_WAIT,        /* reducer: PF_WaitAnyRbuf in PF_StoreBuffer */
	PF_PHASE_RED_MERGE_PATCHES,    /* reducer: MergePatches calls */
	PF_PHASE_RED_FINAL_SORT,       /* reducer: EndSort in PF_ForwardTermsToMaster */
	PF_PHASE_RED_FORWARD_WAIT,     /* reducer: MPI_Wait when sending to master */
	PF_PHASE_RED_FORWARD_MPI,      /* reducer: MPI_Isend to master */
	PF_PHASE_MAS_DISTRIBUTE,       /* master: term-distribution loop */
	PF_PHASE_MAS_DISTRIBUTE_WAIT,  /* master: PF_Wait4Slave */
	PF_PHASE_MAS_FINAL_SORT,       /* master: EndSort merge tree */
	PF_PHASE_MAS_COLLECT,          /* master: PF_LongSingleReceive loop */
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
	PF_OS_COUNT
};

/* Extra per-rank counters tracked outside the phase timers. */
enum {
	PF_EX_BYTES_SENT = 0,        /* mapper: total bytes shipped to reducers */
	PF_EX_BYTES_TO_MASTER,       /* reducer: total bytes shipped to master */
	PF_EX_TERMS_SENT,            /* reserved (filled from existing PF_linterms in CSV) */
	PF_EX_PATCHES_BUILT,         /* reducer: number of MergePatches calls */
	PF_EX_WALLCLOCK_US,          /* per-rank wallclock for the module */
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
} PF_OSCounters;

/* Per-rank profile slots (sent via PF_LongSinglePack; received and stored
   in pf_profile_stats[rank] on the master). */
typedef struct {
	LONG phase_us[PF_PHASE_COUNT];
	LONG os_diff[PF_OS_COUNT];
	LONG extras[PF_EX_COUNT];
} PF_ProfileSlot;

/* Globals: filled per-module by each rank, then packed and sent to master. */
extern LONG pf_phase_us[PF_PHASE_COUNT];
extern LONG pf_os_diff[PF_OS_COUNT];
extern LONG pf_extras[PF_EX_COUNT];

/* Master-only: per-rank receive buffer, allocated lazily on first dump. */
extern PF_ProfileSlot *pf_profile_stats;

/* Macros. Always evaluate cheaply; MPI_Wtime() is ~30 ns on Linux. */
#define PF_TIMER_BEGIN(P) double _pf_t_##P = MPI_Wtime()
#define PF_TIMER_END(P) \
	do { \
		pf_phase_us[PF_PHASE_##P] += (LONG)((MPI_Wtime() - _pf_t_##P) * 1.0e6); \
	} while (0)
/* Runtime-indexed timer: phase ID resolved at runtime (e.g. PF_ISendSbuf
   chooses MAP_SEND_* vs RED_FORWARD_* based on caller's role). */
#define PF_TIMER_BEGIN_RT(name) double _pf_t_rt_##name = MPI_Wtime()
#define PF_TIMER_END_RT(name, idx) \
	do { \
		if ((idx) >= 0) pf_phase_us[(idx)] += (LONG)((MPI_Wtime() - _pf_t_rt_##name) * 1.0e6); \
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
