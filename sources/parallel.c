/** @file parallel.c
 *
 *  Message passing library independent functions of parform
 *
 *  This file contains functions needed for the parallel version of form3
 *  these functions need no real link to the message passing libraries, they
 *  only need some interface dependent preprocessor definitions (check
 *  parallel.h). So there still need two different objectfiles to be compiled
 *  for mpi and pvm!
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
/*
  	#[ includes :
*/
#include "form3.h"
#include "vector.h"
#include "pf_profile.h"

/*
#define PF_DEBUG_BCAST_LONG
#define PF_DEBUG_BCAST_BUF
#define PF_DEBUG_BCAST_PREDOLLAR
#define PF_DEBUG_BCAST_RHSEXPR
#define PF_DEBUG_BCAST_DOLLAR
#define PF_DEBUG_BCAST_PREVAR
#define PF_DEBUG_BCAST_CBUF
#define PF_DEBUG_BCAST_EXPRFLAGS
#define PF_DEBUG_REDUCE_DOLLAR
*/

/* mpi.c */
LONG PF_RealTime(int);
int PF_LibInit(int*, char***);
int PF_LibTerminate(int);
int PF_Probe(int*);
int PF_RecvWbuf(WORD*,LONG*,int*);
int PF_IRecvRbuf(PF_BUFFER*,int,int);
int PF_WaitRbuf(PF_BUFFER *,int,LONG *);
int PF_RawSend(int dest, void *buf, LONG l, int tag);
LONG PF_RawRecv(int *src,void *buf,LONG thesize,int *tag);
int PF_RawProbe(int *src, int *tag, int *bytesize);

/* Private functions */

static int PF_WaitAllSlaves(void);

static void PF_PackRedefinedPreVars(void);
static void PF_UnpackRedefinedPreVars(void);

static int PF_Wait4MasterIP(int tag);
static int PF_DoOneExpr(void);
static int PF_ReadMaster(void);/*reads directly to its scratch!*/
static int PF_Slave2MasterIP(int src);/*both master and slave*/
static int PF_Master2SlaveIP(int dest, EXPRESSIONS e);
static int PF_WalkThrough(WORD *t, LONG l, LONG chunk, LONG *count);
static int PF_SendChunkIP(FILEHANDLE *curfile,  POSITION *position, int to, LONG thesize);
static int PF_RecvChunkIP(FILEHANDLE *curfile, int from, LONG thesize);

void PF_ReceiveErrorMessage(int src, int tag);
static void PF_CatchErrorMessages(int *src, int *tag);
static void PF_CatchErrorMessagesForAll(void);
static int PF_ProbeWithCatchingErrorMessages(int *src);

/* Variables */

PARALLELVARS PF;
#ifdef MPI2
 WORD *PF_shared_buff;
#endif

static LONG     PF_goutterms;  /* (master) Total out terms at PF_EndSort(), used in PF_Statistics(). */
static POSITION PF_exprsize;   /* (master) The size of the expression at PF_EndSort(), used in PF_Processor(). */

/*
	This will work well only under Linux, see
		#ifdef PF_WITH_SCHED_YIELD
	below in PF_WaitAllSlaves().
*/
#ifdef PF_WITH_SCHED_YIELD
 #include <sched.h>
#endif

#ifdef PF_WITHLOG
 #define PRINTFBUF(TEXT,TERM,SIZE)  { UBYTE lbuf[24]; if(PF.log){ WORD iii;\
  NumToStr(lbuf,AC.CModule); \
  fprintf(stderr,"[%d|%s] %s : ",PF.me,lbuf,(char*)TEXT);\
  if(TERM){ fprintf(stderr,"[%d] ",(int)(*TERM));\
    if((SIZE)<500 && (SIZE)>0) for(iii=1;iii<(SIZE);iii++)\
      fprintf(stderr,"%d ",TERM[iii]); }\
  fprintf(stderr,"\n");\
  fflush(stderr); } }
#else
 #define PRINTFBUF(TEXT,TERM,SIZE) {}
#endif

/**
 * Swaps the variables \a x and \a y. If sizeof(x) != sizeof(y) then a compilation error
 * will occur. A set of memcpy calls with constant sizes is expected to be inlined by the optimisation.
 */
#define SWAP(x, y) \
	do { \
		char swap_tmp__[sizeof(x) == sizeof(y) ? (int)sizeof(x) : -1]; \
		memcpy(swap_tmp__, &y, sizeof(x)); \
		memcpy(&y, &x, sizeof(x)); \
		memcpy(&x, swap_tmp__, sizeof(x)); \
	} while (0)

/**
 * Packs a LONG value \a n to a WORD buffer \a p.
 */
#define PACK_LONG(p, n) \
	do { \
		*(p)++ = (UWORD)((ULONG)(n) & (ULONG)WORDMASK); \
		*(p)++ = (UWORD)(((ULONG)(n) >> BITSINWORD) & (ULONG)WORDMASK); \
	} while (0)

/**
 * Unpacks a LONG value \a n from a WORD buffer \a p.
 */
#define UNPACK_LONG(p, n) \
	do { \
		(n) = (LONG)((((ULONG)(p)[1] & (ULONG)WORDMASK) << BITSINWORD) | ((ULONG)(p)[0] & (ULONG)WORDMASK)); \
		(p) += 2; \
	} while (0)

/**
 * A simple check for unrecoverable errors.
 */
