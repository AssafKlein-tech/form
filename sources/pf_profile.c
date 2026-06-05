/** @file pf_profile.c
 *
 *  MRmpi per-phase profiling implementation.
 *
 *  All bodies are inside #ifdef PF_PROFILE; in production builds without
 *  the flag this translation unit compiles to nothing.
 */

#ifdef PF_PROFILE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/resource.h>
#include <mpi.h>

#include "form3.h"
#include "pf_profile.h"

/* Per-rank globals filled by the timer macros. phase_first_us / phase_last_us
   are wall-clock offsets from pf_module_t0 in microseconds; -1 means the phase
   never fired on this rank for the current module. */
LONG pf_phase_us[PF_PHASE_COUNT];
LONG pf_phase_first_us[PF_PHASE_COUNT];
LONG pf_phase_last_us[PF_PHASE_COUNT];
LONG pf_os_diff[PF_OS_COUNT];
LONG pf_extras[PF_EX_COUNT];

/* Module-start time reference; written by parallel.c at module entry. */
double pf_module_t0 = 0.0;
int pf_module_smrflag = 0;

/* Master-only receive buffer for per-rank profile slots. */
PF_ProfileSlot *pf_profile_stats = NULL;

/* Master-only Compare1 K-prefix first-diff histogram. Populated by sort.c's
   Compare1 when pf_compare_kcap>0; reset per-module here; dumped to a
   side CSV in pf_profile_dump_master_csv. */
LONG pf_compare1_diff_hist[PF_COMPARE1_HIST_BINS];
static FILE *s_hist_csv = NULL;
static int s_hist_csv_header_written = 0;

/* Node-leader detection state. Set by pf_profile_per_node_init(). */
static int   s_node_leader_inited = 0;
static int   s_is_node_leader = 0;
static dev_t s_formtmp_dev = 0;
static unsigned int s_formtmp_major = 0;
static unsigned int s_formtmp_minor = 0;
static int   s_diskstats_unavailable = 0;

/* IB NIC counter state. Resolved from UCX_NET_DEVICES at init. The two
   paths point at /sys/class/infiniband/<dev>/ports/<port>/counters/<name>;
   the values there are in 4-byte units per the IB spec, multiplied by 4
   when read so callers see bytes. We prefer port_xmit_data_extended /
   port_rcv_data_extended (64-bit, ConnectX-5+) and fall back to the
   32-bit names when only those are present, with wrap detection in
   the diff. */
static int   s_ib_unavailable = 0;
static int   s_ib_counter_extended = 0;
static char  s_ib_xmit_path[256] = {0};
static char  s_ib_rcv_path[256]  = {0};

/* CSV state: written once per run. */
static FILE *s_csv = NULL;
static int   s_csv_header_written = 0;

void pf_profile_reset_module(void)
{
	memset(pf_phase_us, 0, sizeof(pf_phase_us));
	memset(pf_os_diff, 0, sizeof(pf_os_diff));
	memset(pf_extras, 0, sizeof(pf_extras));
	memset(pf_compare1_diff_hist, 0, sizeof(pf_compare1_diff_hist));
	for ( int i = 0; i < PF_PHASE_COUNT; i++ ) {
		pf_phase_first_us[i] = -1;
		pf_phase_last_us[i]  = -1;
	}
}

/*
	Read /proc/<pid>/diskstats field 13 (time_in_io_ms) for the FORMTMP
	device. Returns -1 if the device cannot be found (e.g. tmpfs) or if
	per-node init was never run.
*/
static LONG read_node_disk_time_in_io_ms(void)
{
	if ( !s_is_node_leader || s_diskstats_unavailable ) return -1;
	FILE *fp = fopen("/proc/diskstats", "r");
	if ( !fp ) { s_diskstats_unavailable = 1; return -1; }
	char line[512];
	LONG result = -1;
	while ( fgets(line, sizeof(line), fp) ) {
		unsigned int maj = 0, min = 0;
		char name[64];
		unsigned long long rd_ios, rd_merges, rd_sec, rd_tms;
		unsigned long long wr_ios, wr_merges, wr_sec, wr_tms;
		unsigned long long ios_in_prog, time_in_io_ms;
		int n = sscanf(line, "%u %u %63s %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
			&maj, &min, name,
			&rd_ios, &rd_merges, &rd_sec, &rd_tms,
			&wr_ios, &wr_merges, &wr_sec, &wr_tms,
			&ios_in_prog, &time_in_io_ms);
		if ( n < 13 ) continue;
		if ( maj == s_formtmp_major && min == s_formtmp_minor ) {
			result = (LONG)time_in_io_ms;
			break;
		}
	}
	fclose(fp);
	return result;
}

/*
	Read an IB sysfs counter file. Returns the raw counter value
	multiplied by 4 (IB spec: counters are in units of 4 bytes), or -1
	on failure / non-leader.
*/
static LONG read_ib_counter(const char *path)
{
	if ( !s_is_node_leader || s_ib_unavailable || !path[0] ) return -1;
	FILE *fp = fopen(path, "r");
	if ( !fp ) return -1;
	unsigned long long v = 0;
	int n = fscanf(fp, "%llu", &v);
	fclose(fp);
	if ( n != 1 ) return -1;
	return (LONG)(v * 4ULL);
}

void pf_profile_snapshot_os(PF_OSCounters *out)
{
	memset(out, 0, sizeof(*out));
	out->node_disk_time_in_io_ms = -1;
	out->node_wallclock_us = -1;
	out->node_nic_xmit_bytes = -1;
	out->node_nic_rcv_bytes  = -1;

	FILE *fp = fopen("/proc/self/io", "r");
	if ( fp ) {
		char key[64];
		long long val;
		while ( fscanf(fp, "%63[^:]: %lld\n", key, &val) == 2 ) {
			if      ( !strcmp(key, "rchar") )       out->io_rchar       = (LONG)val;
			else if ( !strcmp(key, "wchar") )       out->io_wchar       = (LONG)val;
			else if ( !strcmp(key, "read_bytes") )  out->io_read_bytes  = (LONG)val;
			else if ( !strcmp(key, "write_bytes") ) out->io_write_bytes = (LONG)val;
			else if ( !strcmp(key, "syscr") )       out->io_syscr       = (LONG)val;
			else if ( !strcmp(key, "syscw") )       out->io_syscw       = (LONG)val;
		}
		fclose(fp);
	}

	struct rusage ru;
	if ( getrusage(RUSAGE_SELF, &ru) == 0 ) {
		out->ru_maxrss = (LONG)ru.ru_maxrss;
		out->ru_minflt = (LONG)ru.ru_minflt;
		out->ru_majflt = (LONG)ru.ru_majflt;
		out->ru_nvcsw  = (LONG)ru.ru_nvcsw;
		out->ru_nivcsw = (LONG)ru.ru_nivcsw;
	}

	if ( s_is_node_leader ) {
		out->node_disk_time_in_io_ms = read_node_disk_time_in_io_ms();
		out->node_wallclock_us = (LONG)(MPI_Wtime() * 1.0e6);
		out->node_nic_xmit_bytes = read_ib_counter(s_ib_xmit_path);
		out->node_nic_rcv_bytes  = read_ib_counter(s_ib_rcv_path);
	}
}

void pf_profile_diff_os(const PF_OSCounters *start, const PF_OSCounters *end, LONG *out_diff)
{
	out_diff[PF_OS_IO_RCHAR]       = end->io_rchar       - start->io_rchar;
	out_diff[PF_OS_IO_WCHAR]       = end->io_wchar       - start->io_wchar;
	out_diff[PF_OS_IO_READ_BYTES]  = end->io_read_bytes  - start->io_read_bytes;
	out_diff[PF_OS_IO_WRITE_BYTES] = end->io_write_bytes - start->io_write_bytes;
	out_diff[PF_OS_IO_SYSCR]       = end->io_syscr       - start->io_syscr;
	out_diff[PF_OS_IO_SYSCW]       = end->io_syscw       - start->io_syscw;
	out_diff[PF_OS_RU_MAXRSS]      = end->ru_maxrss; /* report end value (HWM) */
	out_diff[PF_OS_RU_MINFLT]      = end->ru_minflt - start->ru_minflt;
	out_diff[PF_OS_RU_MAJFLT]      = end->ru_majflt - start->ru_majflt;
	out_diff[PF_OS_RU_NVCSW]       = end->ru_nvcsw  - start->ru_nvcsw;
	out_diff[PF_OS_RU_NIVCSW]      = end->ru_nivcsw - start->ru_nivcsw;
	if ( start->node_disk_time_in_io_ms < 0 || end->node_disk_time_in_io_ms < 0 ) {
		out_diff[PF_OS_NODE_DISK_TIME_IN_IO_MS] = -1;
		out_diff[PF_OS_NODE_WALLCLOCK_US] = -1;
	} else {
		out_diff[PF_OS_NODE_DISK_TIME_IN_IO_MS] =
			end->node_disk_time_in_io_ms - start->node_disk_time_in_io_ms;
		out_diff[PF_OS_NODE_WALLCLOCK_US] =
			end->node_wallclock_us - start->node_wallclock_us;
	}
	if ( start->node_nic_xmit_bytes < 0 || end->node_nic_xmit_bytes < 0
	     || end->node_nic_xmit_bytes < start->node_nic_xmit_bytes ) {
		/* negative delta on the 32-bit-counter fallback signals wrap; emit -1 */
		out_diff[PF_OS_NODE_NIC_XMIT_BYTES] = -1;
	} else {
		out_diff[PF_OS_NODE_NIC_XMIT_BYTES] =
			end->node_nic_xmit_bytes - start->node_nic_xmit_bytes;
	}
	if ( start->node_nic_rcv_bytes < 0 || end->node_nic_rcv_bytes < 0
	     || end->node_nic_rcv_bytes < start->node_nic_rcv_bytes ) {
		out_diff[PF_OS_NODE_NIC_RCV_BYTES] = -1;
	} else {
		out_diff[PF_OS_NODE_NIC_RCV_BYTES] =
			end->node_nic_rcv_bytes - start->node_nic_rcv_bytes;
	}
}