#define CHECK(condition) _CHECK(condition, __FILE__, __LINE__)
#define _CHECK(condition, file, line) __CHECK(condition, file, line)
#define __CHECK(condition, file, line) \
	do { \
		if ( !(condition) ) { \
			Error0("Fatal error at " file ":" #line); \
			Terminate(-1); \
		} \
	} while (0)

/*
 * For debugging.
 */
#define DBGOUT(lv1, lv2, a) do { if ( lv1 >= lv2 ) { printf a; fflush(stdout); } } while (0)

/* (AN.ninterms of master) == max(AN.ninterms of slaves) == sum(PF_linterms of slaves) at EndSort(). */
#define DBGOUT_NINTERMS(lv, a)
/* #define DBGOUT_NINTERMS(lv, a) DBGOUT(1, lv, a) */

/* Upper bound on the K-prefix word count used by the master-side prefix-drain
   (AM.MR.HashPrefixWords clamped to this in PF_Init). K=1024 is huge for
   current routing (production uses K≈128); the clamp is just a sanity bound. */
#define PF_DRAIN_MAX_K 1024

/*
  	#] includes : 
  	#[ statistics :
 		#[ variables : (should be part of a struct?)
*/
static LONG PF_linterms;     /* local interms on this proces: PF_Proces */
#define PF_STATS_SIZE 7
static LONG **PF_stats = NULL;/* space for collecting statistics of all procs */
static LONG PF_laststat;     /* last realtime when statistics were printed */
static LONG PF_statsinterval;/* timeinterval for printing statistics */
int PF_shuffle_nocompress = 0;/* if nonzero, mappers skip per-reducer delta compression in lowmr_sort */
/*
 		#] variables : 
 		#[ PF_Statistics :
*/

/**
 * Prints statistics every PF_statinterval seconds.
 * For \a proc = 0 it prints final statistics for EndSort().
 *
 * @param  stats  the pointer to an array: LONG stats[proc][5] = {cpu,space,in,gen,left}.
 * @param  proc   the source process number.
 * @return        0 if OK, nonzero on error.
 */
static int PF_Statistics(LONG **stats, int proc)
{
	GETIDENTITY
	LONG real, cpu;
	WORD rpart, cpart;
	int i, j;

	if ( AT.SS == AM.S0 && PF.me == MASTER ) {
		real = PF_RealTime(PF_TIME); rpart = (WORD)(real%100); real /= 100;

		if ( PF_stats == NULL ) {
			PF_stats = (LONG**)Malloc1(PF.numtasks*sizeof(LONG*),"PF_stats 1");
			for ( i = 0; i < PF.numtasks; i++ ) {
				PF_stats[i] = (LONG*)Malloc1(PF_STATS_SIZE*sizeof(LONG),"PF_stats 2");
				for ( j = 0; j < PF_STATS_SIZE; j++ ) PF_stats[i][j] = 0;
			}
		}
		if ( proc > 0 ) for ( i = 0; i < PF_STATS_SIZE; i++ ) PF_stats[proc][i] = stats[0][i];

		if ( real >= PF_laststat + PF_statsinterval || proc == 0 ) {
			LONG sum[PF_STATS_SIZE];

			for ( i = 0; i < PF_STATS_SIZE; i++ ) sum[i] = 0;
			sum[0] = cpu = TimeCPU(1);
			cpart = (WORD)(cpu%1000);
			cpu /= 1000;
			cpart /= 10;
			if ( AC.OldParallelStats ) MesPrint("");
			if ( proc > 0 && AC.StatsFlag && AC.OldParallelStats ) {
				MesPrint("proc          CPU         in        gen       left       byte");
				MesPrint("%3d  : %7l.%2i %10l",0,cpu,cpart,AN.ninterms);
			}
			else if ( AC.StatsFlag && AC.OldParallelStats ) {
				MesPrint("proc          CPU         in        gen       out        byte");
				MesPrint("%3d  : %7l.%2i %10l %10l %10l",0,cpu,cpart,AN.ninterms,0,PF_goutterms);
			}

			for ( i = 1; i < PF.numtasks; i++ ) {
				cpart = (WORD)(PF_stats[i][0]%1000);
				cpu = PF_stats[i][0] / 1000;
				cpart /= 10;
				if ( AC.StatsFlag && AC.OldParallelStats )
					MesPrint("%3d  : %7l.%2i %10l %10l %10l",i,cpu,cpart,
							PF_stats[i][2],PF_stats[i][3],PF_stats[i][4]);
				for ( j = 0; j < PF_STATS_SIZE; j++ ) sum[j] += PF_stats[i][j];
			}
			cpart = (WORD)(sum[0]%1000);
			cpu = sum[0] / 1000;
			cpart /= 10;
			if ( AC.StatsFlag && AC.OldParallelStats ) {
				MesPrint("Sum  = %7l.%2i %10l %10l %10l",cpu,cpart,sum[2],sum[3],sum[4]);
				MesPrint("Real = %7l.%2i %20s (%l) %16s",
						real,rpart,AC.Commercial,AC.CModule,EXPRNAME(AR.CurExpr));
				MesPrint("");
			}
			PF_laststat = real;
		}
	}
	return(0);
}
/*
 		#] PF_Statistics : 
  	#] statistics : 
  	#[ sort.c :
 		#[ sort variables :
*/

/**
 * A node for the tree of losers in the final sorting on the master.
 */
typedef struct NoDe {
	struct NoDe *left;
	struct NoDe *rght;
	int lloser;
	int rloser;
	int lsrc;
	int rsrc;
} NODE;

/*
	should/could be put in one struct
*/
static  NODE *PF_root;			/* root of tree of losers */
static  WORD PF_loser;			/* this is the last loser */
static  WORD **PF_term;			/* these point to the active terms */
static  WORD **PF_newcpos;		/* new coeffs of merged terms */
static  WORD *PF_newclen;		/* length of new coefficients */

/* Stable K+1-word snapshot of the drained leaf's boundary anchor T_i, captured
   at drain ENTRY (before the scan's cross-chunk IRecv can overwrite T_i's rbuf
   slot). After the drain, PF_term[src] is repointed here so the next PF_PutIn
   decompresses the boundary term against valid bytes. See the drain block. */
static  WORD *PF_drain_lastterm = NULL;
static  int   PF_drain_lastterm_cap = 0;

/* K-cap value WANTED by the master's merge (set by PF_EndSort when drain
   is active). The actual sort.c global pf_compare_kcap is set transiently
   ONLY around the loser-tree CompareTerms call in PF_GetLoser, so the
   cap does NOT leak into other CompareTerms calls happening during the
   merge loop -- in particular PutOut->PutBracketInIndex's bracket-equality
   check (index.c:365) compares whole brackets, not just K-prefixes, so it
   must NOT see the cap. Otherwise the bracket index collapses different
   brackets sharing a K-prefix into one entry. */
static  WORD pf_compare_kcap_want = 0;

/*
	preliminary: could also write somewhere else?
*/

static  WORD *PF_WorkSpace;		/* used in PF_EndSort() */
static  UWORD *PF_ScratchSpace;	/* used in PF_GetLoser() */

/*
 		#] sort variables : 
 		#[ PF_AllocBuf :
*/

/**
 * Allocates one PF_BUFFER struct with \a nbuf cyclic buffers of size \a bsize.
 * For the first \a free buffers there is no space allocated.
 * For example, if \a free == 1 then for the first (index 0) buffer there is
 * no space allocated(!!!) (one can reuse existing space for it) and
 * actually buff[0], stop[0], fill[0] and full[0] in the returned
 * PF_BUFFER struct are undefined.
 *
 * @param  nbufs  the number of cyclic buffers for PF_BUFFER struct.
 * @param  bsize  the memory allocation size in bytes for each buffer.
 * @param  free   the number of the buffers without the memory allocation.
 * @return        the pointer to the PF_BUFFER struct if succeeded. NULL if failed.
 *
 * @todo Maybe this should be really hidden in the send/recv routines and pvm/mpi
 *       files, it is only complicated because of nonblocking send/receives!
 */
static PF_BUFFER *PF_AllocBuf(int nbufs, LONG bsize, WORD free)
{
	PF_BUFFER *buf;
	UBYTE *p, *stop;
	LONG allocsize;
	int i;

	allocsize = (LONG)(sizeof(PF_BUFFER) + 4*nbufs*sizeof(WORD*) + (nbufs-free)*bsize);

	allocsize +=
		(LONG)( nbufs * (  2 * sizeof(MPI_Status)
		                 +     sizeof(MPI_Request)
		                 +     sizeof(MPI_Datatype)
		                )  );
	allocsize += (LONG)( nbufs * 3 * sizeof(int) );

	if ( ( buf = (PF_BUFFER*)Malloc1(allocsize,"PF_AllocBuf") ) == NULL ) {
		MesPrint("ERROR: PF_AllocBuf failed to allocate %ld bytes for %d buffers", allocsize, nbufs);
		return(NULL);
	}

	p = ((UBYTE *)buf) + sizeof(PF_BUFFER);
	stop = ((UBYTE *)buf) + allocsize;

	buf->numbufs = nbufs;
	buf->active = 0;

	buf->buff    = (WORD**)p;		  p += buf->numbufs*sizeof(WORD*);
	buf->fill    = (WORD**)p;		  p += buf->numbufs*sizeof(WORD*);
	buf->full    = (WORD**)p;		  p += buf->numbufs*sizeof(WORD*);
	buf->stop    = (WORD**)p;		  p += buf->numbufs*sizeof(WORD*);
	buf->status  = (MPI_Status *)p;	  p += buf->numbufs*sizeof(MPI_Status);
	buf->retstat = (MPI_Status *)p;	  p += buf->numbufs*sizeof(MPI_Status);
	buf->request = (MPI_Request *)p;  p += buf->numbufs*sizeof(MPI_Request);
	buf->type    = (MPI_Datatype *)p; p += buf->numbufs*sizeof(MPI_Datatype);
	buf->index   = (int *)p;		  p += buf->numbufs*sizeof(int);

	for ( i = 0; i < buf->numbufs; i++ ) buf->request[i] = MPI_REQUEST_NULL;
	buf->tag     = (int *)p;		  p += buf->numbufs*sizeof(int);
	buf->from    = (int *)p;		  p += buf->numbufs*sizeof(int);
/*
		and finally the real bufferspace
*/
	for ( i = free; i < buf->numbufs; i++ ) {
		buf->buff[i] = (WORD*)p; p += bsize;
		buf->stop[i] = (WORD*)p;
		buf->fill[i] = buf->full[i] = buf->buff[i];
	}
	if ( p != stop ) {
		MesPrint("Error in PF_AllocBuf p = %x stop = %x\n",p,stop);
		return(NULL);
	}
	return(buf);
}

/*
 		#] PF_AllocBuf : 
 		#[ PF_InitTree :
*/

/**
 * Initializes the sorting tree on the master.
 * It allocates bufferspace (if necessary) for
 *   \li  pointers to terms in the tree and their coefficients
 *   \li  the cyclic receive buffers for nonblocking receives (PF.rbufs)
 *   \li  the nodes of the actual tree
 *
 * and initializes these with (hopefully) correct values.
 *
 * @return  the number of nodes in the merge tree if succeeded. -1 if failed.
*/
/* Map a tree-leaf index `src` to the MPI rank that feeds that leaf. Single
   source of truth for PF_InitTree, PF_PutIn, and master's drain re-arm. */
static int pf_loser_src_to_rank(int src)
{
	if ( AC.sMRflag != NO_MAPREDUCE && PF.is_merger && PF.merger_leaf_ranks )
		return PF.merger_leaf_ranks[src];        /* merger: leaf reducer */
	if ( AC.sMRflag != NO_MAPREDUCE && PF.me == MASTER && PF.nummergers > 0 ) {
		/* master's merger leaves: node-local placement scatters mergers
		   across nodes. merger_child_ranks ([0]=MASTER sentinel) carries the
		   ascending merger rank list; built in PF_Processor. */
		if ( PF.merger_child_ranks ) return PF.merger_child_ranks[src];
		return src;
	}
	if ( AC.sMRflag == NO_MAPREDUCE && PF.me == MASTER )
		return src;                              /* non-MR master: rank 1..N */
	return src % PF.numreducers + PF.nummappers; /* MR master, no mergers */
}

static int PF_InitTree(void)
{
	GETIDENTITY
	PF_BUFFER **rbuf = PF.rbufs;
	/* numtasks = number of LEAVES (sources) in this rank's loser tree, +1.
	   - non-MR master: PF.nummappers (all slaves)
	   - MR master, mergers active: PF.nummergers + 1
	   - MR master, no mergers: PF.numreducers + 1
	   - merger:                  PF.merger_groupsz + 1                  */
	int numtasks;
	if ( AC.sMRflag != NO_MAPREDUCE && PF.is_merger )
		numtasks = PF.merger_groupsz + 1;
	else if ( AC.sMRflag == NO_MAPREDUCE )
		numtasks = PF.nummappers;
	else if ( PF.nummergers > 0 )
		numtasks = PF.nummergers + 1;
	else
		numtasks = PF.numreducers + 1;
	/* PF.rbufs is allocated once and cached across modules; size the array
	   (and its PF_AllocBuf fill loop) to the largest numtasks this rank will
	   ever pass, NOT the current module's. The per-module working loops
	   below still use `numtasks`. Master: a non-MR module fans in from every
	   rank -> PF.numtasks is the max. Merger: merger_groupsz+1, constant
	   across all PF_MergerLoop calls, so its own numtasks is the max. */
	int alloc_numtasks = PF.is_merger ? numtasks : PF.numtasks;
	int workerIdx,numrbufs;
	int i, j, src, numnodes;
	int numslaves = numtasks - 1;
	LONG size;
/*
 		#[ the buffers : for the new coefficients and the terms
 		   we need one for each slave
*/
	if (PF_term == NULL && PF_allocatePFTerm(PF.numtasks)) {MesPrint("Error in InitTree"); return -1;}
/*
 		#] the buffers : 
 		#[ the receive buffers :
*/
	numrbufs = PF.numrbufs;
/*
		this is the size we have in the combined sortbufs for one slave
*/
	/* Use PF.shuffle_arena_words (mapper-config arena, identical on every rank)
	   so sender slot == receiver slot across all (mapper,reducer,merger,master)
	   pairs even when reducer* form.set overrides grow the reducer's local arena. */
	size = (PF.shuffle_arena_words - 1)/(PF.numtasks - 1);
	if( size <= (LONG)(AM.MaxTer/sizeof(WORD) + 2)) size = (LONG)(2*(AM.MaxTer/sizeof(WORD) + 2));
	//size = size / 512;

	if ( rbuf == NULL ) {
		if ( ( rbuf = (PF_BUFFER**)Malloc1(alloc_numtasks*sizeof(PF_BUFFER*), "Master: rbufs") ) == NULL ) return(-1);
		if ( (rbuf[0] = PF_AllocBuf(1,0,1) ) == NULL ) return(-1);
		for ( i = 1; i < alloc_numtasks; i++ ) {
			if (!(rbuf[i] = PF_AllocBuf(numrbufs,sizeof(WORD)*size,1))) return(-1);
		}
	}
	rbuf[0]->buff[0] = AT.SS->lBuffer;
	rbuf[0]->full[0] = rbuf[0]->fill[0] = rbuf[0]->buff[0];
	rbuf[0]->stop[0] = rbuf[1]->buff[0] = rbuf[0]->buff[0] + 1;
	rbuf[1]->full[0] = rbuf[1]->fill[0] = rbuf[1]->buff[0];
	for ( i = 2; i < numtasks ; i++ ) {
		rbuf[i-1]->stop[0] = rbuf[i]->buff[0] = rbuf[i-1]->buff[0] + size;
		rbuf[i]->full[0] = rbuf[i]->fill[0] = rbuf[i]->buff[0];
	}
	rbuf[numtasks -1]->stop[0] = rbuf[numtasks -1]->buff[0] + size;

	for ( i = 1; i < numtasks; i++ ) {
		rbuf[i]->active = 0;
		for ( j = 0; j < rbuf[i]->numbufs; j++ ) {
			rbuf[i]->full[j] = rbuf[i]->fill[j] = rbuf[i]->buff[j] + AM.MaxTer/sizeof(WORD) + 2;
		}
		PF_term[i] = rbuf[i]->fill[rbuf[i]->active];
		*PF_term[i] = 0;
		workerIdx = pf_loser_src_to_rank(i);
		PF_IRecvRbuf(rbuf[i],rbuf[i]->active,workerIdx);
	}
	rbuf[0]->active = 0;
	PF_term[0] = rbuf[0]->buff[0];
	PF_term[0][0] = 0;  /* PF_term[0] is used for a zero term. */
	PF.rbufs = rbuf;
/*
 		#] the receive buffers : 
 		#[ the actual tree :

	 calculate number of nodes in mergetree and allocate space for them
*/
	if ( numslaves < 3 ) numnodes = 1;
	else {
		numnodes = 2;
		while ( numnodes < numslaves ) numnodes *= 2;
		numnodes -= 1;
	}

	if ( PF_root == NULL )
	{
		if ( ( PF_root = (NODE*)Malloc1(sizeof(NODE)*numnodes,"nodes in mergetree") ) == NULL )
			return(-1);
	}
/*
		then initialize all the nodes
*/
	src = 1;
	for ( i = 0; i < numnodes; i++ ) {
		if ( 2*(i+1) <= numnodes ) {
			PF_root[i].left = &(PF_root[2*(i+1)-1]);
			PF_root[i].lsrc = 0;
		}
		else {
			PF_root[i].left = 0;
			if ( src < numtasks ) PF_root[i].lsrc = src++;
			else                  PF_root[i].lsrc = 0;
		}
		PF_root[i].lloser = 0;
	}
	for ( i = 0; i < numnodes; i++ ) {
		if ( 2*(i+1)+1 <= numnodes ) {
			PF_root[i].rght = &(PF_root[2*(i+1)]);
			PF_root[i].rsrc = 0;
		}
		else {
			PF_root[i].rght = 0;
			if (src<numtasks) PF_root[i].rsrc = src++;
			else              PF_root[i].rsrc = 0;
		}
		PF_root[i].rloser = 0;
	}
/*
 		#] the actual tree : 
*/
	return(numnodes);
}

/*
 		#] PF_InitTree : 
 		#[ PF_PutIn :
*/

/**
 * Replaces PutIn() on the master process and is used in PF_GetLoser().
 * It puts in the next term from slaveprocess \a src into the tree of losers
 * on the master and is a lot like GetTerm(). The main problems are:
 * buffering and decompression.
 *
 * If \a src == 0, it returns the zero term (PF_term[0]).
 *
 * If \a src != 0, it receives terms from another machine.
 * They are stored in the large sortbuffer which is divided into buff[i]
 * in the PF.rbufs[src], if PF.numrbufs > 1.
 *
 * @param  src  the source process.
 * @return      the next term.
 *
 * @remark  PF_term[0][0] == 0 (see InitTree()), so PF_term[0] can be used to be
 *          the returnvalue for a zero term (== no more terms).
 */
static WORD *PF_PutIn(int src)
{
	int tag;
	WORD im, r;
	WORD *m1, *m2;
	LONG size;
	PF_BUFFER *rbuf = PF.rbufs[src];
	int workerIdx = pf_loser_src_to_rank(src);
	int a = rbuf->active;
	int next = a+1 >= rbuf->numbufs ? 0 : a+1 ;
	WORD *lastterm = PF_term[src];
	WORD *term = rbuf->fill[a];

	//MesPrint("PF_PutIn: PutIn from %d active buffer %d next buffer %d", workerIdx, a, next);
	if ( src <= 0 ) return(PF_term[0]);

	if ( rbuf->full[a] == rbuf->buff[a] + AM.MaxTer/sizeof(WORD) + 2 ) {
/*
			very first term from this src
*/
		{
#ifdef PF_PROFILE
			int _pf_rw = PF.in_merger_phase ? PF_PHASE_MER_RECV_WAIT
			                                : PF_PHASE_MAS_MERGE_RECV_WAIT;
#endif
			PF_TIMER_BEGIN_RT(rw);
			tag = PF_WaitRbuf(rbuf,a,&size);
			PF_TIMER_END_RT(rw, _pf_rw);
		}
		rbuf->full[a] += size;
		if ( tag == PF_ENDBUFFER_MSGTAG ) *rbuf->full[a]++ = 0;
		else if ( rbuf->numbufs > 1 ) {
/*
				post a nonblock. recv. for the next buffer
*/
			rbuf->full[next] = rbuf->buff[next] + AM.MaxTer/sizeof(WORD) + 2;
			size = (LONG)(rbuf->stop[next] - rbuf->full[next]);
			PF_IRecvRbuf(rbuf,next,workerIdx);
		}
	}
	//last term from the buffer
	if ( *term == 0 && term != rbuf->full[a] ) return(PF_term[0]);
/*
		exception is for rare cases when the terms fitted exactly into buffer
*/
	if ( term + *term > rbuf->full[a] || term + 1 >= rbuf->full[a] ) {
newterms:
		//MesPrint("PF_PutIn: Need new terms, copies %d bytes or %d bytes", term - rbuf->buff[a], *term);
		m1 = rbuf->buff[next] + AM.MaxTer/sizeof(WORD) + 1;
		/*
			copy beginning of term to the next buffer so that it ends at m1
		*/
		if ( *term < 0 || term == rbuf->full[a] ) {
/*
			copy term and lastterm to the new buffer, so that they end at m1
*/
			//MesPrint("PF_PutIn: copy lastterm also");
			m2 = rbuf->full[a] - 1;
			while ( m2 >= term ) *m1-- = *m2--;
			rbuf->fill[next] = term = m1 + 1;
			m2 = lastterm + *lastterm - 1;
			while ( m2 >= lastterm ) *m1-- = *m2--;
			lastterm = m1 + 1;
		}
		else {
/*
			copy beginning of term to the next buffer so that it ends at m1
*/
			m2 = rbuf->full[a] - 1;
			while ( m2 >= term ) *m1-- = *m2--;
			rbuf->fill[next] = term = m1 + 1;
		}
		if ( rbuf->numbufs == 1 ) {
			rbuf->full[a] = rbuf->buff[a] + AM.MaxTer/sizeof(WORD) + 2;
			size = (LONG)(rbuf->stop[a] - rbuf->full[a]);
			//MesPrint("[%d] PF_PutIn: First Post non blocking receive from %d size %d", PF.me, src, size);
			PF_IRecvRbuf(rbuf,a,workerIdx);
		}
/*
			wait for new terms in the next buffer
*/
		//MesPrint("[%d] PF_PutIn: Wait the next buffer to be filled from %d active buffer %d ", PF.me, workerIdx, next);
		rbuf->full[next] = rbuf->buff[next] + AM.MaxTer/sizeof(WORD) + 2;
		{
#ifdef PF_PROFILE
			int _pf_rw = PF.in_merger_phase ? PF_PHASE_MER_RECV_WAIT
			                                : PF_PHASE_MAS_MERGE_RECV_WAIT;
#endif
			PF_TIMER_BEGIN_RT(rw);
			tag = PF_WaitRbuf(rbuf,next,&size);
			PF_TIMER_END_RT(rw, _pf_rw);
		}
		//MesPrint("[%d] PF_PutIn: got new terms tag %d", PF.me, tag);
		rbuf->full[next] += size;
		if ( tag == PF_ENDBUFFER_MSGTAG ) {
			*rbuf->full[next]++ = 0;
		}
		else if ( rbuf->numbufs > 1 ) {
/*
			post a nonblock. recv. for active buffer, it is not needed anymore
*/
			rbuf->full[a] = rbuf->buff[a] + AM.MaxTer/sizeof(WORD) + 2;
			size = (LONG)(rbuf->stop[a] - rbuf->full[a]);
			PF_IRecvRbuf(rbuf,a,workerIdx);
		}
/*
			now safely make next buffer active
*/
		a = rbuf->active = next;
	}

	if ( *term < 0 ) {
/*
			We need to decompress the term
*/
		im = *term;
		r = term[1] - im + 1;
		m1 = term + 2;
		m2 = lastterm - im + 1;
		while ( ++im <= 0 ) *--m1 = *--m2;
		*--m1 = r;
		rbuf->fill[a] = term = m1;
		if ( term + *term > rbuf->full[a] ) goto newterms;
	}
	rbuf->fill[a] += *term;
	return(term);
}

/*
 		#] PF_PutIn :
 		#[ pf_emit_compressed_bulk :
*/

/**
 * Prefix-drain bulk emit (master or mapper-merger): copy a run of n_terms
 * consecutive compressed terms from rbuf (already verified to share the
 * K-prefix with the just-emitted term T_i) straight to fout's POfill,
 * bypassing PutOut and all per-term decompression. The buffer-full flush
 * goes to disk on the master and upstream via MPI on a merger
 * (branch on PF.in_merger_phase).
 *
 * Correctness:
 *   - Wire format both reducer and master produce is identical:
 *     [-share, tail_len, tail_words]. Term j in the run encodes a delta
 *     against term j-1 IN THE INPUT STREAM, which is also term j-1 in the
 *     OUTPUT stream (we just wrote it). So the deltas decode the same on
 *     replay. For the first drained term the previous output term is T_i
 *     (just emitted by PutOut) -- same as its input predecessor.
 *   - AR.CompressPointer stays = T_i (set by PutOut). The boundary term's
 *     PutOut will compress against T_i; since boundary_share <= K and
 *     T_i[1..K] == T_{i+k}[1..K] (drain invariant), the compression match
 *     length is the same as it would be against T_{i+k}. No refresh needed.
 *
 * The drain is disabled when a bracket index is being built (the caller
 * guards on !dobracketindex), so this routine never has to maintain one --
 * it is a pure compressed-bytes copy.
 *
 * @param src               Start of compressed bulk in rbuf.
 * @param nwords            Total WORDs in the bulk.
 * @param n_terms           Number of compressed terms in the bulk.
 * @return 0 on success, -1 on I/O error.
 */
static int pf_emit_compressed_bulk(WORD *src,
                                   int nwords, int n_terms,
                                   FILEHANDLE *fi, POSITION *position)
{
	WORD *p = fi->POfill;
	WORD *s = src;
	LONG remaining = (LONG)nwords;
	LONG RetCode;

	while ( remaining > 0 ) {
		LONG space = (LONG)(fi->POstop - p);
		if ( space <= 0 ) {
		  if ( PF.in_merger_phase && !PF.merger_to_file ) {
			/* Merger (gather/LAST): the output buffer is an MPI send buffer, not a file.
			   Flush the full buffer upstream to MASTER, mirroring PutOut's
			   sbuf-flush block (sort.c). The merger's single send buffer is
			   PF.sbufs[MASTER]; PF_WISendSbuf passes BUFFER framing through
			   unchanged while PF.in_merger_phase is set. */
			PF_BUFFER *sbuf = PF.sbufs[MASTER];
			sbuf->fill[sbuf->active] = fi->POstop;
			PF_WISendSbuf(PF_BUFFER_MSGTAG, PF.merger_parent);
			p = fi->PObuffer = fi->POfill = fi->POfull
			  = sbuf->full[sbuf->active] = sbuf->fill[sbuf->active]
			  = sbuf->buff[sbuf->active];
			fi->POstop = sbuf->stop[sbuf->active];
			space = (LONG)(fi->POstop - p);
		  }
		  else {
			/* Flush. Same shape as PutOut's block. */
			if ( fi->handle < 0 ) {
				if ( ( RetCode = CreateFile(fi->name) ) >= 0 ) {
					fi->handle = (WORD)RetCode;
					PUTZERO(fi->filesize);
					PUTZERO(fi->POposition);
				}
				else {
					MLOCK(ErrorMessageLock);
					MesPrint("Cannot create scratch file %s", fi->name);
					MUNLOCK(ErrorMessageLock);
					return(-1);
				}
			}
#ifdef ALLLOCK
			LOCK(fi->pthreadslock);
#endif
			if ( fi == AR.hidefile ) { LOCK(AS.inputslock); }
			SeekFile(fi->handle, &(fi->POposition), SEEK_SET);
			if ( ( RetCode = WriteFile(fi->handle, (UBYTE *)(fi->PObuffer), fi->POsize) ) != fi->POsize ) {
				if ( fi == AR.hidefile ) { UNLOCK(AS.inputslock); }
#ifdef ALLLOCK
				UNLOCK(fi->pthreadslock);
#endif
				MLOCK(ErrorMessageLock);
				MesPrint("Write error during sort. Disk full?");
				MUNLOCK(ErrorMessageLock);
				return(-1);
			}
			ADDPOS(fi->filesize, fi->POsize);
			ADDPOS(fi->POposition, fi->POsize);
			p = fi->PObuffer;
			if ( fi == AR.hidefile ) { UNLOCK(AS.inputslock); }
#ifdef ALLLOCK
			UNLOCK(fi->pthreadslock);
#endif
			space = (LONG)(fi->POstop - p);
		  }
		}
		{
			LONG ncopy = (remaining < space) ? remaining : space;
			memcpy(p, s, (size_t)(ncopy * (LONG)sizeof(WORD)));
			p += ncopy;
			s += ncopy;
			remaining -= ncopy;
		}
	}
	fi->POfull = fi->POfill = p;

	ADDPOS(*position, (LONG)nwords * sizeof(WORD));
	(void)n_terms;
	return 0;
}

/*
 		#] pf_emit_compressed_bulk :
		#[ PF_StoreBuffer :
*/
static int PF_StoreBuffer()
{
	int tag, err, a, next;
	SORTING *S = AT.SS;;
	LONG size = 0;
	POSITION pp;
	LONG lSpace, sSpace;
	WORD *ss, *sss, *lfill;
	PF_BUFFER *rbuf;
	PF_Dispatch* d = &PF.dispatch;
	int src = 0;
	PF_TIME_ELAPSED(TIMERESET);
newsrc: ;
	//MesPrint("[%d] PF_StoreBuffer: WaitAnyRbuf", PF.me);
	PF_TIMER_BEGIN(RED_RECV_WAIT);
	PF_TIME_ELAPSED(TIMESTART);
	tag = PF_WaitAnyRbuf(PF.rbufs,&src,&size);
	PF_TIME_ELAPSED(TIMESTOP);
	PF_TIMER_END(RED_RECV_WAIT);
	if( tag  == PF_ENDSHUFFLEALL_MSGTAG)
	{
		src = 0;
		return 0;
	}
	rbuf = PF.rbufs[src];
	a = rbuf->active;
	next = a+1 >= rbuf->numbufs ? 0 : a+1 ;
	rbuf->full[a] += size;
	if ( tag == PF_SHUFFLE_MSGTAG && rbuf->numbufs > 1 ) {
/*
		post a nonblock. recv. for the next buffer
*/
		rbuf->full[next] = rbuf->buff[next] + AM.MaxTer/sizeof(WORD) + 2;
		size = (LONG)(rbuf->stop[next] - rbuf->full[next]);
		//MesPrint("[%d] PF_StoreBuffer: set receive request of size %d from %d", PF.me, size, src);
		err = PF_IRecvRbuf(rbuf,next,src);
		if (err) {MesPrint("[%d] PF_StoreBuffer: PF_IRecvRbuf error %d from %d", PF.me, err, src); return(-1); }
		int k = src * PF.numrbufs + next;
		d->reqs[k] = rbuf->request[next];
	}
	else if ( tag != PF_ENDSHUFFLE_MSGTAG)
	{
		return(-1);
	}
	rbuf = PF.rbufs[src];
	if (!rbuf || !rbuf->buff || !rbuf->full || !rbuf->fill) {
		return -1;
	}
	PF_TIMER_INC(PF_EX_BUFFERS_RECEIVED);

	/* RED_STORE_TOTAL covers per-buffer processing cost (memcpy + maybe a
	   MergePatches), excluding the recv wait above. Baseline for plan 1b
	   which would add a SplitMerge inside this window. */
	PF_TIMER_BEGIN(RED_STORE_TOTAL);
	sSpace = rbuf->full[a] - rbuf->fill[a];
	/* Per-link reducer bandwidth: bytes consumed per buffer. */
	PF_TIMER_ADD_BYTES(PF_EX_RED_BYTES_RECEIVED, sSpace * sizeof(WORD));
	//MesPrint("[%d] PF_StoreBuffer: saving patch of size %d to large buffer", PF.me, sSpace);
	lSpace = sSpace + (S->lFill - S->lBuffer)
				 - (AM.MaxTer/sizeof(WORD))*((LONG)S->lPatch);
	SETBASEPOSITION(pp,lSpace);
	MULPOS(pp,sizeof(WORD));
	{
		int _pf_merge_max_patches = (S->lPatch >= S->MaxPatches);
		int _pf_merge_lbuf_full = ((WORD *)(((UBYTE *)(S->lFill + sSpace)) + 2*AM.MaxTer)) >= S->lTop;
		if ( _pf_merge_max_patches || _pf_merge_lbuf_full ) {
/*
			The large buffer is too full. Merge and write it
*/
			//MesPrint("[%d] PF_StoreBuffer: before MergePatches call. S->lPatch= %d,S->MaxPatches=%d, S->lFill= %d, S->lTop=%d",PF.me,S->lPatch,S->MaxPatches,((WORD *)(((UBYTE *)(S->lFill + sSpace)) + 2*AM.MaxTer )),S->lTop);

			if (_pf_merge_max_patches) PF_TIMER_INC(PF_EX_MERGE_MAX_PATCHES);
			if (_pf_merge_lbuf_full)   PF_TIMER_INC(PF_EX_MERGE_LBUFFER_FULL);
			PF_TIMER_BEGIN(RED_MERGE_PATCHES);
			PF_TIMER_INC(PF_EX_PATCHES_BUILT);
			if ( MergePatches(1) ) return (-1);
			PF_TIMER_END(RED_MERGE_PATCHES);

			SETBASEPOSITION(pp,sSpace);
			MULPOS(pp,sizeof(WORD));
			ADD2POS(pp,S->fPatches[S->fPatchN]);

			S->lPatch = 0;
			S->lFill = S->lBuffer;
		}
	}
	PF_TIMER_BEGIN(RED_BUFFER_COPY);
	S->Patches[S->lPatch++] = S->lFill;
	lfill = (WORD *)(((UBYTE *)(S->lFill)) + AM.MaxTer);

	ss = rbuf->fill[a];
	while ( ss != rbuf->full[a] ) {
		if (*ss == 0) break;
		S->TermsLeft++;
		sss =  (*ss < 0) ? ss + ss[1] + 2 : ss + *ss ;
#ifdef MR_DEBUG
		/* Mappers produce well-formed term streams; this is a self-consistency
		   check. Behind MR_DEBUG so the hot loop stays a single load+memcpy. */
		if ( sss > rbuf->full[a] || sss <= rbuf->fill[a]){
			MesPrint("[%d] PF_StoreBuffer: Error in term %d sss= %d, ss = %d full = %d", PF.me, *ss, sss, ss, rbuf->full[a]);
			return(-1);
		}
#endif
		{
			size_t n = (size_t)(sss - ss);
			memcpy(lfill, ss, n * sizeof(WORD));
			lfill += n;
			ss = sss;
		}
	}
	*lfill++ = 0;
	S->lFill = lfill;
	S->sTerms = 0;
	S->PoinFill = S->sPointer;
	*(S->PoinFill) = S->sFill = S->sBuffer;
	PF_TIMER_END(RED_BUFFER_COPY);
	PF_TIMER_END(RED_STORE_TOTAL);
	a = rbuf->active = next;
	goto newsrc;
}

/*
		#] PF_StoreBuffer :
 		#[ PF_GetLoser :
*/

/**
 * Finds the 'smallest' of all the PF_terms. Take also care of changing
 * coefficients and cancelling terms. When the coefficient changes, the new is
 * sitting in the array PF_newcpos, the length of the new coefficient in
 * PF_newclen. The original term will be untouched until it is copied to the
 * output buffer!
 *
 * Calling PF_GetLoser() with argument node will return the loser of the
 * subtree under node when the next term of the stream # PF_loser
 * (the last "loserstream") is filled into the tree.
 * PF_loser == 0 means we are just starting and should fill new terms into
 * all the leaves of the tree.
 *
 * @param  n  the node.
 * @return    the loser of the subtree under the node n.
 *            0 indicates there are no more terms.
 *            -1 indicates an error.
 */
static int PF_GetLoser(NODE *n)
{
	GETIDENTITY
	WORD comp;

	if ( PF_loser == 0 ) {
/*
			this is for the right initialization of the tree only
*/
		if ( n->left ) n->lloser = PF_GetLoser(n->left);
		else {
			n->lloser = n->lsrc;
			if ( *(PF_term[n->lsrc] = PF_PutIn(n->lsrc)) == 0) n->lloser = 0;
		}
		PF_loser = 0;
		if ( n->rght ) n->rloser = PF_GetLoser(n->rght);
		else{
			n->rloser = n->rsrc;
			if ( *(PF_term[n->rsrc] = PF_PutIn(n->rsrc)) == 0 ) n->rloser = 0;
		}
		PF_loser = 0;
	}
	else if ( PF_loser == n->lloser ) {
		if ( n->left ) n->lloser = PF_GetLoser(n->left);
		else {
			n->lloser = n->lsrc;
			if ( *(PF_term[n->lsrc] = PF_PutIn(n->lsrc)) == 0 ) n->lloser = 0;
		}
	}
	else if ( PF_loser == n->rloser ) {
newright:
		if ( n->rght ) n->rloser = PF_GetLoser(n->rght);
		else {
			n->rloser = n->rsrc;
			if ( *(PF_term[n->rsrc] = PF_PutIn(n->rsrc)) == 0 ) n->rloser = 0;
		}
	}
	if ( n->lloser > 0 && n->rloser > 0 ) {
		/* Apply the K-prefix cap ONLY for the loser-tree compare, where the
		   routing invariant guarantees cross-leaf terms differ within K.
		   Restore immediately so PutOut->PutBracketInIndex (called outside
		   this function from the merge loop) sees the uncapped CompareTerms. */
		pf_compare_kcap = pf_compare_kcap_want;
		comp = CompareTerms(BHEAD PF_term[n->lloser],PF_term[n->rloser],(WORD)0);
		pf_compare_kcap = 0;
		if ( comp > 0 )     return(n->lloser);
		else if (comp < 0 ) return(n->rloser);
		else {
/*
 		#[ terms are equal :
*/
			WORD *lcpos, *rcpos;
			UWORD *newcpos;
			WORD lclen, rclen, newclen, newnlen;
			SORTING *S = AT.SS;

			if ( S->PolyWise ) {
/*
			#[ Here we work with PolyFun :
*/
				WORD *tt1, *w;
				WORD r1,r2;
				WORD *ml = PF_term[n->lloser];
				WORD *mr = PF_term[n->rloser];

				if ( ( r1 = (int)*PF_term[n->lloser] ) <= 0 ) r1 = 20;
				if ( ( r2 = (int)*PF_term[n->rloser] ) <= 0 ) r2 = 20;
				tt1 = ml;
				ml += S->PolyWise;
				mr += S->PolyWise;
				if ( S->PolyFlag == 2 ) {
					w = poly_ratfun_add(BHEAD ml,mr);
					if ( *tt1 + w[1] - ml[1] > AM.MaxTer/((LONG)sizeof(WORD)) ) {
						MesPrint("Term too complex in PolyRatFun addition. MaxTermSize of %10l is too small",AM.MaxTer);
						Terminate(-1);
					}
					AT.WorkPointer = w;
				}
				else {
					w = AT.WorkPointer;
					if ( w + ml[1] + mr[1] > AT.WorkTop ) {
						MesPrint("A WorkSpace of %10l is too small",AM.WorkSize);
						Terminate(-1);
					}
					AddArgs(BHEAD ml,mr,w);
				}
				r1 = w[1];
				if ( r1 <= FUNHEAD || ( w[FUNHEAD] == -SNUMBER && w[FUNHEAD+1] == 0 ) ) {
					goto cancelled;
				}
				if ( r1 == ml[1] ) {
					NCOPY(ml,w,r1);
				}
				else if ( r1 < ml[1] ) {
					r2 = ml[1] - r1;
					mr = w + r1;
					ml += ml[1];
					while ( --r1 >= 0 ) *--ml = *--mr;
					mr = ml - r2;
					r1 = S->PolyWise;
					while ( --r1 >= 0 ) *--ml = *--mr;
					*ml -= r2;
					PF_term[n->lloser] = ml;
				}
				else {
					r2 = r1 - ml[1];
					if ( r2 > 2*AM.MaxTal )
						MesPrint("warning: new term in polyfun is large");
					mr = tt1 - r2;
					r1 = S->PolyWise;
					ml = tt1;
					*ml += r2;
					PF_term[n->lloser] = mr;
					NCOPY(mr,ml,r1);
					r1 = w[1];
					NCOPY(mr,w,r1);
				}
				PF_newclen[n->rloser] = 0;
				PF_loser = n->rloser;
				goto newright;
/*
			#] Here we work with PolyFun : 
*/
			}
			if ( ( lclen = PF_newclen[n->lloser] ) != 0 ) lcpos = PF_newcpos[n->lloser];
			else {
				lcpos = PF_term[n->lloser];
				lclen = *(lcpos += *lcpos - 1);
				lcpos -= ABS(lclen) - 1;
			}
			if ( ( rclen = PF_newclen[n->rloser] ) != 0 ) rcpos = PF_newcpos[n->rloser];
			else {
				rcpos = PF_term[n->rloser];
				rclen = *(rcpos += *rcpos - 1);
				rcpos -= ABS(rclen) -1;
			}
			lclen = ( (lclen > 0) ? (lclen-1) : (lclen+1) ) >> 1;
			rclen = ( (rclen > 0) ? (rclen-1) : (rclen+1) ) >> 1;
			newcpos = PF_ScratchSpace;
			if ( AddRat(BHEAD (UWORD *)lcpos,lclen,(UWORD *)rcpos,rclen,newcpos,&newnlen) ) return(-1);
			if ( AN.ncmod != 0 ) {
				if ( ( AC.modmode & POSNEG ) != 0 ) {
					NormalModulus(newcpos,&newnlen);
				}
				if ( BigLong(newcpos,newnlen,(UWORD *)AC.cmod,ABS(AN.ncmod)) >=0 ) {
					WORD ii;
					SubPLon(newcpos,newnlen,(UWORD *)AC.cmod,ABS(AN.ncmod),newcpos,&newnlen);
					newcpos[newnlen] = 1;
					for ( ii = 1; ii < newnlen; ii++ ) newcpos[newnlen+ii] = 0;
				}
			}
			if ( newnlen == 0 ) {
/*
					terms cancel, get loser of left subtree and then of right subtree
*/
cancelled:
				PF_loser = n->lloser;
				PF_newclen[n->lloser] = 0;
				if ( n->left ) n->lloser = PF_GetLoser(n->left);
				else {
					n->lloser = n->lsrc;
					if ( *(PF_term[n->lsrc] = PF_PutIn(n->lsrc)) == 0 ) n->lloser = 0;
				}
				PF_loser = n->rloser;
				PF_newclen[n->rloser] = 0;
				goto newright;
			}
			else {
/*
					keep the left term and get the loser of right subtree
*/
				newnlen *= 2;
				newclen = ( newnlen > 0 ) ? ( newnlen + 1 ) : ( newnlen - 1 );
				if ( newnlen < 0 ) newnlen = -newnlen;
				PF_newclen[n->lloser] = newclen;
				lcpos = PF_newcpos[n->lloser];
				if ( newclen < 0 ) newclen = -newclen;
				while ( newclen-- ) *lcpos++ = *newcpos++;
				PF_loser = n->rloser;
				PF_newclen[n->rloser] = 0;
				goto newright;
			}
/*
 		#] terms are equal : 
*/
		}
	}
	if (n->lloser > 0) return(n->lloser);
	if (n->rloser > 0) return(n->rloser);
	return(0);
}
/*
 		#] PF_GetLoser : 
 		#[ PF_EndSort :
*/

/**
 * Finishes a master sorting with collecting terms from slaves.
 * Called by EndSort().
 *
 * If this is not the masterprocess, just initialize the sendbuffers and
 * return 0, else PF_EndSort() sends the rest of the terms in the sendbuffer
 * to the next slave and a dummy message to all slaves with tag
 * PF_ENDSORT_MSGTAG. Then it receives the sorted terms, sorts them using a
 * recursive 'tree of losers' (PF_GetLoser()) and writes them to the
 * outputfile.
 *
 * @return   1  if the sorting on the master was done.
 *           0  if EndSort() still must perform a regular sorting because it is not
 *              at the ground level or not on the master or in the sequential mode
 *              or in the InParallel mode.
 *          -1  if an error occurred.
 *
 * @remark  The slaves will send the sorted terms back to the master in the regular
 *          sorting (after the initialization of the send buffer in PF_EndSort()).
 *          See PutOut() and FlushOut().
 *
 * @remark  This function has been changed such that when it returns 1,
 *          AM.S0->TermsLeft is set correctly. But AM.S0->GenTerms is not set:
 *          it will be set after collecting the statistics from the slaves
 *          at the end of PF_Processor(). (TU 30 Jun 2011)
 */

/* --- Master-bypass via partitioned merger output -------------------------
   In the interior of an MR chain (MAPREDUCE_FIRST and MAPREDUCE) each merger
   rank writes its merged stream to a node-local scratch file and reports a tiny
   done message (PF_MERGERDONE_MSGTAG, {term-count, byte-size}) to the master,
   instead of streaming it upstream for a global merge. The master then skips
   its loser-tree merge entirely. The chain-exit module (MAPREDUCE_LAST) gathers
   the files back to a global file. All gated on PF.nummergers > 0, so a run
   without the merger tier (PF_MERGERS unset) keeps the classic behaviour
   bit-for-bit. */
static int pf_output_partitioned(void)
{
	/* Output goes to node-local merger files (partitioned) on the two chain-body
	   states. The single gather state MAPREDUCE_LAST is excluded here -- it streams
	   its output to the master instead (re-globalize) -- so `.sort(gather)`, `.end`,
	   and the off-parallel fallback all land outside this set via the sMRflag
	   transition (execute.c), with no per-flag special-case needed. */
	return ( PF.nummergers > 0 &&
	         ( AC.sMRflag == MAPREDUCE_FIRST || AC.sMRflag == MAPREDUCE ) );
}

/* Lazily allocate this merger's node-local scratch pair (lands in FORMTMP via
   AllocFileHandle's FG.fname2 base; unique names through AN.filenum). Allocated
   once per rank and reused/swapped across modules. */
static int pf_ensure_merger_files(void)
{
	if ( PF.merger_outfile == NULL ) {
		if ( ( PF.merger_outfile = AllocFileHandle(0,(char *)0) ) == NULL ) return -1;
	}
	if ( PF.merger_infile == NULL ) {
		if ( ( PF.merger_infile = AllocFileHandle(0,(char *)0) ) == NULL ) return -1;
	}
	return 0;
}

/* Input is partitioned (the previous module wrote merger files; the mergers
   distribute, the master does not) for the chain interior and the exit:
   MAPREDUCE and MAPREDUCE_LAST. */
static int pf_input_partitioned(void)
{
	return ( PF.nummergers > 0 &&
	         ( AC.sMRflag == MAPREDUCE || AC.sMRflag == MAPREDUCE_LAST ) );
}

/* Ship the just-filled bucket (slot 0 of `sbuf`) to recipient rank `next`, with
   the master's swap-pipelining and slow-start ramp. Shared by the master's
   PF_Processor distribute loop and the mapper-merger's node-local redistribution
   (PF_MergerDistribute) so both get identical load-balancing.

   The swap moves slot 0's filled buffer into slot[next] and hands next's freed
   buffer to slot 0, so the next bucket fills while this one is in flight. The
   ramp doubles `maxinterms` toward ProcessBucketSize after ~nummappers ships so
   every worker gets an initial bucket quickly. `ninterms` stamps the next
   bucket's header (1-based index of its first term; the receiver reads it as
   AN.ninterms = header - 1).

   `is_merger` only selects the send primitive: the master uses PF_ISendSbuf
   (PF.sbufs[0], which also manages the ENDSORT `finished` counter); the merger
   issues the same async per-recipient send directly on PF.distbuf (it cannot use
   PF_ISendSbuf because that routes a mapper rank's sends to reducer slots). */
static void pf_ship_bucket(PF_BUFFER *sbuf, int next, LONG ninterms,
                           LONG *maxinterms, int *cmaxinterms, int is_merger)
{
	sbuf->fill[next] = sbuf->fill[0];
	sbuf->full[next] = sbuf->full[0];
	SWAP(sbuf->stop[next], sbuf->stop[0]);
	SWAP(sbuf->buff[next], sbuf->buff[0]);
	sbuf->fill[0] = sbuf->full[0] = sbuf->buff[0];
	sbuf->active = next;

	if ( is_merger ) {
		int a = sbuf->active;                 /* == next */
		LONG sz = sbuf->fill[a] - sbuf->buff[a];
		sbuf->fill[a] = sbuf->buff[a];
		if ( sbuf->request[next] != MPI_REQUEST_NULL )
			MPI_Wait(&sbuf->request[next], &sbuf->retstat[next]);
		MPI_Isend(sbuf->buff[a], (int)sz, PF_WORD, next, PF_TERM_MSGTAG,
		          PF_COMM, &sbuf->request[a]);
	}
	else {
#ifdef MPI2
		if ( PF_Put_origin(next) == 0 ) printf("PF_Put_origin error...\n");
#else
		PF_ISendSbuf(next, PF_TERM_MSGTAG);
#endif
	}

	/* Initialize the next bucket header. */
	PACK_LONG(sbuf->fill[0], ninterms);

	/* "slow startup": double maxinterms up to ProcessBucketSize after (hopefully)
	   all workers got some terms. */
	if ( *cmaxinterms >= PF.nummappers - 2 ) {
		*maxinterms *= 2;
		if ( *maxinterms >= AC.mProcessBucketSize ) {
			*cmaxinterms = -1;
			*maxinterms = AC.mProcessBucketSize;
		}
	}
	else if ( *cmaxinterms >= 0 ) {
		(*cmaxinterms)++;
	}
}

static int PF_Wait4Slave(int src);   /* defined below; used by pf_distribute_terms */

/* The full on-demand distribute loop, shared by the master's PF_Processor and the
   mapper-merger's PF_MergerDistribute. Reads terms from the source and ships
   maxinterms-sized buckets (pf_ship_bucket) to recipients that signal PF_READY,
   with the slow-start ramp. The CALLER owns setup (NewSort, sbuf alloc, prototype)
   and termination: the master leaves the final partial bucket for PF_WaitAllSlaves
   (last chunk + ENDSORT); the merger sends its own ENDSORT to each recipient
   after this returns.

   `is_merger` changes only the three things that genuinely differ:
     - term source : GetTerm(AR.infile) [master] vs GetOneTerm(mfile) [merger,
       with the per-call AR.CompressPointer reset for delta-decode];
     - recipient   : PF_Wait4Slave (any mapper) [master] vs PF_Receive(PF_READY)
       (a node-local mapper) [merger];
     - Collect / deferred-bracket accounting (GetMoreTerms + dd) : master only.
   ramp_count >= 0 caps the initial maxinterms at ramp_count/ndest/4 (master passes
   e->counter; merger passes -1 = no cap). Returns terms distributed, or -1. */
static LONG pf_distribute_terms(PF_BUFFER *sbuf, int is_merger, FILEHANDLE *mfile,
                                LONG ramp_count, int ndest)
{
	GETIDENTITY
	WORD *term = AT.WorkPointer, *s; int j, next;
	LONG maxinterms, termsinbucket = 0, dd = 0, ninterms = 0, nbuckets = 0;
	int cmaxinterms = 0;
	POSITION rpos; WORD *save_cp = 0;

	maxinterms = AC.mProcessBucketSize / 100;
	if ( ramp_count >= 0 ) {
		LONG cap = ( ndest > 0 ) ? ramp_count / ndest / 4 : maxinterms;
		if ( maxinterms > cap ) maxinterms = cap;
	}
	if ( maxinterms < 1 ) maxinterms = 1;

	if ( is_merger ) { PUTZERO(rpos); save_cp = AR.CompressPointer; }
	else AN.ninterms = 0;
	PACK_LONG(sbuf->fill[0], 1);

	for ( ;; ) {
		if ( is_merger ) {
			WORD got;
			AR.CompressPointer = AR.CompressBuffer;
			got = GetOneTerm(BHEAD term, mfile, &rpos, 0);
			if ( got < 0 ) { AR.CompressPointer = save_cp; return -1; }
			if ( got == 0 ) break;
			ninterms++;
		}
		else {
			if ( !GetTerm(BHEAD term) ) break;
			AN.ninterms++; dd = AN.deferskipped; ninterms = AN.ninterms;
			if ( AC.CollectFun && *term <= (LONG)(AM.MaxTer/(2*sizeof(WORD))) ) {
				if ( GetMoreTerms(term) < 0 ) { LowerSortLevel(); return -1; }
			}
		}
		if ( termsinbucket >= maxinterms || sbuf->fill[0] + *term >= sbuf->stop[0] ) {
			if ( is_merger ) {
				int t = 0;
				PF_TIMER_BEGIN(MER_DISTRIBUTE_WAIT);
				/* Serve whoever sends PF_READY -- our node-local mappers and,
				   under PF_STEAL, stolen mappers the master steered here. A stolen
				   mapper sends the ordinary PF_READY, so there is nothing special
				   to do for it: same receive in both cases. */
				PF_Receive(PF_ANY_SOURCE, PF_READY_MSGTAG, &next, &t);
				PF_TIMER_END(MER_DISTRIBUTE_WAIT);
			}
			else {
				PF_TIMER_BEGIN(MAS_DISTRIBUTE_WAIT);
				next = PF_Wait4Slave(PF_ANY_SOURCE);
				PF_TIMER_END(MAS_DISTRIBUTE_WAIT);
			}
			pf_ship_bucket(sbuf, next, ninterms, &maxinterms, &cmaxinterms, is_merger);
			termsinbucket = 0;
			/* Work-stealing: every 64 buckets, tell the master how many terms
			   we have shipped so it can derive terms_left = initial - shipped and
			   steer stealers to the neediest merger. Ssend into the near-idle
			   master poll loop; the wait is one poll iteration, amortized over 64
			   buckets. (Only the merger reports; the master distributor doesn't.) */
			if ( is_merger && PF.steal && ( ++nbuckets % 64 ) == 0 ) {
				PF_PreparePack();
				PF_Pack(&ninterms, 1, PF_LONG);
				PF_Send(MASTER, PF_MERGER_PROGRESS_MSGTAG);
			}
		}
		j = *(s = term);
		NCOPY(sbuf->fill[0], s, j);
		termsinbucket++;
	}

	if ( is_merger ) AR.CompressPointer = save_cp;
	else AN.ninterms += dd;
	return ninterms;
}

/* Distribute this merger's node-local input file (merger_infile = the previous
   module's partitioned output, content-only [terms][0]) to the node-local
   mappers, on demand, mirroring the master's PF_Processor distribute loop but
   scoped to one node. Uses a DEDICATED per-mapper buffer PF.distbuf so it never
   aliases the shuffle/forward sbufs the same rank reuses in its empty-EndSort and
   PF_MergerLoop. The mapper side is unchanged: PF_GetTerm sends PF_READY to its
   PF.input_src (= this merger) and PF_RecvWbuf receives the bucket. */
static int PF_MergerDistribute(void)
{
	GETIDENTITY
	int M = PF.nummappers, r, nrecip = 0, k, i;
	int *recips;
	LONG slotwords, ninterms;
	PF_BUFFER *db;

	if ( PF.merger_infile == NULL ) return -1;

	/* Collect's GetMoreTerms combines terms read from the MODULE INPUT; it can't
	   be served from this merger's node-local file. A partitioned MR module must
	   therefore not use Collect. (Spin/dRGT run Collect `off parallel`, which is
	   never a partitioned module.) Fail loudly rather than silently mis-collect. */
	if ( AC.CollectFun ) {
		MesPrint("[%d] PF_MergerDistribute: Collect is unsupported inside a "
		         "partitioned MR module; run Collect with `off parallel`", PF.me);
		return -1;
	}

	recips = (int*)Malloc1((LONG)(M > 0 ? M : 1)*sizeof(int), "merger recips");
	if ( recips == NULL ) return -1;
	for ( r = 1; r < M; r++ )
		if ( PF.rank_node && PF.rank_node[r] == PF.node_id && r != PF.me )
			recips[nrecip++] = r;

	/* A distributing merger needs at least one OTHER node-local mapper to feed.
	   If it is the sole mapper on its (reducer-bearing) node, its partition has
	   no node-local consumer -- fail loudly rather than hang on PF_Receive or
	   silently drop the data. Production layouts keep >= 2 mappers per node. */
	if ( nrecip == 0 ) {
		MesPrint("[%d] PF_MergerDistribute: no node-local recipient mapper "
		         "(need >= 2 non-master mappers on a reducer-bearing node)", PF.me);
		M_free(recips,"merger recips");
		return -1;
	}

	/* Per-recipient distribution buffer, one slot per task like the master's
	   PF.sbufs[0], so pf_ship_bucket's swap-pipelining works (slot[next] carries a
	   recipient's bucket in flight while slot 0 refills). Sized exactly like the
	   master's distribute slots (lBuffer share, capped at the receiver's POsize-1).
	   Owns its memory (free=0) since, unlike the master, we don't point buff[] at
	   lBuffer. Lazily allocated, reused across modules. */
	slotwords = (AT.SS->sTop2 - AT.SS->lBuffer) / PF.numtasks;
	if ( slotwords > (LONG)(AR.infile->POsize/sizeof(WORD)) - 1 )
		slotwords = (LONG)(AR.infile->POsize/sizeof(WORD)) - 1;
	if ( slotwords < 1 ) slotwords = 1;
	if ( PF.distbuf == NULL ) {
		if ( ( PF.distbuf = PF_AllocBuf(PF.numtasks, slotwords*sizeof(WORD), 0) ) == NULL ) {
			M_free(recips,"merger recips"); return -1;
		}
	}
	db = PF.distbuf;
	for ( i = 0; i < db->numbufs; i++ ) db->fill[i] = db->full[i] = db->buff[i];
	db->active = 0;

	/* Read merger_infile and ship buckets to the node-local mappers via the same
	   on-demand loop the master uses (slow-start ramp + swap-pipelining). No ramp
	   cap (ramp_count = -1): the merger's file share is already a fraction of the
	   global expression. */
	PF_TIMER_BEGIN(MER_DISTRIBUTE);
	ninterms = pf_distribute_terms(db, 1, PF.merger_infile, -1, nrecip);
	PF_TIMER_END(MER_DISTRIBUTE);
	if ( ninterms < 0 ) { M_free(recips,"merger recips"); return -1; }
	PF_TIMER_ADD_COUNT(PF_EX_MER_DISTRIBUTE_TERMS, ninterms);

	/* Termination -- this merger's file is now exhausted. */
	if ( !PF.steal ) {
		/* Node-local only (default): each recipient gets one ENDSORT bucket and
		   ends (the first carries the residual partial bucket in slot 0; the rest
		   are empty -- the mapper turns an empty bucket into its end sentinel).
		   Blocking send for the tail; wait any prior in-flight TERM first. */
		for ( k = 0; k < nrecip; k++ ) {
			int next = -1, tag;
			PF_Receive(PF_ANY_SOURCE, PF_READY_MSGTAG, &next, &tag);
			if ( db->request[next] != MPI_REQUEST_NULL )
				MPI_Wait(&db->request[next], &db->retstat[next]);
			if ( MPI_Ssend(db->buff[0], (int)(db->fill[0]-db->buff[0]), PF_WORD,
			               next, PF_ENDSORT_MSGTAG, PF_COMM) != MPI_SUCCESS ) {
				M_free(recips,"merger recips"); return -1;
			}
			db->fill[0] = db->full[0] = db->buff[0];
			PACK_LONG(db->fill[0], ninterms);
		}
	}
	else {
		/* Work-stealing responder phase. Our file is drained. Tell the master
		   (PF_MERGER_PROGRESS = -1 == FINISHED) to drop us from the live set, then
		   answer every late PF_READY until the master says all mappers are done:
		     - the first asker, if we hold a residual partial bucket, gets it as a
		       real PF_TERM bucket (it processes those terms, then asks again);
		     - every other asker gets PF_MERGER_EXHAUSTED, sending it back to the
		       master to be steered to a still-live merger.
		   Loop until PF_STEAL_STOP arrives (all plain mappers done -> no more READY).
		   We never block on a send to the master, and the master only ever sends us
		   STOP, so there is no bidirectional control cycle -- plain Ssend is safe. */
		int stop = 0;
		int residual_shipped = ( (db->fill[0] - db->buff[0]) <= 2 ); /* <=2 WORDs == header only == empty */
		LONG finished = -1;
		PF_PreparePack();
		PF_Pack(&finished, 1, PF_LONG);
		PF_Send(MASTER, PF_MERGER_PROGRESS_MSGTAG);
		while ( !stop ) {
			int flag = 0; MPI_Status st;
			if ( MPI_Iprobe(MPI_ANY_SOURCE, PF_READY_MSGTAG, PF_COMM, &flag, &st)
			     == MPI_SUCCESS && flag ) {
				int next = -1, t = 0;
				PF_Receive(st.MPI_SOURCE, PF_READY_MSGTAG, &next, &t);
				if ( !residual_shipped ) {
					if ( db->request[next] != MPI_REQUEST_NULL )
						MPI_Wait(&db->request[next], &db->retstat[next]);
					if ( MPI_Ssend(db->buff[0], (int)(db->fill[0]-db->buff[0]), PF_WORD,
					               next, PF_TERM_MSGTAG, PF_COMM) != MPI_SUCCESS ) {
						M_free(recips,"merger recips"); return -1;
					}
					db->fill[0] = db->full[0] = db->buff[0];
					PACK_LONG(db->fill[0], ninterms);
					residual_shipped = 1;
				}
				else {
					if ( MPI_Ssend(db->buff[0], 0, PF_WORD, next,
					               PF_MERGER_EXHAUSTED_MSGTAG, PF_COMM) != MPI_SUCCESS ) {
						M_free(recips,"merger recips"); return -1;
					}
				}
				continue;
			}
			if ( MPI_Iprobe(MASTER, PF_STEAL_STOP_MSGTAG, PF_COMM, &flag, &st)
			     == MPI_SUCCESS && flag ) {
				int s = MASTER, t = 0;
				PF_Receive(MASTER, PF_STEAL_STOP_MSGTAG, &s, &t);
				stop = 1;
				continue;
			}
			usleep(50);
		}
	}
	/* Drain any still-in-flight pipelined TERM sends before the buffers are
	   reused next module. */
	for ( i = 0; i < db->numbufs; i++ )
		if ( db->request[i] != MPI_REQUEST_NULL )
			MPI_Wait(&db->request[i], &db->retstat[i]);
	M_free(recips,"merger recips");
	return 0;
}

int PF_EndSort(void)
{
	GETIDENTITY
	/* Partitioned-output merger: write the merged stream to this merger's own
	   node-local scratch file instead of the global AR.outfile. Because
	   merger_outfile != AR.outfile/hidefile, every "fi==AR.outfile" MPI gate in
	   PutOut (sort.c:2111) and FlushOut (sort.c:2243) falls through to the disk
	   path automatically; only pf_emit_compressed_bulk needs an explicit
	   merger_to_file check (it gates purely on in_merger_phase). */
	FILEHANDLE *fout = ( PF.in_merger_phase && PF.merger_to_file && PF.merger_outfile )
	                   ? PF.merger_outfile : AR.outfile;
	SORTING *S = AT.SS;
	WORD *outterm,*pp;
	LONG size, noutterms;
	POSITION position, oldposition;
	WORD cc;
	int oldgzipCompress;

	if ( AT.SS != AT.S0 || !PF.parallel ) return 0;

	/* Three execution profiles share the merge-loop body below:
	     1. MASTER: collect terms from reducers (or mergers when G>0).
	     2. mapper-merger (PF.me in 1..G, PF.in_merger_phase == 1): collect
	        sorted streams from leaf reducers and forward to MASTER.
	     3. plain reducer / non-MR slave: prepare send buffer, then return so
	        the regular EndSort path can sort and forward via PutOut/FlushOut.
	   Cases 1 & 2 fall through to the shared merge body; case 3 returns now. */
	if ( PF.me != MASTER && !PF.in_merger_phase ) {
		if ( AC.sMRflag != NO_MAPREDUCE && PF.me < PF.nummappers )
			return 0; /* plain mapper: nothing to do here */
		if ((size = PF_allocateSbuf()) == 0 ) {
			MesPrint("[%d] ERROR in endsort: Failed to allocate send buffer", PF.me);
			return -1;
		}
		AR.CompressPointer = AR.CompressBuffer;
		*AR.CompressPointer = 0;
		return(0);
	}

	/* WaitAllSlaves is the generation/sort barrier and only meaningful on
	   MASTER. A mapper-merger arrives here already past its own
	   mapper-phase done-handshake; the master has been the one waiting on
	   it. Skipped during the off-parallel-exit gather (in_gather): there the
	   mappers/reducers are idle (returned at the off-parallel guard) and only
	   the mergers stream their files, so there is no mapper barrier to wait on
	   -- waiting would hang on done-sends that never come. */
	if ( PF.me == MASTER && !PF.in_gather )
	{
		PF_WaitAllSlaves();
		LONG cpu = TimeCPU(1);
		WORD cpart = (WORD)(cpu%1000);
		cpart /= 10;
		WORD rpart = cpu / 1000;
#ifdef PF_PROFILE
		MesPrint("[%d] PF_EndSort: All slaves started endsort Time: %7l.%2i", PF.me, rpart, cpart);
#else
		(void)rpart; (void)cpart;
#endif
	}

	/* Partitioned output: the mergers wrote their merged streams to node-local
	   files and report {count,size}; collect those instead of running the
	   loser-tree merge over merger streams (there are none). e->size stays the
	   prototype-only size on the master scratch; e->counter is the global sum.
	   The LAST-module gather (a later step) reads the files back. */
	if ( PF.me == MASTER && pf_output_partitioned() ) {
		LONG total = 0, total_bytes = 0;
		int got = 0;
		/* Per-merger output term counts -> each merger's INPUT count next module.
		   Seeds the load balancer's terms_left[]/live[] in PF_WaitAllSlaves
		   (PF_STEAL). Lazily allocated, length nummergers, indexed by merger
		   ordinal g-1 (merger_child_ranks[g]). */
		if ( PF.merger_total_terms == NULL && PF.nummergers > 0 )
			PF.merger_total_terms =
			    (LONG*)Malloc1((LONG)PF.nummergers*sizeof(LONG), "merger_total_terms");
		if ( PF.merger_total_terms )
			for ( int g = 0; g < PF.nummergers; g++ ) PF.merger_total_terms[g] = 0;
		/* (This whole partitioned branch is timed as MAS_MERGERDONE by the master's
		   EndSort wrapper in PF_Processor -- WaitAllSlaves + this collect + the
		   prototype FlushOut -- since pf_output_partitioned() is true here.) */
		/* Poll for each merger's PF_MERGERDONE while draining the leaf reducers'
		   PF_STDOUT/PF_LOG stats (from WriteStats). The master no longer runs its
		   loser-tree merge loop here, which is where that drain normally happens
		   (mpi.c PF_WaitRbuf). Without it the reducers' PF_RawSend(MASTER,...) in
		   WriteStats blocks, so they never finish forwarding to their merger and
		   the whole reducer->merger->done chain deadlocks. */
		while ( got < PF.nummergers ) {
			int flag = 0;
			MPI_Status st;
			if ( MPI_Iprobe(MPI_ANY_SOURCE, PF_MERGERDONE_MSGTAG, PF_COMM, &flag, &st)
			     == MPI_SUCCESS && flag ) {
				int rsrc = st.MPI_SOURCE, rtag = 0;
				LONG cnt = 0, sz = 0;
				PF_Receive(rsrc, PF_MERGERDONE_MSGTAG, &rsrc, &rtag);
				PF_Unpack(&cnt, 1, PF_LONG);
				PF_Unpack(&sz,  1, PF_LONG);
				total += cnt;
				total_bytes += sz;
				/* Record this merger's term count under its ordinal (seeds the
				   PF_STEAL load balancer's terms_left[]/live[]). */
				if ( PF.merger_total_terms && PF.merger_child_ranks )
					for ( int g = 1; g <= PF.nummergers; g++ )
						if ( PF.merger_child_ranks[g] == rsrc ) {
							PF.merger_total_terms[g-1] = cnt; break;
						}
				got++;
				continue;
			}
			{
				int drained = 0;
				if ( MPI_Iprobe(MPI_ANY_SOURCE, PF_STDOUT_MSGTAG, PF_COMM, &flag, &st)
				     == MPI_SUCCESS && flag ) {
					PF_ReceiveErrorMessage(st.MPI_SOURCE, PF_STDOUT_MSGTAG); drained = 1;
				}
				if ( MPI_Iprobe(MPI_ANY_SOURCE, PF_LOG_MSGTAG, PF_COMM, &flag, &st)
				     == MPI_SUCCESS && flag ) {
					PF_ReceiveErrorMessage(st.MPI_SOURCE, PF_LOG_MSGTAG); drained = 1;
				}
				if ( !drained ) usleep(200);
			}
		}
		/* Terminate the master scratch as a valid [prototype][0] expression so
		   the next module's GetTerm(AR.infile) reads the prototype then EOF. The
		   content lives in the merger files; only the prototype (written before
		   EndSort) sits in fout here. Mirror PF_EndSort's tail (SeekScratch +
		   FlushOut) but skip the loser-tree merge loop. */
		{
			POSITION position, oldposition;
			SeekScratch(fout, &position);
			oldposition = position;
			if ( FlushOut(&position, fout, 0) ) return(-1);
			DIFPOS(PF_exprsize, position, oldposition);
		}
		S->TermsLeft = PF_goutterms = total;
		MesPrint("[0] PF_EndSort: partitioned output -- %l terms, %l bytes across %d merger files",
		         total, total_bytes, (WORD)PF.nummergers);
		if ( PF.nummergers > 0 )
			MesPrint("[0] PF_EndSort: partitioned output -- mean %l bytes/merger",
			         total_bytes / (LONG)PF.nummergers);
		return 1;   /* skip InitTree + loser-tree merge */
	}
/*
		Now collect the terms of all slaves and merge them.
		PF_GetLoser gives the position of the smallest term, which is the real
		work. The smallest term needs to be copied to the outbuf: use PutOut.
*/
	PF_InitTree();
	if ( AR.PolyFun == 0 ) { S->PolyFlag = 0; }
	else if ( AR.PolyFunType == 1 ) { S->PolyFlag = 1; }
	else if ( AR.PolyFunType == 2 ) {
		if ( AR.PolyFunExp == 2
		  || AR.PolyFunExp == 3 ) S->PolyFlag = 1;
		else                      S->PolyFlag = 2;
	}
	*AR.CompressPointer = 0;
	SeekScratch(fout, &position);
	oldposition = position;
	oldgzipCompress = AR.gzipCompress;
	AR.gzipCompress = 0;

	noutterms = 0;

	/* Prefix-drain: PF_HASH_PREFIX_WORDS=K>0 enables both prefix routing
	   (mappers) and the master drain. Exploits the routing invariant --
	   any two terms with byte-equal first-K symbolic words land on the
	   same reducer. So once src wins with K-prefix P, every subsequent
	   src-term whose wire-format compression-share (against the just-
	   emitted term) exceeds K has the same K-prefix P, must be the
	   correct next emit (no other reducer can hold P), AND has wire
	   bytes that are already a valid delta against the output stream's
	   previous term. The drain copies those bytes straight to fout,
	   skipping both the master's loser-tree sift and PutOut's per-word
	   compression search. Master decisions drop from O(terms) to
	   O(distinct-K-prefix-buckets). Disabled when S->PolyFlag != 0
	   (PolyFun equal-terms path needs the full PF_GetLoser). */
	/* The prefix-drain runs on the master AND on mapper-mergers. The drain
	   invariant is K-prefix based: all terms sharing a K-prefix route to one
	   reducer, so each reducer's -- and thus each merger leaf-group's --
	   K-prefix set is disjoint; a drained run is guaranteed to be the whole
	   bucket and no other input stream can hold those terms.
	   pf_emit_compressed_bulk branches on PF.in_merger_phase: on the master
	   it WriteFile-flushes a full buffer to the scratch file; on a merger it
	   MPI-sends the full buffer upstream to MASTER (the merger's fout is
	   redirected to a send buffer). The K-cap on PF_GetLoser's CompareTerms
	   is likewise correct on the merger -- cross-leaf terms were routed to
	   different reducers, so they differ within the first K words. */
	int drain_active = ( AM.MR.HashPrefixWords > 0
	                  && S->PolyFlag == 0 );
	int dobracketindex = ( AR.sLevel <= 0
	                  && Expressions[AR.CurExpr].newbracketinfo
	                  && ( fout == AR.outfile || fout == AR.hidefile ) ) ? 1 : 0;

#if defined(WITHMPI) && defined(WITHZLIB)
	/* Block compression of the MR expression scratch on disk (no-op unless
	   PF_SCRATCH_COMPRESS=<level> is set; see the bc_* layer in tools.c). Two
	   on-disk writers, both routed through WriteFileToFile (the bc write hook) on
	   their rank, so FlushOut AND the K-prefix drain are compressed:
	     - the true master writes the global .sc0 (fout == AR.outfile);
	     - a merger writes its node-local partitioned file (fout == merger_outfile)
	       when merger_to_file is set; it is read back next module as merger_infile
	       and decompressed automatically (ReadFile/SeekFile are bc-intercepted and
	       the slot stays tracked across the per-module infile<->outfile swap).
	   The LAST-module gather merger redirects fout to an MPI send buffer (no file),
	   so it is excluded. Bracket-indexed output keeps random access -> uncompressed. */
	if ( PF.me == MASTER && !PF.in_merger_phase ) {
		PF_BcTrack(fout);
		PF_bc_compress_now = ( dobracketindex == 0 && fout != AR.hidefile );
	}
	else if ( PF.in_merger_phase && PF.merger_to_file && PF.merger_outfile ) {
		PF_BcTrack(fout);                      /* fout == merger_outfile here */
		PF_bc_compress_now = ( dobracketindex == 0 );
	}
#endif

	/* K-truncated CompareTerms for the master loser tree: cross-reducer
	   terms differ within the first K symbolic words by the routing
	   invariant, so capping Compare1's stoppers at K is correctness-
	   preserving for the loser-tree CompareTerms. PF_GetLoser applies
	   this transiently only around its own compare call -- the cap must
	   NOT leak to other CompareTerms calls (e.g. PutBracketInIndex's
	   bracket-equality check, which needs full compare). */
	pf_compare_kcap_want = drain_active ? (WORD)AM.MR.HashPrefixWords : 0;

	while ( PF_loser >= 0 ) {
		if ( (PF_loser = PF_GetLoser(PF_root)) == 0 ) break;
		outterm = PF_term[PF_loser];
		noutterms++;

		if ( PF_newclen[PF_loser] != 0 ) {
/*
			#[ this is only when new coeff was too long :
*/
			outterm = PF_WorkSpace;
			pp = PF_term[PF_loser];
			cc = *pp;
			while ( cc-- ) *outterm++ = *pp++;
			outterm = (outterm[-1] > 0) ? outterm-outterm[-1] : outterm+outterm[-1];
			if ( PF_newclen[PF_loser] > 0 ) cc =  (WORD)PF_newclen[PF_loser] - 1;
			else                            cc = -(WORD)PF_newclen[PF_loser] - 1;
			pp =  PF_newcpos[PF_loser];
			while ( cc-- ) *outterm++ = *pp++;
			*outterm++ = PF_newclen[PF_loser];
			*PF_WorkSpace = outterm - PF_WorkSpace;
			outterm = PF_WorkSpace;
			*PF_newcpos[PF_loser] = 0;
			PF_newclen[PF_loser] = 0;
/*
			#] this is only when new coeff was too long :
*/
		}
		PutOut(BHEAD outterm,&position,fout,1);

		/* Prefix-drain fast path. After PutOut emits T_i from stream src,
		   walk src's rbuf headers without decompressing: every consecutive
		   compressed term with share > K shares the same K-prefix as T_i
		   (routing invariant => no other reducer can hold these terms; the
		   reducer has already emitted them in canonical order). Bulk-memcpy
		   the run's compressed bytes to the output, then let the NEXT
		   PF_GetLoser call do the boundary refill via its normal PF_PutIn
		   path -- reading PF_term[src] (= scratch T_i) as lastterm is safe
		   since boundary_share <= K and T_i[1..K] == T_{i+k}[1..K]. */
		/* Disable drain when bracket index is active. The bulk-emit's per-term
		   PutBracketInIndex pass would receive T_i as a proxy for every drained
		   term, but T_i's bracket only matches the actual term's bracket when
		   HAAKJE lies within the first K+1 words. For Spin (Keep Brackets,
		   Collect bracket1) HAAKJE can be past K -- different actual brackets
		   would be collapsed into one index entry. Drain skip is correct;
		   normal PutOut handles bracket indexing per-term. */
		if ( drain_active && !AR.NoCompress && !dobracketindex ) {
			int src = PF_loser;
			PF_BUFFER *rbuf = PF.rbufs[src];
			/* Snapshot T_i NOW, before the scan's cross-chunk IRecv can overwrite
			   its rbuf slot. T_i was just emitted by PutOut so *PF_term[src] is a
			   valid positive length here. Copy only the first K+1 words -- all the
			   boundary term's PF_PutIn decompression ever reads (lastterm[1..share],
			   share <= K). Doing this at drain ENTRY (not end) is the fix: at end
			   the value may already be clobbered to a -share header. */
			LONG  lastterm_snap_len = (LONG)*PF_term[src];
			if ( lastterm_snap_len > 0 ) {
				LONG ncopy = lastterm_snap_len;
				LONG cap   = (LONG)(AM.MaxTer / sizeof(WORD));
				if ( ncopy > (LONG)AM.MR.HashPrefixWords + 1 ) ncopy = (LONG)AM.MR.HashPrefixWords + 1;
				if ( ncopy > cap ) ncopy = cap;
				if ( PF_drain_lastterm == NULL ) {
					PF_drain_lastterm = (WORD*)Malloc1(AM.MaxTer, "PF_drain_lastterm");
					PF_drain_lastterm_cap = (int)cap;
				}
				memcpy(PF_drain_lastterm, PF_term[src], (size_t)(ncopy * (LONG)sizeof(WORD)));
			}
			int a = rbuf->active;
			WORD *run_start = rbuf->fill[a];
			WORD *stop = rbuf->full[a];
			WORD *q = run_start;
			int n_drained = 0;
			int K = AM.MR.HashPrefixWords;
			int boundary_found = 0;
			int workerIdx = pf_loser_src_to_rank(src);
			/* Scan forward through compressed term headers [-share, tail_len]
			   until a REAL boundary (share <= K, EOF marker, or uncompressed
			   term). When the scan stalls because the next term's body
			   straddles the active chunk end, mirror PF_PutIn's boundary
			   dance: emit the run so far, copy the partial term into the
			   next rbuf slot's reserved zone, wait on the next slot, post a
			   fresh IRecv on the old active slot, swap active = next, then
			   resume scanning. PF_term[src] (= T_i) stays the shared
			   decompression anchor for the entire run. */
			while ( !boundary_found ) {
				while ( q + 2 <= stop ) {
					int share, compressed_len;
					if ( *q == 0 ) { boundary_found = 1; break; }   /* EOF marker */
					if ( *q > 0 ) { boundary_found = 1; break; }    /* uncompressed term */
					share = (int)(-(*q));
					if ( share <= K ) { boundary_found = 1; break; } /* prefix-group end */
					compressed_len = 2 + (int)q[1];
					if ( q + compressed_len > stop ) break;          /* cross-chunk -- handle below */
					q += compressed_len;
					n_drained++;
				}
				if ( boundary_found ) break;

				/* Cross-chunk path. Emit the accumulated run (if any), then
				   relocate the partial term [q, stop) to the next slot. */
				if ( n_drained > 0 ) {
					int nwords = (int)(q - run_start);
					if ( pf_emit_compressed_bulk(run_start, nwords, n_drained,
					                             fout, &position) < 0 ) {
						AR.gzipCompress = oldgzipCompress;
						return(-1);
					}
					rbuf->fill[a] = q;
					noutterms += n_drained;
					n_drained = 0;
				}

				{
					int next = (a+1 >= rbuf->numbufs) ? 0 : a+1;
					WORD *m1 = rbuf->buff[next] + AM.MaxTer/sizeof(WORD) + 1;
					WORD *m2 = stop - 1;
					LONG size;
					int tag;
					while ( m2 >= q ) *m1-- = *m2--;
					rbuf->fill[next] = m1 + 1;

					if ( rbuf->numbufs == 1 ) {
						rbuf->full[a] = rbuf->buff[a] + AM.MaxTer/sizeof(WORD) + 2;
						PF_IRecvRbuf(rbuf, a, workerIdx);
					}

					rbuf->full[next] = rbuf->buff[next] + AM.MaxTer/sizeof(WORD) + 2;
					{
#ifdef PF_PROFILE
						int _pf_rw = PF.in_merger_phase ? PF_PHASE_MER_RECV_WAIT
						                                : PF_PHASE_MAS_MERGE_RECV_WAIT;
#endif
						PF_TIMER_BEGIN_RT(rw);
						tag = PF_WaitRbuf(rbuf, next, &size);
						PF_TIMER_END_RT(rw, _pf_rw);
					}
					rbuf->full[next] += size;
					if ( tag == PF_ENDBUFFER_MSGTAG ) {
						*rbuf->full[next]++ = 0;
					}
					else if ( rbuf->numbufs > 1 ) {
						rbuf->full[a] = rbuf->buff[a] + AM.MaxTer/sizeof(WORD) + 2;
						PF_IRecvRbuf(rbuf, a, workerIdx);
					}
					a = rbuf->active = next;
					q = run_start = rbuf->fill[a];
					stop = rbuf->full[a];
				}
			}

			/* Final emit for the run accumulated in the current active chunk. */
			if ( n_drained > 0 ) {
				int nwords = (int)(q - run_start);
				if ( pf_emit_compressed_bulk(run_start, nwords, n_drained,
				                             fout, &position) < 0 ) {
					AR.gzipCompress = oldgzipCompress;
					return(-1);
				}
				rbuf->fill[a] = q;
				noutterms += n_drained;
			}
			/* Repoint PF_term[src] at the stable T_i snapshot captured at drain
			   entry. PF_PutIn for the boundary term reads only lastterm[1..share]
			   (share <= K), and T_i[1..K] == T_{i+k}[1..K] by the routing
			   invariant, so the snapshot is a correct decompression anchor.
			   CRITICAL: T_i lived in rbuf, whose cyclic slots the drain's
			   cross-chunk IRecv may have overwritten -- pointing the next
			   PF_PutIn at the original rbuf location decodes the boundary term
			   against garbage (corrupt subterms -> SIGSEGV in the next PutOut). */
			if ( lastterm_snap_len > 0 ) PF_term[src] = PF_drain_lastterm;
		}
	}
	pf_compare_kcap_want = 0;   /* no more loser-tree compares this module */
	if ( FlushOut(&position,fout,0) ) {
		AR.gzipCompress = oldgzipCompress;
		return(-1);
	}
	M_free(PF_root, "PF_root");
	PF_root = NULL;
	S->TermsLeft = PF_goutterms = noutterms;
	DIFPOS(PF_exprsize, position, oldposition);
	AR.gzipCompress = oldgzipCompress;
	return(1);
}

/*
 		#] PF_EndSort : 
  	#] sort.c : 
  	#[ proces.c :
 		#[ variables :
*/

static  WORD *PF_CurrentBracket;

/*
 		#] variables : 
 		#[ PF_GetTerm :
*/

/**
 * Replaces GetTerm() on the slaves, which get their terms from the master,
 * not the infile anymore, is nonblocking and buffered ...
 * use AR.infile->PObuffer as buffer. For the moment, don't care
 * about compression, since terms come uncompressed from master.
 *
 * To enable keep-brackets when AR.DeferFlag is set, we need to do some
 * preparation here:
 *   \li  copy the part outside brackets to current_bracket
 *   \li  skip term if part outside brackets is same as for last term
 *   \li  if POfill >= POfull receive new terms as usual
 *
 * Different from GetTerm() we use an extra buffer for the part outside brackets:
 * PF_CurrentBracket.
 *
 * @param[out]  term  the buffer to store the next term.
 * @return            the length of the next term.
 */
static WORD PF_GetTerm(WORD *term)
{
	GETIDENTITY
	FILEHANDLE *fi = AC.RhsExprInModuleFlag && PF.rhsInParallel ? &PF.slavebuf : AR.infile;
	WORD i;
	WORD *next, *np, *last, *lp = 0, *nextstop, *tp=term;

	/* Only on the slaves. */

	AN.deferskipped = 0;
	if ( fi->POfill >= fi->POfull || fi->POfull == fi->PObuffer ) {
ReceiveNew:
	  {
/*
 		#[ receive new terms from master :
*/
		int src = PF.input_src, tag;
		int follow = 0;
		LONG size,cpu,space = 0;

		if ( PF.log ) {
			fprintf(stderr,"[%d] Starting to send to Master\n",PF.me);
			fflush(stderr);
		}

		cpu = TimeCPU(1);
		PF_PreparePack();
		PF_Pack(&cpu               ,1,PF_LONG);
		PF_Pack(&space             ,1,PF_LONG);
		PF_Pack(&PF_linterms       ,1,PF_LONG);
		PF_Pack(&(AM.S0->GenTerms) ,1,PF_LONG);
		PF_Pack(&(AM.S0->TermsLeft),1,PF_LONG);
		PF_Pack(&follow            ,1,PF_INT );

		if ( PF.log ) {
			fprintf(stderr,"[%d] Now sending with tag = %d\n",PF.me,PF_READY_MSGTAG);
			fflush(stderr);
		}

		PF_Send(src, PF_READY_MSGTAG);

		if ( PF.log ) {
			fprintf(stderr,"[%d] returning from send\n",PF.me);
			fflush(stderr);
		}

		size = fi->POstop - fi->PObuffer - 1;
#ifdef AbsolutelyExtra
		PF_Receive(MASTER,PF_ANY_MSGTAG,&src,&tag);
#ifdef MPI2
		if ( tag == PF_TERM_MSGTAG ) {
			PF_Unpack(&size, 1, PF_LONG);
			if ( PF_Put_target(src) == 0 ) {
				printf("PF_Put_target error ...\n");
			}
		}
		else {
			PF_RecvWbuf(fi->PObuffer,&size,&src);
		}
#else
		PF_RecvWbuf(fi->PObuffer,&size,&src);
#endif
#endif
		PF_TIMER_BEGIN(MAP_GETTERM_WAIT);
		tag=PF_RecvWbuf(fi->PObuffer,&size,&src);
		PF_TIMER_END(MAP_GETTERM_WAIT);

		/* Work-stealing steer replies (PF_STEAL). The mapper asks its current
		   target PF.input_src -- a merger (for a bucket) or the master (to be
		   steered). Neither steer reply carries terms, so retarget and loop back
		   to re-ask, without processing a bucket:
		     PF_ASSIGN(E)        -- the master steers us to merger E;
		     PF_MERGER_EXHAUSTED -- our merger drained, go ask the master next.
		   PF_TERM / PF_ENDSORT fall through to the normal path below; an ENDSORT
		   from the master is the standard terminate, identical to the non-steal
		   case (PF_WaitAllSlaves). */
		if ( PF.steal ) {
			if ( tag == PF_ASSIGN_MSGTAG ) {
				LONG new_merger; WORD *p = fi->PObuffer; UNPACK_LONG(p, new_merger);
				PF.input_src = (int)new_merger;
				goto ReceiveNew;
			}
			if ( tag == PF_MERGER_EXHAUSTED_MSGTAG ) {
				PF.input_src = MASTER;
				goto ReceiveNew;
			}
		}

		fi->POfill = fi->PObuffer;
		/* Get AN.ninterms which sits in the first 2 WORDs. */
		{
			LONG ninterms;
			UNPACK_LONG(fi->POfill, ninterms);
			if ( fi->POfill < fi->POfull ) {
				DBGOUT_NINTERMS(2, ("PF.me=%d AN.ninterms=%d PF_linterms=%d ninterms=%d GET\n", (int)PF.me, (int)AN.ninterms, (int)PF_linterms, (int)ninterms));
				AN.ninterms = ninterms - 1;
			} else {
				DBGOUT_NINTERMS(2, ("PF.me=%d AN.ninterms=%d PF_linterms=%d ninterms=%d GETEND\n", (int)PF.me, (int)AN.ninterms, (int)PF_linterms, (int)ninterms));
			}
		}
		fi->POfull = fi->PObuffer + size;
		if ( tag == PF_ENDSORT_MSGTAG ) *fi->POfull++ = 0;
/*
 		#] receive new terms from master :
*/
	  }
	  if ( PF_CurrentBracket ) *PF_CurrentBracket = 0;
	}
	if ( *fi->POfill == 0 ) {
		fi->POfill = fi->POfull = fi->PObuffer;
		*term = 0;
		goto RegRet;
	}
	if ( AR.DeferFlag ) {
		if ( !PF_CurrentBracket ) {
/*
 		#[ alloc space :
*/
			PF_CurrentBracket =
					(WORD*)Malloc1(AM.MaxTer,"PF_CurrentBracket");
			*PF_CurrentBracket = 0;
/*
 		#] alloc space : 
*/
		}
		while ( *PF_CurrentBracket ) {  /* "for each term in the buffer" */
/*
 		#[ test : bracket & skip if it's equal to the last in PF_CurrentBracket
*/
			next = fi->POfill;
			nextstop = next + *next; nextstop -= ABS(nextstop[-1]);
			next++;
			last = PF_CurrentBracket+1;
			while ( next < nextstop ) {
/*
					scan the next term and PF_CurrentBracket
*/
				if ( *last == HAAKJE && *next == HAAKJE ) {
/*
					the part outside brackets is equal => skip this term
*/
					PRINTFBUF("PF_GetTerm skips",fi->POfill,*fi->POfill);
					break;
				}
/*
					check if the current subterms are equal
*/
				np = next; next += next[1];
				lp = last; last += last[1];
				while ( np < next ) if ( *lp++ != *np++ ) goto strip;
			}
/*
				go on to next term
*/
			fi->POfill += *fi->POfill;
			AN.deferskipped++;
/*
				the usual checks
*/
			if ( fi->POfill >= fi->POfull || fi->POfull == fi->PObuffer )
				goto ReceiveNew;
			if ( *fi->POfill == 0 ) {
				fi->POfill = fi->POfull = fi->PObuffer;
				*term = 0;
				goto RegRet;
			}
/*
 		#] test : 
*/
		}
/*
 		#[ copy :

		this term to CurrentBracket and the part outside of bracket
		to WorkSpace at term
*/
strip:
		next = fi->POfill;
		nextstop = next + *next; nextstop -= ABS(nextstop[-1]);
		next++;
		tp++;
		lp = PF_CurrentBracket + 1;
		while ( next < nextstop ) {
			if ( *next == HAAKJE ) {
				fi->POfill += *fi->POfill;
				while ( next < fi->POfill ) *lp++ = *next++;
				*PF_CurrentBracket = lp - PF_CurrentBracket;
				*lp = 0;
				*tp++ = 1;
				*tp++ = 1;
				*tp++ = 3;
				*term = WORDDIF(tp,term);
				PRINTFBUF("PF_GetTerm new brack",PF_CurrentBracket,*PF_CurrentBracket);
				PRINTFBUF("PF_GetTerm POfill",fi->POfill,*fi->POfill);
				goto RegRet;
			}
			np = next; next += next[1];
			while ( np < next ) *tp++ = *lp++ = *np++;
		}
		tp = term;
/*
 		#] copy : 
*/
	}

	i = *fi->POfill;
	while ( i-- ) *tp++ = *fi->POfill++;
RegRet:
	//PRINTFBUF("PF_GetTerm returns",term,*term);
	return(*term);
}

/*
 		#] PF_GetTerm : 
 		#[ PF_Deferred :
*/

/**
 * Replaces Deferred() on the slaves.
 *
 * @param  term   the term that must be multiplied by the contents of
 *                the current bracket.
 * @param  level  the compiler level.
 * @return        0 if OK, nonzero on error.
 */
WORD PF_Deferred(WORD *term, WORD level)
{
	GETIDENTITY
	WORD *bra, *bstop;
	WORD *tstart;
	FILEHANDLE *fi = AC.RhsExprInModuleFlag && PF.rhsInParallel ? &PF.slavebuf : AR.infile;
	WORD *next = fi->POfill;
	WORD *termout = AT.WorkPointer;
	WORD *oldwork = AT.WorkPointer;

	AT.WorkPointer = (WORD *)((UBYTE *)(AT.WorkPointer) + AM.MaxTer);
	AR.DeferFlag = 0;

	PRINTFBUF("PF_Deferred (Term)   ",term,*term);
	PRINTFBUF("PF_Deferred (Bracket)",PF_CurrentBracket,*PF_CurrentBracket);

	bra = bstop = PF_CurrentBracket;
	if ( *bstop > 0 ) {
		bstop += *bstop;
		bstop -= ABS(bstop[-1]);
	}
	bra++;
	while ( *bra != HAAKJE && bra < bstop ) bra += bra[1];
	if ( bra >= bstop ) {	/* No deferred action! */
		AT.WorkPointer = term + *term;
		if ( Generator(BHEAD term,level) ) goto DefCall;
		AR.DeferFlag = 1;
		AT.WorkPointer = oldwork;
		return(0);
	}
	bstop = bra;
	tstart = bra + bra[1];
	bra = PF_CurrentBracket;
	tstart--;
	*tstart = bra + *bra - tstart;
	bra++;
/*
		Status of affairs:
		First bracket content starts at tstart.
		Next term starts at next.
		The outside of the bracket runs from bra = PF_CurrentBracket to bstop.
*/
	for(;;) {
		if ( InsertTerm(BHEAD term,0,AM.rbufnum,tstart,termout,0) < 0 ) {
			goto DefCall;
		}
/*
			call Generator with new composed term
*/
		AT.WorkPointer = termout + *termout;
		if ( Generator(BHEAD termout,level) ) goto DefCall;
		AT.WorkPointer = termout;
		tstart = next + 1;
		if ( tstart >= fi->POfull ) goto ThatsIt;
		next += *next;
/*
			compare with current bracket
*/
		while ( bra <= bstop ) {
			if ( *bra != *tstart ) goto ThatsIt;
			bra++; tstart++;
		}
/*
			now bra and tstart should both be a HAAKJE
*/
		bra--; tstart--;
		if ( *bra != HAAKJE || *tstart != HAAKJE ) goto ThatsIt;
		tstart += tstart[1];
		tstart--;
		*tstart = next - tstart;
		bra = PF_CurrentBracket + 1;
	}

ThatsIt:
/*
	AT.WorkPointer = oldwork;
*/
	AR.DeferFlag = 1;
	return(0);
DefCall:
	MesCall("PF_Deferred");
	SETERROR(-1);
}

/*
 		#] PF_Deferred : 
 		#[ PF_Wait4Slave :
*/

static LONG **PF_W4Sstats = 0;

/**
 * Waits for the slave \a src to accept terms.
 *
 * @param  src  the slave for waiting (can be PF_ANY_SOURCE).
 * @return      the idle slave.
 */
static int PF_Wait4Slave(int src)
{
	int j, tag, next;

	tag = PF_ANY_MSGTAG;
	PF_CatchErrorMessages(&src, &tag);
	PF_Receive(src, tag, &next, &tag);

	if (tag == PF_BUFFER_MSGTAG ) {
		return next;
	}
	if ( tag != PF_READY_MSGTAG ) {
		MesPrint("[%d] PF_Wait4Slave: received MSGTAG %d",(WORD)PF.me,(WORD)tag);
		return(-1);
	}
	if ( PF_W4Sstats == 0 ) {
		PF_W4Sstats = (LONG**)Malloc1(sizeof(LONG*),"");
		PF_W4Sstats[0] = (LONG*)Malloc1(PF_STATS_SIZE*sizeof(LONG),"");
	}
	PF_Unpack(PF_W4Sstats[0],PF_STATS_SIZE,PF_LONG);
	PF_Statistics(PF_W4Sstats,next);

	PF_Unpack(&j,1,PF_INT);

	if ( j ) {
/*
		actions depending on rest of information in last message
*/
	}
	return(next);
}

/*
 		#] PF_Wait4Slave : 
 		#[ PF_Wait4SlaveIP :
*/
/*
	array of expression numbers for PF_InParallel processor.
	Each time the master sends expression "i" to the slave
	"next" it sets partodoexr[next]=i:
*/
static WORD *partodoexr=NULL;

/**
 * InParallel version of PF_Wait4Slave(). Returns tag as src.
 *
 * @param[in,out]  src  the slave for waiting (can be PF_ANY_SOURCE).
 *                      As output, the tag value of the idle slave.
 * @return              the idle slave.
 */
static int PF_Wait4SlaveIP(int *src)
{
	int j,tag,next;

	tag = PF_ANY_MSGTAG;
	PF_CatchErrorMessages(src, &tag);
	PF_Receive(*src, tag, &next, &tag);
	*src=tag;
	if ( PF_W4Sstats == 0 ) {
		PF_W4Sstats = (LONG**)Malloc1(sizeof(LONG*),"");
		PF_W4Sstats[0] = (LONG*)Malloc1(PF_STATS_SIZE*sizeof(LONG),"");
	}

	PF_Unpack(PF_W4Sstats[0],PF_STATS_SIZE,PF_LONG);
	if ( tag == PF_DATA_MSGTAG )
		AR.CurExpr = partodoexr[next];
	PF_Statistics(PF_W4Sstats,next);

	PF_Unpack(&j,1,PF_INT);

	if ( j ) {
	/* actions depending on rest of information in last message */
	}

	return(next);
}
/*
 		#] PF_Wait4SlaveIP :
 		#[ PF_WaitAllSlaves :
*/

/* Map a merger's global rank to its 0-based ordinal (index into the master's
   per-merger work-stealing arrays live[]/terms_left[]/helpers[]), or -1 if the
   rank is not a merger. */
static int pf_merger_ordinal(int rank)
{
	int g;
	for ( g = 1; g <= PF.nummergers; g++ )
		if ( PF.merger_child_ranks && PF.merger_child_ranks[g] == rank ) return g - 1;
	return -1;
}

/**
 * Waits until all slaves are ready to send terms back to the master.
 * If some slave is not working, it sends PF_ENDSORT_MSGTAG and waits for the answer.
 * Messages from slaves will be read only after all slaves are ready,
 * further in caller function.
 *
 * @return  0 if OK, nonzero on error.
 */
static int PF_WaitAllSlaves(void)
{
	int i, readySlaves, tag, next = PF_ANY_SOURCE;
	UBYTE *has_sent = 0;

	/* Work-stealing steer state (PF_STEAL on an input-partitioned module). The
	   master steers idle mappers to the merger that needs help most. Per merger
	   ordinal g (0-based): live[g] = still has work; terms_left[g] = the master's
	   estimate, seeded from this module's per-merger input counts and refined by
	   PF_MERGER_PROGRESS; helpers[g] = mappers currently feeding it, seeded with its
	   node-local mapper count and ++ on each steer. plain_done counts plain
	   (non-merger) mappers that have finished; at num_plain we broadcast
	   PF_STEAL_STOP so drained mergers leave their responder phase. */
	int steal_active = ( PF.steal && pf_input_partitioned() && PF.nummergers > 0 );
	UBYTE *live = 0; LONG *terms_left = 0; int *helpers = 0;
	int plain_done = 0, num_plain = 0, stop_sent = 0;
	if ( steal_active ) {
		int g, r;
		live       = (UBYTE*)Malloc1(sizeof(UBYTE)*PF.nummergers,"PF_steal live");
		terms_left = (LONG*) Malloc1(sizeof(LONG) *PF.nummergers,"PF_steal terms_left");
		helpers    = (int*)  Malloc1(sizeof(int)  *PF.nummergers,"PF_steal helpers");
		//initialize live[] and terms_left[] from the master's per-merger input counts, and helpers[] from node-local mapper counts:
		for ( g = 0; g < PF.nummergers; g++ ) {
			LONG merger_terms = ( PF.merger_total_terms ) ? PF.merger_total_terms[g] : 0;
			terms_left[g] = merger_terms;
			live[g] = ( merger_terms > 0 ) ? 1 : 0;
			helpers[g] = 0;
		}
		/* Seed helpers[g] = merger g's node-local mapper count (its initial feeders,
		   attached without going through the master). */
		if ( PF.rank_node && PF.merger_child_ranks ) {
			for ( g = 0; g < PF.nummergers; g++ ) {
				int mr = PF.merger_child_ranks[g+1], nd = PF.rank_node[mr];
				for ( r = 1; r < PF.nummappers; r++ )
					if ( r != mr && PF.rank_node[r] == nd ) helpers[g]++;
				if ( helpers[g] < 1 ) helpers[g] = 1;   /* avoid div-by-zero */
			}
		}
		else for ( g = 0; g < PF.nummergers; g++ ) helpers[g] = 1;
		num_plain = PF.nummappers - 1 - PF.nummergers;
	}

	//allocate an arraay for all the slaves (mappers and reducers) - numtaks + 1- for each slave and for
	has_sent = (UBYTE*)Malloc1(sizeof(UBYTE)*(PF.numtasks + 1),"PF_WaitAllSlaves");

	for ( i = 0; i < PF.numtasks; i++ ) has_sent[i] = 0; // reset array
	for ( readySlaves = 1; readySlaves < PF.numtasks; ) { //loop until all expected slaves are ready
		if ( next != PF_ANY_SOURCE) { /*Go to the next slave:*/
			do{ /*Note, here readySlaves<PF.numtasks, so this loop can't be infinite*/
				if ( ++next >= PF.numtasks ) next = 1;
			} while ( has_sent[next] == 1 );
		}
/*
			Here PF_ProbeWithCatchingErrorMessages() is BLOCKING function if next = PF_ANY_SOURCE:
*/
		tag = PF_ProbeWithCatchingErrorMessages(&next); //only probing
/*
			Here next != PF_ANY_SOURCE
*/
		switch ( tag ) {
			case PF_BUFFER_MSGTAG:
			case PF_ENDBUFFER_MSGTAG:
/*
					Slaves are ready to send their results back
*/
				if ( has_sent[next] == 0 ) {
					has_sent[next] = 1;
					readySlaves++;
				}
				else {  /*error?*/
					fprintf(stderr,"ERROR next=%d tag=%d\n",next,tag);
				}
				/* Work-stealing: a finishing plain (non-merger) mapper is one fewer
				   stealer. When the last finishes, no more PF_READY will come, so
				   release the mergers from their responder phase with PF_STEAL_STOP. */
				if ( steal_active && next >= 1 && next < PF.nummappers
				     && pf_merger_ordinal(next) < 0 ) {
					if ( ++plain_done >= num_plain && !stop_sent ) {
						int g; PF_PreparePack();
						for ( g = 1; g <= PF.nummergers; g++ )
							PF_Send(PF.merger_child_ranks[g], PF_STEAL_STOP_MSGTAG);
						stop_sent = 1;
					}
				}
				if ( AC.sMRflag != NO_MAPREDUCE && next < PF.nummappers)
					PF_Wait4Slave(next);
				/* Merger tier: leaf reducer's data goes to a merger, not to
				   master, so master's PF_InitTree Irecv from this leaf never
				   exists. The leaf's explicit PF_Send handshake (sent from
				   PF_ForwardTermsToMaster) needs to be drained here so the
				   leaf can unblock and proceed into its sort. */
				else if ( AC.sMRflag != NO_MAPREDUCE
				          && PF.nummergers > 0
				          && next >= PF.nummappers )
					PF_Wait4Slave(next);
				//MesPrint("[0] PF_WaitAllSlaves: %d starts endsort", next);
/*
					Note, we do NOT read results here! Messages from these slaves will be read
					only after all slaves are ready, further in caller function
*/
				break;
			case 0:
/*
					The slave is not ready. Just go to the next slave.
					It may appear that there are no more ready slaves, and the master
					will wait them in infinite loop. Stupid situation - the master can
					receive buffers from ready slaves!
*/
#ifdef PF_WITH_SCHED_YIELD
/*
						Relinquish the processor:
*/
					sched_yield();
#endif
				break;
			case PF_DATA_MSGTAG:
				tag=next;
				next=PF_Wait4SlaveIP(&tag);
/*
	tag must be == PF_DATA_MSGTAG!
*/
				PF_Statistics(PF_stats,0);
				PF_Slave2MasterIP(next);
				PF_Master2SlaveIP(next,NULL);
				if ( has_sent[next] == 0 ) {
					has_sent[next]=1;
					readySlaves++;
				}else{
					/*error?*/
					fprintf(stderr,"ERROR next=%d tag=%d\n",next,tag);
				}/*if ( has_sent[next] == 0 )*/
				break;
			case PF_EMPTY_MSGTAG:
				tag=next;
				next=PF_Wait4SlaveIP(&tag);
/*
	tag must be == PF_EMPTY_MSGTAG!
*/
				PF_Master2SlaveIP(next,NULL);
				if ( has_sent[next] == 0 ) {
					has_sent[next]=1;
					readySlaves++;
				}else{
					/*error?*/
					fprintf(stderr,"ERROR next=%d tag=%d\n",next,tag);
				}/*if ( has_sent[next] == 0 )*/
				break;
			/* Work-stealing: a merger reports progress -- terms shipped so far (>=0),
			   from which we refine terms_left = initial - shipped, or -1 to say it has
			   FINISHED (drop it from the live set so we stop steering to it). One-way,
			   no reply; not a done-send, so readySlaves is unchanged. */
			case PF_MERGER_PROGRESS_MSGTAG:
				{
					int psrc = 0, ptag = 0, g; LONG shipped = 0;
					PF_Receive(next, PF_MERGER_PROGRESS_MSGTAG, &psrc, &ptag);
					PF_Unpack(&shipped, 1, PF_LONG);
					g = pf_merger_ordinal(next);
					if ( steal_active && g >= 0 ) {
						if ( shipped < 0 ) live[g] = 0;
						else {
							LONG c = ( PF.merger_total_terms ) ? PF.merger_total_terms[g] : 0;
							terms_left[g] = ( c > shipped ) ? c - shipped : 0;
						}
					}
				}
				break;
			// all mappers sent ready for the next term chunk. We need to tell them to stop.
			case PF_READY_MSGTAG:
/*
					idle slave
					May be only PF_READY_MSGTAG:
*/
				/* Work-stealing: on an input-partitioned module a PF_READY at the
				   master comes from an idle stealer (its merger drained, or its node
				   never had one). Steer it to the live merger that needs help most
				   (max terms_left/helpers) by replying PF_ASSIGN(rank); if none is
				   live, terminate it with the standard minimal PF_ENDSORT end-marker
				   (PF.sbufs[0] is not built in bypass mode, hence the direct send). */
				if ( steal_active ) {
					int g, best = -1;
					next = PF_Wait4Slave(next);
					if ( next == -1 ) return(next);
					for ( g = 0; g < PF.nummergers; g++ ) {
						if ( !live[g] ) continue;
						if ( best < 0 ||
						     terms_left[g]*(LONG)helpers[best] > terms_left[best]*(LONG)helpers[g] )
							best = g;
					}
					if ( best >= 0 ) {
						WORD abuf[4]; WORD *ap = abuf;
						LONG erank = PF.merger_child_ranks[best+1];
						helpers[best]++;
						PACK_LONG(ap, erank);
						if ( MPI_Ssend(abuf, (int)(ap-abuf), PF_WORD, next,
						               PF_ASSIGN_MSGTAG, PF_COMM) != MPI_SUCCESS ) return(-1);
					}
					else {
						WORD ebuf[8]; WORD *ep = ebuf;
						PACK_LONG(ep, (LONG)0);
						if ( MPI_Ssend(ebuf, (int)(ep-ebuf), PF_WORD, next,
						               PF_ENDSORT_MSGTAG, PF_COMM) != MPI_SUCCESS ) return(-1);
					}
					break;
				}
				next = PF_Wait4Slave(next);
				if ( next == -1 ) return(next); /*Cannot be!*/
				if ( has_sent[0] == 0 ) {  /*Send the last chunk to the slave*/
					PF.sbufs[0]->active = 0;
					has_sent[0] = 1;
				}
				else {
/*
						Last chunk was sent, so just send to slave ENDSORT
						AN.ninterms must be sent because the slave expects it:
*/
					PACK_LONG(PF.sbufs[0]->fill[next], AN.ninterms);
/*
						This will tell to the slave that there are no more terms:
*/
					*(PF.sbufs[0]->fill[next])++ = 0;
					PF.sbufs[0]->active = next;
				}
/*
					Send ENDSORT
*/
				PF_ISendSbuf(next,PF_ENDSORT_MSGTAG);
				break;
			default:
/*
					Error?
					Indicates the error. This will force exit from the main loop:
*/
				MesPrint("!!!Unexpected MPI message src=%d tag=%d.", next, tag);
				readySlaves = PF.numtasks;  /* force exit */
				break;
		}
	}

	if ( has_sent ) M_free(has_sent,"PF_WaitAllSlaves");
	if ( live ) M_free(live,"PF_steal live");
	if ( terms_left ) M_free(terms_left,"PF_steal terms_left");
	if ( helpers ) M_free(helpers,"PF_steal helpers");
/*
		0 on success (exit from the main loop by loop condition), or -1 if fails
*/
	return(PF.numtasks - readySlaves);
}

/*
 		#] PF_WaitAllSlaves :
 		#[ off-parallel-exit gather :
*/

/* Off-parallel-exit gather (merger side). Stream this merger's node-local file
   (PF.merger_infile = the partitioned chain's accumulated output) upstream to the
   master, which loser-tree merges the G streams. Mirrors PF_MergerLoop's gather
   forward (in_merger_phase + PutOut/FlushOut to MASTER) but sources terms from the
   file via GetOneTerm instead of leaf reducers. Every merger sends exactly one
   stream (BUFFER...ENDBUFFER), even if its file is empty, so the master's G-way
   loser tree receives all of its inputs. */
static int pf_merger_gather_to_master(void)
{
	GETIDENTITY
	FILEHANDLE *fout = AR.outfile;
	WORD *sv_PObuffer = fout->PObuffer, *sv_POstop = fout->POstop;
	LONG  sv_POsize   = fout->POsize;
	WORD *sv_POfill   = fout->POfill, *sv_POfull = fout->POfull;
	WORD *term = AT.WorkPointer, *save_cp;
	POSITION pos, rpos;
	int rc = 0;

	NewSort(BHEAD0);
	PF.parallel = 1;
	PF.in_merger_phase = 1;
	PF.merger_to_file  = 0;            /* forward upstream to MASTER, not to a file */
	if ( PF_allocateSbuf() == 0 ) { PF.in_merger_phase = 0; return -1; }

	SeekScratch(fout, &pos);
	PUTZERO(rpos);
	save_cp = AR.CompressPointer;

	/* GetOneTerm (read) and PutOut (write) BOTH use AR.CompressPointer (against the
	   AR.ComprTop bound) for their delta state, and here they are interleaved per
	   term -- GetOneTerm's just-decoded term would become the "previous written
	   term" PutOut compresses against, so every term compresses against itself.
	   Give the read its OWN scratch buffer (CompressPointer+ComprTop swapped each
	   iteration) so PutOut's output-delta cursor (put_cp into the real
	   AR.CompressBuffer) is preserved across iterations. */
	WORD *real_top     = AR.ComprTop;
	WORD *read_scratch = (WORD*)Malloc1((AM.CompressSize+10)*sizeof(WORD), "gather read cbuf");
	if ( read_scratch == NULL ) { PF.in_merger_phase = 0; return -1; }
	WORD *read_top = read_scratch + AM.CompressSize;
	AR.CompressPointer = AR.CompressBuffer; *AR.CompressPointer = 0;   /* PutOut: no previous term yet */
	WORD *put_cp = AR.CompressPointer;
	PF_TIMER_BEGIN(MER_GATHER);
	if ( PF.merger_infile != NULL ) {
		for ( ;; ) {
			WORD got;
			AR.CompressPointer = read_scratch; AR.ComprTop = read_top;   /* read scope */
			got = GetOneTerm(BHEAD term, PF.merger_infile, &rpos, 0);
			AR.ComprTop = real_top;                                       /* write scope */
			if ( got < 0 ) { rc = -1; break; }
			if ( got == 0 ) break;
			AR.CompressPointer = put_cp;
			if ( PutOut(BHEAD term, &pos, fout, 1) < 0 ) { rc = -1; break; }
			put_cp = AR.CompressPointer;
		}
	}
	AR.ComprTop = real_top;
	AR.CompressPointer = put_cp;
	if ( rc == 0 && FlushOut(&pos, fout, 0) ) rc = -1;   /* ENDBUFFER to MASTER */
	PF_TIMER_END(MER_GATHER);
	M_free(read_scratch, "gather read cbuf");
	AR.CompressPointer = save_cp;
	PF.in_merger_phase = 0;

	fout->PObuffer = sv_PObuffer; fout->POstop = sv_POstop; fout->POsize = sv_POsize;
	fout->POfill = sv_POfill; fout->POfull = sv_POfull;
	LowerSortLevel();   /* match NewSort -- restore AR.sLevel for the next module */
	return rc;
}

/* Off-parallel-exit gather (master side). At a serial `off parallel` module that
   follows a partitioned MR chain (sMRflag == MAPREDUCE_LAST, nummergers>0) the
   global expression is scattered across the G node-local merger files. Before the
   master runs the module serially it re-globalizes: loser-tree merge the G merger
   streams (sent by pf_merger_gather_to_master) into one scratch -- exactly a normal
   MR module's final sort, but sourced from the merger files and WITHOUT the
   mapper-phase barrier (in_gather skips PF_WaitAllSlaves). Leaves AR.infile =
   [prototype][global sorted content][0], positioned at the prototype so proces.c's
   serial GetTerm(AR.infile) reads it. */
static int pf_master_gather(EXPRESSIONS e, WORD i)
{
	GETIDENTITY
	WORD *term = AT.WorkPointer;
	POSITION position;

	/* Fallback path: the preceding MR module partitioned its output to merger files,
	   and now a serial module needs it globally. This works, but the streaming path
	   is cheaper -- nudge the user to mark the preceding module with `.sort(gather)`. */
	MesPrint("WARNING: a serial (off parallel) module is reading partitioned MR output "
	         "via the fallback gather of expression %s. Terminate the preceding parallel "
	         "module with `.sort(gather)` to stream it to the master directly.", EXPRNAME(i));
	LONG ret;

	/* 1. carry the prototype across to the fresh output scratch (mirrors the
	   master branch's prototype write at the top of PF_Processor). */
	SetScratch(AR.infile, &(e->onfile));
	if ( GetTerm(BHEAD term) <= 0 ) {
		MesPrint("[0] pf_master_gather: expression %d has no prototype", i);
		return -1;
	}
	term[3] = i;
	SeekScratch(AR.outfile, &position);
	e->onfile = position;
	if ( PutOut(BHEAD term, &position, AR.outfile, 0) < 0 ) return -1;
	AR.DeferFlag = 0;

	/* 2. loser-tree merge the G merger streams into AR.outfile. in_gather skips
	   PF_WaitAllSlaves; LAST is not output-partitioned so PF_EndSort's fout stays
	   AR.outfile and it runs the normal master merge body over the merger rbufs. */
	NewSort(BHEAD0);
	PF.parallel  = 1;
	PF.in_gather = 1;
	PF_TIMER_BEGIN(MAS_GATHER);
	ret = EndSort(BHEAD AM.S0->sBuffer, 0);
	PF_TIMER_END(MAS_GATHER);
	PF.in_gather = 0;
	PF.parallel  = 0;
	if ( ret < 0 ) return -1;

	e->counter = PF_goutterms;
	e->size    = PF_exprsize;
	AR.GetFile = 0;

	/* 3. swap so AR.infile holds the gathered global expression. The module
	   boundary's RevertScratch swaps again after the serial module writes its
	   output to AR.outfile (= the now-stale partitioned scratch), so the next
	   module reads the serial output as usual. */
	{ FILEHANDLE *t = AR.infile; AR.infile = AR.outfile; AR.outfile = t; }
	/* The post-swap AR.outfile is the now-stale partitioned scratch ([prototype][0]
	   from the previous module). Reset it to empty so the serial module writes its
	   output to a fresh file -- exactly what the module-boundary RevertScratch does
	   for a normal module. Without this, stale content perturbs the serial pass. */
	if ( AR.outfile->handle >= 0 ) {
		CloseFile(AR.outfile->handle);
		AR.outfile->handle = -1;
		remove(AR.outfile->name);
	}
	PUTZERO(AR.outfile->POposition);
	PUTZERO(AR.outfile->filesize);
	AR.outfile->POfill = AR.outfile->POfull = AR.outfile->PObuffer;
	SetScratch(AR.infile, &(e->onfile));
	return 0;
}

/*
 		#] off-parallel-exit gather :
 		#[ PF_Processor :
*/
enum Role { ROLE_MAPPER = 1, ROLE_REDUCER = 2 };
/**
 * Replaces parts of Processor() on the masters and slaves.
 * On the master PF_Processor() is responsible for proper distribution of terms
 * from the input file to the slaves.
 * On the slaves it calls Generator() for all the terms that this process gets,
 * but PF_GetTerm() gets terms from the master (not directly from infile).
 *
 * @param  e               The pointer to the current expression.
 * @param  i               The index for the current expression.
 * @param  LastExpression  The flag indicating whether it is the last expression.
 * @return                 0 if OK, nonzero on error.
 */
int PF_Processor(EXPRESSIONS e, WORD i, WORD LastExpression)
{
	GETIDENTITY
	WORD *term = AT.WorkPointer;
	LONG dd = 0;
	WORD j;
	LONG size, cpu;
	POSITION position;
	int role, k, src, tag;
	FILEHANDLE *oldoutfile = AR.outfile;
	PF_TIME_ELAPSED(TIMERESET);

#ifdef PF_PROFILE
	pf_profile_per_node_init();
	pf_profile_reset_module();
	PF_OSCounters _pf_os_start;
	pf_profile_snapshot_os(&_pf_os_start);
	pf_module_t0 = MPI_Wtime();
	pf_module_smrflag = (int)AC.sMRflag;   /* label this module FIRST/MAPREDUCE/LAST/none in the CSV */
	if ( PF.me == MASTER ) pf_profile_alloc_master(PF.numtasks);
#endif

	PF.numreducers = (PF.numtasks - 1)*AM.ReducerPer / 100;
	if (PF.numreducers <2 ) PF.numreducers = 2;
	PF.nummappers = AC.sMRflag != NO_MAPREDUCE ? (PF.numtasks - PF.numreducers): PF.numtasks;
	PF.numreducers = PF.numtasks - PF.nummappers;
	role = (PF.me < PF.nummappers) ? ROLE_MAPPER : ROLE_REDUCER;
	/* Where a mapper requests its input term buckets. Default = MASTER (the
	   master distributes). For input-partitioned modules (MAPREDUCE/LAST) the
	   node's merger distributes instead; set below once placement is known. */
	PF.input_src = MASTER;
	/* Merger overlay (Phase 1 of reducer merge tree). nummergers comes from
	   PF_MERGERS env, broadcast in PF_Init. Clamp here where mapper/reducer
	   counts are final. Mergers are mapper ranks 1..nummergers (rank 0 = master). */
	if ( AC.sMRflag == NO_MAPREDUCE ) {
		PF.is_merger = 0;
		PF.merger_parent = MASTER;
	} else {
		/* Merger tier (Phase 1 of the reducer merge tree). When PF_MERGERS>0
		   the tier is ON; placement is always node-local -- exactly one merger
		   per physical node, draining only that node's reducers, so the
		   reducer->merger transfer stays intra-node (UCX sysv shm). The
		   PF_MERGERS numeric value is just the on/off switch; the merger count
		   is derived as the number of reducer-bearing nodes. If node-local
		   placement cannot be realised (topology discovery failed, or a
		   reducer-bearing node has no spare mapper to host its merger) the
		   tier is disabled for this run -- reducers feed the master directly;
		   the run still completes, just without the merger optimization. */
		PF.is_merger     = 0;
		PF.merger_parent = MASTER;
		if ( PF.nummergers > 0 && PF.rank_node != NULL && PF.numnodes >= 1 ) {
			int nn = PF.numnodes, nummappers = PF.nummappers, numtasks = PF.numtasks;
			int *node_merger = (int*)malloc((size_t)nn*sizeof(int));
			int *node_hasred = (int*)malloc((size_t)nn*sizeof(int));
			int inmerger = ( node_merger != NULL && node_hasred != NULL );
			if ( inmerger ) {
				int n, r, nummergers = 0;
				for ( n = 0; n < nn; n++ ) { node_merger[n] = -1; node_hasred[n] = 0; }
				/* lowest non-master mapper on each node hosts that node's merger */
				for ( r = nummappers - 1; r >= 1; r-- ) node_merger[PF.rank_node[r]] = r;
				/* which nodes own at least one reducer */
				for ( r = nummappers; r < numtasks; r++ )      node_hasred[PF.rank_node[r]] = 1;
				/* check if we can assign a mapper on the node- a reducer-bearing node with no spare mapper cannot host a merger */
				for ( n = 0; n < nn; n++ )
					if ( node_hasred[n] && node_merger[n] < 0 ) inmerger = 0;
				if ( inmerger ) {
					for ( n = 0; n < nn; n++ ) if ( node_hasred[n] ) nummergers++;
					PF.nummergers    = nummergers;
					PF.is_merger     = ( node_hasred[PF.node_id]
					                     && PF.me == node_merger[PF.node_id] );
					PF.merger_parent = ( PF.me >= nummappers )
					                   ? node_merger[PF.rank_node[PF.me]]
					                   : MASTER;
					/* Input-partitioned modules (MAPREDUCE/LAST): a non-merger
					   mapper fetches its input buckets from its node's merger
					   (which distributes its file) instead of from the master.
					   FIRST/NO_MAPREDUCE keep input_src = MASTER (set above). */
					if ( ( AC.sMRflag == MAPREDUCE || AC.sMRflag == MAPREDUCE_LAST )
					     && PF.me != MASTER && PF.me < nummappers && !PF.is_merger
					     && node_hasred[PF.node_id] )
						PF.input_src = node_merger[PF.node_id];
					if ( PF.me == MASTER ) {
						/* child table: merger ranks ascending, [0]=MASTER */
						if ( PF.merger_child_ranks == NULL )
							PF.merger_child_ranks =
							    (int*)malloc((size_t)(numtasks+1)*sizeof(int));
						if ( PF.merger_child_ranks != NULL ) {
							int g = 1, prev = -1, lo;
							PF.merger_child_ranks[0] = MASTER;
							for ( ;; ) {
								lo = -1;
								for ( n = 0; n < nn; n++ )
									if ( node_hasred[n] && node_merger[n] > prev
									     && ( lo < 0 || node_merger[n] < lo ) )
										lo = node_merger[n];
								if ( lo < 0 ) break;
								PF.merger_child_ranks[g++] = lo;
								prev = lo;
							}
						}
					}
				}
			}
			if ( !inmerger ) {
				/* node-local placement not possible -- disable the merger tier */
				if ( PF.me == MASTER ) {
					static int warned = 0;
					if ( !warned ) {
						warned = 1;
						MesPrint("PF: merger tier requested but node-local placement failed "
						         "(topology/spare-mapper) -- running without mergers");
					}
				}
				PF.nummergers    = 0;
				PF.is_merger     = 0;
				PF.merger_parent = MASTER;
			}
			if ( node_merger ) free(node_merger);
			if ( node_hasred ) free(node_hasred);
		} else {
			/* PF_MERGERS=0, or node topology unavailable -- no merger tier */
			PF.nummergers = 0;
		}
	}
	/* One-time master announcement of the merger tier actually in effect,
	   so a run is self-documenting. */
	if ( PF.me == MASTER && AC.sMRflag != NO_MAPREDUCE && PF.nummergers > 0 ) {
		static int s_merger_announced = 0;
		if ( !s_merger_announced ) {
			s_merger_announced = 1;
			MesPrint("PF: merger tier active -- node-local, %d mergers across %d nodes",
			         PF.nummergers, PF.numnodes);
		}
	}
	/* Record whether THIS processed module partitions its output to merger files.
	   Read by the sMRflag transition (execute.c) for the NEXT module to decide
	   partitioned-input (MAPREDUCE/LAST) vs master-distributed FIRST. Set on every
	   rank that reaches here (PF_Processor is called per expression on all ranks),
	   so the value is consistent cluster-wide. A module that processes no
	   expression never reaches here, leaving prev_partitioned unchanged -- which is
	   exactly right: an MR-flagged but empty .sort must not advance the chain. */
	PF.prev_partitioned = pf_output_partitioned() ? 1 : 0;

#ifdef MPI2
	if ( PF_shared_buff == NULL ) {
		if ( PF_SMWin_Init() == 0 ) {
			MesPrint("PF_SMWin_Init error");
			exit(-1);
		}
	}
#endif

	if ( ( (WORD *)(((UBYTE *)(AT.WorkPointer)) + AM.MaxTer ) ) > AT.WorkTop ) {
		MesWork();
	}

	/* For redefine statements. */
	if ( AC.numpfirstnum > 0 ) {
		for ( j = 0; j < AC.numpfirstnum; j++ ) {
			AC.inputnumbers[j] = -1;
		}
	}
	if ( AC.mparallelflag != PARALLELFLAG ){
		/* MAPREDUCE_LAST is the chain-exit gather module: non-parallel
		   (mMRflag==NO_MAPREDUCE) but sMRflag still labels it LAST so the data
		   path knows the global expression is scattered across the G node-local
		   merger files and must be re-globalized before this serial module reads
		   AR.infile. Collective over master + mergers (others fall straight to
		   return(0)). When nummergers==0 there was no partitioning, so nothing to
		   gather -- it skips serially just like NO_MAPREDUCE. */
		if ( PF.nummergers > 0 && AC.sMRflag == MAPREDUCE_LAST ) {
			if ( PF.is_merger ) {
				if ( pf_merger_gather_to_master() < 0 ) return(-1);
			}
			else if ( PF.me == MASTER ) {
				if ( pf_master_gather(e,i) < 0 ) return(-1);
			}
		}
		if(AC.sMRflag == NO_MAPREDUCE || AC.sMRflag == MAPREDUCE_LAST) {return(0);}
		MesPrint("ERROR: Calling Map Reduce without parallel");
		return (-1);
	}

	if ( PF.me == MASTER ) {
/*
 		#[ Master:
			#[ write prototype to outfile:
*/
		WORD oldBracketOn = AR.BracketOn;
		WORD *oldBrackBuf = AT.BrackBuf;
		WORD oldbracketindexflag = AT.bracketindexflag;

		if ( PF.log && AC.CModule >= PF.log )
			MesPrint("[%d] working on expression %s in module %l",PF.me,EXPRNAME(i),AC.CModule);
		if ( GetTerm(BHEAD term) <= 0 ) {
			MesPrint("[%d] Expression %d has problems in scratchfile",PF.me,i);
			return(-1);
		}
		term[3] = i;
		if ( AC.sMRflag != NO_MAPREDUCE )
			MesPrint("We have %d mappers and %d reducers", PF.nummappers, PF.numreducers);
		if ( AR.outtohide ) {
			SeekScratch(AR.hidefile,&position);
			e->onfile = position;
			if ( PutOut(BHEAD term,&position,AR.hidefile,0) < 0 ) return(-1);
		}
		else {
			SeekScratch(AR.outfile,&position);
			e->onfile = position;
			if ( PutOut(BHEAD term,&position,AR.outfile,0) < 0 ) return(-1);
		}
		AR.DeferFlag = 0;  /* The master leave the brackets!!! */
		AR.Eside = RHSIDE;
		if ( ( e->vflags & ISFACTORIZED ) != 0 ) {
			AR.BracketOn = 1;
			AT.BrackBuf = AM.BracketFactors;
			AT.bracketindexflag = 1;
		}
		if ( AT.bracketindexflag > 0 ) OpenBracketIndex(i);
/*
			#] write prototype to outfile: 
			#[ initialize sendbuffer if necessary:

			the size of the sendbufs is:
			MIN(1/PF.numtasks*(AT.SS->sBufsize+AT.SS->lBufsize),AR.infile->POsize)
			No allocation for extra buffers necessary, just make sb->buf... point
			to the right places in the sortbuffers.
*/
		NewSort(BHEAD0);   /* we need AT.SS to be set for this!!! */
		if ((size = PF_allocateSbuf()) == 0 ) {MesPrint("Error Master PF_Processor"); return -1;}

		PF_BUFFER *sb = PF.sbufs[0] ;
/*
			#] initialize sendbuffer if necessary: 
			#[ loop for all terms in infile:
*/
		/*
		 * Distribute the input terms on demand: small initial buckets (slow-start
		 * ramp, capped by e->counter) doubling toward ProcessBucketSize, with
		 * swap-pipelined sends. Shared with the mapper-merger's node-local
		 * redistribution via pf_distribute_terms (is_merger = 0 here). The final
		 * partial bucket and the ENDSORT are sent later in PF_WaitAllSlaves
		 * (reached via EndSort() below).
		 */
		PF_TIMER_BEGIN(MAS_DISTRIBUTE);
		if ( pf_distribute_terms(sb, 0, (FILEHANDLE *)0, e->counter, PF.nummappers - 1) < 0 )
			return(-1);
		PF_TIMER_END(MAS_DISTRIBUTE);
/*
			#] loop for all terms in infile: 
			#[ Clean up & EndSort:
*/
		if ( LastExpression ) {
			UpdateMaxSize();
			if ( AR.infile->handle >= 0 ) {
				CloseFile(AR.infile->handle);
				AR.infile->handle = -1;
				remove(AR.infile->name);
				PUTZERO(AR.infile->POposition);
			}
			AR.infile->POfill = AR.infile->POfull = AR.infile->PObuffer;
		}
		if ( AR.outtohide ) AR.outfile = AR.hidefile;
		PF.parallel = 1;
#ifdef PF_PROFILE
		MesPrint("[0] PF_Processor: Master finished sending terms");
#endif
		/* Attribute the master's EndSort by what it actually does this module:
		   - output-partitioned (FIRST/MAPREDUCE): EndSort returns fast via the
		     partitioned branch (WaitAllSlaves + PF_MERGERDONE collect + prototype
		     flush, NO merge) -> MAS_MERGERDONE. MAS_FINAL_SORT stays 0, which is the
		     whole point of the bypass.
		   - chain-exit gather (MAPREDUCE_LAST, `.sort(gather)`): merges the G merger
		     streams to the global scratch -> MAS_GATHER.
		   - classic non-MR module: the real loser-tree merge -> MAS_FINAL_SORT. */
		PF_TIMER_BEGIN_RT(mas_es);
		if ( EndSort(BHEAD AM.S0->sBuffer,0) < 0 ) return(-1);
		PF_TIMER_END_RT(mas_es, pf_output_partitioned() ? PF_PHASE_MAS_MERGERDONE
		                        : ( AC.sMRflag == MAPREDUCE_LAST
		                            ? PF_PHASE_MAS_GATHER : PF_PHASE_MAS_FINAL_SORT ));
		PF.parallel = 0;
		if ( AR.outtohide ) {
			AR.outfile = oldoutfile;
			AR.hidefile->POfull = AR.hidefile->POfill;
		}
		UpdateMaxSize();
		AR.BracketOn = oldBracketOn;
		AT.BrackBuf = oldBrackBuf;
		if ( ( e->vflags & TOBEFACTORED ) != 0 )
			poly_factorize_expression(e);
		else if ( ( ( e->vflags & TOBEUNFACTORED ) != 0 )
		 && ( ( e->vflags & ISFACTORIZED ) != 0 ) )
			poly_unfactorize_expression(e);
		AT.bracketindexflag = oldbracketindexflag;
		AR.GetFile = 0;
		AR.outtohide = 0;
		/*
		 * NOTE: e->numdummies, e->vflags and AR.exprflags will be updated
		 *       after gathering the information from all slaves.
		 */
/*
			#] Clean up & EndSort: 
			#[ Collect (stats,prepro,...):
*/
		DBGOUT_NINTERMS(1, ("PF.me=%d AN.ninterms=%d ENDSORT\n", (int)PF.me, (int)AN.ninterms));
		PF_CatchErrorMessagesForAll();
		e->numdummies = 0;
		PF_TIMER_BEGIN(MAS_COLLECT);
		for ( k = 1; k < PF.numtasks; k++ ) {
			PF_LongSingleReceive(PF_ANY_SOURCE, PF_ENDSORT_MSGTAG, &src, &tag);
			PF_LongSingleUnpack(PF_stats[src], PF_STATS_SIZE, PF_LONG);
#ifdef PF_PROFILE
			PF_LongSingleUnpack(pf_profile_stats[src].phase_us,       PF_PHASE_COUNT, PF_LONG);
			PF_LongSingleUnpack(pf_profile_stats[src].phase_first_us, PF_PHASE_COUNT, PF_LONG);
			PF_LongSingleUnpack(pf_profile_stats[src].phase_last_us,  PF_PHASE_COUNT, PF_LONG);
			PF_LongSingleUnpack(pf_profile_stats[src].os_diff,        PF_OS_COUNT,    PF_LONG);
			PF_LongSingleUnpack(pf_profile_stats[src].extras,         PF_EX_COUNT,    PF_LONG);
#endif
			{
				WORD numdummies, expchanged;
				PF_LongSingleUnpack(&numdummies, 1, PF_WORD);
				PF_LongSingleUnpack(&expchanged, 1, PF_WORD);
				if ( e->numdummies < numdummies ) e->numdummies = numdummies;
				AR.expchanged |= expchanged;
			}
			/* Now handle redefined preprocessor variables. */
			if ( AC.numpfirstnum > 0 ) PF_UnpackRedefinedPreVars();
		}
		PF_TIMER_END(MAS_COLLECT);
		/* Broadcast redefined preprocessor variables. */
		if ( AC.numpfirstnum > 0 ) {
			int RetCode = PF_BroadcastRedefinedPreVars();
			if ( RetCode ) return RetCode;
		}
		if ( ! AC.OldParallelStats ) {
			/* Now we can calculate AT.SS->GenTerms from the statistics of the slaves. */
			LONG genterms = 0;
			for ( k = 1; k < PF.numtasks; k++ ) {
				genterms += PF_stats[k][3];
			}
			AT.SS->GenTerms = genterms;
			WriteStats(&PF_exprsize, STATSPOSTSORT, NOCHECKLOGTYPE);
			Expressions[AR.CurExpr].size = PF_exprsize;
		}
		PF_Statistics(PF_stats,0);
#ifdef PF_PROFILE
		{
			PF_OSCounters _pf_os_end;
			pf_profile_snapshot_os(&_pf_os_end);
			pf_profile_diff_os(&_pf_os_start, &_pf_os_end, pf_os_diff);
			pf_extras[PF_EX_WALLCLOCK_US] = (LONG)((MPI_Wtime() - pf_module_t0) * 1.0e6);
			memcpy(pf_profile_stats[0].phase_us,       pf_phase_us,       sizeof(pf_phase_us));
			memcpy(pf_profile_stats[0].phase_first_us, pf_phase_first_us, sizeof(pf_phase_first_us));
			memcpy(pf_profile_stats[0].phase_last_us,  pf_phase_last_us,  sizeof(pf_phase_last_us));
			memcpy(pf_profile_stats[0].os_diff,        pf_os_diff,        sizeof(pf_os_diff));
			memcpy(pf_profile_stats[0].extras,         pf_extras,         sizeof(pf_extras));
			pf_profile_dump_master_csv(AC.CModule, (const char *)EXPRNAME(i),
			                           PF.nummappers, PF.numreducers, PF.numtasks);
		}
#endif
/*
			#] Collect (stats,prepro,...): 
			#[ Update flags :
*/
		if ( AM.S0->TermsLeft ) e->vflags &= ~ISZERO;
		else e->vflags |= ISZERO;
		if ( AR.expchanged == 0 ) e->vflags |= ISUNMODIFIED;
		if ( AM.S0->TermsLeft ) AR.expflags |= ISZERO;
		if ( AR.expchanged ) AR.expflags |= ISUNMODIFIED;
/*
			#] Update flags : 
 		#] Master: 
*/
	}
	else if (role==ROLE_MAPPER) {
/*
 		#[ Mapper/Slave :
*/
/*
			#[ Generator Loop & EndSort :

			loop for all terms to get from master, call Generator for each of them
			then call EndSort and do cleanup (to be implemented)
*/
		WORD oldBracketOn = AR.BracketOn;
		WORD *oldBrackBuf = AT.BrackBuf;
		WORD oldbracketindexflag = AT.bracketindexflag;

		/* For redefine statements. */
		if ( AC.numpfirstnum > 0 ) {
			for ( j = 0; j < AC.numpfirstnum; j++ ) {
				AC.inputnumbers[j] = -1;
			}
		}

		SeekScratch(AR.outfile,&position);
		e->onfile = position;
		AR.DeferFlag = AC.ComDefer;
		AR.Eside = RHSIDE;
		if ( ( e->vflags & ISFACTORIZED ) != 0 ) {
			AR.BracketOn = 1;
			AT.BrackBuf = AM.BracketFactors;
			AT.bracketindexflag = 1;
		}
		NewSort(BHEAD0);
		AR.MaxDum = AM.IndDum;
		AN.ninterms = 0;
		PF_linterms = 0;
		PF.parallel = 1;
#ifdef MPI2
		AR.infile->POfull = AR.infile->POfill = AR.infile->PObuffer = PF_shared_buff;
#endif
		{
			FILEHANDLE *fi = AC.RhsExprInModuleFlag && PF.rhsInParallel ? &PF.slavebuf : AR.infile;
			fi->POfull = fi->POfill = fi->PObuffer;
		}
		if ((size = PF_allocateSbuf()) == 0 ) {MesPrint("Error in endsort"); return -1;}

		if( AC.sMRflag != NO_MAPREDUCE && PF.me < PF.nummappers) //if we in mapreduce and this is a mapper
		{
			for (int k = PF.nummappers; k < PF.numtasks; k++){ //allocating the buffers in the destined reducers indices
				if(PF.sbufs[k] == NULL){
					if ( (PF.sbufs[k] = PF_AllocBuf(PF.numsbufs, size*sizeof(WORD), 0)) == NULL ) return -1;
					//MesPrint("[%d] PF_EndSort: Allocating send buffer %d size %d", PF.me, k, size);
				}
				else{
					for ( i = 0; i < PF.numsbufs; i++ )
						PF.sbufs[k]->fill[i] = PF.sbufs[k]->full[i] = PF.sbufs[k]->buff[i];
					PF.sbufs[k]->active = 0;
				}
			}
		}
/*
 		#] the slaves have to initialize their sendbuffer : 
*/
		if( AC.sMRflag != NO_MAPREDUCE && PF.me < PF.nummappers ){
			if(AR.CompressBuffers == NULL)
			{
				AR.CompressBuffers = (WORD**)Malloc1(PF.numtasks*sizeof(WORD*), "CompressBuffers in PF_Processor");
				AR.CompressPointers = (WORD**)Malloc1(PF.numtasks*sizeof(WORD*), "CompressBuffers in PF_Processor");
				AR.CompressBuffers[0] = AR.CompressPointers[0] = AR.CompressBuffer;
				for(i=0;i<PF.numtasks;i++) AR.CompressBuffers[i] = NULL;
			}
			for(i=PF.nummappers;i<PF.numtasks;i++) {
				if (AR.CompressBuffers[i] == NULL ){
					AR.CompressBuffers[i] = (WORD *)Malloc1((AM.CompressSize+10)*sizeof(WORD),"compresssize");
				}
				AR.CompressPointers[i] = AR.CompressBuffers[i];
				AR.CompressBuffers[i][0] = 0;
			}
		}
		/* FIXME: AN.ninterms is still broken when AN.deferskipped is non-zero.
		 *        It still needs some work, also in PF_GetTerm(). (TU 30 Aug 2011) */
		LONG send_time = TimeCPU(1);
		if ( pf_input_partitioned() && PF.is_merger ) {
		/* Distributor merger: hand merger_infile content to the node's mappers
		   instead of generating. The (empty) mapper EndSort that follows still
		   emits ENDSHUFFLE to every reducer so their Waitany accounting completes
		   (mpi.c:666). */
			if ( PF_MergerDistribute() < 0 ) return -1;
		}
		else {
		PF_TIMER_BEGIN(MAP_GENERATOR);
		while ( PF_GetTerm(term) ) {
			PF_linterms++; AN.ninterms++; dd = AN.deferskipped;
			AT.WorkPointer = term + *term;
			AN.RepPoint = AT.RepCount + 1;
			if ( ( e->vflags & ISFACTORIZED ) != 0 && term[1] == HAAKJE ) {
				StoreTerm(BHEAD term);
				continue;
			}
			if ( AR.DeferFlag ) {
				AR.CurDum = AN.IndDum = Expressions[AR.CurExpr].numdummies + AM.IndDum;
			}
			else {
				AN.IndDum = AM.IndDum;
				AR.CurDum = ReNumber(BHEAD term);
			}
			if ( AC.SymChangeFlag ) MarkDirty(term,DIRTYSYMFLAG);
			if ( AN.ncmod ) {
				if ( ( AC.modmode & ALSOFUNARGS ) != 0 ) MarkDirty(term,DIRTYFLAG);
				else if ( AR.PolyFun ) PolyFunDirty(BHEAD term);
			}
			else if ( AC.PolyRatFunChanged ) PolyFunDirty(BHEAD term);
			if ( ( AR.PolyFunType == 2 ) && ( AC.PolyRatFunChanged == 0 )
				&& ( e->status == LOCALEXPRESSION || e->status == GLOBALEXPRESSION ) ) {
				PolyFunClean(BHEAD term);
			}
			if ( Generator(BHEAD term,0) ) {
				MesPrint("[%d] PF_Processor: Error in Generator",PF.me);
				LowerSortLevel(); return(-1);
			}
			PF_linterms += dd; AN.ninterms += dd;
		}
		PF_TIMER_END(MAP_GENERATOR);
		PF_linterms += dd; AN.ninterms += dd;
		}  /* end else: normal generator path (non-distributor) */
		{
			/*
			 * EndSort() overrides AR.outfile->PObuffer etc. (See also PF_EndSort()),
			 * but it causes a problem because
			 *  (1) PF_EndSort() sets AR.outfile->PObuffer to a send-buffer.
			 *  (2) RevertScratch() clears AR.infile, but then swaps buffers of AR.infile
			 *      and AR.outfile.
			 *  (3) RHS expressions are stored to AR.infile->PObuffer.
			 *  (4) Again, PF_EndSort() sets AR.outfile->PObuffer, but now AR.outfile->PObuffer
			 *      == AR.infile->PObuffer because of (1) and (2).
			 *  (5) The result goes to AR.outfile. This breaks the RHS expressions,
			 *      which may be needed for the next expression.
			 * Solution: backup & restore AR.outfile->PObuffer etc. (TU 14 Sep 2011)
			 */
			FILEHANDLE *fout = AR.outfile;
			WORD *oldbuff = fout->PObuffer;
			WORD *oldstop = fout->POstop;
			LONG  oldsize = fout->POsize;
			//MesPrint("[%d] PF_Processor: starting endsort", PF.me);
			PF_TIMER_BEGIN(MAP_ENDSORT_TOTAL);
			if ( EndSort(BHEAD AM.S0->sBuffer, 0) < 0 ) return -1;
			PF_TIMER_END(MAP_ENDSORT_TOTAL);
			//MesPrint("[%d] PF_Processor: finished endsort", PF.me);
			fout->PObuffer = oldbuff;
			fout->POstop   = oldstop;
			fout->POsize   = oldsize;
			fout->POfill = fout->POfull = fout->PObuffer;
		}
		send_time = TimeCPU(1) - send_time;
		LONG waittime = PF_TIME_ELAPSED_GET();
		if( AC.sMRflag != NO_MAPREDUCE) PF_Send(MASTER, PF_BUFFER_MSGTAG); //Send update to Master that the mapper is done sending terms to reducers
		/* Mapper-merger overlay (Phase 1 of reducer merge tree). On ranks
		   1..PF.nummergers, after the mapper-phase EndSort + done-send, run
		   a second merge pass over a group of leaf reducer streams and
		   forward the merged output to MASTER. Sequencing: a leaf reducer's
		   EndSort cannot complete until ALL mappers (including this rank)
		   have sent ENDSHUFFLE, so the merger's mapper phase is always done
		   before any leaf-reducer data starts arriving here. */
		if ( PF.is_merger ) {
			if ( PF_MergerLoop() < 0 ) {
				MesPrint("[%d] PF_Processor: PF_MergerLoop failed", PF.me);
				return -1;
			}
		}
		AR.BracketOn = oldBracketOn;
		AT.BrackBuf = oldBrackBuf;
		AT.bracketindexflag = oldbracketindexflag;
/*
			#] Generator Loop & EndSort : 
			#[ Collect (stats,prepro...) :
*/
		DBGOUT_NINTERMS(1, ("PF.me=%d AN.ninterms=%d PF_linterms=%d ENDSORT\n", (int)PF.me, (int)AN.ninterms, (int)PF_linterms));
		PF_PrepareLongSinglePack();
		cpu = TimeCPU(1);
		size = 0;
		PF_LongSinglePack(&cpu,              1, PF_LONG);
		PF_LongSinglePack(&size,             1, PF_LONG);
		PF_LongSinglePack(&PF_linterms,      1, PF_LONG);
		PF_LongSinglePack(&AM.S0->GenTerms,  1, PF_LONG);
		PF_LongSinglePack(&AM.S0->TermsLeft, 1, PF_LONG);
		PF_LongSinglePack(&waittime,		 1, PF_LONG);
		PF_LongSinglePack(&send_time,		 1, PF_LONG);
#ifdef PF_PROFILE
		{
			PF_OSCounters _pf_os_end;
			pf_profile_snapshot_os(&_pf_os_end);
			pf_profile_diff_os(&_pf_os_start, &_pf_os_end, pf_os_diff);
			pf_extras[PF_EX_WALLCLOCK_US] = (LONG)((MPI_Wtime() - pf_module_t0) * 1.0e6);
			pf_extras[PF_EX_TERMS_SENT] = PF_linterms;
			PF_LongSinglePack(pf_phase_us,       PF_PHASE_COUNT, PF_LONG);
			PF_LongSinglePack(pf_phase_first_us, PF_PHASE_COUNT, PF_LONG);
			PF_LongSinglePack(pf_phase_last_us,  PF_PHASE_COUNT, PF_LONG);
			PF_LongSinglePack(pf_os_diff,        PF_OS_COUNT,    PF_LONG);
			PF_LongSinglePack(pf_extras,         PF_EX_COUNT,    PF_LONG);
		}
#endif

		{
			WORD numdummies = AR.MaxDum - AM.IndDum;
			PF_LongSinglePack(&numdummies,    1, PF_WORD);
			PF_LongSinglePack(&AR.expchanged, 1, PF_WORD);
		}
		/* Now handle redefined preprocessor variables. */
		if ( AC.numpfirstnum > 0 ) PF_PackRedefinedPreVars();
		PF_LongSingleSend(MASTER, PF_ENDSORT_MSGTAG);
		/* Broadcast redefined preprocessor variables. */
		if ( AC.numpfirstnum > 0 ) {
			int RetCode = PF_BroadcastRedefinedPreVars();
			if ( RetCode ) return RetCode;
		}
/*
			#] Collect (stats,prepro...) :

		This operation is moved to the beginning of each block, see PreProcessor
		in pre.c.

 		#] Slave :
*/
#ifdef PF_PROFILE
		if ( PF.log ) {
			UBYTE lbuf[24];
			NumToStr(lbuf,AC.CModule);
			fprintf(stderr,"[%d|%s] Endsort,Collect,Broadcast done\n",PF.me,lbuf);
			fflush(stderr);
		}
#endif
	}
	else { // role==ROLE_REDUCER
/*
		#[ the receive buffers :
		*/
		NewSort(BHEAD0);
		PF.parallel = 1;
		int err = PF_ReducerInit();
		if ( err ) {
			MesPrint("PF_ReducerInit error");
			return err;
		}
/*
 		#] the receive buffers : 
		#[ Reducer Loop & EndSort :
*/
		LONG ret = PF_ForwardTermsToMaster();
		if ( ret < 0 ) {
			MesPrint("PF_forwardTermsToMaster error");
			return ret;
		}
/*
		#[ Reducer Loop & EndSort :
		#[ Collect (stats,prepro...) :
*/
		LONG waittime = PF_TIME_ELAPSED_GET();
		DBGOUT_NINTERMS(1, ("PF.me=%d AN.ninterms=%d PF_linterms=%d ENDSORT\n", (int)PF.me, (int)AN.ninterms, (int)PF_linterms));
		PF_PrepareLongSinglePack();
		cpu = TimeCPU(1);
		size = 0;
		PF_LongSinglePack(&cpu,              1, PF_LONG);
		PF_LongSinglePack(&size,             1, PF_LONG);
		PF_LongSinglePack(&PF_linterms,      1, PF_LONG);
		PF_LongSinglePack(&AM.S0->GenTerms,  1, PF_LONG);
		PF_LongSinglePack(&AM.S0->TermsLeft, 1, PF_LONG);
		PF_LongSinglePack(&waittime,		 1, PF_LONG);
		PF_LongSinglePack(&ret,		 		 1, PF_LONG);
#ifdef PF_PROFILE
		{
			PF_OSCounters _pf_os_end;
			pf_profile_snapshot_os(&_pf_os_end);
			pf_profile_diff_os(&_pf_os_start, &_pf_os_end, pf_os_diff);
			pf_extras[PF_EX_WALLCLOCK_US] = (LONG)((MPI_Wtime() - pf_module_t0) * 1.0e6);
			PF_LongSinglePack(pf_phase_us,       PF_PHASE_COUNT, PF_LONG);
			PF_LongSinglePack(pf_phase_first_us, PF_PHASE_COUNT, PF_LONG);
			PF_LongSinglePack(pf_phase_last_us,  PF_PHASE_COUNT, PF_LONG);
			PF_LongSinglePack(pf_os_diff,        PF_OS_COUNT,    PF_LONG);
			PF_LongSinglePack(pf_extras,         PF_EX_COUNT,    PF_LONG);
		}
#endif
		{
			WORD numdummies = AR.MaxDum - AM.IndDum;
			PF_LongSinglePack(&numdummies,    1, PF_WORD);
			PF_LongSinglePack(&AR.expchanged, 1, PF_WORD);
		}
		/* Now handle redefined preprocessor variables. */
		if ( AC.numpfirstnum > 0 ) PF_PackRedefinedPreVars();
		PF_LongSingleSend(MASTER, PF_ENDSORT_MSGTAG);
		/* Broadcast redefined preprocessor variables. */
		if ( AC.numpfirstnum > 0 ) {
			int RetCode = PF_BroadcastRedefinedPreVars();
			if ( RetCode ) return RetCode;
		}
/*
			#] Collect (stats,prepro...) : 

		This operation is moved to the beginning of each block, see PreProcessor
		in pre.c.

 		#] Slave : 
*/
#ifdef PF_PROFILE
		if ( PF.log ) {
			UBYTE lbuf[24];
			NumToStr(lbuf,AC.CModule);
			fprintf(stderr,"[%d|%s] Endsort,Collect,Broadcast done\n",PF.me,lbuf);
			fflush(stderr);
		}
#endif
	}
	return(0);
}

/*
 		#] PF_Processor : 
		#[ PF_ReducerInit :
*/
/**
 * Allocates and resets the data structures for the Reduce stage
 *
 * @return      0 if OK, -1 otherwise
 *
 */
int PF_ReducerInit()
{
	GETIDENTITY
	int i, j;
	int err;
	PF_BUFFER **rbuf = PF.rbufs;
	int numtasks = PF.nummappers; 
	int numrbufs = PF.numrbufs; //for each mapper
	/* Use PF.shuffle_arena_words (mapper-config arena) -- same rationale as PF_InitTree:473. */
	LONG size = (PF.shuffle_arena_words - 1)/(PF.numtasks - 1);
	if( size <= (LONG)(AM.MaxTer/sizeof(WORD) + 2)) size = (LONG)(2*(AM.MaxTer/sizeof(WORD) + 2));
	if ( rbuf == NULL ) {
		if ( ( rbuf = (PF_BUFFER**)Malloc1(numtasks*sizeof(PF_BUFFER*), "Reducer: rbufs") ) == NULL ) return(-1);
		if ( (rbuf[0] = PF_AllocBuf(1,1,0) ) == NULL ) return(-1);
		for ( i = 1; i < numtasks; i++ ) {
			if (!(rbuf[i] = PF_AllocBuf(numrbufs,sizeof(WORD)*size,0))) return(-1);
		}
	}
	if (PF_term == NULL && PF_allocatePFTerm(numtasks)) {MesPrint("Error in ReducerInit"); return -1;}
	// create a receivers requests flat view
	PF_SetupFlatRequestsView();
	PF_Dispatch* d = &PF.dispatch;
	for ( i = 1; i < numtasks; i++ ) {
		rbuf[i]->active = 0;
		for ( j = 0; j < rbuf[i]->numbufs; j++ ) {
			rbuf[i]->full[j] = rbuf[i]->fill[j] = rbuf[i]->buff[j] + AM.MaxTer/sizeof(WORD) + 2;
			rbuf[i]->request[j] = MPI_REQUEST_NULL;
		}
		PF_term[i] = rbuf[i]->fill[rbuf[i]->active];
		*PF_term[i] = 0;
		size = (LONG)(rbuf[i]->stop[rbuf[i]->active] - rbuf[i]->full[rbuf[i]->active]);
		err = PF_IRecvRbuf(rbuf[i],rbuf[i]->active,i);
		if (err) return err;
		int k = i * numrbufs;
		d->reqs[k] = rbuf[i]->request[rbuf[i]->active];
	}
	PF.rbufs = rbuf;
	PF_term[0] = rbuf[0]->buff[0];
	PF_term[0][0] = 0;
	return 0;
}
/*
	    #] PF_ReducerInit :
  	 	#[ PF_ForwardTermsToMaster : 
*/ 
/**
 * Forwards terms from the Mappers to the Masters.
 * This is the main Reduce stage. It receives terms from the mappers, sort them and sends them to the master
 *
 * @return      0 if OK, -1 otherwise
 *
 */
LONG PF_ForwardTermsToMaster()
{
	POSITION oldposition, position;
	AR.CompressPointer = AR.CompressBuffer;
	*AR.CompressPointer = 0;
	SeekScratch(AR.outfile,&position);
	oldposition = position;
	if(PF_StoreBuffer() == -1) return -1;
	/* Merger tier: bulk data goes to merger (not master), so the in-flow
	   pre-handshake in PF_ISendSbuf (mpi.c) never fires for master. Send an
	   explicit PF_BUFFER_MSGTAG to master now, after PF_StoreBuffer is done
	   and before the local sort starts. Master's PF_WaitAllSlaves expects one
	   such signal per slave (mappers + reducers) and drains it via
	   PF_Wait4Slave so the leaf doesn't block on this PF_Send. */
	if ( PF.nummergers > 0 ) {
		PF_PreparePack();
		PF_Send(MASTER, PF_BUFFER_MSGTAG);
	}
	FILEHANDLE *fout = AR.outfile;
	WORD *oldbuff = fout->PObuffer;
	WORD *oldstop = fout->POstop;
	LONG  oldsize = fout->POsize;
	LONG sort_time = TimeCPU(1);
	PF_TIMER_BEGIN(RED_FINAL_SORT);
	if ( EndSort(BHEAD AM.S0->sBuffer, 0) < 0 ) return -1;
	PF_TIMER_END(RED_FINAL_SORT);
	sort_time = TimeCPU(1) - sort_time;
	fout->PObuffer = oldbuff;
	fout->POstop   = oldstop;
	fout->POsize   = oldsize;
	fout->POfill = fout->POfull = fout->PObuffer;
	position = oldposition;
    return (sort_time);
}
/*
 	#] PF_ForwardTermsToMaster :
 	#[ pf_setup_merger_group :
*/
/**
 * Build PF.merger_leaf_ranks[1..groupsz] and set PF.merger_groupsz for this
 * mapper-merger rank. Group membership: leaf reducer rank r belongs to this
 * merger iff ((r - PF.nummappers) % PF.nummergers) + 1 == PF.me.
 *
 * Called once at the start of PF_MergerLoop; the actual rbuf/loser-tree setup
 * is then done by PF_InitTree using these fields.
 *
 * @return number of group leaves on success, -1 on error.
 */
static int pf_setup_merger_group(void)
{
	int M = PF.nummappers, R = PF.numreducers;
	int groupsz = 0;
	int i, j;
	/* Node-local placement: leaf reducer i belongs to this merger iff it sits
	   on this merger's physical node. The merger groups form a disjoint
	   partition of the reducers across mergers. */
	if ( PF.nummergers <= 0 || !PF.is_merger || PF.rank_node == NULL ) return -1;

	for ( i = M; i < M + R; i++ )
		if ( PF.rank_node[i] == PF.node_id ) groupsz++;
	PF.merger_groupsz = groupsz;
	if ( groupsz < 1 ) return -1;

	if ( PF.merger_leaf_ranks == NULL ) {
		PF.merger_leaf_ranks = (int*)Malloc1((LONG)(groupsz + 1) * sizeof(int),
		                                     "merger_leaf_ranks");
		if ( PF.merger_leaf_ranks == NULL ) return -1;
		PF.merger_leaf_ranks[0] = MASTER;  /* sentinel: PF_term[0] is the zero term */
		j = 1;
		for ( i = M; i < M + R; i++ )
			if ( PF.rank_node[i] == PF.node_id ) PF.merger_leaf_ranks[j++] = i;
	}
	return groupsz;
}
/*
	#] pf_setup_merger_group :
	#[ PF_MergerLoop :
*/
/**
 * Mapper-merger merge loop. Runs after the mapper's own EndSort + done-send.
 * Sets up the merger-specific state (group rank table, sort context,
 * sbuf redirection, in_merger_phase) and delegates to PF_EndSort, which
 * shares the loser-tree merge body with the master. PF_EndSort uses
 * pf_loser_src_to_rank() and PF.merger_groupsz to size the tree and route
 * Irecvs to PF.merger_leaf_ranks[i].
 *
 * @return 0 on success, -1 on error.
 */
LONG PF_MergerLoop(void)
{
	GETIDENTITY
	if ( !PF.is_merger || PF.nummergers <= 0 ) return 0;

	if ( pf_setup_merger_group() < 0 ) return -1;

	/* --- Output redirection: where this merger's merged stream goes. -----------
	   Two modes, decided once and torn down symmetrically below:

	     partitioned (FIRST/MAPREDUCE): write to this merger's node-local file.
	       We set merger_to_file and truncate merger_outfile; PF_EndSort then picks
	       fout = PF.merger_outfile, and because that handle is NOT AR.outfile every
	       "fi==AR.outfile" disk-vs-MPI gate in PutOut/FlushOut routes to disk. So
	       AR.outfile is never touched and needs no save/restore.

	     gather (LAST / no partition): stream upstream to the master via
	       PF.sbufs[MASTER]. PF_allocateSbuf redirects AR.outfile->PObuffer to the
	       sbuf slot, so we snapshot AR.outfile here and restore it after EndSort. */
	int partitioned = pf_output_partitioned();
	FILEHANDLE *fout = AR.outfile;
	WORD *save_PObuffer = 0, *save_POstop = 0, *save_POfill = 0, *save_POfull = 0;
	LONG  save_POsize = 0;

	if ( partitioned ) {
		if ( pf_ensure_merger_files() < 0 ) return -1;
		FILEHANDLE *mf = PF.merger_outfile;       /* fresh output file: truncate + rewind */
		if ( mf->handle >= 0 ) { CloseFile(mf->handle); mf->handle = -1; }
		mf->POfill = mf->POfull = mf->PObuffer;
		PUTZERO(mf->POposition);
		PUTZERO(mf->filesize);
		PF.merger_to_file = 1;
	}

	NewSort(BHEAD0);
	PF.parallel = 1;
	/* in_merger_phase gates: sort.c FlushOut routing (upstream to merger_parent,
	   not reducers), PF_LowMRsort (no hash routing in the merge phase),
	   PF_WISendSbuf (BUFFER/ENDBUFFER tags upstream not SHUFFLE), and PF_EndSort's
	   slave-init branch (fall into the shared merge body). */
	PF.in_merger_phase = 1;

	if ( !partitioned ) {
		save_PObuffer = fout->PObuffer; save_POstop = fout->POstop;
		save_POsize   = fout->POsize;   save_POfill = fout->POfill;
		save_POfull   = fout->POfull;
		if ( PF_allocateSbuf() == 0 ) {
			MesPrint("[%d] PF_MergerLoop: PF_allocateSbuf failed", PF.me);
			PF.in_merger_phase = 0;
			return -1;
		}
	}

	/* Run the shared merge body. Call EndSort (sort.c) rather than PF_EndSort
	   directly: EndSort handles AR.sLevel-- cleanup, which is critical so the
	   next module's NewSort lands at sLevel=0 (AT.S0) instead of sub-sort.
	   EndSort calls PF_EndSort first; PF_EndSort's master path returns 1
	   here (in_merger_phase set), and EndSort then jumps to RetRetval which
	   decrements sLevel. MER_MERGE times the whole merge pass; the recv-wait
	   sub-component is attributed to MER_RECV_WAIT inside PF_PutIn. */
	LONG ret;
	{
		PF_TIMER_BEGIN(MER_MERGE);
		ret = EndSort(BHEAD AM.S0->sBuffer, 0);
		PF_TIMER_END(MER_MERGE);
	}

	PF.in_merger_phase = 0;

	if ( partitioned ) {
		PF.merger_to_file = 0;
		/* Report done + this merger's output count and byte size to the master.
		   PF_goutterms / PF_exprsize were set by the merger's own PF_EndSort. */
		LONG cnt = PF_goutterms;
		LONG sz  = BASEPOSITION(PF_exprsize);
		PF_TIMER_ADD_COUNT(PF_EX_MER_PARTITION_BYTES, sz);
		PF_PreparePack();
		PF_Pack(&cnt, 1, PF_LONG);
		PF_Pack(&sz,  1, PF_LONG);
		PF_Send(MASTER, PF_MERGERDONE_MSGTAG);
		/* Swap so merger_infile holds THIS module's output, ready for the next
		   module's PF_MergerDistribute; merger_outfile (old infile) is reused and
		   re-truncated at the top of the next partitioned PF_MergerLoop. */
		{
			FILEHANDLE *tmp = PF.merger_infile;
			PF.merger_infile  = PF.merger_outfile;
			PF.merger_outfile = tmp;
		}
	}
	else {
		/* Un-redirect fout (PF_allocateSbuf clobbered it). */
		fout->PObuffer = save_PObuffer;
		fout->POstop   = save_POstop;
		fout->POsize   = save_POsize;
		fout->POfill   = save_POfill;
		fout->POfull   = save_POfull;
	}

	return ret < 0 ? -1 : 0;
}
/*
	#] PF_MergerLoop :
	#[ PF_allocateSbuf :
*/
/**
 *	Allocates the Send buffers
 *
 *	@return  the size of each buffer, 0 if there is an error
 */
LONG PF_allocateSbuf()
{
	if( PF.sbufs == NULL )
	{
		if ((PF.sbufs = (PF_BUFFER**)Malloc1(PF.numtasks*sizeof(PF_BUFFER), "Reducer: sbufs") ) == NULL ) {MesPrint("Error in endsort"); return 0;}
		PF.sbufs[0] = 0;
		for(int i = 0 ; i < PF.numtasks; i++)
			PF.sbufs[i] = NULL;
	}
	PF_BUFFER *sbuf=PF.sbufs[0];
	LONG size;
	if(PF.me == MASTER){
		size = (LONG)((AT.SS->sTop2 - AT.SS->lBuffer)/(PF.numtasks));
		if ( size > (LONG)(AR.infile->POsize/sizeof(WORD) - 1) )
			size = AR.infile->POsize/sizeof(WORD) - 1;
		if ( sbuf == 0 || sbuf->buff[0] != AT.SS->lBuffer ) {
			size = (LONG)((AT.SS->sTop2 - AT.SS->lBuffer)/(PF.numtasks));
			if ( size > (LONG)(AR.infile->POsize/sizeof(WORD) - 1) )
				size = AR.infile->POsize/sizeof(WORD) - 1;
			if ( sbuf == 0 ) { // allocate a buffer for each slave - use lbuffer space
				if ( ( sbuf = PF_AllocBuf(PF.numtasks,size*sizeof(WORD),PF.numtasks) ) == NULL )
					return(-1);
			}
			sbuf->buff[0] = AT.SS->lBuffer;
			sbuf->full[0] = sbuf->fill[0] = sbuf->buff[0];
			for (int j = 1; j < PF.numtasks; j++ ) {
				sbuf->stop[j-1] = sbuf->buff[j] = sbuf->buff[j-1] + size;
			}
			sbuf->stop[PF.numtasks-1] = sbuf->buff[PF.numtasks-1] + size;
			PF.sbufs[0] = sbuf;
		}
		for (int j = 0; j < PF.numtasks; j++ ) {
			sbuf->full[j] = sbuf->fill[j] = sbuf->buff[j];
		}
	}
	else{
		/* Worker sbuf -- used by mapper (->reducer shuffle), reducer (->master
		   or merger forward), and merger (->master forward). All three are in
		   the shuffle-pair chain: receiver Irecv capacity is sized by
		   PF.shuffle_arena_words (PF_InitTree:473 / PF_ReducerInit:2812), so
		   the sender slot must use the same arena to avoid MPI_ERR_TRUNCATE
		   when a reducer's local arena is larger via reducer* form.set keys. */
		size = (PF.shuffle_arena_words - 1)/(PF.numtasks - 1);
		size -= (AM.MaxTer/sizeof(WORD) + 2);
		if( size <= 0) size = (LONG)(2*(AM.MaxTer/sizeof(WORD) + 2));
		if( sbuf == 0){
			if ( (sbuf = PF_AllocBuf(PF.numsbufs, size*sizeof(WORD), 1)) == NULL ) return 0;
			sbuf->active = 0;
		}
		for (int i = 0; i < PF.numsbufs; i++ )
			sbuf->fill[i] = sbuf->full[i] = sbuf->buff[i];
		FILEHANDLE *fout = AR.outfile;
		sbuf->buff[0] = fout->PObuffer;
		sbuf->stop[0] = fout->PObuffer+size;
		if ( sbuf->stop[0] > fout->POstop ) return -1;
		sbuf->fill[0] = sbuf->full[0] = sbuf->buff[0];
		sbuf->active = 0;

		fout->PObuffer = sbuf->buff[sbuf->active];
		fout->POstop = sbuf->stop[sbuf->active];
		fout->POsize = size*sizeof(WORD);
		fout->POfill = fout->POfull = fout->PObuffer;
	}
	PF.sbufs[0] = sbuf;
	return size;
	//size = (PF.sbufs[0]->stop[0] - PF.sbufs[0]->buff[0])/sizeof(WORD);
}
/*
	#] PF_allocateSbuf :
	#[ PF_allocatePFTerm :
*/
/**
 *	Allocates the PF_Term structure to store previous term for compression
 *
 *	@param numtasks Number of tasks the process can receive from
 *	@return  Regular return conventions (OK -> 0)
 */
int PF_allocatePFTerm(int numtasks)
{
	UBYTE *p, *stop;
	int size =  2*numtasks*sizeof(WORD*) + sizeof(WORD)*
		( numtasks*(1 + AM.MaxTal) + (AM.MaxTer/sizeof(WORD)+1) + 2*(AM.MaxTal+2));

	PF_term = (WORD **)Malloc1(size,"PF_term");
	stop = ((UBYTE*)PF_term) + size;
	p = ((UBYTE*)PF_term) + numtasks*sizeof(WORD*);

	PF_newcpos = (WORD **)p;  p += sizeof(WORD*) * numtasks;
	PF_newclen =  (WORD *)p;  p += sizeof(WORD)  * numtasks;
	for (int i = 0; i < numtasks; i++ ) {
		PF_newcpos[i] = (WORD *)p; p += sizeof(WORD)*AM.MaxTal;
		PF_newclen[i] = 0;
	}
	PF_WorkSpace = (WORD *)p;    p += AM.MaxTer+sizeof(WORD);
	PF_ScratchSpace = (UWORD*)p; p += 2*(AM.MaxTal+2)*sizeof(UWORD);

	if ( p != stop ) { MesPrint("error in PF_InitTree"); return(-1); }
	return 0;
	
}
/*
	#] PF_allocatePFTerm :
  	#] proces.c : 
  	#[ startup :, prepro & compile
 		#[ PF_Init :
*/

/**
 * All the library independent stuff.
 * PF_LibInit() should do all library dependent initializations.
 *
 * @param  argc  pointer to the number of arguments.
 * @param  argv  pointer to the arguments.
 * @return       0 if OK, nonzero on error.
 */
int PF_Init(int *argc, char ***argv)
{
/*
		this should definitely be somewhere else ...
*/
	PF_CurrentBracket = 0;

	PF.numtasks = 0; /* number of tasks, is determined in PF_LibInit ! */
	PF.numsbufs = 2; /* might be changed by the environment variable on the master ! */
	PF.numrbufs = 2; /* might be changed by the environment variable on the master ! */

	int ret = PF_LibInit(argc,argv);
	if (ret) { return ret; }
	PF_RealTime(PF_RESET);

	PF.log = 0;
	PF.parallel = 0;
	PF_statsinterval = 10;
	PF.rhsInParallel=1;
	PF.exprbufsize=4096;/*in WORDs*/
	AM.MR.HashPrefixWords = 0;
	PF.steal = 0;

#ifdef PF_WITHGETENV
	if ( PF.me == MASTER ) {
		char *c;
/*
			get these from the environment at the moment should be in setfile/tail
*/
		if ( ( c = getenv("PF_LOG") ) != 0 ) {
			if ( *c ) PF.log = (int)atoi(c);
			else PF.log = 1;
			fprintf(stderr,"[%d] changing PF.log to %d\n",PF.me,PF.log);
			fflush(stderr);
		}
		if ( ( c = (char*)getenv("PF_RBUFS") ) != 0 ) {
			PF.numrbufs = (int)atoi(c);
			//fprintf(stdout,"[%d] changing numrbufs to: %d\n",PF.me,PF.numrbufs);
			//fflush(stdout);
		}
		if ( ( c = (char*)getenv("PF_SBUFS") ) != 0 ) {
			PF.numsbufs = (int)atoi(c);
			//fprintf(stdout,"[%d] changing numsbufs to: %d\n",PF.me,PF.numsbufs);
			//fflush(stdout);
		}
		if ( PF.numsbufs > 10 ) PF.numsbufs = 10;
		if ( PF.numsbufs <  1 ) PF.numsbufs = 1;
		if ( PF.numrbufs >  2 ) PF.numrbufs = 2;
		if ( PF.numrbufs <  1 ) PF.numrbufs = 1;

		if ( ( c = (char*)getenv("PF_SHUFFLE_NOCOMPRESS") ) != 0 ) {
			PF_shuffle_nocompress = (int)atoi(c);
			fprintf(stderr,"[%d] PF_shuffle_nocompress=%d\n",PF.me,PF_shuffle_nocompress);
			fflush(stderr);
		}

		if ( ( c = getenv("PF_STATS") ) ) {
			UBYTE lbuf[24];
			PF_statsinterval = (int)atoi(c);
			NumToStr(lbuf,PF_statsinterval);
			fprintf(stderr,"[%d] changing PF_statsinterval to %s\n",PF.me,lbuf);
			fflush(stderr);
			if ( PF_statsinterval < 1 ) PF_statsinterval = 10;
		}

		if ( ( c = (char*)getenv("PF_HASH_PREFIX_WORDS") ) != 0 ) {
			AM.MR.HashPrefixWords = (int)atoi(c);
			if ( AM.MR.HashPrefixWords < 0 ) AM.MR.HashPrefixWords = 0;
			if ( AM.MR.HashPrefixWords > PF_DRAIN_MAX_K )
				AM.MR.HashPrefixWords = PF_DRAIN_MAX_K;
		}
		if ( ( c = (char*)getenv("PF_MERGERS") ) != 0 ) {
			/* On/off switch for the merger tier. Any value > 0 turns it on;
			   placement is always node-local and the merger count is derived
			   in PF_Processor as the number of reducer-bearing nodes. */
			PF.nummergers = (int)atoi(c);
			if ( PF.nummergers < 0 ) PF.nummergers = 0;
		}
		if ( ( c = (char*)getenv("PF_STEAL") ) != 0 ) {
			/* Work-stealing load balancer (Step 2). Only meaningful with the
			   merger tier on (PF_MERGERS); a drained node's mappers steal
			   node-remote buckets via the master broker. Default off. */
			PF.steal = ( (int)atoi(c) > 0 ) ? 1 : 0;
		}
	}
#endif
/*
  	#[ Broadcast settings from getenv: could also be done in PF_DoSetup
*/
	if ( PF.me == MASTER ) {
		PF_PreparePack();
		PF_Pack(&PF.log,1,PF_INT);
		PF_Pack(&PF.numrbufs,1,PF_WORD);
		PF_Pack(&PF.numsbufs,1,PF_WORD);
		PF_Pack(&AM.MR.HashPrefixWords,1,PF_INT);
		PF_Pack(&PF.nummergers,1,PF_INT);
		PF_Pack(&PF.steal,1,PF_INT);
	}
	PF_Broadcast();
	if ( PF.me != MASTER ) {
		PF_Unpack(&PF.log,1,PF_INT);
		PF_Unpack(&PF.numrbufs,1,PF_WORD);
		PF_Unpack(&PF.numsbufs,1,PF_WORD);
		PF_Unpack(&AM.MR.HashPrefixWords,1,PF_INT);
		PF_Unpack(&PF.nummergers,1,PF_INT);
		PF_Unpack(&PF.steal,1,PF_INT);
		if ( PF.log ) {
			fprintf(stderr, "[%d] log=%d rbufs=%d sbufs=%d mergers=%d\n",
			        PF.me, PF.log, PF.numrbufs, PF.numsbufs, PF.nummergers);
			fflush(stderr);
		}
	}
/*
  	#] Broadcast settings from getenv: 
*/
	return(0);
}
/*
 		#] PF_Init : 
 		#[ PF_Terminate :
*/

/**
 * Performs the finalization of ParFORM.
 * To be called by Terminate().
 *
 * @param  error  an error code.
 * @return        0 if OK, nonzero on error.
 */
int PF_Terminate(int errorcode)
{
	return PF_LibTerminate(errorcode);
}

/*
 		#] PF_Terminate : 
 		#[ PF_GetSlaveTimes :
*/

/**
 * Returns the total CPU time of all slaves together.
 * This function must be called on the master and all slaves.
 *
 * @return  on the master, the sum of CPU times on all slaves.
 */
LONG PF_GetSlaveTimes(void)
{
	LONG slavetimes = 0;
	LONG t = PF.me == MASTER ? 0 : AM.SumTime + TimeCPU(1);
	MPI_Reduce(&t, &slavetimes, 1, PF_LONG, MPI_SUM, MASTER, PF_COMM);
	return slavetimes;
}

/*
 		#] PF_GetSlaveTimes : 
  	#] startup : 
  	#[ PF_BroadcastNumber :
*/

/**
 * Broadcasts a LONG value from the master to the all slaves.
 *
 * @param  x  the number to be broadcast (set on the master).
 * @return    the synchronised result.
 */
LONG PF_BroadcastNumber(LONG x)
{
#ifdef PF_DEBUG_BCAST_LONG
	if ( PF.me == MASTER ) {
		MesPrint(">> Broadcast LONG: %l", x);
	}
#endif
	PF_Bcast(&x, sizeof(LONG));
	return x;
}

/*
  	#] PF_BroadcastNumber : 
  	#[ PF_BroadcastBuffer :
*/

/**
 * Broadcasts a buffer from the master to all the slaves.
 *
 * @param[in,out]  buffer  on the master, the buffer to be broadcast. On the
 * slaves, the buffer will be allocated if the length is greater than 0. The
 * caller must free it.
 *
 * @param[in,out]  length  on the master, the length of the buffer to be
 * broadcast. On the slaves, it receives the length of transferred buffer.
 * The actual transfer occurs only if the length is greater than 0.
 */
void PF_BroadcastBuffer(WORD **buffer, LONG *length)
{
	WORD *p;
	LONG rest;
#ifdef PF_DEBUG_BCAST_BUF
	if ( PF.me == MASTER ) {
		MesPrint(">> Broadcast Buffer: length=%l", *length);
	}
#endif
	/* Initialize the buffer on the slaves. */
	if ( PF.me != MASTER ) {
		*buffer = NULL;
	}
	/* Broadcast the length of the buffer. */
	*length = PF_BroadcastNumber(*length);
	if ( *length <= 0 ) return;
	/* Allocate the buffer on the slaves. */
	if ( PF.me != MASTER ) {
		*buffer = (WORD *)Malloc1(*length * sizeof(WORD), "PF_BroadcastBuffer");
	}
	/* Broadcast the data in the buffer. */
	p = *buffer;
	rest = *length;
	while ( rest > 0 ) {
		int l = rest < (LONG)PF.exprbufsize ? (int)rest : PF.exprbufsize;
		PF_Bcast(p, l * sizeof(WORD));
		p += l;
		rest -= l;
	}
}

/*
  	#] PF_BroadcastBuffer : 
  	#[ PF_BroadcastString :
*/

/**
 * Broadcasts a string from the master to all slaves.
 *
 * @param[in,out]  str  The pointer to a null-terminated string.
 * @return              0 if OK, nonzero on error.
 */
int PF_BroadcastString(UBYTE *str)
{
	int clength = 0;
/*
		If string does not fit to the PF_buffer, it
		will be split into chunks. Next chunk is started at  str+clength
*/
		UBYTE *cstr=str;
/*
		Note, compilation is performed INDEPENDENTLY on AC.mparallelflag!
		No if ( AC.mparallelflag == PARALLELFLAG ) !!
*/
	do {
		cstr += clength; /*at each step for all slaves and master */

		if ( MASTER == PF.me ) { /*Pack str*/
/*
				initialize buffers
*/
			if ( PF_PreparePack() != 0 ) Terminate(-1);
			if ( ( clength = PF_PackString(cstr) ) <0  ) Terminate(-1);
		}
		PF_Broadcast();

		if ( MASTER != PF.me ) {
/*
				Slave - unpack received string
				For slaves buffers are initialised automatically.
*/
			if ( ( clength = PF_UnpackString(cstr) ) < 0 ) Terminate(-1);
		}
	} while ( cstr[clength-1] != '\0' );
	return (0);
}

/*
  	#] PF_BroadcastString : 
  	#[ PF_BroadcastPreDollar :
*/

/**
 * Broadcasts dollar variables set as a preprocessor variables.
 * Only the master is able to make an assignment like #$a=g; where g
 * is an expression: only the master has an access to the expression.
 * So, the master broadcasts the result to slaves.
 *
 * The result is in *dbuffer of the size is *newsize (in number of WORDs),
 * +1 for trailing zero. For slave newsize and numterms are output
 * parameters.
 *
 * @param[in,out]  dbuffer   the buffer for a dollar variable.
 * @param[in,out]  newsize   the size of the dollar variable in WORDs.
 * @param[in,out]  numterms  the number of terms in the dollar variable.
 * @return                   0 if OK, nonzero on error.
 */
int PF_BroadcastPreDollar(WORD **dbuffer, LONG *newsize, int *numterms)
{
	int err = 0;
	LONG i;
/*
		Note, compilation is performed INDEPENDENTLY on AC.mparallelflag!
		No if(AC.mparallelflag==PARALLELFLAG) !!
*/
	if ( MASTER == PF.me ) {
/*
			The problem is that sometimes dollar variables are longer
			than PF_packbuf! So we split long expression into chunks.
			There are n filled chunks and one partially filled chunk:
*/
		LONG n = ((*newsize)+1)/PF_maxDollarChunkSize;
/*
			...and one more chunk for the rest; if the expression fits to
			the buffer without splitting, the latter will be the only one.

			PF_maxDollarChunkSize is the maximal number of items fitted to
			the buffer. It is calculated in PF_LibInit() in mpi.c.
			PF_maxDollarChunkSize is calculated for the first step, when
			two fields (numterms and newsize, see below) are already packed.
			For simplicity, this value is used also for all steps, in
			despite  of it is	a bit less than maximally available space.
*/
		WORD *thechunk = *dbuffer;

		err = PF_PreparePack();             /* initialize buffers */
		err |= PF_Pack(numterms,1,PF_INT);
		err |= PF_Pack(newsize,1,PF_LONG); /* pack the size */
/*
			Pack and broadcast completely filled chunks.
			It may happen, this loop is not entered at all:
*/
		for ( i = 0; i < n; i++ ) {
			err |= PF_Pack(thechunk,PF_maxDollarChunkSize,PF_WORD);
			err |= PF_Broadcast();
			thechunk +=PF_maxDollarChunkSize;
			PF_PreparePack();
		}
/*
			Pack and broadcast the rest:
*/
		if ( ( n = ( (*newsize)+1)%PF_maxDollarChunkSize ) != 0 ) {
			err |= PF_Pack(thechunk,n,PF_WORD);
			err |= PF_Broadcast();
		}
#ifdef PF_DEBUG_BCAST_PREDOLLAR
		MesPrint(">> Broadcast PreDollar: newsize=%d numterms=%d", (int)*newsize, *numterms);
#endif
	}
	if ( MASTER != PF.me ) {  /* Slave - unpack received buffer */
		WORD *thechunk;
		LONG n, therest, thesize;
		err |= PF_Broadcast();
		err |=PF_Unpack(numterms,1,PF_INT);
		err |=PF_Unpack(newsize,1,PF_LONG);
/*
			Now we know the buffer size.
*/
		thesize = (*newsize)+1;
/*
			Evaluate the number of completely filled chunks. The last step must be
			treated separately, so -1:
*/
		n = (thesize/PF_maxDollarChunkSize) - 1;
/*
			Note, here n can be <0, this is ok.
*/
		therest = thesize % PF_maxDollarChunkSize;
		thechunk = *dbuffer =
			(WORD*)Malloc1( thesize * sizeof(WORD),"$-buffer slave");
		if ( thechunk == NULL ) return(err|4);
/*
			Unpack completely filled chunks and receive the next portion.
			It may happen, this loop is not entered at all:
*/
		for ( i = 0; i < n; i++ ) {
			err |= PF_Unpack(thechunk,PF_maxDollarChunkSize,PF_WORD);
			thechunk += PF_maxDollarChunkSize;
			err |= PF_Broadcast();
		}
/*
			Now the last completely filled chunk:
*/
		if ( n >= 0 ) {
			err |= PF_Unpack(thechunk,PF_maxDollarChunkSize,PF_WORD);
			thechunk += PF_maxDollarChunkSize;
			if ( therest != 0 ) err |= PF_Broadcast();
		}
/*
			Unpack the rest (it is already received!):
*/
		if ( therest != 0 ) err |= PF_Unpack(thechunk,therest,PF_WORD);
	}
	return (err);
}

/*
  	#] PF_BroadcastPreDollar : 
  	#[ Synchronization of modified dollar variables :
 		#[ Helper functions :
			#[ dollarlen :
*/

/**
 * Returns the size of \a terms in WORDs, not including the null terminator.
 */
static inline LONG dollarlen(const WORD *terms)
{
	const WORD *p = terms;
	while ( *p ) p += *p;
	return p - terms;  /* Not including the null terminator. */
}

/*
			#] dollarlen : 
			#[ dollar_mod_type :
*/

/**
 * Returns the module option type of a dollar variable specified by \a index.
 * If no module option is given for the variable, this function returns -1.
 */
static inline WORD dollar_mod_type(WORD index)
{
	int i;
	for ( i = 0; i < NumModOptdollars; i++ )
		if ( ModOptdollars[i].number == index ) break;
	if ( i >= NumModOptdollars ) return -1;
	return ModOptdollars[i].type;
}


/*
			#] dollar_mod_type : 
 		#] Helper functions : 
 		#[ PF_CollectModifiedDollars :
*/

/*
			#[ dollar_to_be_collected :
*/

/**
 * Returns true if the dollar variable specified by \a index has to be collected
 * from each slave to the master, i.e., declared as MODSUM, MODMAX or MODMIN.
 */
static inline int dollar_to_be_collected(WORD index)
{
	switch ( dollar_mod_type(index) ) {
		case MODSUM:
		case MODMAX:
		case MODMIN:
			return 1;
		default:
			return 0;
	}
}

/*
			#] dollar_to_be_collected : 
			#[ copy_dollar :
*/

/**
 * Copy the data given by \a type, \a where and \a size to a dollar variable
 * specified by \a index.
 */
static inline void copy_dollar(WORD index, WORD type, const WORD *where, LONG size)
{
	DOLLARS d = Dollars + index;

	CleanDollarFactors(d);

	if ( type != DOLZERO && where != NULL && where != &AM.dollarzero && where[0] != 0 && size > 0 ) {
		if ( size > d->size || size < d->size / 4 ) {  /* Reallocate if not enough or too much. */
			if ( d->where && d->where != &AM.dollarzero )
				M_free(d->where, "old content of dollar");
			d->where = Malloc1(sizeof(WORD) * size, "copy buffer to dollar");
			d->size  = size;
		}
		d->type  = type;
		WCOPY(d->where, where, size);
	}
	else {
		if ( d->where && d->where != &AM.dollarzero )
			M_free(d->where, "old content of dollar");
		d->type  = DOLZERO;
		d->where = &AM.dollarzero;
		d->size  = 0;
	}
}

/*
			#] copy_dollar : 
			#[ compare_two_expressions :
*/

/**
 * Compares two expressions \a e1 and \a e2 and returns a positive value if
 * \a e1 > \a e2, a negative value if \a e1 < \a e2, or zero if \a e1 == \a e2.
 */
static inline int compare_two_expressions(const WORD *e1, const WORD *e2)
{
	GETIDENTITY
	/*
	 * We consider the cases that
	 *   (1) the expression has no term,
	 *   (2) the expression has only one term and it is a number,
	 *   (3) otherwise.
	 * Assume that the expressions are sorted and all terms are normalized.
	 * The numerators of the coefficients must never be zero.
	 *
	 * Note that TwoExprCompare() is not adequate for our purpose
	 * (as of 6 Aug. 2013), e.g., TwoExprCompare({0}, {4, 1, 1, -1}, LESS)
	 * returns TRUE.
	 */
	if ( e1[0] == 0 ) {
		if ( e2[0] == 0 ) {
			return(0);
		}
		else if ( e2[e2[0]] == 0 && e2[0] == ABS(e2[e2[0] - 1]) + 1 ) {
			if ( e2[e2[0] - 1] > 0 )
				return(-1);
			else
				return(+1);
		}
	}
	else if ( e1[e1[0]] == 0 && e1[0] == ABS(e1[e1[0] - 1]) + 1 ) {
		if ( e2[0] == 0 ) {
			if ( e1[e1[0] - 1] > 0 )
				return(+1);
			else
				return(-1);
		}
		else if ( e2[e2[0]] == 0 && e2[0] == ABS(e2[e2[0] - 1]) + 1 ) {
			return(CompCoef((WORD *)e1, (WORD *)e2));
		}
	}
	/* The expressions are not so simple. Define the order by each term. */
	while ( e1[0] && e2[0] ) {
		int c = CompareTerms(BHEAD (WORD *)e1, (WORD *)e2, 1);
		if ( c < 0 )
			return(-1);
		else if ( c > 0 )
			return(+1);
		e1 += e1[0];
		e2 += e2[0];
	}
	if ( e1[0] ) return(+1);
	if ( e2[0] ) return(-1);
	return(0);
}

/*
			#] compare_two_expressions : 
			#[ Variables :
*/

typedef struct {
	VectorStruct(WORD) buf;
	LONG size;
	WORD type;
} dollar_buf;

/* Buffers used to store data for each variable from each slave. */
static Vector(dollar_buf, dollar_slave_bufs);

/*
			#] Variables : 
*/

/**
 * Combines modified dollar variables on the all slaves, and store them into
 * those on the master.
 *
 * The potentially modified dollar variables are given in PotModdollars,
 * and the number of them is given by NumPotModdollars.
 *
 * The current module could be executed in parallel only if all potentially
 * modified variables are listed in ModOptdollars, otherwise the module was
 * switched to the sequential mode.
 *
 * @return  0 if OK, nonzero on error.
 */
int PF_CollectModifiedDollars(void)
{
	int i, j, ndollars;
	/*
	 * If the current module was executed in the sequential mode,
	 * there are no modified module on the slaves.
	 */
	if ( AC.mparallelflag != PARALLELFLAG && !AC.partodoflag ) return 0;
	/*
	 * Count the number of (potentially) modified dollar variables, which we need to collect.
	 * Here we need to collect all max/min/sum variables.
	 */
	ndollars = 0;
	for ( i = 0; i < NumPotModdollars; i++ ) {
		WORD index = PotModdollars[i];
		if ( dollar_to_be_collected(index) ) ndollars++;
	}
	if ( ndollars == 0 ) return 0;  /* No dollars to be collected. */

	if ( PF.me == MASTER ) {
/*
			#[ Master :
*/
		int nslaves, nvars;
		/* Prepare receive buffers. We need ndollars*(PF.numtasks-1) buffers. */
		int nbufs = ndollars * (PF.numtasks - 1);
		VectorReserve(dollar_slave_bufs, nbufs);
		for ( i = VectorSize(dollar_slave_bufs); i < nbufs; i++ ) {
			VectorInit(VectorPtr(dollar_slave_bufs)[i].buf);
		}
		VectorSize(dollar_slave_bufs) = nbufs;
		/* Receive data from each slave. */
		for ( nslaves = 1; nslaves < PF.numtasks; nslaves++ ) {
			int src;
			PF_LongSingleReceive(PF_ANY_SOURCE, PF_DOLLAR_MSGTAG, &src, NULL);
			nvars = 0;
			for ( i = 0; i < NumPotModdollars; i++ ) {
				WORD index = PotModdollars[i];
				dollar_buf *b;
				if ( !dollar_to_be_collected(index) ) continue;
				b = &VectorPtr(dollar_slave_bufs)[(PF.numtasks - 1) * nvars + (src - 1)];
				PF_LongSingleUnpack(&b->type, 1, PF_WORD);
				if ( b->type != DOLZERO ) {
					LONG size;
					WORD *where;
					PF_LongSingleUnpack(&size, 1, PF_LONG);
					VectorReserve(b->buf, size + 1);
					where = VectorPtr(b->buf);
					PF_LongSingleUnpack(where, size, PF_WORD);
					where[size] = 0;  /* The null terminator is needed. */
					b->size = size + 1;  /* Including the null terminator. */
					/* Note that we don't collect factored stuff for max/min/sum variables. */
				}
				else {
					VectorReserve(b->buf, 1);
					VectorPtr(b->buf)[0] = 0;
					b->size = 0;
				}
				nvars++;
			}
		}
		/*
		 * Combine received dollars. The FORM reference manual says maximum/minimum/sum
		 * $-variables must have a numerical value, however, this routine should work also
		 * for non-numerical cases, although the maximum/minimum value for non-numerical
		 * terms has ambiguity.
		 */
		nvars = 0;
		for ( i = 0; i < NumPotModdollars; i++ ) {
			WORD index = PotModdollars[i];
			WORD dtype;
			DOLLARS d;
			dollar_buf *b;
			if ( !dollar_to_be_collected(index) ) continue;
			d = Dollars + index;
			b = &VectorPtr(dollar_slave_bufs)[(PF.numtasks - 1) * nvars];
			dtype = dollar_mod_type(index);
			switch ( dtype ) {
				case MODMAX:
				case MODMIN: {
/*
			#[ MODMAX & MODMIN :
*/
					int selected = 0;
					for ( j = 1; j < PF.numtasks - 1; j++ ) {
						int c = compare_two_expressions(VectorPtr(b[j].buf), VectorPtr(b[selected].buf));
						if ( (dtype == MODMAX && c > 0) || (dtype == MODMIN && c < 0) )
							selected = j;
					}
					b = b + selected;
					copy_dollar(index, b->type, VectorPtr(b->buf), b->size);
/*
			#] MODMAX & MODMIN : 
*/
					break;
				}
				case MODSUM: {
/*
			#[ MODSUM :
*/
					GETIDENTITY
					int err = 0;

					CBUF *C = cbuf + AM.rbufnum;
					WORD *oldwork = AT.WorkPointer, *oldcterm = AN.cTerm;
					WORD olddefer = AR.DeferFlag, oldnumlhs = AR.Cnumlhs, oldnumrhs = C->numrhs;

					LONG size;
					WORD type, *dbuf;

					AN.cTerm = 0;
					AR.DeferFlag = 0;

					if ( ((WORD *)((UBYTE *)AT.WorkPointer + AM.MaxTer)) > AT.WorkTop ) {
						err = -1;
						goto cleanup;
						MesWork();
					}

					if ( NewSort(BHEAD0) ) {
						err = -1;
						goto cleanup;
					}
					if ( NewSort(BHEAD0) ) {
						LowerSortLevel();
						err = -1;
						goto cleanup;
					}

					/*
					 * Sum up the original $-variable in the master and $-variables on all slaves.
					 * Note that $-variables on the slaves are set to zero at the beginning of
					 * the module (See also DoExecute()).
					 */
					for ( j = 0; j < PF.numtasks; j++ ) {
						const WORD *r;
						for ( r = j == 0 ? Dollars[index].where : VectorPtr(b[j - 1].buf); *r; r += *r ) {
							WCOPY(AT.WorkPointer, r, *r);
							AT.WorkPointer += *r;
							AR.Cnumlhs = 0;
							if ( Generator(BHEAD oldwork, 0) ) {
								LowerSortLevel(); LowerSortLevel();
								err = -1;
								goto cleanup;
							}
							AT.WorkPointer = oldwork;
						}
					}

					size = EndSort(BHEAD (WORD *)&dbuf, 2);
					if ( size < 0 ) {
						LowerSortLevel();
						err = -1;
						goto cleanup;
					}
					LowerSortLevel();

					/* Find special cases. */
					type = DOLTERMS;
					if ( dbuf[0] == 0 ) {
						type = DOLZERO;
					}
					else if ( dbuf[dbuf[0]] == 0 ) {
						const WORD *t = dbuf, *w;
						WORD n, nsize;
						n = *t;
						nsize = t[n - 1];
						if ( nsize < 0 ) nsize = -nsize;
						if ( nsize == n - 1 ) {
							nsize = (nsize - 1) / 2;
							w = t + 1 + nsize;
							if ( *w == 1 ) {
								w++; while ( w < t + n - 1 ) { if ( *w ) break; w++; }
								if ( w >= t + n - 1 ) type =  DOLNUMBER;
							}
							else if ( n == 7 && t[6] == 3 && t[5] == 1 && t[4] == 1 && t[1] == INDEX && t[2] == 3 ) {
								type = DOLINDEX;
								d->index = t[3];
							}
						}
					}
					copy_dollar(index, type, dbuf, dollarlen(dbuf) + 1);
					M_free(dbuf, "temporary dollar buffer");
cleanup:
					AR.Cnumlhs = oldnumlhs;
					C->numrhs = oldnumrhs;
					AR.DeferFlag = olddefer;
					AN.cTerm = oldcterm;
					AT.WorkPointer = oldwork;

					if ( err ) return err;
/*
			#] MODSUM : 
*/
					break;
				}
			}
			if ( d->type == DOLTERMS )
				cbuf[AM.dbufnum].CanCommu[index] = numcommute(d->where, &cbuf[AM.dbufnum].NumTerms[index]);
			cbuf[AM.dbufnum].rhs[index] = d->where;
			nvars++;
#ifdef PF_DEBUG_REDUCE_DOLLAR
			MesPrint("<< Reduce $-var: %s", AC.dollarnames->namebuffer + d->name);
#endif
		}
/*
			#] Master : 
*/
	}
	else {
/*
			#[ Slave :
*/
		PF_PrepareLongSinglePack();
		/* Pack each variable. */
		for ( i = 0; i < NumPotModdollars; i++ ) {
			WORD index = PotModdollars[i];
			DOLLARS d;
			if ( !dollar_to_be_collected(index) ) continue;
			d = Dollars + index;
			PF_LongSinglePack(&d->type, 1, PF_WORD);
			if ( d->type != DOLZERO ) {
				/*
				 * NOTE: d->size is the allocated buffer size for d->where in WORDs.
				 *       So dollarlen(d->where) can be < d->size-1. (TU 15 Dec 2011)
				 */
				LONG size = dollarlen(d->where);
				PF_LongSinglePack(&size, 1, PF_LONG);
				PF_LongSinglePack(d->where, size, PF_WORD);
				/* Note that we don't collect factored stuff for max/min/sum variables. */
			}
		}
		PF_LongSingleSend(MASTER, PF_DOLLAR_MSGTAG);
/*
			#] Slave : 
*/
	}
	return 0;
}

/*
 		#] PF_CollectModifiedDollars : 
 		#[ PF_BroadcastModifiedDollars :
*/

/*
			#[ dollar_to_be_broadcast :
*/

/**
 * Returns true if the dollar variable specified by \a index has to be broadcast
 * from the master to the all slaves, i.e., non-local.
 */
static inline int dollar_to_be_broadcast(WORD index)
{
	switch ( dollar_mod_type(index) ) {
		case MODLOCAL:
			return 0;
		default:
			return 1;
	}
}

/*
			#] dollar_to_be_broadcast : 
*/

/**
 * Broadcasts modified dollar variables on the master to the all slaves.
 *
 * The potentially modified dollar variables are given in PotModdollars,
 * and the number of them is given by NumPotModdollars.
 *
 * The current module could be executed in parallel only if all potentially
 * modified variables are listed in ModOptdollars, otherwise the module was
 * switched to the sequential mode. In either cases, we need to broadcast them.
 *
 * @return  0 if OK, nonzero on error.
 */
int PF_BroadcastModifiedDollars(void)
{
	int i, j, ndollars;
	/*
	 * Count the number of (potentially) modified dollar variables, which we need to broadcast.
	 * Here we need to broadcast all non-local variables.
	 */
	ndollars = 0;
	for ( i = 0; i < NumPotModdollars; i++ ) {
		WORD index = PotModdollars[i];
		if ( dollar_to_be_broadcast(index) ) ndollars++;
	}
	if ( ndollars == 0 ) return 0;  /* No dollars to be broadcast. */

	if ( PF.me == MASTER ) {
/*
			#[ Master :
*/
		PF_PrepareLongMultiPack();
		/* Pack each variable. */
		for ( i = 0; i < NumPotModdollars; i++ ) {
			WORD index = PotModdollars[i];
			DOLLARS d;
			if ( !dollar_to_be_broadcast(index) ) continue;
			d = Dollars + index;
			PF_LongMultiPack(&d->type, 1, PF_WORD);
			if ( d->type != DOLZERO ) {
				/*
				 * NOTE: d->size is the allocated buffer size for d->where in WORDs.
				 *       So dollarlen(d->where) can be < d->size-1. (TU 15 Dec 2011)
				 */
				LONG size = dollarlen(d->where);
				PF_LongMultiPack(&size, 1, PF_LONG);
				PF_LongMultiPack(d->where, size, PF_WORD);
				/* ...and the factored stuff. */
				PF_LongMultiPack(&d->nfactors, 1, PF_WORD);
				if ( d->nfactors > 1 ) {
					for ( j = 0; j < d->nfactors; j++ ) {
						FACDOLLAR *f = &d->factors[j];
						PF_LongMultiPack(&f->type, 1, PF_WORD);
						PF_LongMultiPack(&f->size, 1, PF_LONG);
						if ( f->size > 0 )
							PF_LongMultiPack(f->where, f->size, PF_WORD);
						else
							PF_LongMultiPack(&f->value, 1, PF_WORD);
					}
				}
			}
#ifdef PF_DEBUG_BCAST_DOLLAR
			MesPrint(">> Broadcast $-var: %s", AC.dollarnames->namebuffer + d->name);
#endif
		}
/*
			#] Master : 
*/
	}
	if ( PF_LongMultiBroadcast() ) return -1;
	if ( PF.me != MASTER ) {
/*
			#[ Slave :
*/
		for ( i = 0; i < NumPotModdollars; i++ ) {
			WORD index = PotModdollars[i];
			DOLLARS d;
			if ( !dollar_to_be_broadcast(index) ) continue;
			d = Dollars + index;
			/* Clear the contents of the dollar variable. */
			if ( d->where && d->where != &AM.dollarzero )
				M_free(d->where, "old content of dollar");
			d->where = &AM.dollarzero;
			d->size = 0;
			CleanDollarFactors(d);
			/* Unpack and store the contents. */
			PF_LongMultiUnpack(&d->type, 1, PF_WORD);
			if ( d->type != DOLZERO ) {
				LONG size;
				PF_LongMultiUnpack(&size, 1, PF_LONG);
				d->size = size + 1;
				d->where = (WORD *)Malloc1(sizeof(WORD) * d->size, "dollar content");
				PF_LongMultiUnpack(d->where, size, PF_WORD);
				d->where[size] = 0;  /* The null terminator is needed. */
				/* ...and the factored stuff. */
				PF_LongMultiUnpack(&d->nfactors, 1, PF_WORD);
				if ( d->nfactors > 1 ) {
					d->factors = (FACDOLLAR *)Malloc1(sizeof(FACDOLLAR) * d->nfactors, "dollar factored stuff");
					for ( j = 0; j < d->nfactors; j++ ) {
						FACDOLLAR *f = &d->factors[j];
						PF_LongMultiUnpack(&f->type, 1, PF_WORD);
						PF_LongMultiUnpack(&f->size, 1, PF_LONG);
						if ( f->size > 0 ) {
							f->where = (WORD *)Malloc1(sizeof(WORD) * (f->size + 1), "dollar factor content");
							PF_LongMultiUnpack(f->where, f->size, PF_WORD);
							f->where[f->size] = 0;  /* The null terminator is needed. */
							f->value = 0;
						}
						else {
							f->where = NULL;
							PF_LongMultiUnpack(&f->value, 1, PF_WORD);
						}
					}
				}
			}
			if ( d->type == DOLTERMS )
				cbuf[AM.dbufnum].CanCommu[index] = numcommute(d->where, &cbuf[AM.dbufnum].NumTerms[index]);
			cbuf[AM.dbufnum].rhs[index] = d->where;
		}
/*
			#] Slave : 
*/
	}
	return 0;
}

/*
 		#] PF_BroadcastModifiedDollars : 
  	#] Synchronization of modified dollar variables : 
  	#[ Synchronization of redefined preprocessor variables :
 		#[ Variables :
*/

/* A buffer used in receivers. */
static Vector(UBYTE, prevarbuf);

/*
 		#] Variables : 
 		#[ PF_PackRedefinedPreVars :
*/

/**
 * Packs information of redefined preprocessor variables into the long single
 * pack buffer, with the corresponding value in AC.inputnumbers.
 *
 * The potentially redefined preprocessor variables are given in AC.pfirstnum,
 * and the number of them is given by AC.numpfirstnum. For an actually redefined
 * variable, the corresponding value in AC.inputnumbers is non-negative.
 */
static void PF_PackRedefinedPreVars(void)
{
	int i;
	/* First, pack the number of redefined preprocessor variables. */
	int nredefs = 0;
	for ( i = 0; i < AC.numpfirstnum; i++ )
		if ( AC.inputnumbers[i] >= 0 ) nredefs++;
	PF_LongSinglePack(&nredefs, 1, PF_INT);
	/* Then, pack each variable. */
	for ( i = 0; i < AC.numpfirstnum; i++ )
		if ( AC.inputnumbers[i] >= 0) {
			WORD index = AC.pfirstnum[i];
			UBYTE *value = PreVar[index].value;
			int bytes = strlen((char *)value);
			PF_LongSinglePack(&index, 1, PF_WORD);
			PF_LongSinglePack(&bytes, 1, PF_INT);
			PF_LongSinglePack(value, bytes, PF_BYTE);
			PF_LongSinglePack(&AC.inputnumbers[i], 1, PF_LONG);
	}
}

/*
 		#] PF_PackRedefinedPreVars : 
 		#[ PF_UnpackRedefinedPreVars :
*/

/**
 * Unpacks information of redefined preprocessor variables from the long single
 * pack buffer. If the attached value of the input number is greater than
 * the corresponding current value in AC.inputnumbers, this function updates
 * the preprocessor variable.
 *
 * The potentially redefined preprocessor variables are given in AC.pfirstnum,
 * and the number of them is AC.numpfirstnum.
 */
static void PF_UnpackRedefinedPreVars(void)
{
	int i, j;
	/* Unpack the number of redefined preprocessor variables. */
	int nredefs;
	PF_LongSingleUnpack(&nredefs, 1, PF_INT);
	if ( nredefs > 0 ) {
		/* Then unpack each variable. */
		for ( i = 0; i < nredefs; i++ ) {
			WORD index;
			int bytes;
			UBYTE *value;
			LONG inputnumber;
			PF_LongSingleUnpack(&index, 1, PF_WORD);
			PF_LongSingleUnpack(&bytes, 1, PF_INT);
			VectorReserve(prevarbuf, bytes + 1);
			value = VectorPtr(prevarbuf);
			PF_LongSingleUnpack(value, bytes, PF_BYTE);
			value[bytes] = '\0';  /* The null terminator is needed. */
			PF_LongSingleUnpack(&inputnumber, 1, PF_LONG);
			/* Put this variable if it must be updated. */
			for ( j = 0; j < AC.numpfirstnum; j++ )
				if ( AC.pfirstnum[j] == index ) break;
			if ( AC.inputnumbers[j] < inputnumber ) {
				AC.inputnumbers[j] = inputnumber;
				PutPreVar(PreVar[index].name, value, NULL, 1);
			}
		}
	}
}

/*
 		#] PF_UnpackRedefinedPreVars : 
 		#[ PF_BroadcastRedefinedPreVars :
*/

/**
 * Broadcasts preprocessor variables, which were changed by the Redefine statements
 * in the current module, from the master to the all slaves.
 *
 * The potentially redefined preprocessor variables are given in AC.pfirstnum,
 * and the number of them is given by AC.numpfirstnum. For an actually redefined
 * variable, the corresponding value in AC.inputnumbers is non-negative.
 *
 * @return  0 if OK, nonzero on error.
 */
int PF_BroadcastRedefinedPreVars(void)
{
	/*
	 * NOTE: Because the compilation is performed on the all processes
	 * independently on AC.mparallelflag, we always have to broadcast redefined
	 * preprocessor variables from the master to the all slaves.
	 */
	if ( PF.me == MASTER ) {
/*
			#[ Master :
*/
		int i, nredefs;
		PF_PrepareLongMultiPack();
		/* First, pack the number of redefined preprocessor variables. */
		nredefs = 0;
		for ( i = 0; i < AC.numpfirstnum; i++ )
			if ( AC.inputnumbers[i] >= 0 ) nredefs++;
		PF_LongMultiPack(&nredefs, 1, PF_INT);
		/* Then, pack each variable. */
		for ( i = 0; i < AC.numpfirstnum; i++ )
			if ( AC.inputnumbers[i] >= 0) {
				WORD index = AC.pfirstnum[i];
				UBYTE *value = PreVar[index].value;
				int bytes = strlen((char *)value);
				PF_LongMultiPack(&index, 1, PF_WORD);
				PF_LongMultiPack(&bytes, 1, PF_INT);
				PF_LongMultiPack(value, bytes, PF_BYTE);
#ifdef PF_DEBUG_BCAST_PREVAR
				MesPrint(">> Broadcast PreVar: %s = \"%s\"", PreVar[index].name, value);
#endif
			}
/*
			#] Master : 
*/
	}
	if ( PF_LongMultiBroadcast() ) return -1;
	if ( PF.me != MASTER ) {
/*
			#[ Slave :
*/
		int i, nredefs;
		/* Unpack the number of redefined preprocessor variables. */
		PF_LongMultiUnpack(&nredefs, 1, PF_INT);
		if ( nredefs > 0 ) {
			/* Then unpack each variable and put it. */
			for ( i = 0; i < nredefs; i++ ) {
				WORD index;
				int bytes;
				UBYTE *value;
				PF_LongMultiUnpack(&index, 1, PF_WORD);
				PF_LongMultiUnpack(&bytes, 1, PF_INT);
				VectorReserve(prevarbuf, bytes + 1);
				value = VectorPtr(prevarbuf);
				PF_LongMultiUnpack(value, bytes, PF_BYTE);
				value[bytes] = '\0';  /* The null terminator is needed. */
				PutPreVar(PreVar[index].name, value, NULL, 1);
			}
		}
/*
			#] Slave : 
*/
	}
	return 0;
}

/*
 		#] PF_BroadcastRedefinedPreVars : 
  	#] Synchronization of redefined preprocessor variables : 
  	#[ Preprocessor Inside instruction :
 		#[ Variables :
*/

/* Saved values of AC.RhsExprInModuleFlag, PotModdollars and AC.pfirstnum. */
static WORD oldRhsExprInModuleFlag;
static Vector(WORD, oldPotModdollars);
static Vector(WORD, oldpfirstnum);

/*
 		#] Variables : 
 		#[ PF_StoreInsideInfo :
*/

/*
 * Saves the current values of AC.RhsExprInModuleFlag, PotModdollars
 * and AC.pfirstnum.
 *
 * Called by DoInside().
 *
 * @return  0  if OK, nonzero on error.
 */
int PF_StoreInsideInfo(void)
{
	int i;
	oldRhsExprInModuleFlag = AC.RhsExprInModuleFlag;
	VectorClear(oldPotModdollars);
	for ( i = 0; i < NumPotModdollars; i++ )
		VectorPushBack(oldPotModdollars, PotModdollars[i]);
	VectorClear(oldpfirstnum);
	for ( i = 0; i < AC.numpfirstnum; i++ )
		VectorPushBack(oldpfirstnum, AC.pfirstnum[i]);
	return 0;
}

/*
 		#] PF_StoreInsideInfo : 
 		#[ PF_RestoreInsideInfo :
*/

/*
 * Restores the saved values of AC.RhsExprInModuleFlag, PotModdollars
 * and AC.pfirstnum.
 *
 * Called by DoEndInside().
 *
 * @return  0  if OK, nonzero on error.
 */
int PF_RestoreInsideInfo(void)
{
	int i;
	AC.RhsExprInModuleFlag = oldRhsExprInModuleFlag;
	NumPotModdollars = VectorSize(oldPotModdollars);
	for ( i = 0; i < NumPotModdollars; i++ )
		PotModdollars[i] = VectorPtr(oldPotModdollars)[i];
	AC.numpfirstnum = VectorSize(oldpfirstnum);
	for ( i = 0; i < AC.numpfirstnum; i++ )
		AC.pfirstnum[i] = VectorPtr(oldpfirstnum)[i];
	return 0;
}

/*
 		#] PF_RestoreInsideInfo : 
  	#] Preprocessor Inside instruction : 
  	#[ PF_BroadcastCBuf :
*/

/**
 * Broadcasts a compiler buffer specified by \a bufnum from the master
 * to the all slaves.
 *
 * @param  bufnum  The index of the compiler buffer to be broadcast.
 * @return         0 if OK, nonzero on error.
 */
int PF_BroadcastCBuf(int bufnum)
{
	CBUF *C = cbuf + bufnum;
	int i;
	LONG l;
	if ( PF.me == MASTER ) {
/*
 		#[ Master :
*/
		PF_PrepareLongMultiPack();
		/* Pack CBUF struct except pointers. */
		PF_LongMultiPack(&C->BufferSize, 1, PF_LONG);
		PF_LongMultiPack(&C->numlhs, 1, PF_INT);
		PF_LongMultiPack(&C->numrhs, 1, PF_INT);
		PF_LongMultiPack(&C->maxlhs, 1, PF_INT);
		PF_LongMultiPack(&C->maxrhs, 1, PF_INT);
		PF_LongMultiPack(&C->mnumlhs, 1, PF_INT);
		PF_LongMultiPack(&C->mnumrhs, 1, PF_INT);
		PF_LongMultiPack(&C->numtree, 1, PF_INT);
		PF_LongMultiPack(&C->rootnum, 1, PF_INT);
		PF_LongMultiPack(&C->MaxTreeSize, 1, PF_INT);
		/* Now pointers. Pointer, lhs and rhs are packed as offsets. We don't pack Top. */
		l = C->Pointer - C->Buffer;
		PF_LongMultiPack(&l, 1, PF_LONG);
		PF_LongMultiPack(C->Buffer, l, PF_WORD);
		for ( i = 0; i < C->numlhs + 1; i++ ) {
			l = C->lhs[i] - C->Buffer;
			PF_LongMultiPack(&l, 1, PF_LONG);
		}
		for ( i = 0; i < C->numrhs + 1; i++ ) {
			l = C->rhs[i] - C->Buffer;
			PF_LongMultiPack(&l, 1, PF_LONG);
		}
		PF_LongMultiPack(C->CanCommu, C->numrhs + 1, PF_LONG);
		PF_LongMultiPack(C->NumTerms, C->numrhs + 1, PF_LONG);
		PF_LongMultiPack(C->numdum, C->numrhs + 1, PF_WORD);
		PF_LongMultiPack(C->dimension, C->numrhs + 1, PF_WORD);
		if ( C->MaxTreeSize > 0 )
			PF_LongMultiPack(C->boomlijst, (C->numtree + 1) * (sizeof(COMPTREE) / sizeof(int)), PF_INT);
#ifdef PF_DEBUG_BCAST_CBUF
		MesPrint(">> Broadcast CBuf %d", bufnum);
#endif
/*
 		#] Master : 
*/
	}
	if ( PF_LongMultiBroadcast() ) return -1;
	if ( PF.me != MASTER ) {
/*
 		#[ Slave :
*/
		/* First, free already allocated buffers. */
		finishcbuf(bufnum);
		/* Unpack CBUF struct except pointers. */
		PF_LongMultiUnpack(&C->BufferSize, 1, PF_LONG);
		PF_LongMultiUnpack(&C->numlhs, 1, PF_INT);
		PF_LongMultiUnpack(&C->numrhs, 1, PF_INT);
		PF_LongMultiUnpack(&C->maxlhs, 1, PF_INT);
		PF_LongMultiUnpack(&C->maxrhs, 1, PF_INT);
		PF_LongMultiUnpack(&C->mnumlhs, 1, PF_INT);
		PF_LongMultiUnpack(&C->mnumrhs, 1, PF_INT);
		PF_LongMultiUnpack(&C->numtree, 1, PF_INT);
		PF_LongMultiUnpack(&C->rootnum, 1, PF_INT);
		PF_LongMultiUnpack(&C->MaxTreeSize, 1, PF_INT);
		/* Allocate new buffers. */
		C->Buffer = (WORD *)Malloc1(C->BufferSize * sizeof(WORD), "compiler buffer");
		C->Top = C->Buffer + C->BufferSize;
		C->lhs = (WORD **)Malloc1(C->maxlhs * sizeof(WORD *), "compiler buffer");
		C->rhs = (WORD **)Malloc1(C->maxrhs * (sizeof(WORD *) + 2 * sizeof(LONG) + 2 * sizeof(WORD)), "compiler buffer");
		C->CanCommu = (LONG *)(C->rhs + C->maxrhs);
		C->NumTerms = C->CanCommu + C->maxrhs;
		C->numdum = (WORD *)(C->NumTerms + C->maxrhs);
		C->dimension = C->numdum + C->maxrhs;
		if ( C->MaxTreeSize > 0 )
			C->boomlijst = (COMPTREE *)Malloc1(C->MaxTreeSize * sizeof(COMPTREE), "compiler buffer");
		/* Unpack buffers. */
		PF_LongMultiUnpack(&l, 1, PF_LONG);
		PF_LongMultiUnpack(C->Buffer, l, PF_WORD);
		C->Pointer = C->Buffer + l;
		for ( i = 0; i < C->numlhs + 1; i++ ) {
			PF_LongMultiUnpack(&l, 1, PF_LONG);
			C->lhs[i] = C->Buffer + l;
		}
		for ( i = 0; i < C->numrhs + 1; i++ ) {
			PF_LongMultiUnpack(&l, 1, PF_LONG);
			C->rhs[i] = C->Buffer + l;
		}
		PF_LongMultiUnpack(C->CanCommu, C->numrhs + 1, PF_LONG);
		PF_LongMultiUnpack(C->NumTerms, C->numrhs + 1, PF_LONG);
		PF_LongMultiUnpack(C->numdum, C->numrhs + 1, PF_WORD);
		PF_LongMultiUnpack(C->dimension, C->numrhs + 1, PF_WORD);
		if ( C->MaxTreeSize > 0 )
			PF_LongMultiUnpack(C->boomlijst, (C->numtree + 1) * (sizeof(COMPTREE) / sizeof(int)), PF_INT);
/*
 		#] Slave : 
*/
	}
	return 0;
}

/*
  	#] PF_BroadcastCBuf : 
  	#[ PF_BroadcastExpFlags :
*/

/**
 * Broadcasts AR.expflags and several properties of each expression,
 * e.g., e->vflags, from the master to all slaves.
 *
 * @return 0 if OK, nonzero on error.
 */
int PF_BroadcastExpFlags(void)
{
	WORD i;
	EXPRESSIONS e;
	if ( PF.me == MASTER ) {
/*
 		#[ Master :
*/
		PF_PrepareLongMultiPack();
		PF_LongMultiPack(&AR.expflags, 1, PF_WORD);
		for ( i = 0; i < NumExpressions; i++ ) {
			e = &Expressions[i];
			PF_LongMultiPack(&e->counter,    1, PF_WORD);
			PF_LongMultiPack(&e->vflags,     1, PF_WORD);
			PF_LongMultiPack(&e->uflags,     1, PF_WORD);
			PF_LongMultiPack(&e->numdummies, 1, PF_WORD);
			PF_LongMultiPack(&e->numfactors, 1, PF_WORD);
#ifdef PF_DEBUG_BCAST_EXPRFLAGS
			MesPrint(">> Broadcast ExprFlags: %s", AC.exprnames->namebuffer + e->name);
#endif
		}
/*
 		#] Master : 
*/
	}
	if ( PF_LongMultiBroadcast() ) return -1;
	if ( PF.me != MASTER ) {
/*
 		#[ Slave :
*/
		PF_LongMultiUnpack(&AR.expflags, 1, PF_WORD);
		for ( i = 0; i < NumExpressions; i++ ) {
			e = &Expressions[i];
			PF_LongMultiUnpack(&e->counter,    1, PF_WORD);
			PF_LongMultiUnpack(&e->vflags,     1, PF_WORD);
			PF_LongMultiUnpack(&e->uflags,     1, PF_WORD);
			PF_LongMultiUnpack(&e->numdummies, 1, PF_WORD);
			PF_LongMultiUnpack(&e->numfactors, 1, PF_WORD);
		}
/*
 		#] Slave : 
*/
	}
	return 0;
}

/*
  	#] PF_BroadcastExpFlags : 
  	#[ PF_SetScratch :
*/

/**
 * Same as SetScratch() except it always fills the buffer from the given position.
 *
 * @param  f         the file handle.
 * @param  position  the position to be loaded into the buffer.
 */
static void PF_SetScratch(FILEHANDLE *f,POSITION *position)
{
	if(
			( f->handle >= 0) && ISGEPOS(*position,f->POposition) &&
			( ISGEPOSINC(*position,f->POposition,(f->POfull-f->PObuffer)*sizeof(WORD)) ==0 )
		)/*position is inside the buffer! SetScratch() will do nothing.*/
			f->POfull=f->PObuffer;/*force SetScratch() to re-read the position from the beginning:*/
	SetScratch(f,position);
}

/*
  	#] PF_SetScratch : 
  	#[ PF_pushScratch :
*/

/**
 * Flushes a scratch file.
 *
 * @param  f  the scratch file to be flushed.
 * @return    0 if OK, nonzero on error.
 */
static int PF_pushScratch(FILEHANDLE *f)
{
	LONG size,RetCode;
	if ( f->handle < 0){
		/*Create the file*/
		if ( ( RetCode = CreateFile(f->name) ) >= 0 ) {
			f->handle = (WORD)RetCode;
			PUTZERO(f->filesize);
			PUTZERO(f->POposition);
		}
		else{
			MesPrint("Cannot create scratch file %s",f->name);
			return(-1);
		}
	}/*if ( f->handle < 0)*/
	size = (f->POfill-f->PObuffer)*sizeof(WORD);
	if( size > 0 ){
		SeekFile(f->handle,&(f->POposition),SEEK_SET);
		if ( WriteFile(f->handle,(UBYTE *)(f->PObuffer),size) != size ){
			MesPrint("Error while writing to disk. Disk full?");
			return(-1);
		}
		ADDPOS(f->filesize,size);
		ADDPOS(f->POposition,size);
		f->POfill = f->POfull=f->PObuffer;
	}/*if( size > 0 )*/
	return(0);
}

/*
  	#] PF_pushScratch : 
  	#[ Broadcasting RHS expressions :
 		#[ PF_WalkThroughExprMaster :
	Returns <=0 if the expression is ready, or dl+1;
*/

static int PF_WalkThroughExprMaster(FILEHANDLE *curfile, int dl)
{
	LONG l=0;
	for(;;){
		if(curfile->POfull-curfile->POfill < dl){
			POSITION pos;
			SeekScratch(curfile,&pos);
			PF_SetScratch(curfile,&pos);
		}/*if(curfile->POfull-curfile->POfill < dl)*/
		curfile->POfill+=dl;
		l+=dl;
		if( l >= PF.exprbufsize){
			if( l == PF.exprbufsize){
				if( *(curfile->POfill) == 0)/*expression is ready*/
					return(0);
				}
			l-=PF.exprbufsize;
			curfile->POfill-=l;
			return l+1;
		}

		dl=*(curfile->POfill);
		if(dl == 0)
			return l-PF.exprbufsize;

		if(dl<0){/*compressed term*/
			if(curfile->POfull-curfile->POfill < 1){
				POSITION pos;
				SeekScratch(curfile,&pos);
				PF_SetScratch(curfile,&pos);
			}/*if(curfile->POfull-curfile->POfill < 1)*/
			dl=*(curfile->POfill+1)+2;
		}/*if(*(curfile->POfill)<0)*/
	}/*for(;;)*/
}

/*
 		#] PF_WalkThroughExprMaster : 
 		#[ PF_WalkThroughExprSlave :
	Returns <=0 if the expression is ready, or dl+1;
*/

static int PF_WalkThroughExprSlave(FILEHANDLE *curfile, LONG *counter, int dl)
{
	LONG l=0;
	for(;;){
		if(curfile->POstop-curfile->POfill < dl){
			if(PF_pushScratch(curfile))
				return(-PF.exprbufsize-1);
		}
		curfile->POfill+=dl;
		curfile->POfull=curfile->POfill;
		l+=dl;
		if( l >= PF.exprbufsize){
			if( l == PF.exprbufsize){
				/*
				 * This access is valid because PF.exprbufsize+1 WORDs are
				 * broadcasted, this shortcut is not mandatory though. (TU 15 Sep 2011)
				 */
				if( *(curfile->POfill) == 0)/*expression is ready*/
					return(0);
				}
			l-=PF.exprbufsize;
			curfile->POfill-=l;
			curfile->POfull=curfile->POfill;
			return l+1;
		}

		dl=*(curfile->POfill);
		if(dl == 0)
			return l-PF.exprbufsize;
		(*counter)++;
		if(dl<0){/*compressed term*/
			if(curfile->POstop-curfile->POfill < 1){
				if(PF_pushScratch(curfile))
					return(-PF.exprbufsize-1);
			}
			/*
			 * This access is always valid because PF.exprbufsize+1 WORDs are
			 * broadcasted. (TU 15 Sep 2011)
			 */
			dl=*(curfile->POfill+1)+2;
		}/*if(*(curfile->POfill)<0)*/
	}/*for(;;)*/
}

/*
 		#] PF_WalkThroughExprSlave : 
 		#[ PF_rhsBCastMaster :
*/

/**
 * On the master, broadcasts an expression to the all slaves.
 *
 * @param  curfile  the scratch file in which the expression is stored.
 * @param  e        the expression to be broadcasted.
 * @return          0 if OK, nonzero on error.
 */
static int PF_rhsBCastMaster(FILEHANDLE *curfile, EXPRESSIONS e)
{
	LONG l=1;/*PF_WalkThroughExpr returns length + 1*/
	SetScratch(curfile,&(e->onfile));
	do{
		/*
		 * We need to broadcast PF.exprbufsize+1 WORDs because PF_WalkThroughExprSlave
		 * may access to an additional 1 WORD. It is better to rewrite the routines
		 * in such a way as to broadcast only PF.exprbufsize WORDs. (TU 15 Sep 2011)
		 */
		if ( curfile->POfull - curfile->POfill < PF.exprbufsize + 1 ) {
			POSITION pos;
			SeekScratch(curfile,&pos);
			PF_SetScratch(curfile,&pos);
		}
		if ( PF_Bcast(curfile->POfill, (PF.exprbufsize + 1) * sizeof(WORD)) )
			return -1;
		l=PF_WalkThroughExprMaster(curfile,l-1);
	}while(l>0);
	if(l<0)/*The tail is extra, decrease POfill*/
		curfile->POfill-=l;
	return(0);
}

/*
 		#] PF_rhsBCastMaster : 
 		#[ PF_rhsBCastSlave :
*/

/**
 * On the slave, receives an expression broadcasted from the master.
 *
 * @param  curfile  the scratch file to store the broadcasted expression
 *                  (AR.infile or AR.hidefile).
 * @param  e        the expression to be broadcasted.
 * @return          0 if OK, nonzero on error.
 */
static int PF_rhsBCastSlave(FILEHANDLE *curfile, EXPRESSIONS e)
{
	LONG l=1;/*PF_WalkThroughExpr returns length + 1*/
	LONG counter = 0;
	do{
		/*
		 * We need to broadcast PF.exprbufsize+1 WORDs because PF_WalkThroughExprSlave
		 * may access to an additional 1 WORD. It is better to rewrite the routines
		 * in such a way as to broadcast only PF.exprbufsize WORDs. (TU 15 Sep 2011)
		 */
		if ( curfile->POstop - curfile->POfill < PF.exprbufsize + 1 ) {
			if(PF_pushScratch(curfile))
				return(-1);
		}
		if ( PF_Bcast(curfile->POfill, (PF.exprbufsize + 1) * sizeof(WORD)) )
			return(-1);
		l = PF_WalkThroughExprSlave(curfile, &counter, l - 1);
	}while(l>0);
	if(l<0){/*The tail is extra, decrease POfill*/
		if(l<-PF.exprbufsize)/*error due to a PF_pushScratch() failure */
			return(-1);
		curfile->POfill-=l;
	}
	if ( curfile->handle >= 0 ) {
		if ( PF_pushScratch(curfile) ) return -1;
	}
	curfile->POfull=curfile->POfill;
	if ( curfile != AR.hidefile ) AR.InInBuf = curfile->POfull-curfile->PObuffer;
	else                          AR.InHiBuf = curfile->POfull-curfile->PObuffer;
	CHECK(counter == e->counter + 1);  /* The first term is the prototype. */
	return(0);
}

/*
 		#] PF_rhsBCastSlave : 
 		#[ PF_BroadcastExpr :
*/

/**
 * Broadcasts an expression from the master to the all slaves.
 *
 * @param  e     The expression to be broadcast.
 * @param  file  The file in which the expression is sitting.
 * @return       0 if OK, nonzero on error.
 */
int PF_BroadcastExpr(EXPRESSIONS e, FILEHANDLE *file)
{
	if ( PF.me == MASTER ) {
		if ( PF_rhsBCastMaster(file, e) ) return -1;
#ifdef PF_DEBUG_BCAST_RHSEXPR
		MesPrint(">> Broadcast RhsExpr: %s", AC.exprnames->namebuffer + e->name);
#endif
	}
	else {
		POSITION pos;
		SetEndHScratch(file, &pos);
		e->onfile = pos;
		if ( PF_rhsBCastSlave(file, e) ) return -1;
	}
	return 0;
}

/*
 		#] PF_BroadcastExpr : 
 		#[ PF_BroadcastRHS :
*/

/**
 * Broadcasts expressions appearing in the right-hand side from
 * the master to the all slaves.
 *
 * @return  0 if OK, nonzero on error.
 */
int PF_BroadcastRHS(void)
{
	int i;
	for ( i = 0; i < NumExpressions; i++ ) {
		EXPRESSIONS e = &Expressions[i];
		if ( !(e->vflags & ISINRHS) ) continue;
		switch ( e->status ) {
			case LOCALEXPRESSION:
			case SKIPLEXPRESSION:
			case DROPLEXPRESSION:
			case GLOBALEXPRESSION:
			case SKIPGEXPRESSION:
			case DROPGEXPRESSION:
			case HIDELEXPRESSION:
			case HIDEGEXPRESSION:
			case INTOHIDELEXPRESSION:
			case INTOHIDEGEXPRESSION:
				if ( PF_BroadcastExpr(e, AR.infile) ) return -1;
				break;
			case HIDDENLEXPRESSION:
			case HIDDENGEXPRESSION:
			case DROPHLEXPRESSION:
			case DROPHGEXPRESSION:
			case UNHIDELEXPRESSION:
			case UNHIDEGEXPRESSION:
				if ( PF_BroadcastExpr(e, AR.hidefile) ) return -1;
				break;
		}
	}
	if ( PF.me != MASTER )
		UpdatePositions();
	return 0;
}

/*
 		#] PF_BroadcastRHS : 
  	#] Broadcasting RHS expressions : 
  	#[ InParallel mode :
 		#[ PF_InParallelProcessor :
*/

/**
 * Processes expressions in the InParallel mode, i.e.,
 * dividing expressions marked by partodo over the slaves.
 *
 * @return  0 if OK, nonzero on error.
 */
int PF_InParallelProcessor(void)
{
	GETIDENTITY
	int i, next,tag;
	EXPRESSIONS e;
	/*
	 * Skip expressions with zero terms. All the master and slaves need to
	 * change the "partodo" flag.
	 */
	if ( PF.numtasks >= 3 ) {
		for ( i = 0; i < NumExpressions; i++ ) {
			e = Expressions + i;
			if ( e->partodo > 0 && e->counter == 0 ) {
				e->partodo = 0;
			}
		}
	}
	if(PF.me == MASTER){
		if ( PF.numtasks >= 3 ) {
			partodoexr = (WORD*)Malloc1(sizeof(WORD)*(PF.numtasks+1),"PF_InParallelProcessor");
			for ( i = 0; i < NumExpressions; i++ ) {
				e = Expressions+i;
				if ( e->partodo <= 0 ) continue;
				switch(e->status){
					case LOCALEXPRESSION:
					case GLOBALEXPRESSION:
					case UNHIDELEXPRESSION:
					case UNHIDEGEXPRESSION:
					case INTOHIDELEXPRESSION:
					case INTOHIDEGEXPRESSION:
						tag=PF_ANY_SOURCE;
						next=PF_Wait4SlaveIP(&tag);
						if(next<0)
							return(-1);
						if(tag == PF_DATA_MSGTAG){
							PF_Statistics(PF_stats,0);
							if(PF_Slave2MasterIP(next))
								return(-1);
						}
						if(PF_Master2SlaveIP(next,e))
							return(-1);
						partodoexr[next]=i;
						break;
					default:
						e->partodo = 0;
						continue;
				}/*switch(e->status)*/
			}/*for ( i = 0; i < NumExpressions; i++ )*/
			/*Here some slaves are working, other are waiting on PF_Send.
				Wait all of them.*/
			/*At this point no new slaves may be launched so PF_WaitAllSlaves()
				does not modify partodoexr[].*/
			if(PF_WaitAllSlaves())
				return(-1);
			/**/
			if ( AC.CollectFun ) AR.DeferFlag = 0;
			if(partodoexr){
				M_free(partodoexr,"PF_InParallelProcessor");
				partodoexr=NULL;
			}/*if(partodoexr)*/
		}/*if ( PF.numtasks >= 3 ) */
		else {
			for ( i = 0; i < NumExpressions; i++ ) {
				Expressions[i].partodo = 0;
			}
		}
		return(0);
	}/*if(PF.me == MASTER)*/
	/*Slave:*/
	if(PF_Wait4MasterIP(PF_EMPTY_MSGTAG))
		return(-1);
	/*master is ready to listen to me*/
	do{
		WORD *oldwork= AT.WorkPointer;
		tag=PF_ReadMaster();/*reads directly to its scratch!*/
		if(tag<0)
			return(-1);
		if(tag == PF_DATA_MSGTAG){
			oldwork = AT.WorkPointer;

			/* For redefine statements. */
			if ( AC.numpfirstnum > 0 ) {
				int j;
				for ( j = 0; j < AC.numpfirstnum; j++ ) {
					AC.inputnumbers[j] = -1;
				}
			}

			if(PF_DoOneExpr())/*the processor*/
				return(-1);
			if(PF_Wait4MasterIP(PF_DATA_MSGTAG))
				return(-1);
			if(PF_Slave2MasterIP(PF.me))/*both master and slave*/
				return(-1);
			AT.WorkPointer=oldwork;
		}/*if(tag == PF_DATA_MSGTAG)*/
	}while(tag!=PF_EMPTY_MSGTAG);
	PF.exprtodo=-1;
	return(0);
}/*PF_InParallelProcessor*/

/*
 		#] PF_InParallelProcessor : 
 		#[ PF_Wait4MasterIP :
*/

static int PF_Wait4MasterIP(int tag)
{
	int follow = 0;
	LONG cpu,space = 0;

	if(PF.log){
		fprintf(stderr,"[%d] Starting to send to Master\n",PF.me);
		fflush(stderr);
	}

	PF_PreparePack();
	cpu = TimeCPU(1);
	PF_Pack(&cpu               ,1,PF_LONG);
	PF_Pack(&space             ,1,PF_LONG);
	PF_Pack(&PF_linterms       ,1,PF_LONG);
	PF_Pack(&(AM.S0->GenTerms) ,1,PF_LONG);
	PF_Pack(&(AM.S0->TermsLeft),1,PF_LONG);
	PF_Pack(&follow            ,1,PF_INT );

	if(PF.log){
		fprintf(stderr,"[%d] Now sending with tag = %d\n",PF.me,tag);
		fflush(stderr);
	}

	PF_Send(MASTER, tag);

	if(PF.log){
		fprintf(stderr,"[%d] returning from send\n",PF.me);
		fflush(stderr);
	}
	return(0);
}
/*
 		#] PF_Wait4MasterIP : 
 		#[ PF_DoOneExpr :
*/

/**
 * Processes an expression specified by PF.exprtodo.
 *
 * See also "case DOONEEXPRESSION" in RunThread().
 *
 * @return        0 if OK, nonzero on error.
 */
static int PF_DoOneExpr(void)/*the processor*/
{
				GETIDENTITY
				EXPRESSIONS e;
				int i;
				WORD *term;
				POSITION position, outposition;
				FILEHANDLE *fi, *fout;
				LONG dd = 0;
				WORD oldBracketOn = AR.BracketOn;
				WORD *oldBrackBuf = AT.BrackBuf;
				WORD oldbracketindexflag = AT.bracketindexflag;
				e = Expressions + PF.exprtodo;
				i = PF.exprtodo;
				AR.CurExpr = i;
				AR.SortType = AC.SortType;
				AR.expchanged = 0;
				if ( ( e->vflags & ISFACTORIZED ) != 0 ) {
					AR.BracketOn = 1;
					AT.BrackBuf = AM.BracketFactors;
					AT.bracketindexflag = 1;
				}

				position = AS.OldOnFile[i];
				if ( e->status == HIDDENLEXPRESSION || e->status == HIDDENGEXPRESSION ) {
					AR.GetFile = 2; fi = AR.hidefile;
				}
				else {
					AR.GetFile = 0; fi = AR.infile;
				}
/*
				PUTZERO(fi->POposition);
				if ( fi->handle >= 0 ) {
					fi->POfill = fi->POfull = fi->PObuffer;
				}
*/
				SetScratch(fi,&position);
				term = AT.WorkPointer;
				AR.CompressPointer = AR.CompressBuffer;
				AR.CompressPointer[0] = 0;
				AR.KeptInHold = 0;
				if ( GetTerm(BHEAD term) <= 0 ) {
					MesPrint("Expression %d has problems in scratchfile",i);
					Terminate(-1);
				}
				if ( AT.bracketindexflag > 0 ) OpenBracketIndex(i);
				term[3] = i;
				PUTZERO(outposition);
				fout = AR.outfile;
				fout->POfill = fout->POfull = fout->PObuffer;
				fout->POposition = outposition;
				if ( fout->handle >= 0 ) {
					fout->POposition = outposition;
				}
/*
				The next statement is needed because we need the system
				to believe that the expression is at position zero for
				the moment. In this worker, with no memory of other expressions,
				it is. This is needed for when a bracket index is made
				because there e->onfile is an offset. Afterwards, when the
				expression is written to its final location in the masters
				output e->onfile will get its real value.
*/
				PUTZERO(e->onfile);
				if ( PutOut(BHEAD term,&outposition,fout,0) < 0 ) return -1;

				AR.DeferFlag = AC.ComDefer;

/*				AR.sLevel = AB[0]->R.sLevel;*/
				term = AT.WorkPointer;
				NewSort(BHEAD0);
				AR.MaxDum = AM.IndDum;
				AN.ninterms = 0;
				while ( GetTerm(BHEAD term) ) {
				  SeekScratch(fi,&position);
				  AN.ninterms++; dd = AN.deferskipped;
				  if ( ( e->vflags & ISFACTORIZED ) != 0 && term[1] == HAAKJE ) {
					  StoreTerm(BHEAD term);
				  }
				  else {
				  if ( AC.CollectFun && *term <= (AM.MaxTer/(2*(LONG)sizeof(WORD))) ) {
					if ( GetMoreTerms(term) < 0 ) {
					  LowerSortLevel(); return(-1);
					}
				    SeekScratch(fi,&position);
				  }
				  AT.WorkPointer = term + *term;
				  AN.RepPoint = AT.RepCount + 1;
				  if ( AR.DeferFlag ) {
					AR.CurDum = AN.IndDum = Expressions[PF.exprtodo].numdummies;
				  }
				  else {
					AN.IndDum = AM.IndDum;
					AR.CurDum = ReNumber(BHEAD term);
				  }
				  if ( AC.SymChangeFlag ) MarkDirty(term,DIRTYSYMFLAG);
				  if ( AN.ncmod ) {
					if ( ( AC.modmode & ALSOFUNARGS ) != 0 ) MarkDirty(term,DIRTYFLAG);
					else if ( AR.PolyFun ) PolyFunDirty(BHEAD term);
				  }
				  else if ( AC.PolyRatFunChanged ) PolyFunDirty(BHEAD term);
				  if ( ( AR.PolyFunType == 2 ) && ( AC.PolyRatFunChanged == 0 )
						&& ( e->status == LOCALEXPRESSION || e->status == GLOBALEXPRESSION ) ) {
						PolyFunClean(BHEAD term);
				  }
				  if ( Generator(BHEAD term,0) ) {
					LowerSortLevel(); return(-1);
				  }
				  AN.ninterms += dd;
				  }
				  SetScratch(fi,&position);
				  if ( fi == AR.hidefile ) {
					AR.InHiBuf = (fi->POfull-fi->PObuffer)
						-DIFBASE(position,fi->POposition)/sizeof(WORD);
				  }
				  else {
					AR.InInBuf = (fi->POfull-fi->PObuffer)
						-DIFBASE(position,fi->POposition)/sizeof(WORD);
				  }
				}
				AN.ninterms += dd;
				if ( EndSort(BHEAD AM.S0->sBuffer,0) < 0 ) return(-1);
				e->numdummies = AR.MaxDum - AM.IndDum;
				AR.BracketOn = oldBracketOn;
				AT.BrackBuf = oldBrackBuf;
				if ( ( e->vflags & TOBEFACTORED ) != 0 )
						poly_factorize_expression(e);
				else if ( ( ( e->vflags & TOBEUNFACTORED ) != 0 )
				 && ( ( e->vflags & ISFACTORIZED ) != 0 ) )
						poly_unfactorize_expression(e);
				if ( AM.S0->TermsLeft )   e->vflags &= ~ISZERO;
				else                      e->vflags |= ISZERO;
				if ( AR.expchanged == 0 ) e->vflags |= ISUNMODIFIED;
/*				if ( AM.S0->TermsLeft ) AR.expflags |= ISZERO;
				if ( AR.expchanged )    AR.expflags |= ISUNMODIFIED;*/
				AR.GetFile = 0;
				AT.bracketindexflag = oldbracketindexflag;

				fout->POfull = fout->POfill;
	return(0);
}

/*
 		#] PF_DoOneExpr : 
 		#[ PF_Slave2MasterIP :
*/

typedef struct bufIPstruct {
	LONG i;
	struct ExPrEsSiOn e;
} bufIPstruct_t;

static int PF_Slave2MasterIP(int src)/*both master and slave*/
{
	EXPRESSIONS e;
	bufIPstruct_t exprData;
	int i,l;
	FILEHANDLE *fout=AR.outfile;
	POSITION pos;
	/*Here we know the length of data to send in advance:
		slave has the only one expression in its scratch file, and it sends
		this information to the master.*/
	if(PF.me != MASTER){/*slave*/
		e = Expressions + PF.exprtodo;
		/*Fill in the expression data:*/
		memcpy(&(exprData.e), e, sizeof(struct ExPrEsSiOn));
		SeekScratch(fout,&pos);
		exprData.i=BASEPOSITION(pos);
		/*Send the metadata:*/
		if(PF_RawSend(MASTER,&exprData,sizeof(bufIPstruct_t),0))
			return(-1);
		i=exprData.i;
		SETBASEPOSITION(pos,0);
		do{
			int blen=PF.exprbufsize*sizeof(WORD);
			if(i<blen)
				blen=i;
			l=PF_SendChunkIP(fout,&pos, MASTER, blen);
			/*Here always l == blen!*/
			if(l<0)
				return(-1);
			ADDPOS(pos,l);
			i-=l;
		}while(i>0);
		if ( fout->handle >= 0 ) { /* Now get rid of the file */
			CloseFile(fout->handle);
			fout->handle = -1;
			remove(fout->name);
			PUTZERO(fout->POposition);
			PUTZERO(fout->filesize);
			fout->POfill = fout->POfull = fout->PObuffer;
		}
		/* Now handle redefined preprocessor variables. */
		if ( AC.numpfirstnum > 0 ) {
			PF_PrepareLongSinglePack();
			PF_PackRedefinedPreVars();
			PF_LongSingleSend(MASTER, PF_MISC_MSGTAG);
		}
		return(0);
	}/*if(PF.me != MASTER)*/
	/*Master*/
	/*partodoexr[src] is the number of expression.*/
	e = Expressions +partodoexr[src];
	/*Get metadata:*/
	if (PF_RawRecv(&src, &exprData,sizeof(bufIPstruct_t),&i)!= sizeof(bufIPstruct_t))
		return(-1);
	/*Fill in the expression data:*/
/*	memcpy(e, &(exprData.e), sizeof(struct ExPrEsSiOn)); */
	e->counter    = exprData.e.counter;
	e->vflags     = exprData.e.vflags;
	e->uflags     = exprData.e.uflags;
	e->numdummies = exprData.e.numdummies;
	e->numfactors = exprData.e.numfactors;
	if ( !(e->vflags & ISZERO) )       AR.expflags |= ISZERO;
	if ( !(e->vflags & ISUNMODIFIED) ) AR.expflags |= ISUNMODIFIED;
	SeekScratch(fout,&pos);
	e->onfile = pos;
	i=exprData.i;
	while(i>0){
		int blen=PF.exprbufsize*sizeof(WORD);
		if(i<blen)
			blen=i;
		l=PF_RecvChunkIP(fout,src,blen);
		/*Here always l == blen!*/
		if(l<0)
			return(-1);
		i-=l;
	}
	/* Now handle redefined preprocessor variables. */
	if ( AC.numpfirstnum > 0 ) {
		PF_LongSingleReceive(src, PF_MISC_MSGTAG, NULL, NULL);
		PF_UnpackRedefinedPreVars();
	}
	return(0);
}

/*
 		#] PF_Slave2MasterIP : 
 		#[ PF_Master2SlaveIP :
*/

static int PF_Master2SlaveIP(int dest, EXPRESSIONS e)
{
	bufIPstruct_t exprData;
	FILEHANDLE *fi;
	POSITION pos;
	int l;
	LONG ll=0,count=0;
	WORD *t;
	if(e==NULL){/*Say to the slave that no more job:*/
		if(PF_RawSend(dest,&exprData,sizeof(bufIPstruct_t),PF_EMPTY_MSGTAG))
			return(-1);
		return(0);
	}
	memcpy(&(exprData.e), e, sizeof(struct ExPrEsSiOn));
	exprData.i=e-Expressions;
	if ( AC.StatsFlag && AC.OldParallelStats ) {
		MesPrint("");
		MesPrint(" Sending expression %s to slave %d",EXPRNAME(exprData.i),dest);
	}
	if(PF_RawSend(dest,&exprData,sizeof(bufIPstruct_t),PF_DATA_MSGTAG))
		return(-1);
	if ( e->status == HIDDENLEXPRESSION || e->status == HIDDENGEXPRESSION )
		fi = AR.hidefile;
	else
		fi = AR.infile;
	pos=e->onfile;
	SetScratch(fi,&pos);
	do{
		l=PF_SendChunkIP(fi, &pos, dest, PF.exprbufsize*sizeof(WORD));
		if(l<0)
			return(-1);
		t=fi->PObuffer+ (DIFBASE(pos,fi->POposition))/sizeof(WORD);
		ll=PF_WalkThrough(t,ll,l/sizeof(WORD),&count);
		ADDPOS(pos,l);
	}while(ll>-2);
	return(0);
}

/*
 		#] PF_Master2SlaveIP : 
 		#[ PF_ReadMaster :
*/

static int PF_ReadMaster(void)/*reads directly to its scratch!*/
{
	bufIPstruct_t exprData;
	int tag,m=MASTER;
	EXPRESSIONS e;
	FILEHANDLE *fi;
	POSITION pos;
	LONG count=0;
	WORD *t;
	LONG ll=0;
	int l;
	/*Get metadata:*/
	if (PF_RawRecv(&m, &exprData,sizeof(bufIPstruct_t),&tag)!= sizeof(bufIPstruct_t))
		return(-1);

	if(tag == PF_EMPTY_MSGTAG)/*No data, no job*/
		return(tag);

	/*data expected, tag must be == PF_DATA_MSTAG!*/
	PF.exprtodo=exprData.i;
	e=Expressions + PF.exprtodo;
	/*Fill in the expression data:*/
/*	memcpy(e, &(exprData.e), sizeof(struct ExPrEsSiOn)); */
	if ( e->status == HIDDENLEXPRESSION || e->status == HIDDENGEXPRESSION )
		fi = AR.hidefile;
	else
		fi = AR.infile;
	SetEndHScratch(fi,&pos);
	e->onfile=AS.OldOnFile[PF.exprtodo]=pos;

	do{
		l=PF_RecvChunkIP(fi,MASTER,PF.exprbufsize*sizeof(WORD));
		if(l<0)
			return(-1);
		t=fi->POfull-l/sizeof(WORD);
		ll=PF_WalkThrough(t,ll,l/sizeof(WORD),&count);
	}while(ll>-2);
	/*Now -ll-2 is the number of "extra" elements transferred from the master.*/
	fi->POfull-=-ll-2;
	fi->POfill=fi->POfull;
	return(PF_DATA_MSGTAG);
}

/*
 		#] PF_ReadMaster : 
 		#[ PF_SendChunkIP :
	thesize is in bytes. Returns the number of sent bytes or <0 on error:
*/

static int PF_SendChunkIP(FILEHANDLE *curfile, POSITION *position, int to, LONG thesize)
{
	LONG l=thesize;
	if(
		ISLESSPOS(*position,curfile->POposition) ||
		ISGEPOSINC(*position,curfile->POposition,
		((curfile->POfull-curfile->PObuffer)*sizeof(WORD)-thesize) )
	){
		if(curfile->handle< 0)
			l=(curfile->POfull-curfile->PObuffer)*sizeof(WORD) - (LONG)(position->p1);
		else{
			PF_SetScratch(curfile,position);
			if(
				ISGEPOSINC(*position,curfile->POposition,
				((curfile->POfull-curfile->PObuffer)*sizeof(WORD)-thesize) )
				)
			l=(curfile->POfull-curfile->PObuffer)*sizeof(WORD) - (LONG)position->p1;
		}
	}
	/*Now we are able to sent l bytes from the
		curfile->PObuffer[position-curfile->POposition]*/
	if(PF_RawSend(to,curfile->PObuffer+ (DIFBASE(*position,curfile->POposition))/sizeof(WORD),l,0))
		return(-1);
	return(l);
}

/*
 		#] PF_SendChunkIP : 
 		#[ PF_RecvChunkIP :
	thesize is in bytes. Returns the number of sent bytes or <0 on error:
*/

static int PF_RecvChunkIP(FILEHANDLE *curfile, int from, LONG thesize)
{
	LONG receivedBytes;

	if( (LONG)((curfile->POstop - curfile->POfull)*sizeof(WORD)) < thesize )
		if(PF_pushScratch(curfile))
			return(-1);
	/*Now there is enough space from curfile->POfill to curfile->POstop*/
	{/*Block:*/
		int tag=0;
		receivedBytes=PF_RawRecv(&from,curfile->POfull,thesize,&tag);
	}/*:Block*/
	if(receivedBytes >= 0 ){
		curfile->POfull+=receivedBytes/sizeof(WORD);
		curfile->POfill=curfile->POfull;
	}/*if(receivedBytes >= 0 )*/
	return(receivedBytes);
}

/*
 		#] PF_RecvChunkIP : 
 		#[ PF_WalkThrough :
	Returns:
	>=  0 -- initial offset,
		-1 -- the first element of t contains the length of the tail of compressed term,
	<= -2 -- -(d+2), where d is the number of extra transferred elements.
	Expects:
	l -- initial offset or -1,
	chunk -- number of transferred elements (not bytes!)
	*count -- incremented each time a new term is found
*/

static int PF_WalkThrough(WORD *t, LONG l, LONG chunk, LONG *count)
{
	if(l<0) /*==-1!*/
		l=(*t)+1;/*the first element of t contains the length of
						the tail of compressed term*/
	else{
		if(l>=chunk)/*next term is out of the chunk*/
			return(l-chunk);
		t+=l;
		chunk-=l;/*note, l was less than chunk so chunk >0!*/
		l=*t;
	}
	/*Main loop:*/
	while(l!=0){
		if(l>0){/*an offset to the next term*/
			if(l<chunk){
				t+=l;
				chunk-=l;/*note, l was less than chunk so chunk >0!*/
				l=*t;
				(*count)++;
			}/*if(l<chunk)*/
			else
				return(l-chunk);
		}/*if(l>0)*/
		else{ /* l<0 */
			if(chunk < 2)/*i.e., chunk == 1*/
				return(-1);/*the first WORD in the next chunk is length of the tail of the compressed term*/
			l=*(t+1)+2;/*+2 since
					1. t points to the length field -1,
					2. the size of a tail of compressed term is equal to the number of WORDs in this tail*/
		}
	}/*while(l!=0)*/
	return(-1-chunk);/* -(2+(chunk-1)), chunk>0 ! */
}

/*
 		#] PF_WalkThrough : 
  	#] InParallel mode : 
  	#[ PF_SendFile :
*/

#define PF_SNDFILEBUFSIZE 4096

/**
 * Sends a file to the process specified by \a to.
 *
 * @param  to  the destination process number.
 * @param  fd  the file to be sent.
 * @return     the size of sent data in bytes, or -1 on error.
 */
int PF_SendFile(int to, FILE *fd)
{
	size_t len=0;
	if(fd == NULL){
		if(PF_RawSend(to,&to,sizeof(int),PF_EMPTY_MSGTAG))
			return(-1);
		return(0);
	}
	for(;;){
		char buf[PF_SNDFILEBUFSIZE];
		size_t l;
		l=fread(buf, 1, PF_SNDFILEBUFSIZE, fd);
		len+=l;
		if(l==PF_SNDFILEBUFSIZE){
			if(PF_RawSend(to,buf,PF_SNDFILEBUFSIZE,PF_BUFFER_MSGTAG))
				return(-1);
		}
		else{
			if(PF_RawSend(to,buf,l,PF_ENDBUFFER_MSGTAG))
				return(-1);
			break;
		}
	}/*for(;;)*/
	return(len);
}

/*
  	#] PF_SendFile : 
  	#[ PF_RecvFile :
*/

/**
 * Receives a file from the process specified by \a from.
 *
 * @param  from  the source process number.
 * @param  fd    the file to save the received data.
 * @return       the size of received data in bytes, or -1 on error.
 */
int PF_RecvFile(int from, FILE *fd)
{
	size_t len=0;
	int tag;
	do{
		char buf[PF_SNDFILEBUFSIZE];
		int l;
			l=PF_RawRecv(&from,buf,PF_SNDFILEBUFSIZE,&tag);
			if(l<0)
				return(-1);
			if(tag == PF_EMPTY_MSGTAG)
				return(-1);

			if( fwrite(buf,l,1,fd)!=1 )
				return(-1);
			len+=l;
	}while(tag!=PF_ENDBUFFER_MSGTAG);
	return(len);
}

/*
  	#] PF_RecvFile : 
  	#[ Synchronised output :
 		#[ Explanations :
*/

/*
 * If the master and slaves output statistics or error messages to the same stream
 * or file (e.g., the standard output or the log file) simultaneously, then
 * a mixing of their outputs can occur. To avoid this, TFORM uses a lock of
 * ErrorMessageLock, but there is no locking functionality in the original MPI
 * specification. We need to synchronise the output from the master and slaves.
 *
 * The idea of the synchronised output (by, e.g., MesPrint()) implemented here is
 *   Slaves:
 *     1. Save the output by WriteFile() (set to PF_WriteFileToFile())
 *        into some buffers between MLOCK(ErrorMessageLock) and
 *        MUNLOCK(ErrorMessageLock), which call PF_MLock() and PF_MUnlock(),
 *        respectively. The output for AM.StdOut and AC.LogHandle are saved to
 *        the buffers.
 *     2. At MUNLOCK(ErrorMessageLock), send the output in the buffer to the master,
 *        with PF_STDOUT_MSGTAG or PF_LOG_MSGTAG.
 *   Master:
 *     1. Receive the buffered output from slaves, and write them by
 *        WriteFileToFile().
 *   The main problem is how and where the master receives messages from
 *   the slaves (PF_ReceiveErrorMessage()). For this purpose there are three
 *   helper functions: PF_CatchErrorMessages() and PF_CatchErrorMessagesForAll()
 *   which remove messages with PF_STDOUT_MSGTAG or PF_LOG_MSGTAG from the top
 *   of the message queue, and PF_ProbeWithCatchingErrorMessages() which is same as
 *   PF_Probe() except removing these messages.
 */

/*
 		#] Explanations : 
 		#[ Variables :
*/

static int errorMessageLock = 0;     /* (slaves) The lock count. See PF_MLock() and PF_MUnlock(). */
static Vector(UBYTE, stdoutBuffer);  /* (slaves) The buffer for AM.StdOut. */
static Vector(UBYTE, logBuffer);     /* (slaves) The buffer for AC.LogHandle. */
#define recvBuffer logBuffer         /* (master) The buffer for receiving messages. */

/*
 * If PF_ENABLE_STDOUT_BUFFERING is defined, the master performs the line buffering
 * (using stdoutBuffer) at PF_WriteFileToFile().
 */
#ifndef PF_ENABLE_STDOUT_BUFFERING
#ifdef UNIX
#define PF_ENABLE_STDOUT_BUFFERING
#endif
#endif

/*
 		#] Variables : 
 		#[ PF_MLock :
*/

/**
 * A function called by MLOCK(ErrorMessageLock) for slaves.
 */
void PF_MLock(void)
{
	/* Only on slaves. */
	if ( errorMessageLock++ > 0 ) return;
	VectorClear(stdoutBuffer);
	VectorClear(logBuffer);
}

/*
 		#] PF_MLock : 
 		#[ PF_MUnlock :
*/

/**
 * A function called by MUNLOCK(ErrorMessageLock) for slaves.
 */
void PF_MUnlock(void)
{
	/* Only on slaves. */
	if ( --errorMessageLock > 0 ) return;
	if ( !VectorEmpty(stdoutBuffer) ) {
		PF_RawSend(MASTER, VectorPtr(stdoutBuffer), VectorSize(stdoutBuffer), PF_STDOUT_MSGTAG);
	}
	if ( !VectorEmpty(logBuffer) ) {
		PF_RawSend(MASTER, VectorPtr(logBuffer), VectorSize(logBuffer), PF_LOG_MSGTAG);
	}
}

/*
 		#] PF_MUnlock : 
 		#[ PF_WriteFileToFile :
*/

/**
 * Replaces WriteFileToFile() on the master and slaves.
 *
 * It copies the given buffer into internal buffers if called between
 * MLOCK(ErrorMessageLock) and MUNLOCK(ErrorMessageLock) for slaves and
 * handle is StdOut or LogHandle, otherwise calls WriteFileToFile().
 *
 * @param  handle  a file handle that specifies the output.
 * @param  buffer  a pointer to the source buffer containing the data to be written.
 * @param  size    the size of data to be written in bytes.
 * @return         the actual size of data written to the output in bytes.
 */
LONG PF_WriteFileToFile(int handle, UBYTE *buffer, LONG size)
{
	if ( PF.me != MASTER && errorMessageLock > 0 ) {
		if ( handle == AM.StdOut ) {
			VectorPushBacks(stdoutBuffer, buffer, size);
			return size;
		}
		else if ( handle == AC.LogHandle ) {
			VectorPushBacks(logBuffer, buffer, size);
			return size;
		}
	}
#ifdef PF_ENABLE_STDOUT_BUFFERING
	/*
	 * On my computer, sometimes a single linefeed "\n" sent to the standard
	 * output is ignored on the execution of mpiexec. A typical example is:
	 *   $ cat foo.c
	 *     #include <unistd.h>
	 *     int main() {
	 *       write(1, "    ", 4);
	 *       write(1, "\n", 1);
	 *       write(1, "    ", 4);
	 *       write(1, "123\n", 4);
	 *       return 0;
	 *     }
	 * or even as a shell script:
	 *   $ cat foo.sh
	 *     #! bin/sh
	 *     printf "    "
	 *     printf "\n"
	 *     printf "    "
	 *     printf "123\n"
	 * When I ran it on mpiexec
	 *   $ while :; do mpiexec -np 1 ./foo.sh; done
	 * I observed the single linefeed (printf "\n") was sometimes ignored. Even
	 * though this phenomenon might be specific to my environment, I added this
	 * code because someone may encounter a similar phenomenon and feel it
	 * frustrating. (TU 16 Jun 2011)
	 *
	 * Phenomenon:
	 *   A single linefeed sent to the standard output occasionally ignored
	 *   on mpiexec.
	 *
	 * Environment:
	 *   openSUSE 11.4 (x86_64)
	 *   kernel: 2.6.37.6-0.5-desktop
	 *   gcc: 4.5.1 20101208
	 *   mpich2-1.3.2p1 configured with '--enable-shared --with-pm=smpd'
	 *
	 * Solution:
	 *   In Unix (in which Uwrite() calls write() system call without any buffering),
	 *   we perform the line buffering here. A single linefeed is also buffered.
	 *
	 * XXX:
	 *   At the end of the program the buffered output (text without LF) will not be flushed,
	 *   i.e., will not be written to the standard output. This is not problematic at a normal run.
	 *   The buffer can be explicitly flushed by PF_FlushStdOutBuffer().
	 */
	if ( PF.me == MASTER && handle == AM.StdOut ) {
		size_t oldsize;
		/* Assume the newline character is LF (when UNIX is defined). */
		if ( (size > 0 && buffer[size - 1] != LINEFEED) || (size == 1 && buffer[0] == LINEFEED) ) {
			VectorPushBacks(stdoutBuffer, buffer, size);
			return size;
		}
		if ( (oldsize = VectorSize(stdoutBuffer)) > 0 ) {
			LONG ret;
			VectorPushBacks(stdoutBuffer, buffer, size);
			ret = WriteFileToFile(handle, VectorPtr(stdoutBuffer), VectorSize(stdoutBuffer));
			VectorClear(stdoutBuffer);
			if ( ret < 0 ) {
				return ret;
			}
			else if ( ret < (LONG)oldsize ) {
				return 0;  /* This means the buffered output in previous calls is lost. */
			}
			else {
				return ret - (LONG)oldsize;
			}
		}
	}
#endif
	return WriteFileToFile(handle, buffer, size);
}

/*
 		#] PF_WriteFileToFile : 
 		#[ PF_FlushStdOutBuffer :
*/

/**
 * Explicitly Flushes the buffer for the standard output on the master, which is
 * used if PF_ENABLE_STDOUT_BUFFERING is defined.
 */
void PF_FlushStdOutBuffer(void)
{
#ifdef PF_ENABLE_STDOUT_BUFFERING
	if ( PF.me == MASTER && VectorSize(stdoutBuffer) > 0 ) {
		WriteFileToFile(AM.StdOut, VectorPtr(stdoutBuffer), VectorSize(stdoutBuffer));
		VectorClear(stdoutBuffer);
	}
#endif
}

/*
 		#] PF_FlushStdOutBuffer : 
 		#[ PF_ReceiveErrorMessage :
*/

/**
 * Receives an error message from a slave's PF_MUnlock() call, and writes
 * the message to the corresponding output.
 * instead of LOCK(ErrorMessageLock) and UNLOCK(ErrorMessageLock).
 *
 * @param  src  the source process.
 * @param  tag  the tag value (must be PF_STDOUT_MSGTAG or PF_LOG_MSGTAG or PF_ANY_MSGTAG).
 */
void PF_ReceiveErrorMessage(int src, int tag)
{
	/* Only on the master. */
	int size;
	int ret = PF_RawProbe(&src, &tag, &size);
	CHECK(ret == 0);
	switch ( tag ) {
		case PF_STDOUT_MSGTAG:
		case PF_LOG_MSGTAG:
			VectorReserve(recvBuffer, size);
			ret = PF_RawRecv(&src, VectorPtr(recvBuffer), size, &tag);
			CHECK(ret == size);
			if ( size > 0 ) {
				int handle = (tag == PF_STDOUT_MSGTAG) ? AM.StdOut : AC.LogHandle;
#ifdef PF_ENABLE_STDOUT_BUFFERING
				if ( handle == AM.StdOut ) PF_WriteFileToFile(handle, VectorPtr(recvBuffer), size);
				else
#endif
				WriteFileToFile(handle, VectorPtr(recvBuffer), size);
			}
			break;
	}
}

/*
 		#] PF_ReceiveErrorMessage : 
 		#[ PF_CatchErrorMessages :
*/

/**
 * Processes all incoming messages whose tag is PF_STDOUT_MSGTAG
 * or PF_LOG_MSGTAG. It ensures that the next PF_Receive(src, tag, ...)
 * will not receive the message with PF_STDOUT_MSGTAG or PF_LOG_MSGTAG.
 *
 * @param[in,out]  src  the source process.
 * @param[in,out]  tag  the tag value.
 */
static void PF_CatchErrorMessages(int *src, int *tag)
{
	/* Only on the master. */
	for (;;) {
		int asrc = *src;
		int atag = *tag;
		int ret = PF_RawProbe(&asrc, &atag, NULL);
		CHECK(ret == 0);
		if ( atag == PF_STDOUT_MSGTAG || atag == PF_LOG_MSGTAG ) {
			PF_ReceiveErrorMessage(asrc, atag);
			continue;
		}
		*src = asrc;
		*tag = atag;
		break;
	}
}

/*
 		#] PF_CatchErrorMessages : 
 		#[ PF_CatchErrorMessagesForAll :
*/

/**
 * Calls PF_CatchErrorMessages() for all slaves and PF_ANY_MSGTAG.
 * Note that it is NOT equivalent to PF_CatchErrorMessages() with PF_ANY_SOURCE.
 */
static void PF_CatchErrorMessagesForAll(void)
{
	/* Only on the master. */
	int i;
	for ( i = 1; i < PF.numtasks; i++ ) {
		int src = i;
		int tag = PF_ANY_MSGTAG;
		PF_CatchErrorMessages(&src, &tag);
	}
}

/*
 		#] PF_CatchErrorMessagesForAll : 
 		#[ PF_ProbeWithCatchingErrorMessages :
*/

/**
 * Same as PF_Probe() except processing incoming messages with PF_STDOUT_MSGTAG
 * and PF_LOG_MSGTAG.
 *
 * @param[in,out]  src  the source process. The output value is that of the actual found message.
 * @return              the tag value of the next incoming message if found,
 *                      0 if a nonblocking probe (input src != PF_ANY_SOURCE) did not
 *                      find any messages. The negative returned value indicates an error.
 */
static int PF_ProbeWithCatchingErrorMessages(int *src)
{
	for (;;) {
		int newsrc = *src;
		int tag = PF_Probe(&newsrc);
		if ( tag == PF_STDOUT_MSGTAG || tag == PF_LOG_MSGTAG ) {
			PF_ReceiveErrorMessage(newsrc, tag);
			continue;
		}
		if ( tag > 0 ) *src = newsrc;
		return tag;
	}
}

/*
 		#] PF_ProbeWithCatchingErrorMessages : 
 		#[ PF_FreeErrorMessageBuffers :
*/

/**
 * Frees the buffers allocated for the synchronized output.
 *
 * Currently, not used anywhere, but could be used in PF_Terminate().
 */
void PF_FreeErrorMessageBuffers(void)
{
	VectorFree(stdoutBuffer);
	VectorFree(logBuffer);
}

/*
 		#] PF_FreeErrorMessageBuffers : 
  	#] Synchronised output : 
*/