int pf_profile_per_node_init(void)
{
	if ( s_node_leader_inited ) return 0;
	s_node_leader_inited = 1;

	MPI_Comm node_comm;
	int local_rank = 0;
	if ( MPI_Comm_split_type(MPI_COMM_WORLD, MPI_COMM_TYPE_SHARED, 0,
	                         MPI_INFO_NULL, &node_comm) == MPI_SUCCESS ) {
		MPI_Comm_rank(node_comm, &local_rank);
		MPI_Comm_free(&node_comm);
	}
	s_is_node_leader = (local_rank == 0);

	if ( s_is_node_leader ) {
		/*
			Resolve the dir FORM is actually writing scratch to. ReserveTempFiles
			(startup.c:740) has already run by the time PF_Processor enters, so
			AM.TempSortDir / AM.TempDir reflect the full priority chain:
			-T flag, form.set tempdir/tempsortdir, FORMTMPSORT, FORMTMP, ".".
			Stat'ing whichever of these is populated guarantees we measure the
			same filesystem the sort I/O is hitting, regardless of how the
			operator configured the path.
		*/
		const char *path = NULL;
		if ( AM.TempSortDir && *AM.TempSortDir ) path = (const char *)AM.TempSortDir;
		else if ( AM.TempDir   && *AM.TempDir   ) path = (const char *)AM.TempDir;
		else {
			path = getenv("FORMTMPSORT");
			if ( !path || !*path ) path = getenv("FORMTMP");
			if ( !path || !*path ) path = ".";
		}
		struct stat st;
		if ( stat(path, &st) == 0 ) {
			s_formtmp_dev = st.st_dev;
			s_formtmp_major = major(st.st_dev);
			s_formtmp_minor = minor(st.st_dev);
			fprintf(stderr, "[%d] pf_profile: tracking disk %u:%u (%s)\n",
			        PF.me, s_formtmp_major, s_formtmp_minor, path);
			fflush(stderr);
		} else {
			s_diskstats_unavailable = 1;
			fprintf(stderr, "[%d] pf_profile: stat(%s) failed; disk %%util disabled\n",
			        PF.me, path);
			fflush(stderr);
		}

		/*
			Resolve the IB NIC counter paths from UCX_NET_DEVICES. Format
			is "<dev>:<port>" (e.g. "mlx5_0:1"); the part after the colon
			is optional and defaults to port 1. Read-only sampling at
			module boundaries -- no resets, no perturbation of co-tenant
			traffic. See feedback memory feedback_concurrent_profile_runs.
		*/
		const char *ucxdev = getenv("UCX_NET_DEVICES");
		if ( !ucxdev || !*ucxdev ) {
			s_ib_unavailable = 1;
			fprintf(stderr, "[%d] pf_profile: NIC tracking disabled "
			        "(UCX_NET_DEVICES not set)\n", PF.me);
			fflush(stderr);
		} else {
			char dev[128]; dev[0] = '\0';
			int port = 1;
			const char *colon = strchr(ucxdev, ':');
			size_t devlen = colon ? (size_t)(colon - ucxdev) : strlen(ucxdev);
			if ( devlen >= sizeof(dev) ) devlen = sizeof(dev) - 1;
			memcpy(dev, ucxdev, devlen);
			dev[devlen] = '\0';
			if ( colon && *(colon + 1) ) {
				int p = atoi(colon + 1);
				if ( p > 0 ) port = p;
			}
			char probe[256];
			snprintf(probe, sizeof(probe),
			         "/sys/class/infiniband/%s/ports/%d/counters/port_xmit_data_extended",
			         dev, port);
			struct stat pst;
			s_ib_counter_extended = (stat(probe, &pst) == 0);
			const char *xmit_name = s_ib_counter_extended
				? "port_xmit_data_extended" : "port_xmit_data";
			const char *rcv_name = s_ib_counter_extended
				? "port_rcv_data_extended" : "port_rcv_data";
			snprintf(s_ib_xmit_path, sizeof(s_ib_xmit_path),
			         "/sys/class/infiniband/%s/ports/%d/counters/%s",
			         dev, port, xmit_name);
			snprintf(s_ib_rcv_path, sizeof(s_ib_rcv_path),
			         "/sys/class/infiniband/%s/ports/%d/counters/%s",
			         dev, port, rcv_name);
			if ( stat(s_ib_xmit_path, &pst) != 0 ) {
				s_ib_unavailable = 1;
				fprintf(stderr, "[%d] pf_profile: NIC tracking disabled "
				        "(%s missing)\n", PF.me, s_ib_xmit_path);
				fflush(stderr);
			} else {
				fprintf(stderr, "[%d] pf_profile: tracking NIC %s:%d (%s)\n",
				        PF.me, dev, port, xmit_name);
				fflush(stderr);
			}
		}
	}
	return 0;
}

int pf_profile_is_node_leader(void)
{
	return s_is_node_leader;
}

void pf_profile_alloc_master(int numtasks)
{
	if ( pf_profile_stats != NULL ) return;
	pf_profile_stats = (PF_ProfileSlot *)calloc((size_t)numtasks, sizeof(PF_ProfileSlot));
}

static void write_csv_header(FILE *fp)
{
	fprintf(fp,
		"timestamp,module,expr,rank,role,nummappers,numreducers,nummergers,wallclock_us,"
		"t_map_generator_us,t_map_endsort_total_us,t_map_send_wait_us,t_map_send_mpi_us,t_map_getterm_wait_us,"
		"t_red_recv_wait_us,t_red_buffer_copy_us,t_red_merge_patches_us,t_red_final_sort_us,t_red_forward_wait_us,t_red_forward_mpi_us,"
		"t_mas_distribute_us,t_mas_distribute_wait_us,t_mas_final_sort_us,t_mas_collect_us,t_mas_merge_recv_wait_us,"
		"t_mer_merge_us,t_mer_recv_wait_us,t_mer_forward_wait_us,t_mer_forward_mpi_us,"
		/* Mapper-attack instrumentation: split the previously uncounted 14% gap
		   in the per-term hot path. See plan i-want-to-attack-dazzling-gizmo.md. */
		"t_map_small_flush_total_us,t_map_splitmerge_us,t_map_compress_batch_us,"
		"t_map_hash_route_us,t_map_delta_compress_us,t_map_sbuf_copy_us,"
		"t_map_testsub_us,t_map_normalize_us,t_map_preppoly_us,t_map_storeterm_us,"
		"t_red_store_total_us,"
		"t_map_testmatch_us,t_map_testsub_postmatch_us,t_map_polyfunmul_us,"
		"t_map_takeidfunction_us,t_map_putbracket_us,"
		"bytes_sent,bytes_to_master,terms_sent,patches_built,buffers_received,merge_lbuffer_full,merge_max_patches,bytes_mer_to_master,"
		"map_sbuf_flushes,map_bytes_precompress,map_bytes_postcompress,map_bytes_shuffled,"
		"map_terms_in,map_norm_clean_in,map_norm_changed,map_testsub_prev_rule_hit,map_testsub_no_match,"
		"red_bytes_received,"
		"io_rchar,io_wchar,io_read_bytes,io_write_bytes,io_syscr,io_syscw,"
		"maxrss_kb,minflt,majflt,nvcsw,nivcsw,"
		"node_disk_time_in_io_ms,node_wallclock_us,"
		"node_nic_xmit_bytes,node_nic_rcv_bytes,"
		/* Per-phase Gantt timestamps (us from module start; -1 = phase didn't fire). */
		"t_map_generator_first_us,t_map_generator_last_us,"
		"t_map_endsort_total_first_us,t_map_endsort_total_last_us,"
		"t_map_send_wait_first_us,t_map_send_wait_last_us,"
		"t_map_send_mpi_first_us,t_map_send_mpi_last_us,"
		"t_map_getterm_wait_first_us,t_map_getterm_wait_last_us,"
		"t_red_recv_wait_first_us,t_red_recv_wait_last_us,"
		"t_red_buffer_copy_first_us,t_red_buffer_copy_last_us,"
		"t_red_merge_patches_first_us,t_red_merge_patches_last_us,"
		"t_red_final_sort_first_us,t_red_final_sort_last_us,"
		"t_red_forward_wait_first_us,t_red_forward_wait_last_us,"
		"t_red_forward_mpi_first_us,t_red_forward_mpi_last_us,"
		"t_mas_distribute_first_us,t_mas_distribute_last_us,"
		"t_mas_distribute_wait_first_us,t_mas_distribute_wait_last_us,"
		"t_mas_final_sort_first_us,t_mas_final_sort_last_us,"
		"t_mas_collect_first_us,t_mas_collect_last_us,"
		"t_mas_merge_recv_wait_first_us,t_mas_merge_recv_wait_last_us,"
		"t_mer_merge_first_us,t_mer_merge_last_us,"
		"t_mer_recv_wait_first_us,t_mer_recv_wait_last_us,"
		"t_mer_forward_wait_first_us,t_mer_forward_wait_last_us,"
		"t_mer_forward_mpi_first_us,t_mer_forward_mpi_last_us,"
		"t_map_small_flush_total_first_us,t_map_small_flush_total_last_us,"
		"t_map_splitmerge_first_us,t_map_splitmerge_last_us,"
		"t_map_compress_batch_first_us,t_map_compress_batch_last_us,"
		"t_map_hash_route_first_us,t_map_hash_route_last_us,"
		"t_map_delta_compress_first_us,t_map_delta_compress_last_us,"
		"t_map_sbuf_copy_first_us,t_map_sbuf_copy_last_us,"
		"t_map_testsub_first_us,t_map_testsub_last_us,"
		"t_map_normalize_first_us,t_map_normalize_last_us,"
		"t_map_preppoly_first_us,t_map_preppoly_last_us,"
		"t_map_storeterm_first_us,t_map_storeterm_last_us,"
		"t_red_store_total_first_us,t_red_store_total_last_us,"
		"t_map_testmatch_first_us,t_map_testmatch_last_us,"
		"t_map_testsub_postmatch_first_us,t_map_testsub_postmatch_last_us,"
		"t_map_polyfunmul_first_us,t_map_polyfunmul_last_us,"
		"t_map_takeidfunction_first_us,t_map_takeidfunction_last_us,"
		"t_map_putbracket_first_us,t_map_putbracket_last_us,"
		/* master-bypass merger-tier phases + counters + per-module chain state */
		"smrflag,"
		"t_mer_distribute_us,t_mer_distribute_wait_us,t_mas_gather_us,t_mer_gather_us,t_mas_mergerdone_us,"
		"mer_distribute_terms,mer_partition_bytes,"
		"t_mer_distribute_first_us,t_mer_distribute_last_us,"
		"t_mer_distribute_wait_first_us,t_mer_distribute_wait_last_us,"
		"t_mas_gather_first_us,t_mas_gather_last_us,"
		"t_mer_gather_first_us,t_mer_gather_last_us,"
		"t_mas_mergerdone_first_us,t_mas_mergerdone_last_us\n");
}

static const char *role_name(int rank, int nummappers, int nummergers)
{
	if ( rank == 0 ) return "master";
	/* Mapper-mergers are mapper ranks 1..nummergers; they overlay a second
	   merge role after their mapper phase. Tag them "merger" so the
	   visualization can profile the merge tier separately. */
	if ( nummergers > 0 && rank >= 1 && rank <= nummergers ) return "merger";
	return (rank < nummappers) ? "mapper" : "reducer";
}

static void write_csv_row(FILE *fp, time_t ts, int module_num, const char *expr_name,
                          int rank, int nummappers, int numreducers,
                          const PF_ProfileSlot *slot)
{
	const LONG *p  = slot->phase_us;
	const LONG *pf = slot->phase_first_us;
	const LONG *pl = slot->phase_last_us;
	const LONG *os = slot->os_diff;
	const LONG *ex = slot->extras;
	fprintf(fp,
		"%lld,%d,%s,%d,%s,%d,%d,%d,%lld,"
		"%lld,%lld,%lld,%lld,%lld,"
		"%lld,%lld,%lld,%lld,%lld,%lld,"
		"%lld,%lld,%lld,%lld,%lld,"
		"%lld,%lld,%lld,%lld,"
		/* mapper-attack timers (11 phase-1 + 5 phase-2 = 16 new phases) */
		"%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,"
		"%lld,%lld,%lld,%lld,%lld,"
		"%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,"
		/* mapper-attack counters (10 new) */
		"%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,"
		"%lld,%lld,%lld,%lld,%lld,%lld,"
		"%lld,%lld,%lld,%lld,%lld,"
		"%lld,%lld,%lld,%lld,"
		/* Gantt timestamps (20 phases x first,last). */
		"%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,"
		"%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,"
		"%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,"
		"%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,"
		/* Gantt timestamps for 11 phase-1 + 5 phase-2 = 16 new phases x first,last */
		"%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,"
		"%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,"
		"%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,"
		/* master-bypass: smrflag, 5 phase totals + 2 counters, 5 first/last pairs */
		"%d,%lld,%lld,%lld,%lld,%lld,%lld,%lld,"
		"%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld\n",
		(long long)ts, module_num, expr_name ? expr_name : "",
		rank, role_name(rank, nummappers, PF.nummergers),
		nummappers, numreducers, PF.nummergers,
		(long long)ex[PF_EX_WALLCLOCK_US],
		(long long)p[PF_PHASE_MAP_GENERATOR],
		(long long)p[PF_PHASE_MAP_ENDSORT_TOTAL],
		(long long)p[PF_PHASE_MAP_SEND_WAIT],
		(long long)p[PF_PHASE_MAP_SEND_MPI],
		(long long)p[PF_PHASE_MAP_GETTERM_WAIT],
		(long long)p[PF_PHASE_RED_RECV_WAIT],
		(long long)p[PF_PHASE_RED_BUFFER_COPY],
		(long long)p[PF_PHASE_RED_MERGE_PATCHES],
		(long long)p[PF_PHASE_RED_FINAL_SORT],
		(long long)p[PF_PHASE_RED_FORWARD_WAIT],
		(long long)p[PF_PHASE_RED_FORWARD_MPI],
		(long long)p[PF_PHASE_MAS_DISTRIBUTE],
		(long long)p[PF_PHASE_MAS_DISTRIBUTE_WAIT],
		(long long)p[PF_PHASE_MAS_FINAL_SORT],
		(long long)p[PF_PHASE_MAS_COLLECT],
		(long long)p[PF_PHASE_MAS_MERGE_RECV_WAIT],
		(long long)p[PF_PHASE_MER_MERGE],
		(long long)p[PF_PHASE_MER_RECV_WAIT],
		(long long)p[PF_PHASE_MER_FORWARD_WAIT],
		(long long)p[PF_PHASE_MER_FORWARD_MPI],
		(long long)p[PF_PHASE_MAP_SMALL_FLUSH_TOTAL],
		(long long)p[PF_PHASE_MAP_SPLITMERGE],
		(long long)p[PF_PHASE_MAP_COMPRESS_BATCH],
		(long long)p[PF_PHASE_MAP_HASH_ROUTE],
		(long long)p[PF_PHASE_MAP_DELTA_COMPRESS],
		(long long)p[PF_PHASE_MAP_SBUF_COPY],
		(long long)p[PF_PHASE_MAP_TESTSUB],
		(long long)p[PF_PHASE_MAP_NORMALIZE],
		(long long)p[PF_PHASE_MAP_PREPPOLY],
		(long long)p[PF_PHASE_MAP_STORETERM],
		(long long)p[PF_PHASE_RED_STORE_TOTAL],
		(long long)p[PF_PHASE_MAP_TESTMATCH],
		(long long)p[PF_PHASE_MAP_TESTSUB_POSTMATCH],
		(long long)p[PF_PHASE_MAP_POLYFUNMUL],
		(long long)p[PF_PHASE_MAP_TAKEIDFUNCTION],
		(long long)p[PF_PHASE_MAP_PUTBRACKET],
		(long long)ex[PF_EX_BYTES_SENT],
		(long long)ex[PF_EX_BYTES_TO_MASTER],
		(long long)ex[PF_EX_TERMS_SENT],
		(long long)ex[PF_EX_PATCHES_BUILT],
		(long long)ex[PF_EX_BUFFERS_RECEIVED],
		(long long)ex[PF_EX_MERGE_LBUFFER_FULL],
		(long long)ex[PF_EX_MERGE_MAX_PATCHES],
		(long long)ex[PF_EX_BYTES_MER_TO_MASTER],
		(long long)ex[PF_EX_MAP_SBUF_FLUSHES],
		(long long)ex[PF_EX_MAP_BYTES_PRECOMPRESS],
		(long long)ex[PF_EX_MAP_BYTES_POSTCOMPRESS],
		(long long)ex[PF_EX_MAP_BYTES_SHUFFLED],
		(long long)ex[PF_EX_MAP_TERMS_IN],
		(long long)ex[PF_EX_MAP_NORM_CLEAN_IN],
		(long long)ex[PF_EX_MAP_NORM_CHANGED],
		(long long)ex[PF_EX_MAP_TESTSUB_PREV_RULE_HIT],
		(long long)ex[PF_EX_MAP_TESTSUB_NO_MATCH],
		(long long)ex[PF_EX_RED_BYTES_RECEIVED],
		(long long)os[PF_OS_IO_RCHAR],
		(long long)os[PF_OS_IO_WCHAR],
		(long long)os[PF_OS_IO_READ_BYTES],
		(long long)os[PF_OS_IO_WRITE_BYTES],
		(long long)os[PF_OS_IO_SYSCR],
		(long long)os[PF_OS_IO_SYSCW],
		(long long)os[PF_OS_RU_MAXRSS],
		(long long)os[PF_OS_RU_MINFLT],
		(long long)os[PF_OS_RU_MAJFLT],
		(long long)os[PF_OS_RU_NVCSW],
		(long long)os[PF_OS_RU_NIVCSW],
		(long long)os[PF_OS_NODE_DISK_TIME_IN_IO_MS],
		(long long)os[PF_OS_NODE_WALLCLOCK_US],
		(long long)os[PF_OS_NODE_NIC_XMIT_BYTES],
		(long long)os[PF_OS_NODE_NIC_RCV_BYTES],
		(long long)pf[PF_PHASE_MAP_GENERATOR],      (long long)pl[PF_PHASE_MAP_GENERATOR],
		(long long)pf[PF_PHASE_MAP_ENDSORT_TOTAL],  (long long)pl[PF_PHASE_MAP_ENDSORT_TOTAL],
		(long long)pf[PF_PHASE_MAP_SEND_WAIT],      (long long)pl[PF_PHASE_MAP_SEND_WAIT],
		(long long)pf[PF_PHASE_MAP_SEND_MPI],       (long long)pl[PF_PHASE_MAP_SEND_MPI],
		(long long)pf[PF_PHASE_MAP_GETTERM_WAIT],   (long long)pl[PF_PHASE_MAP_GETTERM_WAIT],
		(long long)pf[PF_PHASE_RED_RECV_WAIT],      (long long)pl[PF_PHASE_RED_RECV_WAIT],
		(long long)pf[PF_PHASE_RED_BUFFER_COPY],    (long long)pl[PF_PHASE_RED_BUFFER_COPY],
		(long long)pf[PF_PHASE_RED_MERGE_PATCHES],  (long long)pl[PF_PHASE_RED_MERGE_PATCHES],
		(long long)pf[PF_PHASE_RED_FINAL_SORT],     (long long)pl[PF_PHASE_RED_FINAL_SORT],
		(long long)pf[PF_PHASE_RED_FORWARD_WAIT],   (long long)pl[PF_PHASE_RED_FORWARD_WAIT],
		(long long)pf[PF_PHASE_RED_FORWARD_MPI],    (long long)pl[PF_PHASE_RED_FORWARD_MPI],
		(long long)pf[PF_PHASE_MAS_DISTRIBUTE],     (long long)pl[PF_PHASE_MAS_DISTRIBUTE],
		(long long)pf[PF_PHASE_MAS_DISTRIBUTE_WAIT],(long long)pl[PF_PHASE_MAS_DISTRIBUTE_WAIT],
		(long long)pf[PF_PHASE_MAS_FINAL_SORT],         (long long)pl[PF_PHASE_MAS_FINAL_SORT],
		(long long)pf[PF_PHASE_MAS_COLLECT],            (long long)pl[PF_PHASE_MAS_COLLECT],
		(long long)pf[PF_PHASE_MAS_MERGE_RECV_WAIT],    (long long)pl[PF_PHASE_MAS_MERGE_RECV_WAIT],
		(long long)pf[PF_PHASE_MER_MERGE],          (long long)pl[PF_PHASE_MER_MERGE],
		(long long)pf[PF_PHASE_MER_RECV_WAIT],      (long long)pl[PF_PHASE_MER_RECV_WAIT],
		(long long)pf[PF_PHASE_MER_FORWARD_WAIT],   (long long)pl[PF_PHASE_MER_FORWARD_WAIT],
		(long long)pf[PF_PHASE_MER_FORWARD_MPI],    (long long)pl[PF_PHASE_MER_FORWARD_MPI],
		(long long)pf[PF_PHASE_MAP_SMALL_FLUSH_TOTAL], (long long)pl[PF_PHASE_MAP_SMALL_FLUSH_TOTAL],
		(long long)pf[PF_PHASE_MAP_SPLITMERGE],        (long long)pl[PF_PHASE_MAP_SPLITMERGE],
		(long long)pf[PF_PHASE_MAP_COMPRESS_BATCH],    (long long)pl[PF_PHASE_MAP_COMPRESS_BATCH],
		(long long)pf[PF_PHASE_MAP_HASH_ROUTE],        (long long)pl[PF_PHASE_MAP_HASH_ROUTE],
		(long long)pf[PF_PHASE_MAP_DELTA_COMPRESS],    (long long)pl[PF_PHASE_MAP_DELTA_COMPRESS],
		(long long)pf[PF_PHASE_MAP_SBUF_COPY],         (long long)pl[PF_PHASE_MAP_SBUF_COPY],
		(long long)pf[PF_PHASE_MAP_TESTSUB],           (long long)pl[PF_PHASE_MAP_TESTSUB],
		(long long)pf[PF_PHASE_MAP_NORMALIZE],         (long long)pl[PF_PHASE_MAP_NORMALIZE],
		(long long)pf[PF_PHASE_MAP_PREPPOLY],          (long long)pl[PF_PHASE_MAP_PREPPOLY],
		(long long)pf[PF_PHASE_MAP_STORETERM],         (long long)pl[PF_PHASE_MAP_STORETERM],
		(long long)pf[PF_PHASE_RED_STORE_TOTAL],       (long long)pl[PF_PHASE_RED_STORE_TOTAL],
		(long long)pf[PF_PHASE_MAP_TESTMATCH],         (long long)pl[PF_PHASE_MAP_TESTMATCH],
		(long long)pf[PF_PHASE_MAP_TESTSUB_POSTMATCH], (long long)pl[PF_PHASE_MAP_TESTSUB_POSTMATCH],
		(long long)pf[PF_PHASE_MAP_POLYFUNMUL],        (long long)pl[PF_PHASE_MAP_POLYFUNMUL],
		(long long)pf[PF_PHASE_MAP_TAKEIDFUNCTION],    (long long)pl[PF_PHASE_MAP_TAKEIDFUNCTION],
		(long long)pf[PF_PHASE_MAP_PUTBRACKET],        (long long)pl[PF_PHASE_MAP_PUTBRACKET],
		/* master-bypass: smrflag, 5 phase totals + 2 counters, 5 first/last pairs */
		pf_module_smrflag,
		(long long)p[PF_PHASE_MER_DISTRIBUTE],
		(long long)p[PF_PHASE_MER_DISTRIBUTE_WAIT],
		(long long)p[PF_PHASE_MAS_GATHER],
		(long long)p[PF_PHASE_MER_GATHER],
		(long long)p[PF_PHASE_MAS_MERGERDONE],
		(long long)ex[PF_EX_MER_DISTRIBUTE_TERMS],
		(long long)ex[PF_EX_MER_PARTITION_BYTES],
		(long long)pf[PF_PHASE_MER_DISTRIBUTE],      (long long)pl[PF_PHASE_MER_DISTRIBUTE],
		(long long)pf[PF_PHASE_MER_DISTRIBUTE_WAIT], (long long)pl[PF_PHASE_MER_DISTRIBUTE_WAIT],
		(long long)pf[PF_PHASE_MAS_GATHER],          (long long)pl[PF_PHASE_MAS_GATHER],
		(long long)pf[PF_PHASE_MER_GATHER],          (long long)pl[PF_PHASE_MER_GATHER],
		(long long)pf[PF_PHASE_MAS_MERGERDONE],      (long long)pl[PF_PHASE_MAS_MERGERDONE]);
}

void pf_profile_dump_master_csv(int module_num, const char *expr_name,
                                int nummappers, int numreducers,
                                int numtasks)
{
	if ( s_csv == NULL ) {
		const char *dir = getenv("PF_PROFILE_DIR");
		char path[1024];
		if ( dir && *dir ) snprintf(path, sizeof(path), "%s/pf_profile.csv", dir);
		else               snprintf(path, sizeof(path), "pf_profile.csv");
		struct stat st;
		int existed_nonempty = (stat(path, &st) == 0 && st.st_size > 0);
		s_csv = fopen(path, "a");
		if ( !s_csv ) return;
		s_csv_header_written = existed_nonempty;
	}
	if ( !s_csv_header_written ) {
		write_csv_header(s_csv);
		s_csv_header_written = 1;
	}

	time_t ts = time(NULL);
	for ( int rank = 0; rank < numtasks; rank++ ) {
		write_csv_row(s_csv, ts, module_num, expr_name,
		              rank, nummappers, numreducers,
		              &pf_profile_stats[rank]);
	}
	fflush(s_csv);

	/* Compare1 K-prefix first-diff histogram. Only nonzero on master and only
	   when sort.c's pf_compare_kcap>0 (MR master k-way merge). Skip the write
	   if the histogram is all zeros for this module (non-MR runs, or modules
	   where the master never entered the prefix branch). */
	{
		LONG total = 0;
		for ( int b = 0; b < PF_COMPARE1_HIST_BINS; b++ ) total += pf_compare1_diff_hist[b];
		if ( total > 0 ) {
			if ( s_hist_csv == NULL ) {
				const char *dir = getenv("PF_PROFILE_DIR");
				char path[1024];
				if ( dir && *dir ) snprintf(path, sizeof(path), "%s/pf_compare1_hist.csv", dir);
				else               snprintf(path, sizeof(path), "pf_compare1_hist.csv");
				struct stat st;
				int existed_nonempty = (stat(path, &st) == 0 && st.st_size > 0);
				s_hist_csv = fopen(path, "a");
				if ( s_hist_csv ) {
					s_hist_csv_header_written = existed_nonempty;
					if ( !s_hist_csv_header_written ) {
						fprintf(s_hist_csv, "timestamp,module,expr,bin,count\n");
						s_hist_csv_header_written = 1;
					}
				}
			}
			if ( s_hist_csv ) {
				for ( int b = 0; b < PF_COMPARE1_HIST_BINS; b++ ) {
					if ( pf_compare1_diff_hist[b] != 0 ) {
						fprintf(s_hist_csv, "%ld,%d,%s,%d,%lld\n",
						        (long)ts, module_num, expr_name, b,
						        (long long)pf_compare1_diff_hist[b]);
					}
				}
				fflush(s_hist_csv);
			}
		}
	}
}

#endif /* PF_PROFILE */
