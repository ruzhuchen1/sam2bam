#define _BAM_SAM2BAM_C
/*  bam_sam2bam.c

    (C) Copyright IBM Corp. 2016

    Author: Takeshi Ogasawara, IBM Research - Tokyo

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.  */

#define SORT_BAM_BY_REV
#define SORT_SAM
#define DUMP_THROUGHPUT
#define PAGEOUT_BY_BAMCOPIER
#define PAGEFILE_SEQUENTIAL_WRITE
//#define PROFILE_MEM
//#define PROFILE_BAM_SPACE_CACHE
//#define PAGEFILE_CACHE_PERFILE
#define PAGEFILE_CACHE_PERTHREAD
//#define DUMP

#if defined(__powerpc64__) || defined(__x86_64__)
#define DO_PREFETCH
#define GET_N_PHYSICAL_CORES_FROM_OS
#define CPU_SET_AFFINITY
#endif

#if defined(__powerpc64__)
#define SMT 8
#define HW_ZLIB
#elif defined(__x86_64__)
#define SMT 2
#endif

#define BAM_SPACE_SIZE (BUCKET_SIZE*8)	// the size of pools where bam copiers obtain the memroy spaces for bams
					// max 2TB page file size = 4G(bam_vaddr_t's seqid is 32-bit) x 512KB
//#define BAM_SPACE_SIZE (BUCKET_SIZE*1024)	// the size of pools where bam copiers obtain the memroy spaces for bams
#define N_BAMPTR_SPACE_FREE_LIST 20		// malloc() and then realloc() are avoided in many cases since reusing ~100% bamptr_array_t

#define MAX_LINE_LENGTH		4096	// max length of each line
#define MAX_BLOCK_SEQID		(UINT32_MAX>>2)	// 1G 64KB blocks (64TB file size) (the first 2 bits are reserved for bamid_cmpfunc)
#define my_block_seqid(sid,tid)	(sid<<g_block_reader_id_bits|tid)

#if defined(DO_PREFETCH)
#define PREFETCH_BLKREAD_RW_LOCALITY3(a)	__builtin_prefetch(a, 1 /* rw */, 3 /* temporal locality*/)
#define PREFETCH_SAMPARSE_RW_LOCALITY3(a)	__builtin_prefetch(a, 1 /* rw */, 3 /* temporal locality*/)
#define PREFETCH_BAMCOPY_R_LOCALITY0(a)		__builtin_prefetch(a, 0 /* r */, 0 /* no temporal locality*/)
#define PREFETCH_HASHADD_RW_LOCALITY0(a)	__builtin_prefetch(a, 1 /* rw */, 0 /* no temporal locality*/)
#define PREFETCH_HASHADD_R_LOCALITY0(a)		__builtin_prefetch(a, 0 /* r */, 0 /* no temporal locality*/)
#define PREFETCH_WRITE_R_LOCALITY3(a)		__builtin_prefetch(a, 0 /* r */, 3 /* temporal locality*/)
#define PREFETCH_BAMCOPY_R_LOCALITY3(a)		__builtin_prefetch(a, 0 /* r */, 3 /* temporal locality*/)
#else
#define PREFETCH_BLKREAD_RW_LOCALITY3(a)
#define PREFETCH_SAMPARSE_RW_LOCALITY3(a)
#define PREFETCH_BAMCOPY_R_LOCALITY0(a)
#define PREFETCH_HASHADD_RW_LOCALITY0(a)
#define PREFETCH_HASHADD_R_LOCALITY0(a)
#define PREFETCH_WRITE_R_LOCALITY3(a)
#define PREFETCH_BAMCOPY_R_LOCALITY3(a)
#endif

static int g_drop_cache = 0;
static int g_paging = 0;
static int g_skip_filter = 0;
static int g_unsort = 1;
static int g_n_inputs = 0;
static int g_block_reader_id_bits;

//static int lowmem = 1;
/*  CPU assignment - reading file */
typedef struct {
  int use_nproc;
  int hts_proc_offset;
  int hts_nproc;
  int n_targets; // header->n_targets
  int create_bai;
  const char *fnout;
  int map[];
} cpu_map_t;
static cpu_map_t *cpu_map;  

static int *get_cpu_map(void) { return cpu_map->map; }

#if defined(CPU_SET_AFFINITY)
#define CPU_SET2(i, set)	{CPU_SET(cpu_map->map[i], set);}
/* #define CPU_BLKRD(i)	0	* a single thread bound to a specific core where no other threads run */
#define CPU_BLKRD(i)	(1<g_n_inputs ? ((i)%n_block_reader) : 0)
#define CPU_PAGEWR(i)	2	/* a single thread bound to a specific core where no other threads run */
#define CPU_BAMWT(i)	3	/* WRITE:1 SYNC:2 */
#define N_CORES()	(0==cpu_map->hts_proc_offset ? cpu_map->use_nproc/SMT : (cpu_map->use_nproc - cpu_map->hts_nproc)/SMT + 1/*core0*/)
#if defined(__powerpc64__)
/* cpu affinity for the data read stage */
//#define CPU_LINESPLIT(i)(SMT + SMT*((i)%(n_physical_cores-1)) + (i)/(n_physical_cores-1))
//#define CPU_SAMPARSE(i)    (SMT + SMT*((n_line_splitter+i)%(n_physical_cores-1)) + (n_line_splitter+i)/(n_physical_cores-1))
#define CPU_LINESPLIT(i)(SMT + SMT*((i)%(N_CORES()-1)) + (i)/(N_CORES()-1))
#define CPU_SAMPARSE(i)	(SMT + SMT*((n_line_splitter+i)%(N_CORES()-1)) + (n_line_splitter+i)/(N_CORES()-1))
#define CPU_BAMCOPY(i)  (SMT + SMT*((n_line_splitter+n_sam_parser+i)%(n_physical_cores-1)) + (n_line_splitter+n_sam_parser+i)/(n_physical_cores-1))
#define CPU_HASH(i)	(SMT + SMT*((n_line_splitter+n_sam_parser+n_bam_copier+i)%(n_physical_cores-1)) + (n_line_splitter+n_sam_parser+n_bam_copier+i)/(n_physical_cores-1))
#define CPU_MATEFIND(i)	(SMT*((i)%n_physical_cores) + (i)/n_physical_cores)
#define CPU_SAMSORT(i)	(SMT*((i)%n_physical_cores) + (i)/n_physical_cores)

#elif defined(__x86_64__)
/* cpu affinity for the data read stage */
#define CPU_LINESPLIT(i)(1 + ((i)/(N_CORES()-1))*(N_CORES()) + (i)%(N_CORES()-1))
#define CPU_SAMPARSE(i)	(1 + ((n_line_splitter+i)/(N_CORES()-1))*(N_CORES()) + (n_line_splitter+i)%(N_CORES()-1))
#define CPU_BAMCOPY(i)  (1 + ((n_line_splitter+n_sam_parser+i)/(n_physical_cores-1))*(n_physical_cores) + (n_line_splitter+n_sam_parser+i)%(n_physical_cores-1))
#define CPU_HASH(i)	(1 + ((n_line_splitter+n_sam_parser+n_bam_copier+i)/(n_physical_cores-1))*(n_physical_cores) + (n_line_splitter+n_sam_parser+n_bam_copier+i)%(n_physical_cores-1))
#define CPU_MATEFIND(i)	(i)
#define CPU_SAMSORT(i)	(i)

#endif

static int n_physical_cores;
static int n_physical_cores_valid;
#endif
static int n_block_reader = 1;
static int n_line_splitter = 1;

static int n_sam_parser = 1;
static int n_bam_copier = 1;
static int n_hash_adder = 1;

static int n_bam_writer = 1;	// do not change; a thread keeps the same order of otuput bam's as the input sam's

static struct timespec throttle;
static struct timeval time_begin;

#if	defined(__powerpc64__)
#define IN_SPIN_WAIT_LOOP()	__ppc_set_ppr_low()
#define FINISH_SPIN_WAIT_LOOP()	__ppc_set_ppr_med()
#elif	defined(__x86_64__)
#include <xmmintrin.h>
#define IN_SPIN_WAIT_LOOP()	_mm_pause()
#define FINISH_SPIN_WAIT_LOOP()
#else
#define IN_SPIN_WAIT_LOOP()	nanosleep(&throttle, NULL)
#define FINISH_SPIN_WAIT_LOOP()
#endif


/*  bam_sam2bam.c -- format converter framework

    Author: Takeshi Ogasawara <takeshi@jp.ibm.com>
	$Rev: 91 $
	$Date: 2017-01-24 18:36:10 +0900 (Tue, 24 Jan 2017) $

	*/

#include <stdio.h>
#include <stdbool.h>
#define __USE_GNU
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#define __USE_GNU
#include <string.h>
#include <regex.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/types.h>
#include <fcntl.h>
#include <dirent.h>
#include <strings.h>
#include <dlfcn.h>

#ifdef __powerpc64__
#include <sys/platform/ppc.h>
#endif
#include "htslib/ksort.h"
#include "htslib/khash.h"
#include "htslib/klist.h"
#include "htslib/kstring.h"
#include "htslib/kseq.h"
#include "htslib/sam.h"
#define ASYNC_FLUSH
#include "htslib/hfile.h"
#include "htslib/bgzf.h"

#define __USE_GNU
#include <pthread.h>
#include <malloc.h>
#include "bam_sam2bam.h"

__KS_TYPE(BGZF*);

typedef bam1_t *bam1_p;

static int change_SO(bam_hdr_t *h, const char *so)
{
    char *p, *q, *beg = NULL, *end = NULL, *newtext;
    if (h->l_text > 3) {
        if (strncmp(h->text, "@HD", 3) == 0) {
            if ((p = strchr(h->text, '\n')) == 0) return -1;
            *p = '\0';
            if ((q = strstr(h->text, "\tSO:")) != 0) {
                *p = '\n'; // change back
                if (strncmp(q + 4, so, p - q - 4) != 0) {
                    beg = q;
                    for (q += 4; *q != '\n' && *q != '\t'; ++q);
                    end = q;
                } else return 0; // no need to change
            } else beg = end = p, *p = '\n';
        }
    }
    if (beg == NULL) { // no @HD
        h->l_text += strlen(so) + 15;
        newtext = (char*)malloc(h->l_text + 1);
        sprintf(newtext, "@HD\tVN:1.3\tSO:%s\n", so);
        strcat(newtext, h->text);
    } else { // has @HD but different or no SO
        h->l_text = (beg - h->text) + (4 + strlen(so)) + (h->text + h->l_text - end);
        newtext = (char*)malloc(h->l_text + 1);
        strncpy(newtext, h->text, beg - h->text);
        sprintf(newtext + (beg - h->text), "\tSO:%s", so);
        strcat(newtext, end);
    }
    free(h->text);
    h->text = newtext;
    return 0;
}


#if defined(DEBUG_POS)
static int debug_pos;
static int debug_co;
#endif
#if 0
    public short getLibraryId(final SAMRecord rec) {
        final String library = getLibraryName(this.header, rec);
        Short libraryId = this.libraryIds.get(library);

        if (libraryId == null) {
            libraryId = this.nextLibraryId++;
            this.libraryIds.put(library, libraryId);
        }

        return libraryId;
    }
    public static String getLibraryName(final SAMFileHeader header, final SAMRecord rec) {
        final String readGroupId = (String) rec.getAttribute("RG");

        if (readGroupId != null) {
            final SAMReadGroupRecord rg = header.getReadGroup(readGroupId);
            if (rg != null) {
                final String libraryName = rg.getLibrary();
                if (null != libraryName) return libraryName;
            }
        }

        return "Unknown Library";
    }
#endif

static long	mem_createRg2lib_rgName;
static long	mem_createRg2lib_libName;
static long	mem_createRg2lib_libId;
static long	mem_createRg2lib_done;
static long	mem_getBucket;
static long	mem_getBamp;
static long	mem_readPoolInit1;
static long	mem_readPoolInit2;
static long	mem_core_fnout;
static long	mem_hashQueueMalloc;
#if defined(BAMP_ARRAY)
static long	mem_addToBamptrArray;
#endif
static long	mem_bamptrSpaceMalloc;
static long	mem_bamptrSpaceMalloc2;
static long	mem_bamGetQnameId1;
static long	mem_bamGetQnameId2;
static long	mem_findMates1;
static long	mem_findMates2;
static long	mem_findMates3;
static long	mem_findMates4;
static long	mem_copyToBamSpace;
static long	mem_bamSpaceAlloc;
static long	mem_samparseQueueMalloc;
static long	mem_lineAlloc;
static long	mem_hashInit;
static long	mem_bamcopyInit;
static long	mem_bamcopyQueueMalloc;
static long	mem_samparseInit;
static long	mem_blkrdAlloc;
static long	mem_linesplitInit;
static long	mem_blkrdInit;
static long	mem_bamFreeList2Init;
static long	mem_core_ext;
static long	mem_saveUnmappedReads;
static long	mem_initUnmapSpaceIter;
static long	mem_getBamSpace;
static long	mem_readFromBamSpace;
static long	mem_pagefileInit;
static long	mem_mtPageWriter;
static long	mem_bamwriteInit;
static long	mem_bamwriteQueueMalloc;
static long	mem_bamWriter;
static long	mem_bamwriteQueueArrayInit;
static long	mem_getInstID;
static long	mem_getInstID18;
static long	mem_sortSam1;
static long	mem_sortSam2;
static long	mem_saveUnmapReads;
static long	mem_mtMateFinder__;
static long	mem_blkrdFreeList2Init;

static long md_register_mem_id(const char * mem_id_str);
#define REGISTER_MEM_ID(id) id = md_register_mem_id(#id)

static void md_mem_init(void) {
	REGISTER_MEM_ID(mem_createRg2lib_rgName);
	REGISTER_MEM_ID(mem_createRg2lib_libName);
	REGISTER_MEM_ID(mem_createRg2lib_libId);
	REGISTER_MEM_ID(mem_createRg2lib_done);
	REGISTER_MEM_ID(mem_getBucket);
	REGISTER_MEM_ID(mem_getBamp);
	REGISTER_MEM_ID(mem_readPoolInit1);
	REGISTER_MEM_ID(mem_readPoolInit2);
	REGISTER_MEM_ID(mem_core_fnout);
	REGISTER_MEM_ID(mem_hashQueueMalloc);
#if defined(BAMP_ARRAY)
	REGISTER_MEM_ID(mem_addToBamptrArray);
#endif
	REGISTER_MEM_ID(mem_bamptrSpaceMalloc);
	REGISTER_MEM_ID(mem_bamptrSpaceMalloc2);
	REGISTER_MEM_ID(mem_bamGetQnameId1);
	REGISTER_MEM_ID(mem_bamGetQnameId2);
	REGISTER_MEM_ID(mem_findMates1);
	REGISTER_MEM_ID(mem_findMates2);
	REGISTER_MEM_ID(mem_findMates3);
	REGISTER_MEM_ID(mem_findMates4);
	REGISTER_MEM_ID(mem_copyToBamSpace);
	REGISTER_MEM_ID(mem_bamSpaceAlloc);
	REGISTER_MEM_ID(mem_samparseQueueMalloc);
	REGISTER_MEM_ID(mem_lineAlloc);
	REGISTER_MEM_ID(mem_hashInit);
	REGISTER_MEM_ID(mem_bamcopyInit);
	REGISTER_MEM_ID(mem_bamcopyQueueMalloc);
	REGISTER_MEM_ID(mem_samparseInit);
	REGISTER_MEM_ID(mem_blkrdAlloc);
	REGISTER_MEM_ID(mem_linesplitInit);
	REGISTER_MEM_ID(mem_blkrdInit);
	REGISTER_MEM_ID(mem_bamFreeList2Init);
	REGISTER_MEM_ID(mem_core_ext);
	REGISTER_MEM_ID(mem_saveUnmappedReads);
	REGISTER_MEM_ID(mem_initUnmapSpaceIter);
	REGISTER_MEM_ID(mem_getBamSpace);
	REGISTER_MEM_ID(mem_readFromBamSpace);
	REGISTER_MEM_ID(mem_pagefileInit);
	REGISTER_MEM_ID(mem_mtPageWriter);
	REGISTER_MEM_ID(mem_bamwriteInit);
	REGISTER_MEM_ID(mem_bamwriteQueueMalloc);
	REGISTER_MEM_ID(mem_bamWriter);
	REGISTER_MEM_ID(mem_bamwriteQueueArrayInit);
	REGISTER_MEM_ID(mem_getInstID);
	REGISTER_MEM_ID(mem_getInstID18);
	REGISTER_MEM_ID(mem_sortSam1);
	REGISTER_MEM_ID(mem_sortSam2);
	REGISTER_MEM_ID(mem_saveUnmapReads);
	REGISTER_MEM_ID(mem_mtMateFinder__);
	REGISTER_MEM_ID(mem_blkrdFreeList2Init);
}

static long mem_id = 0; // 0 is not used to detect use of a static id variable (usually zero) before registration
static const char **mem_err = NULL;
#if defined(PROFILE_MEM)
typedef struct profile_mem_data {
  int64_t count;
  int64_t size;
} profile_mem_data_t;
typedef struct profile_mem {
  profile_mem_data_t alloc;
  profile_mem_data_t calloc;
  profile_mem_data_t free;
  profile_mem_data_t realloc;
} profile_mem_t;
static profile_mem_t *mem_stat = NULL;
static void dump_mem_stat0(int memid){
  if(mem_stat[memid].alloc.count) fprintf(stderr,"%.*s.alloc count %ld size %ld\n", 25, mem_err[memid]+4, mem_stat[memid].alloc.count, mem_stat[memid].alloc.size); 
  if(mem_stat[memid].calloc.count) fprintf(stderr,"%.*s.calloc count %ld size %ld\n", 25, mem_err[memid]+4, mem_stat[memid].calloc.count, mem_stat[memid].calloc.size); 
  if(mem_stat[memid].free.count) fprintf(stderr,"%.*s.free count %ld size %ld\n", 25, mem_err[memid]+4, mem_stat[memid].free.count, mem_stat[memid].free.size); 
  if(mem_stat[memid].realloc.count) fprintf(stderr,"%.*s.realloc count %ld size %ld\n", 25, mem_err[memid]+4, mem_stat[memid].realloc.count, mem_stat[memid].realloc.size); 
}
static void dump_bucket_stat();
static void dump_mem_stat(void){
  dump_bucket_stat();

  int i;
  for (i=0; i<mem_id; i++){
    dump_mem_stat0(i);
  }
}
static void *stat_bucket_array;
static void *stat_bucketU_array;
#define PROFILE_MEM_INIT() {memset(&mem_stat[0], 0, sizeof(mem_stat[0])*mem_id); atexit(dump_mem_stat); stat_bucket_array=bucket_array; stat_bucketU_array=bucketU_array;}
#define PROFILE_MEM_MALLOC(size, id, zero) {if(!zero){__atomic_fetch_add(&mem_stat[id].alloc.count,1,__ATOMIC_RELAXED); __atomic_fetch_add(&mem_stat[id].alloc.size,size,__ATOMIC_RELAXED);}else{__atomic_fetch_add(&mem_stat[id].calloc.count,1,__ATOMIC_RELAXED); __atomic_fetch_add(&mem_stat[id].calloc.size,size,__ATOMIC_RELAXED);}}
#define PROFILE_MEM_FREE(size, id) {__atomic_fetch_add(&mem_stat[id].free.count,1,__ATOMIC_RELAXED); __atomic_fetch_add(&mem_stat[id].free.size,size,__ATOMIC_RELAXED);}
#define PROFILE_MEM_REALLOC_1(org, id) size_t const org_size__ = malloc_usable_size(org) 
#define PROFILE_MEM_REALLOC_2(new, id) {					\
	size_t const new_size__ = malloc_usable_size(new);			\
	__atomic_fetch_add(&mem_stat[id].realloc.count,1,__ATOMIC_RELAXED); 	\
	__atomic_fetch_add(&mem_stat[id].realloc.size,new_size__-org_size__,__ATOMIC_RELAXED);	\
}
#define PROFILE_MEM_DUMP() dump_mem_stat()
#else
#define PROFILE_MEM_INIT()
#define PROFILE_MEM_MALLOC(size, id, zero)
#define PROFILE_MEM_FREE(size, id)
#define PROFILE_MEM_REALLOC_1(org, id)
#define PROFILE_MEM_REALLOC_2(p, id)
#define PROFILE_MEM_DUMP()
#endif

#define MEM_ID_INIT_SIZE 64
static long mem_id_slot_size;
// The function should be called in the main thread's context (not thread safe)
static long md_register_mem_id(const char * mem_id_str) {
  if (NULL == mem_err) {
    mem_id_slot_size = MEM_ID_INIT_SIZE;
    mem_err = malloc(mem_id_slot_size * sizeof(mem_err[0]));
#if defined(PROFILE_MEM)
    mem_stat = malloc(mem_id_slot_size * sizeof(mem_stat[0]));
#endif
  } else if (mem_id + 1 >= mem_id_slot_size) {
    mem_id_slot_size *= 2;
    mem_err = realloc(mem_err, mem_id_slot_size * sizeof(mem_err[0]));
#if defined(PROFILE_MEM)
    mem_stat = realloc(mem_stat, mem_id_slot_size * sizeof(mem_stat[0]));
#endif
  }
  if (NULL == mem_err) {
    fprintf(stderr, "md_register_mem_id: malloc failure - slot_size %ld\n", mem_id_slot_size);
    exit(-1);
  }
  mem_err[++mem_id] = mem_id_str;
  return mem_id;
}

static void *md_malloc(size_t size, long memid, long zero_clear) {
  if (0 == memid) {
    fprintf(stderr, "md_malloc: invalid memid=0 used\n");
    exit(-1);
  }
  void *p = zero_clear ? calloc(1, size) : malloc(size);
  if (p) {
    PROFILE_MEM_MALLOC(size, memid, zero_clear);
    return p;
  } else {
    fprintf(stderr, "malloc failure - %s\n", mem_err[memid]);
    exit(-1);
  }
}
static void md_free(void *p, size_t size, long memid){
  if (0 == memid) {
    fprintf(stderr, "md_malloc: invalid memid=0 used\n");
    exit(-1);
  }
  PROFILE_MEM_FREE(size, memid);
  free(p);
}

static void *md_realloc(void *org, size_t size, long memid) {
  if (0 == memid) {
    fprintf(stderr, "md_malloc: invalid memid=0 used\n");
    exit(-1);
  }
  PROFILE_MEM_REALLOC_1(org, memid);
  void *p = realloc(org, size);
  if (p) {
    PROFILE_MEM_REALLOC_2(p, memid);
    return p;
  } else {
    fprintf(stderr, "malloc failure - %s\n", mem_err[memid]);
    exit(-1);
  }
}

struct {
  int size;
  int n;
  char **rg_name;
  char **lib_name;
  int  *lib_id;
} dict_rg2lib;

static void create_rg2lib(bam_hdr_t *h) {
    char *p = h->text;
    char *r = NULL;
    typedef union {
      char c[4];
      short s;
    } at_rg_tab_t;
    at_rg_tab_t argt; // @RG\t
    argt.c[0]='@'; argt.c[1]='R'; argt.c[2]='G'; argt.c[3]='\t';

    dict_rg2lib.size = 20; // use a small array insread of hash, supposing not many @RG lines
    dict_rg2lib.n = 0; // no entries
    dict_rg2lib.rg_name = md_malloc(dict_rg2lib.size*sizeof(char*), mem_createRg2lib_rgName, 0);
    dict_rg2lib.lib_name = md_malloc(dict_rg2lib.size*sizeof(char*), mem_createRg2lib_libName, 0);
    dict_rg2lib.lib_id = md_malloc(dict_rg2lib.size*sizeof(int), mem_createRg2lib_libId, 0);

    do {
      if(*(short*)p == argt.s) {
	int skip = 0;
	// the line is @RG\t ...
	char *id = strstr(p, "\tID:");
	if (id) {
	  dict_rg2lib.rg_name[dict_rg2lib.n] = id+4; // TAB/NL terminated
	} else {
	  // no ID .. skip this @RG
	  skip = 1;
	}
	if (!skip) {
	  char *lb = strstr(p, "\tLB:");
	  if (lb) {
	    dict_rg2lib.lib_name[dict_rg2lib.n] = lb+4; // TAB/NL terminated
	    dict_rg2lib.n ++;
	    if (dict_rg2lib.n == dict_rg2lib.size) {
	      fprintf(stderr, "(I) dict_rg2lib size reaches %d.\n", dict_rg2lib.size);
	      char **t;
	      t = md_realloc(dict_rg2lib.rg_name, (dict_rg2lib.size + 20)*sizeof(char*), mem_createRg2lib_rgName);
	      dict_rg2lib.rg_name = t;
	      t = md_realloc(dict_rg2lib.lib_name, (dict_rg2lib.size + 20)*sizeof(char*), mem_createRg2lib_libName);
	      dict_rg2lib.lib_name = t;
	      dict_rg2lib.lib_id = md_realloc(dict_rg2lib.lib_id, (dict_rg2lib.size + 20)*sizeof(int), mem_createRg2lib_libId);
	      dict_rg2lib.size += 20;
	    }
  	  } else {
	    // no LB .. skip this @RG
	  }
  	}
      }
      r = index(p, '\n');
      p = r + 1;
    } while (r);

    char *done = md_malloc(dict_rg2lib.n*sizeof(char), mem_createRg2lib_done, 1);
    int i;
    int id=-1;
    for(i=0; i < dict_rg2lib.n; i++){
      if(done[i]==0){
        id++;
	dict_rg2lib.lib_id[i] = id;
        int j;
        char *const s1=dict_rg2lib.lib_name[i];
	char *del=strpbrk(s1, "\t\n");
        int const l = del-s1;
        for(j=i+1; j < dict_rg2lib.n; j++){ 
	  if(done[j]==0){
	    char * const s2 = dict_rg2lib.lib_name[j];
	    if(0 == strncmp(s1, s2, l)){
	      dict_rg2lib.lib_id[j] = id;
	      done[j]=1;
	    }
	  }
	}
      }
    }
    md_free(done, dict_rg2lib.n*sizeof(char), mem_createRg2lib_done);

    fprintf(stderr, "(I) dict_rg2lib count %d\n", dict_rg2lib.n);
#if 0
    for(i=0; i < dict_rg2lib.n; i++){
      fprintf(stderr, "(I)   %d %.*s %.*s %d\n", i, (int)(strpbrk(dict_rg2lib.rg_name[i],"\t\n") - dict_rg2lib.rg_name[i]), dict_rg2lib.rg_name[i], (int)(strpbrk(dict_rg2lib.lib_name[i],"\t\n") - dict_rg2lib.lib_name[i]), dict_rg2lib.lib_name[i], dict_rg2lib.lib_id[i]);
    }
#endif
}



static inline int32_t get_unclipped_pos0(bam1_t *b) {
  const uint32_t * const cigar = bam_get_cigar(b);
  int32_t unclipped_pos = b->core.pos;

  if (b->core.flag & BAM_FUNMAP){
    fprintf(stderr, "(E) unsupported: get_unclipped_pos() called for unmapped read\n");
    fprintf(stderr, "(E) %s flag %d pos %d\n", bam_get_qname(b), b->core.flag, b->core.pos);
    fprintf(stderr, "(E) ... abort ... but intentionally cause a SEGV to be trapped in a debugger\n");
    *(char*)0 = 0;
    exit(-1);
  }
  if (0 == (b->core.flag & BAM_FREVERSE)) {
    int i;
    for(i=0; i<b->core.n_cigar; i++) {
      if (bam_cigar_op(cigar[i]) == BAM_CSOFT_CLIP ||
          bam_cigar_op(cigar[i]) == BAM_CHARD_CLIP) {
 	unclipped_pos -= bam_cigar_oplen(cigar[i]);
      } else
        break;
    }
  } else {
    int i;
    for(i=0; i<b->core.n_cigar; i++) {
      switch(bam_cigar_op(cigar[i])){ 
	case BAM_CMATCH:	// M
	case BAM_CDEL: 		// D
	case BAM_CREF_SKIP: 	// N
	case BAM_CEQUAL: 	// EQ
	case BAM_CDIFF: 	// X
	unclipped_pos += bam_cigar_oplen(cigar[i]);
      }
    }
    unclipped_pos += - 1;
    for(i=b->core.n_cigar-1; i>=0; i--) {
      if (bam_cigar_op(cigar[i]) == BAM_CSOFT_CLIP ||
          bam_cigar_op(cigar[i]) == BAM_CHARD_CLIP) {
 	unclipped_pos += bam_cigar_oplen(cigar[i]);
      } else
	break;
    }
  }
//if (b->core.pos != unclipped_pos) fprintf(stderr, "%s pos %d unclipped %d\n", bam_get_qname(b), b->core.pos, unclipped_pos);
  if (unclipped_pos > (int)MAX_POS) { fprintf(stderr, "%s pos %d unclipped_pos 0x%9x (%d) > 0x%09lx\n", bam_get_qname(b), b->core.pos, unclipped_pos, unclipped_pos, MAX_POS); exit(-1);}
  return unclipped_pos;
}

// The unclipped pos can be negative. For example, POS=1 and CIGAR=68S...
//#define CLIPPED_TID_OFFSET	1
//#define CLIPPED_POS_OFFSET	1
#define wrap_bam1_tid(bam1_tid)		((bam1_tid) + 1)	// convert signed (-1~2^31) to unsigned (0~2^31+1)
#define unwrap_tid(tid)			((tid) - 1)
#define wrap_bam1_clipped_pos(bam1_pos)	((bam1_pos) + 1)	// convert signed (-1~2^31) to unsigned (0~2^31+1)
#define unwrap_clipped_pos(pos)		((pos) - 1)
//#define UNCLIPPED_TID_OFFSET	1
//#define UNCLIPPED_POS_OFFSET	(64*1024)
#define UC_POS_OFFSET	(64*1024)
#define wrap_bam1_unclipped_pos(bam1_unclipped_pos)	(assert_wrap_bam1_unclipped_pos(bam1_unclipped_pos), (uint32_t)((bam1_unclipped_pos) + UC_POS_OFFSET))	// convert signed (-X~2^31-X) to unsigned (C-X~2^31+C-X)
#define unwrap_unclipped_pos(unclipped_pos)		((int32_t)((int64_t)(unclipped_pos) - UC_POS_OFFSET))
static inline void assert_wrap_bam1_unclipped_pos(int32_t bam1_unclipped_pos) {
  if(bam1_unclipped_pos < 0 ) {
    if(bam1_unclipped_pos < -UC_POS_OFFSET){
      fprintf(stderr,"wrap_bam1_unclipped_pos: unclipped pos %d must be >= -%d\n", bam1_unclipped_pos, UC_POS_OFFSET); 
      exit(-1);
    }
  } else { // bam1_unclipped_pos >= 0
    if((uint64_t)bam1_unclipped_pos + UC_POS_OFFSET > (uint64_t)UINT_MAX){
      fprintf(stderr,"wrap_bam1_unclipped_pos: unclipped pos %d must be <= %d\n", bam1_unclipped_pos, UINT_MAX - UC_POS_OFFSET); 
      exit(-1);
    }
  }
}

static inline uint32_t get_offset_unclipped_pos(bam1_t *b) {
  //int32_t u_p = wrap_bam1_unclipped_pos(get_unclipped_pos0(b));
  //if (u_p < 0) { fprintf(stderr, "%s pos %d unclipped_pos 0x%9x (%d) + offset %d < 0\n", bam_get_qname(b), b->core.pos, u_p, u_p, UNCLIPPED_POS_OFFSET); exit(-1);}
  //return (uint32_t)u_p;
  return wrap_bam1_unclipped_pos(get_unclipped_pos0(b));
}

static inline uint32_t get_offset_unclipped_pos_from_index(int i, int j){
  return (uint32_t)((((vid_t)i)*BUCKET_SIZE + (vid_t)j)&POS_MASK);
}
static inline uint32_t get_offset_pos_from_index(int i, int j){
  return (uint32_t)((((vid_t)i)*BUCKET_SIZE + (vid_t)j)&POS_MASK);
}
static inline uint32_t get_offset_tid_from_index(int i, int j){
  return (uint32_t)(((((vid_t)i)*BUCKET_SIZE + (vid_t)j)>>POS_BITS)&TID_MASK);
}

static inline uint64_t gen_id_clipped(vid_t tid1, vid_t pos1) {
	//const vid_t tid1 = (vid_t)((int64_t)tid + CLIPPED_TID_OFFSET)/* tid can be -1 for unmapped reads */;
	//const vid_t pos1 = (vid_t)((int64_t)pos + CLIPPED_POS_OFFSET)/* pos can be -1 for unmapped reads */ ;
	if (tid1 > MAX_TID) { 
          fprintf(stderr, "gen_id_clipped: tid 0x%09lx (%ld) > 0x%09lx\n", tid1, tid1, MAX_TID); exit(-1);
        }
	if (pos1 > MAX_POS) { fprintf(stderr, "gen_id_clipped: pos 0x%09lx (%ld) > 0x%09lx\n", pos1, pos1, MAX_POS); exit(-1);}
	return (((tid1)<<POS_BITS)|pos1);
}
static inline uint64_t gen_id_unclipped(vid_t tid1, vid_t pos1) {
	//const vid_t tid1 = (vid_t)wrap_bam1_tid((int64_t)bam1_u_tid);
	//const vid_t pos1 = (vid_t)((int64_t)u_pos + UNCLIPPED_POS_OFFSET)/* pos can be -1 for unmapped reads */ ;
	if (tid1 > MAX_TID) { fprintf(stderr, "gen_id_unclipped: tid 0x%09lx (%ld) > 0x%09lx\n", tid1, tid1, MAX_TID); exit(-1);}
	if (pos1 > MAX_POS) { fprintf(stderr, "gen_id_unclipped: pos 0x%09lx (%ld) > 0x%09lx\n", pos1, pos1, MAX_POS); exit(-1);}
	return (((tid1)<<POS_BITS)|pos1);
}
static inline uint64_t gen_idU(bam1_t *b, int is_mate, uint32_t offset_unclipped_pos) {
	if (is_mate) {
	  fprintf(stderr, "cannot calculate the unclipped position for the mate\n");
	  exit(-1);
	}
	const vid_t tid1 = wrap_bam1_tid(!is_mate ? b->core.tid : b->core.mtid);
	/* alignment position */
	const vid_t pos1 = (!is_mate ? 
				  offset_unclipped_pos :
				  (fprintf(stderr, "(E) cannot calculate the unclipped position for the mate\n"),  exit(-1), 0));
	if (tid1 > MAX_TID) { 
	  fprintf(stderr, "gen_idU: tid 0x%09lx (%ld) > 0x%09lx\n", tid1, tid1, MAX_TID); exit(-1);
	}
	if (pos1 > MAX_POS) { 
	  fprintf(stderr, "gen_idU: pos 0x%09lx (%ld) > 0x%09lx\n", pos1, pos1, MAX_POS); exit(-1);
	}
	return (((tid1)<<POS_BITS)|pos1);
}
//static inline int32_t get_unclipped_pos_from_index(int i, int j){
//  return (int32_t)(((int64_t)get_offset_unclipped_pos_from_index(i, j)) - UNCLIPPED_POS_OFFSET);
//}
static inline uint64_t gen_id(bam1_t *b, int is_mate) {
	const vid_t tid1 = wrap_bam1_tid(!is_mate ? b->core.tid : b->core.mtid);
	/* alignment position */
	const vid_t pos1 = wrap_bam1_clipped_pos(!is_mate ? b->core.pos : b->core.mpos);
	if (tid1 > MAX_TID) { 
	  fprintf(stderr, "gen_id: tid 0x%09lx (%ld) > 0x%09lx\n", tid1, tid1, MAX_TID); exit(-1);
	}
	if (pos1 > MAX_POS) { 
	  fprintf(stderr, "gen_id: pos 0x%09lx (%ld) > 0x%09lx\n", pos1, pos1, MAX_POS); exit(-1);
	}
	return (((tid1)<<POS_BITS)|pos1);
}
static inline int32_t get_bam1_pos_from_index(int i, int j){
  return unwrap_clipped_pos((int32_t)(((int64_t)get_offset_pos_from_index(i,j))));
}
static inline int32_t get_bam1_tid_from_index(int i, int j){
  return unwrap_tid((int32_t)(((int64_t)(((((vid_t)i)*BUCKET_SIZE + (vid_t)j)>>POS_BITS)&TID_MASK))));
}

static inline bucket_t *get_bucket(int bucket_index, bucket_array_t *bucket_array, int alloc);
static inline bamptr_t *get_fifo_list_from_bucket(bucket_t *bucket, int idx2, int unclipped);

#define INVALID_UNCLIP	SHRT_MIN

static inline bamptr_t * bamptr_C2U(bamptr_t *bampC, bucket_array_t *bucketU_array, int i, int j ) {
  const int64_t bam1_c_tid = get_bam1_tid_from_index(i, j);
  const int64_t bam1_u_tid = bam1_c_tid;
  const int64_t bam1_c_pos = get_bam1_pos_from_index(i, j);
  const int16_t unclip = bamptr_get_unclip(bampC);
  bamptr_t *bampU = NULL;

  if (INVALID_UNCLIP != unclip) {
    const int64_t bam1_u_pos = bam1_c_pos + unclip;
    const vid_t u_tid = wrap_bam1_tid(bam1_u_tid);
    const vid_t u_pos = wrap_bam1_unclipped_pos(bam1_u_pos);
    const vid_t u_id = gen_id_unclipped(u_tid, u_pos);
    const int u_bucket_index = get_index1(u_id);
    const int u_bucket_index2 = get_index2(u_id);
    bucket_t * const u_bucket = get_bucket(u_bucket_index, bucketU_array, 0/* no alloc*/);
    bamptr_t * const top = get_fifo_list_from_bucket(u_bucket, u_bucket_index2, 1/*unclip*/);

#if defined(BAMP_ARRAY)
    bamptr_array_t * const ba = container_of(top, bamptr_array_t, bp[0]);
    const uint64_t id = bamptrC_get_bamid(bampC);
    int ix;
    bamptr_t *bamp;
    for(ix=0,bamp=top; ix<ba->size; ix++, bamp=get_bamptr_at(ba, ix, 1/*unclipped*/))
    {
      if(id == bamptrU_get_bamid(bamp)) {
        bampU = bamp;
        break;
      }
    }
    if (ix == ba->size) {
      fprintf(stderr, "(E) bamptr_C2U not found\n");
      exit(-1);
    }
#else
#error "not implemented"
#endif
  }
  return bampU;
}

/*
typedef union bam_vaddr {
  uint64_t va;
  struct {
    uint32_t seqid;
    uint32_t offset;
  } block;

  bam1_t *p_bam;
} bam_vaddr_t;
*/

static inline uint8_t bam_get_libid(bam_hdr_t *h, bam1_t *b);

typedef struct {
  baminfoU1_t u;
  baminfoC1_t c;
#if defined(DEBUG_POS)
  char *qname;
#endif
} baminfo1_t;

enum api_filter { 
  api_get_api_version, 
  api_get_filter_name,
  api_init_filter, 
  api_pre_filter, 
  api_do_filter, 
  api_post_filter, 
  api_end_filter, 
  api_analyze_data,
  n_filter_api 
};
static char *api_filter_name[n_filter_api] = { 
  "get_api_version", 
  "get_filter_name", 
  "init_filter", 
  "pre_filter", 
  "do_filter", 
  "post_filter", 
  "end_filter", 
  "analyze_data" 
};
typedef struct {
  char *p_filter_name;
  void *(*p_filter_api[n_filter_api])();
} filter_api_t;

typedef struct {
  int n_threads;
  bam_hdr_t *header;
  bucket_array_t *bucket_array;
  bucket_array_t *bucketU_array;
} rt_val_t;

typedef struct {
  long n;
  struct {
    const char *name;
    const char *args;
  } args[];
} filter_args_t;

typedef struct {
  const char *name;
  void *(*func)();
} filter_func_t;

static struct {
  long n_filter;
  struct {
    long n;
    filter_func_t *funcs;
  } funcs[n_filter_api];
  filter_args_t *args;
  filter_api_t *filter;
  rt_val_t rt;
  long use_baminfo;
  struct bam_free_list2 *p_bam_free_list2;
} filters;

static inline void init_baminfo1(baminfo1_t *baminfo1, bam1_t *b, uint64_t qnameid, bam_hdr_t *h, bam_vaddr_t bamva, uint64_t bamid, uint32_t offset_unclipped_pos) {
    if (0 == (b->core.flag & BAM_FUNMAP)) {
      baminfo1->c.bamid   = bamid;
    } else {
      baminfo1->c.qnameid = qnameid;
    }
    baminfo1->c.bamva     = bamva;
    if (0 <= b->l_data && b->l_data <= USHRT_MAX) {
      baminfo1->c.l_data  = b->l_data;
    } else {
      fprintf(stderr, "(E) bam1_t l_data %u cannot be expressed in 16 bits. %s %d %d %d\n", b->l_data, bam_get_qname(b), b->core.flag, b->core.tid, b->core.pos);
      exit(-1);
    }
    if (b->core.flag < 1<<(FLAGL_BITS + sizeof(baminfo1->c.flagH)*8)) {
      baminfo1->c.flagL   = b->core.flag&((1<<FLAGL_BITS) - 1);
      baminfo1->c.flagH   = b->core.flag>>FLAGL_BITS;
    } else {
      fprintf(stderr, "(E) flag %u cannot be expressed in %lu bits. %s %d %d\n", b->core.flag, FLAGL_BITS + sizeof(baminfo1->c.flagH)*8, bam_get_qname(b), b->core.tid, b->core.pos);
      exit(-1);
    }
    if (0 == ((b)->core.flag & BAM_FUNMAP)) {
      //int64_t const unclip = (int64_t)offset_unclipped_pos - UNCLIPPED_POS_OFFSET - b->core.pos;
      int64_t const unclip = (int64_t)unwrap_unclipped_pos(offset_unclipped_pos) - b->core.pos;
      if (INT_MIN <= unclip && unclip <= INT_MAX) {
        baminfo1->c.unclip  = unclip;
      } else {
        fprintf(stderr, "(E) clip length %ld cannot be expressed in 32 bits. = unwrap(%u)=%d-%d (%s flag %d tid %d pos %d)\n", unclip, offset_unclipped_pos, unwrap_unclipped_pos(offset_unclipped_pos), b->core.pos, bam_get_qname(b), b->core.flag, b->core.tid, b->core.pos);
        exit(-1);
      }
    } else {
      baminfo1->c.unclip  = INVALID_UNCLIP;
    }

    baminfo1->u.id      = bamid;
    baminfo1->u.qnameid = qnameid;
    baminfo1->u.flag    = b->core.flag;
    baminfo1->u.libid   = bam_get_libid(h,b);
    baminfo1->u.tid     = b->core.tid + 1;		// convert signed (-1~2^31) to unsigned (0~2^31+1)
    baminfo1->u.mtid	= b->core.mtid + 1;		// convert signed (-1~2^31) to unsigned (0~2^31+1)
    baminfo1->u.pos     = b->core.pos + 1;		// convert signed (-1~2^31) to unsigned (0~2^31+1)
    baminfo1->u.mpos    = b->core.mpos + 1;		// convert signed (-1~2^31) to unsigned (0~2^31+1)
    baminfo1->u.mapqL   = (b->core.qual&((1<<MAPQL_BITS)-1));
    baminfo1->u.mapqH   = (b->core.qual>>MAPQL_BITS);
    if (TLEN_MIN <= b->core.l_qseq && b->core.l_qseq <= TLEN_MAX) {
      baminfo1->u.tlen  = b->core.l_qseq;
    } else {
      fprintf(stderr, "(E) tlen not expressed in %d bits\n", TLEN_BITS);
      exit(-1);
    }
    int i;
    for (i=0; i<filters.funcs[api_analyze_data].n; i++) {
      (*filters.funcs[api_analyze_data].funcs[i].func)(b, &baminfo1->u.filter_data, sizeof(baminfo1->u.filter_data));
    }
    // baminfo1->u.reverse =(0 != ((b)->core.flag & BAM_FREVERSE));
    baminfo1->u.offset_unclipped_pos = offset_unclipped_pos; // for markdup to sort baminfo's with the same unclipped pos by their mates' unclipped pos
#if defined(DEBUG_POS)
baminfo1->qname = malloc(b->core.l_qname+1);
memcpy(baminfo1->qname, bam_get_qname(b), b->core.l_qname);
baminfo1->qname[b->core.l_qname] = 0;
#endif
}

#define bamptr_set_mate(bamp, mate)	{assert_u(bamp)(bamp)->baminfoU[0].mate_bamp = (mate);}
//#define bamptr_get_mate(bamp)		(assert_u(bamp)(bamp)->baminfoU[0].mate_bamp)
static void get_qname(uint64_t, char *, long, long *); 

#if defined(BAMP_ARRAY)
// do not enable MATE_CACHE. bamptr->mate pointers are not updated when the target bamptrs are copied. Bamptr must be locked to update bamptr->mate, but currently is not locked. 
// #define MATE_CACHE	// acceleration of finding mates
#if defined(MATE_CACHE)
#error "Do not enable MATE_CACHE."
{{{
#define DCL_ARG_QNAMEID_CACHE(p_cache)	, cache_for_qnameid_t *p_cache
#define ARG_QNAMEID_CACHE(p_cache)	, p_cache
#define PROFILE_MATE_CACHE
#if defined(PROFILE_MATE_CACHE)
static long mc_hit=0, mc_miss=0;
enum prof_mate_cache { pmc_hit, pmc_miss, pmc_new, pmc_skip };
#define PROF_DCL_MATE_CACHE_HIT \
enum prof_mate_cache cachehit=pmc_skip; \
bamptr_t *old_cache=NULL;
#define PROF_MATE_CACHE_HIT cachehit=pmc_hit
#define PROF_MATE_CACHE_MISS { cachehit=pmc_miss; old_cache=p_cache->cache_bamptr; }
#define PROF_MATE_CACHE_NEW cachehit=pmc_new
#define PROF_MATE_CACHE_SKIP cachehit=pmc_skip
static void prof_mate_cache_dump(baminfo1_t *baminfo1, bamptr_t *cache, int cachehit, bamptr_t *bamp, void *p_cache) {
  char str1[256],str2[256];
  long len1=0,len2=0;
  uint64_t qnameid1=0,qnameid2=0;

  switch(cachehit){
  case pmc_hit: __atomic_fetch_add(&mc_hit, 1, __ATOMIC_RELAXED); break;
  case pmc_miss: __atomic_fetch_add(&mc_miss, 1, __ATOMIC_RELAXED); break;
  }
  if (cachehit==pmc_miss) {
    qnameid2 = baminfo1->u.qnameid;
    get_qname(qnameid2, str2, sizeof(str2), &len2);
    qnameid1 = bamptr_get_qnameid(cache);
    get_qname(qnameid1, str1, sizeof(str1), &len1);
    static long cnt = 0;
    cnt++;
    if ((cnt&0xffff)==0){
      fprintf(stderr, "bamptr_init: &cache %p mate hit %.1f miss %.1f "
		"cache %p %.*s flag %d pos %d this %p %.*s flag %d pos %d\t%s\n", 
		p_cache, mc_hit*100.0/(mc_hit+mc_miss), mc_miss*100.0/(mc_hit+mc_miss), 
		(void*)qnameid1, (int)len1, str1, (cache)->baminfoU[0].info1.flag, (cache)->baminfoU[0].info1.pos,
		(void*)qnameid2, (int)len2, str2, (bamp)->baminfoU[0].info1.flag, (bamp)->baminfoU[0].info1.pos,
		cachehit==pmc_hit? "H" : cachehit==pmc_miss? "***M***" : "");
      fflush(stderr);
    }
  }
}
#define PROF_MATE_CACHE_DUMP prof_mate_cache_dump(baminfo1, old_cache, cachehit, bamp, p_cache)
#else
#define PROF_DCL_MATE_CACHE_HIT
#define PROF_MATE_CACHE_HIT
#define PROF_MATE_CACHE_MISS
#define PROF_MATE_CACHE_NEW
#define PROF_MATE_CACHE_SKIP
#define PROF_MATE_CACHE_DUMP
#endif

static inline long is_mate(bamptr_t *bamp1, bamptr_t *bamp2) {
  long ret = bamptr_get_tid(bamp1, 1) == bamptr_get_tid(bamp2, 0)
	  && bamptr_get_pos(bamp1, 1) == bamptr_get_pos(bamp2, 0)
	  && bamptr_get_tid(bamp2, 1) == bamptr_get_tid(bamp1, 0)
	  && bamptr_get_pos(bamp2, 1) == bamptr_get_pos(bamp1, 0);
  return ret;
}

typedef struct {
  bamptr_t *cache_bamptr;
  uint64_t n_miss;
} cache_for_qnameid_t;
}}}
#else
#define DCL_ARG_QNAMEID_CACHE(p_cache)
#define ARG_QNAMEID_CACHE(p_cache)
#endif

//#define has_mate(flag) ((flag) & BAM_FPAIRED && !((flag) & BAM_FUNMAP) && !((flag) & BAM_FMUNMAP))
#define has_mate(flag) ((flag) & BAM_FPAIRED)

static inline void bamptr_init(bamptr_t *bamp, baminfo1_t *baminfo1, int unclipped DCL_ARG_QNAMEID_CACHE(p_cache)) {
  if (unclipped){
    (bamp)->baminfoU[0].info1   = baminfo1->u;
    (bamp)->baminfoU[0].info1.unclipped = 1;
#if !defined(MATE_CACHE)
    (bamp)->baminfoU[0].mate_bamp   = NULL;
#else
{{{
    PROF_DCL_MATE_CACHE_HIT;
    bamptr_t * const cache_bamp = p_cache->cache_bamptr;

    short const flag2 = bamptr_get_flag(bamp); 
    if (cache_bamp) {
      if (bamptr_get_qnameid(cache_bamp) == baminfo1->u.qnameid) {
        //short const flag1 = bamptr_get_flag(*p_cache,0); 
        if (// already checked has_mate(flag)
            has_mate(flag2)) {
          // check if one points to the other
	  if(bamptr_get_tid(cache_bamp,1) == bamptr_get_tid(bamp,0) && bamptr_get_pos(cache_bamp,1) == bamptr_get_pos(bamp,0) &&
             bamptr_get_tid(bamp,1) == bamptr_get_tid(cache_bamp,0) && bamptr_get_pos(bamp,1) == bamptr_get_pos(cache_bamp,0)) { 
	    PROF_MATE_CACHE_HIT;
            bamptr_set_mate(bamp, cache_bamp);
            bamptr_set_mate(cache_bamp, bamp);
            p_cache->cache_bamptr = NULL;
	  } else {
            // not paired || paried but unmap || paired but mate unmap
            // do not update the cache
            PROF_MATE_CACHE_SKIP;
	  }
	} else {
          // not paired || paried but unmap || paired but mate unmap
          // do not update the cache
          PROF_MATE_CACHE_SKIP;
	}
      } else {
        // qnameid1 != qnameid2
        if (has_mate(flag2)) { 
          // ensure that the cache has a "paired && map && mate map" bamptr
	  PROF_MATE_CACHE_MISS;
          p_cache->cache_bamptr = bamp;
	  p_cache->n_miss++;
        } else {
          // not paired || paried but unmap || paired but mate unmap
          // do not update the cache
          PROF_MATE_CACHE_SKIP;
        }
        (bamp)->baminfoU[0].mate_bamp   = NULL;
      }
    } else {
      // p_cache->cache_bamptr == NULL
      if (has_mate(flag2)) { 
        // ensure that the cache has a "paired && map && mate map" bamptr
        p_cache->cache_bamptr = bamp;
        PROF_MATE_CACHE_NEW;
      } else {
        // not paired || paried but unmap || paired but mate unmap
        // do not update the cache
        PROF_MATE_CACHE_SKIP;
      }
      (bamp)->baminfoU[0].mate_bamp   = NULL;
    }
    PROF_MATE_CACHE_DUMP;
}}}
#endif
  } else {
    (bamp)->baminfoC[0].info1   = baminfo1->c;
    (bamp)->baminfoC[0].info1.unclipped = 0;
  }
#if defined(DEBUG_POS)
  (bamp)->dump_qname = baminfo1->qname;
#endif
}
#define bamptr_get_qnameid(bamp)		(assert_u(bamp)(bamp)->baminfoU[0].info1.qnameid)

#define dump_bamptr_get_mtid(bamp) 		(assert_u(bamp)(bamp)->baminfoU[0].mate_bamp ? bamptr_get_tid((bamp)->baminfoU[0].mate_bamp) : -8888 )
#endif

//#define bamptr_match_qname(bamp1, bamp2)	bamptr_match_qname_(bamp1, bamp2)
//#define bamptr_get_dup(bamp)		(assert_c(bamp)(bamp)->baminfoC[0].info1.dup)
#define bamptr_get_bamva(bamp)  	(assert_c(bamp)(bamp)->baminfoC[0].info1.bamva)
#define bamptr_get_l_data(bamp)		(assert_c(bamp)(bamp)->baminfoC[0].info1.l_data)

#define bamptr_get_mpos(bamp)		(assert_u(bamp)(bamp)->baminfoU[0].info1.mpos)
#define bamptr_get_reverse(bamp)	(assert_u(bamp)(0 != ((bamp)->baminfoU[0].info1.flag& BAM_FREVERSE)))

  /*
   * https://en.wikipedia.org/wiki/FASTQ_format#Illumina_sequence_identifiers
   * Illumina
   * 20FUKAAXX10202		The unique instrument name
   * 3				Flowcell lane
   * 23				The number within the flowcell lane
   * 20460			The x-coordinate of the cluster within the title
   * 39157			The y-coordinate of the cluster within the title
   * #0				The index number for a multiplexed sample (0 for no indexing)
   * /1				The number of a pair, /1 or /2 (paired-end or mate-pair reads only)
   */
static int is_illumina(char *qn){
  char * endp;
  char *s = index(qn, ':');
  if (s) {
    strtol(s+1, &endp, 10);
    if (s+1 < endp && ':' == *endp){ // v is lane
      char * endp2;
      strtol(endp+1, &endp2, 10);
      if (endp+1 < endp2 && ':' == *endp2){ // v is number within lane
	char * endp3;
	strtol(endp2+1, &endp3, 10);
	if (endp2+1 < endp3 && ':' == *endp3){ // v is x
	  char * endp4;
	  strtol(endp3+1, &endp4, 10);
	  if (endp3+1 < endp4){ // v is y
	    return 1;
	  }
	}
      }
    }
  }
      return 0;
}
  /*
   * https://en.wikipedia.org/wiki/FASTQ_format#Illumina_sequence_identifiers
   *
   * Illumina Casava 1.8
   HWI-D00402:119:C6BUCANXX:1:1101:1418:1888
   HWI-D00402			The unique instrument name
   119				Run ID
   C6BUCANXX			Flowcell ID
   1				Lane number within the flowcell lane
   1101				Tile number within the lane
   1418				x-coordinate
   1888				y-coordinate
   */
static int is_illumina18(char *qn){
  char * endp;
  char *s = index(qn, ':');
  if (s) {
    strtol(s+1, &endp, 10);
    if (s+1 < endp && ':' == *endp){ // v is run id
      char * endp2;
      endp2 = index(endp+1, ':');
      if (endp+1 < endp2 && ':' == *endp2){ // v is flowcell id
	char * endp3;
	strtol(endp2+1, &endp3, 10);
	if (endp2+1 < endp3 && ':' == *endp3){ // v is lane number
	  char * endp4;
	  strtol(endp3+1, &endp4, 10);
	  if (endp3+1 < endp4){ // v is tile number
	    char * endp5;
	    strtol(endp4+1, &endp5, 10);
	    if (endp4+1 < endp5){ // v is x
	      char * endp6;
	      strtol(endp5+1, &endp6, 10);
	      if (endp5+1 < endp6){ // v is y
	        return 1;
	      }
	    }
	  }
	}
      }
    }
  }
      return 0;
}
static int is_SRR(char *qn){
// http://www.ebi.ac.uk/ena/submit/read-data-format
if ((qn[0]=='D' /* DDBJ */|| qn[0]=='E' /* EBI */|| qn[0]=='S' /* NCBI */) && qn[1]=='R' && qn[2]=='R' /* Run */){
  char *s = &qn[3];
  char * endp;
  strtol(s+1, &endp, 10);
  if (s+1 < endp && '.' == *endp){ // "SRR1234."
    s = endp;
    strtol(s+1, &endp, 10);
    if (s+1 < endp && 0 == *endp){ // "SRR.1234"
      return 1;
    }
  }
}
return 0;
}

#define QNAME_HASH_SIZE (16*1024*1024) /* 24 bits */
#define QNAME_HASH_MASK (0xFFFFFF)
//#define QNAME_HASH_SIZE (64*1024*1024) /* 26 bits */
//#define QNAME_HASH_MASK (0x3FFFFFF)

#define ILM_BITS_Y	20
#define ILM_BITS_X	17
#define ILM_BITS_TILE	14
#define ILM_BITS_LANE	5
#define ILM_BITS_INST	8
#define ILM_MAX_Y	((1UL<<ILM_BITS_Y)-1)
#define ILM_MAX_X	((1UL<<ILM_BITS_X)-1)
#define ILM_MAX_TILE	((1UL<<ILM_BITS_TILE)-1)
#define ILM_MAX_LANE	((1UL<<ILM_BITS_LANE)-1)
#define ILM_MAX_INST	((1UL<<ILM_BITS_INST)-1)

#define ILM18_BITS_Y	18
#define ILM18_BITS_X	15
#define ILM18_BITS_TILE	15
#define ILM18_BITS_LANE	3
#define ILM18_BITS_FLOW	3
#define ILM18_BITS_RUN	8
#define ILM18_BITS_INST	2
#define ILM18_MAX_Y	((1UL<<ILM18_BITS_Y)-1)
#define ILM18_MAX_X	((1UL<<ILM18_BITS_X)-1)
#define ILM18_MAX_TILE	((1UL<<ILM18_BITS_TILE)-1)
#define ILM18_MAX_LANE	((1UL<<ILM18_BITS_LANE)-1)
#define ILM18_MAX_RUN	((1UL<<ILM18_BITS_RUN)-1)
#define ILM18_MAX_FLOW	((1UL<<ILM18_BITS_FLOW)-1)
#define ILM18_MAX_INST	((1UL<<ILM18_BITS_INST)-1)

#define SRA_BITS_ORG	4
#define SRA_BITS_ACC	24
#define SRA_BITS_SPOT	36
#define SRA_MAX_ORG	((1UL<<SRA_BITS_ORG)-1)
#define SRA_MAX_ACC	((1UL<<SRA_BITS_ACC)-1)
#define SRA_MAX_SPOT	((1UL<<SRA_BITS_SPOT)-1)

enum ilm { ilmInst, ilm18Inst, ilm18Flowcell };
static char *inst[ILM_MAX_INST+1];
static char *inst18[ILM18_MAX_INST+1];
static char *flowcell18[ILM18_MAX_FLOW+1];
//static volatile long instID;
//static volatile long inst18ID;
//static volatile long flowcell18ID;

static char *getIlluminaStr(enum ilm type, int i){
  char **id_array;
  switch(type){
  case ilmInst:		id_array = inst; 	break;
  case ilm18Inst:	id_array = inst18; 	break;
  case ilm18Flowcell:	id_array = flowcell18; 	break;
  }
  return id_array[i];
}
static int getIlluminaFieldID(enum ilm type, char *begin, char *end){
  char **id_array = NULL;
  uint64_t max = 0;
  switch(type){
  case ilmInst:		id_array = inst; 	max = ILM_MAX_INST; break;
  case ilm18Inst:	id_array = inst18; 	max = ILM18_MAX_INST; break;
  case ilm18Flowcell:	id_array = flowcell18; 	max = ILM18_MAX_FLOW; break;
  }
  long i;
  for(i=0; id_array[i] && i<max; i++) {
    if(0 == memcmp(begin, id_array[i], end-begin)){
      return i;
    }
  }
  // new flowcell. add it to flowcell[flocellID]
  if (i == max) {
    fprintf(stderr, "(E) the number of illumina instrument/flowcell IDs exceeds limit %ld\n", max);
    exit(-1);
  }
  char * const name = md_malloc(end-begin+1, mem_getInstID, 0);
  memcpy(name, begin, end-begin);
  name[end-begin] = 0;

  while(1) {
    if (__sync_bool_compare_and_swap(&id_array[i],NULL,name)) {
      return i;
    }
    // another thread added a new ID. search the array again
    for(i=0; id_array[i] && i<max; i++) {
      if(0 == memcmp(begin, id_array[i], end-begin)){
        return i;
      }
    }
    if (i == max) {
      fprintf(stderr, "(E) the number of illumina instrument/flowcell IDs exceeds limit %ld\n", max);
      exit(-1);
    }
    // id_array[i] was NULL
  }
}

static void hashv2name_illumina(uint64_t v, char *qn, int l_qname) {
  // 20FUKAAXX100202:3:23:20460:39157
  // | instrument id | lane no. | tile no. | x       | y       | 
  // |       8 bits  | 5 bits   | 14 bits  | 17 bits | 20 bits |
  uint64_t y = v & ILM_MAX_Y;
  uint64_t x = (v & ((uint64_t)ILM_MAX_X<<ILM_BITS_Y))>>ILM_BITS_Y;
  uint64_t tile = (v & ((uint64_t)ILM_MAX_TILE<<(ILM_BITS_X+ILM_BITS_Y)))>>(ILM_BITS_X+ILM_BITS_Y);
  uint64_t lane = (v & ((uint64_t)ILM_MAX_LANE<<(ILM_BITS_TILE+ILM_BITS_X+ILM_BITS_Y)))>>
					(ILM_BITS_TILE+ILM_BITS_X+ILM_BITS_Y);
  uint64_t inst = (v & ((uint64_t)ILM_MAX_INST<<(ILM_BITS_LANE+ILM_BITS_TILE+ILM_BITS_X+ILM_BITS_Y)))>>
					(ILM_BITS_LANE+ILM_BITS_TILE+ILM_BITS_X+ILM_BITS_Y);
  char * instStr = getIlluminaStr(ilmInst, inst);
  int len = strlen(instStr);
  memcpy(qn, instStr, len); 
  qn[len] = ':';
  // 20FUKAAXX100202:
  sprintf(&qn[len+1], "%d", (int)lane+1);
  len = strlen(qn);
  qn[len] = ':';
  // 20FUKAAXX100202:3:
  sprintf(&qn[len+1], "%d", (int)tile);
  len = strlen(qn);
  qn[len] = ':';
  // 20FUKAAXX100202:3:23:
  sprintf(&qn[len+1], "%d", (int)x);
  len = strlen(qn);
  qn[len] = ':';
  // 20FUKAAXX100202:3:23:20460:
  sprintf(&qn[len+1], "%d", (int)y);
  // 20FUKAAXX100202:3:23:20460:39157
  len = strlen(qn);
  if (len + 1 > l_qname) {
    fprintf(stderr, "(E) hashv2name_illumina: name len %d > buffer len %d\n", len+1, l_qname);
    exit(-1);
  }
}

static uint64_t hashv_illumina(char *qn, int l_qname){
  uint64_t c = 0;
  long invalid = 0;
  char * endp;
  char *s = index(qn, ':');
  int const instID = getIlluminaFieldID(ilmInst, qn, s);
  if (instID != -1) {
    uint64_t vl = strtol(s+1, &endp, 10);
    if ((s+1 < endp && ':' == *endp) && vl-1 <= ILM_MAX_LANE){ // v is lane
      char * endp2;
      uint64_t vn = strtol(endp+1, &endp2, 10);
      if ((endp+1 < endp2 && ':' == *endp2) && vn <= ILM_MAX_TILE){ // v is number within lane
        char * endp3;
        uint64_t vx = strtol(endp2+1, &endp3, 10);
        if ((endp2+1 < endp3 && ':' == *endp3) && vx <= ILM_MAX_X){ // v is x
          char * endp4;
          uint64_t vy = strtol(endp3+1, &endp4, 10);
          if (endp3+1 < endp4 && vy <= ILM_MAX_Y){ // v is y
	    // | instrument id | lane no. | tile no. | x       | y       | 
	    // |       8 bits  | 5 bits   | 14 bits  | 17 bits | 20 bits |
  	    c = ((uint64_t)instID)<<(ILM_BITS_LANE+ILM_BITS_TILE+ILM_BITS_X+ILM_BITS_Y)
	    		| (vl/*1..8*/-1)<<(ILM_BITS_TILE+ILM_BITS_X+ILM_BITS_Y) 
	    		| vn<<(ILM_BITS_X+ILM_BITS_Y) 
			| vx<<ILM_BITS_Y 
			| vy;
  	  } else {
	    invalid = 5; // y
	  }
        } else {
	  invalid = 4;	// x
	}
      } else {
        invalid = 3;	// tile
      }
    } else {
      invalid = 2;	// lane
    }
  } else {
    invalid = 1;	// instrment
  }
  if (invalid != 0) {
    switch(invalid) {
    case 1: fprintf(stderr, "(E) too many instrument IDs %.*s\n", l_qname, qn);	break;
    case 2: fprintf(stderr, "(E) lane ID is not numeric or >= max_lane %.*s\n", l_qname, qn);	break;
    case 3: fprintf(stderr, "(E) tile ID is not numeric or >= max_tile %.*s\n", l_qname, qn);	break;
    case 4: fprintf(stderr, "(E) x is not numeric or >= max_x %.*s\n", l_qname, qn);	break;
    case 5: fprintf(stderr, "(E) y is not numeric or >= may_y %.*s\n", l_qname, qn);	break;
    }
    exit(-1);
  }
  return c;
}

static void hashv2name_illumina18(uint64_t v, char *qn, int l_qname) {
  // | instrument id | run no. | flowcell | lane no. | tile no. | x       | y       | 
  // |       2 bits  | 8 bits  | 2 bits   | 5 bits   | 14 bits  | 17 bits | 20 bits |
  uint64_t y = v & ILM18_MAX_Y;
  uint64_t x = (v & ((uint64_t)ILM18_MAX_X<<ILM_BITS_Y))>>ILM_BITS_Y;
  uint64_t tile = (v & ((uint64_t)ILM18_MAX_TILE<<(ILM18_BITS_X+ILM18_BITS_Y)))>>(ILM18_BITS_X+ILM18_BITS_Y);
  uint64_t lane = (v & ((uint64_t)ILM18_MAX_LANE<<(ILM18_BITS_TILE+ILM18_BITS_X+ILM18_BITS_Y)))>>
					(ILM18_BITS_TILE+ILM18_BITS_X+ILM18_BITS_Y);
  uint64_t flow = (v & ((uint64_t)ILM18_MAX_FLOW<<(ILM18_BITS_LANE+ILM18_BITS_TILE+ILM18_BITS_X+ILM18_BITS_Y)))>>
					(ILM18_BITS_LANE+ILM18_BITS_TILE+ILM18_BITS_X+ILM18_BITS_Y);
  uint64_t run = (v & ((uint64_t)ILM18_MAX_RUN<<  (ILM18_BITS_FLOW+ILM18_BITS_LANE+ILM18_BITS_TILE+ILM18_BITS_X+ILM18_BITS_Y)))>>
					(ILM18_BITS_FLOW+ILM18_BITS_LANE+ILM18_BITS_TILE+ILM18_BITS_X+ILM18_BITS_Y);
  uint64_t inst = (v & ((uint64_t)ILM18_MAX_INST<<(ILM18_BITS_RUN+ILM18_BITS_FLOW+ILM18_BITS_LANE+ILM18_BITS_TILE+ILM18_BITS_X+ILM18_BITS_Y)))>>
					(ILM18_BITS_RUN+ILM18_BITS_FLOW+ILM18_BITS_LANE+ILM18_BITS_TILE+ILM18_BITS_X+ILM18_BITS_Y);
  char * instStr = getIlluminaStr(ilm18Inst, inst);
  int len = strlen(instStr);
  memcpy(qn, instStr, len); 
  qn[len] = ':';
  // HWI-D00402:
  sprintf(&qn[len+1], "%d", (int)run);
  len = strlen(qn); 
  qn[len] = ':';
  // HWI-D00402:119:
  char * flowStr = getIlluminaStr(ilm18Flowcell, flow);
  int flowStrLen = strlen(flowStr);
  memcpy(&qn[len+1], flowStr, flowStrLen);
  len += (1+flowStrLen);
  qn[len] = ':';
  // HWI-D00402:119:C6BUCANXX:
  sprintf(&qn[len+1+flowStrLen+1], "%d", (int)lane+1);
  len = strlen(qn);
  qn[len] = ':';
  // HWI-D00402:119:C6BUCANXX:1:
  sprintf(&qn[len+1], "%d", (int)tile);
  len = strlen(qn);
  qn[len] = ':';
  // HWI-D00402:119:C6BUCANXX:1:1101:
  sprintf(&qn[len+1], "%d", (int)x);
  len = strlen(qn);
  qn[len] = ':';
  // HWI-D00402:119:C6BUCANXX:1:1101:1418:
  sprintf(&qn[len+1], "%d", (int)y);
  // HWI-D00402:119:C6BUCANXX:1:1101:1418:1888
  len = strlen(qn);
  if (len + 1 > l_qname) {
    fprintf(stderr, "(E) hashv2name_illumina18: name len %d > buffer len %d\n", len+1, l_qname);
    exit(-1);
  }
}

static uint64_t hashv_illumina18(char *qn, int l_qname){
  uint64_t c = 0;
  int invalid = 0;
  char * endp;
  char *s = index(qn, ':');
  int const instID = getIlluminaFieldID(ilm18Inst, qn, s);
  if (instID != -1) {
    uint64_t vr = strtol(s+1, &endp, 10);
    if ((s+1 < endp && ':' == *endp) && vr <= ILM18_MAX_RUN){ // v is run id
      char * endp2;
      endp2 = index(endp+1, ':');
      if (endp+1 < endp2 && ':' == *endp2){ // v is flowcell id
  	int const flowcellID = getIlluminaFieldID(ilm18Flowcell, endp+1, endp2);
        if (flowcellID != -1) {
          char * endp3;
          uint64_t vl = strtol(endp2+1, &endp3, 10);
          if ((endp2+1 < endp3 && ':' == *endp3) && vl-1 <= ILM18_MAX_LANE){ // v is lane 
            char * endp4;
            uint64_t vn = strtol(endp3+1, &endp4, 10);
            if (endp3+1 < endp4 && vn <= ILM18_MAX_TILE){ // v is tile 
              char * endp5;
              uint64_t vx = strtol(endp4+1, &endp5, 10);
              if (endp4+1 < endp5 && vx <= ILM18_MAX_X){ // v is x 
                char * endp6;
                uint64_t vy = strtol(endp5+1, &endp6, 10);
                if (endp5+1 < endp6 && vy <= ILM18_MAX_Y){ // v is y 
    	          // | instrument id | run no. | flowcell | lane no. | tile no. | x       | y       | 
    	          // |       2 bits  | 8 bits  | 2 bits   | 5 bits   | 14 bits  | 17 bits | 20 bits |
      	          c = ((uint64_t)instID)<<(ILM18_BITS_RUN+ILM18_BITS_FLOW+ILM18_BITS_LANE+ILM18_BITS_TILE+ILM18_BITS_X+ILM18_BITS_Y)
    	    		| vr<<(ILM18_BITS_FLOW+ILM18_BITS_LANE+ILM18_BITS_TILE+ILM18_BITS_X+ILM18_BITS_Y) 
    	    		| ((uint64_t)flowcellID)<<(ILM18_BITS_LANE+ILM18_BITS_TILE+ILM18_BITS_X+ILM18_BITS_Y) 
    	    		| (vl/*1..8*/-1)<<(ILM18_BITS_TILE+ILM18_BITS_X+ILM18_BITS_Y) 
    	    		| vn<<(ILM18_BITS_X+ILM18_BITS_Y) 
    			| vx<<ILM18_BITS_Y 
    			| vy;
    	        } else {
		  invalid = 8;	// y
		}
	      } else {
		invalid = 7;	// x
	      }
    	    } else {
	      invalid = 6;	// tile
	    }
    	  } else {
	    invalid = 5;	// lane
	  }
    	} else {
	  invalid = 4;		// flow cell
	}
      } else {
	invalid = 3;		// flow cell 2
      }
    } else {
      invalid = 2;		// run
    }
  } else {
    invalid = 1;		// instrument
  }
  if (invalid != 0) {
    switch(invalid) {
    case 1: fprintf(stderr, "(E) too many instrument IDs %.*s\n", l_qname, qn);	break;
    case 2: fprintf(stderr, "(E) run ID is not numeric or >= max_run %.*s\n", l_qname, qn);	break;
    case 3: fprintf(stderr, "(E) flowcell ID followed by : not found %.*s\n", l_qname, qn);	break;
    case 4: fprintf(stderr, "(E) too many flowcell IDs %.*s\n", l_qname, qn);	break;
    case 5: fprintf(stderr, "(E) lane ID is not numeric or >= max_lane %.*s\n", l_qname, qn);	break;
    case 6: fprintf(stderr, "(E) tile ID is not numeric or >= max_tile %.*s\n", l_qname, qn);	break;
    case 7: fprintf(stderr, "(E) x is not numeric or >= max_x %.*s\n", l_qname, qn);	break;
    case 8: fprintf(stderr, "(E) y is not numeric or >= may_y %.*s\n", l_qname, qn);	break;
    }
    exit(-1);
  }
  return c;
}

static void hashv2name_SRR(uint64_t v, char *qn, int l_qname) {
  // | org    | acc no. | spot no. |
  // | 4 bits | 20 bits | 40 bits  |
  uint64_t spot = (v & SRA_MAX_SPOT);
  uint64_t acc  = (v & ((uint64_t)SRA_MAX_ACC<<SRA_BITS_SPOT))>>SRA_BITS_SPOT;
  uint64_t org  = (v & ((uint64_t)SRA_MAX_ORG<<(SRA_BITS_ACC+SRA_BITS_SPOT)))>>(SRA_BITS_ACC+SRA_BITS_SPOT);
  qn[0] = (org==0?'S':(org==1?'E':'D'));
  qn[1] = 'R';
  qn[2] = 'R';
  sprintf(&qn[3], "%06d", (int)acc);
  qn[9] = '.';
  sprintf(&qn[10], "%ld", spot);
}

static uint64_t hashv_SRR(char *qn, int l_qname){
    char * endp;
    int64_t const spot = strtoll(qn+10 /* skip xRR012345. */, &endp, 10);
    if (errno == ERANGE) {
      fprintf(stderr, "(E) ** too large spot number: %s\n", qn);
      exit(-1);
    }
    if (spot > SRA_MAX_SPOT) {
      fprintf(stderr, "(E) ** spot number %ld is too large (> %ld)\n", spot, SRA_MAX_SPOT);
      exit(-1);
    }
    int64_t const acc = strtoll(qn+3 /* skip xRR */, &endp, 10);
    if (errno == ERANGE) {
      fprintf(stderr, "(E) ** too large accession number: %s\n", qn);
      exit(-1);
    }
    if (acc > SRA_MAX_ACC /* xRR followed by a 6-digit number */) {
      fprintf(stderr, "(E) ** accession number %ld is too large (>%ld)\n", acc, SRA_MAX_ACC);
      exit(-1);
    }
    uint64_t const hashv = (qn[0]=='S'?0ULL:(qn[0]=='E'?1ULL:2ULL))<<(SRA_BITS_ACC+SRA_BITS_SPOT) 
    		| acc<<SRA_BITS_SPOT
		| spot;

#if 0 // DEBUG
{
char str[256];
long len;
get_qname(hashv, str, sizeof(str), &len);
if(0 != memcmp(str, qn, l_qname)) {
fprintf(stderr, "(D) original %.*s\n(D) restored %.*s\n", l_qname, qn, l_qname, str);
fflush(stderr);
}
}
#endif

    return hashv;
}

static uint64_t hashv_general(char *qn, int l_qname){
  int i;
  union {
    uint64_t ui64;
    uint8_t ui8[8];
  } h;
  uint64_t *p = (uint64_t*)qn;
  h.ui64 = 0;
  for(i=0; i<l_qname; i+=sizeof(uint64_t)){
    h.ui64 ^= *p;
    p++;
  }
  if (i > l_qname){
    i -= sizeof(uint64_t);
    int j=0;
    for(; i<l_qname; i++,j++){
      h.ui8[7-j] ^= qn[i];
    }
  }
  return h.ui64;
}

typedef struct qnameid {
  struct qnameid *next;
  uint64_t id;
  uint8_t l_qname;
  uint8_t qname[];
} qnameid_t;

typedef enum { nc_uninitialized, nc_illumina, nc_illumina18, nc_SRR, nc_unknown } naming_t;
static naming_t naming = nc_uninitialized;

//#define TRACE_QNAME_EQUALS
static int qname_equals(qnameid_t *t, uint8_t *qn2, uint8_t l_qn2) {
#if defined(TRACE_QNAME_EQUALS)
const char *s1 = "20FUKAAXX100202:7:67:11464:165232";
const char *s2 = "20FUKAAXX100202:7:62:19449:164000";
const int equal1 = (0==memcmp(s1,qn1,strlen(s1)));
const int equal2 = (0==memcmp(s2,qn1,strlen(s2)));
const int equal3 = (0==memcmp(s1,qn2,strlen(s1)));
const int equal4 = (0==memcmp(s2,qn2,strlen(s2)));
const int trace = (equal1 || equal2 || equal3 || equal4);
{{{
#define TRACE_BEGIN() 		if(trace)fprintf(stderr, "(equals) begin %s <=> %s\n", qn1, qn2)
#define TRACE_CMP_LEN(l1,l2)	if(trace)fprintf(stderr, "(equals) len %d cmp %d\n", l1, l2)
#define TRACE_CMP_INT64(i,l1,l2)	if(trace)fprintf(stderr, "(equals) i %d 0x%lx cmp 0x%lx\n", i,l1, l2)
#define TRACE_CMP_CHAR(i,l1,l2)	if(trace)fprintf(stderr, "(equals) %d '%c' cmp '%c'\n", i,l1, l2)
#define TRACE_END() 		if(trace)fprintf(stderr, "(equals) end %s == %s\n", qn1, qn2)
}}}
#else
{{{
#define TRACE_BEGIN()
#define TRACE_CMP_LEN(l1,l2)
#define TRACE_CMP_INT64(i,l1,l2)
#define TRACE_CMP_CHAR(i,l1,l2)
#define TRACE_END()
}}}
#endif
if (naming == nc_SRR) {
  uint64_t const *qn1 = (uint64_t*)&t->l_qname;
  return (*qn1 == (uint64_t)qn2 ? 1 : 0);
} else {
  TRACE_BEGIN();
  uint8_t * const qn1 = t->qname;
  uint8_t const l_qn1 = t->l_qname;
  if (l_qn1 != l_qn2) {
    TRACE_CMP_LEN(l_qn1, l_qn2);
    return 0;
  } else {
#define vec_type uint64_t
    int const vec_len = sizeof(vec_type);
    int i = l_qn1 - vec_len;
    int remaining = l_qn1;
    vec_type *lp1 = (vec_type*)&qn1[i];
    vec_type *lp2 = (vec_type*)&qn2[i];
    for (; remaining>=vec_len; remaining-=vec_len,lp1--,lp2--){
      TRACE_CMP_INT64((int)((uint8_t*)lp1-qn1),*lp1, *lp2);
      if (*lp1 != *lp2) {
	return 0;
      }
    }
    if (remaining > 0) {
      i = remaining-1; /* 0 .. remaining-1 */
      for (; i>=0; i--){
        TRACE_CMP_CHAR(i,qn1[i], qn2[i]);
        if (qn1[i] != qn2[i]) {
	  return 0;
	}
      }
    }
#undef vec_type
    /*
    for (i=l_qn1-1; i>=0; i--){
      if (qn1[i] != qn2[i])
	    break;
    }
    */
  }
  TRACE_END();
  return 1;
}
}

static char *bamptr_get_qname(bamptr_t *bamp, char *qn, int qname_l) {
  switch(naming){
  case nc_SRR:
    hashv2name_SRR(bamptr_get_qnameid(bamp), qn, qname_l);
    break;
  case nc_illumina:
    hashv2name_illumina(bamptr_get_qnameid(bamp), qn, qname_l);
    break;
  case nc_illumina18:
    hashv2name_illumina18(bamptr_get_qnameid(bamp), qn, qname_l);
    break;
  default:
    fprintf(stderr, "(W) not implemented yet\n");
    qn[0] = 0;
    break;
  }
  return qn;
}

#define QNAME_SPACE_SIZE (1024*1024-sizeof(struct qname_space *)-sizeof(char*))
typedef struct qname_space {
  struct qname_space *next;
  char *curr;
  char space[QNAME_SPACE_SIZE];
} qname_space_t;

#define QNAME_RESERVED_ID_NUM 1000
typedef struct qname_reserved_id {
  int i;
  uint32_t first_reserved_id;
} qname_reserved_id_t;

static int qname_space_head_n = 0;
static qname_space_t **qname_space_head = NULL;
static qname_reserved_id_t *qname_reserved_id = NULL;
static qnameid_t * volatile *qname_hash_bucket = NULL;
static uint64_t g_qnameid __attribute__((aligned(128))) = 0;
//static uint64_t n_qnameid_equals __attribute__((aligned(128))) = 0;
//static uint64_t n_qnameid_search __attribute__((aligned(128))) = 0;

static uint64_t g_alignments_without_mate_info = 0; // the number alignments whose flag shows paired & map & mate map but mate was not found by a simple cache mechanism

static void malloc_qnameid_space(int n_thread) {
  qname_space_head = md_malloc(sizeof(qname_space_head[0])*n_thread, mem_bamGetQnameId1, 1);
  qname_space_head_n = n_thread;

  qname_reserved_id = md_malloc(sizeof(qname_reserved_id[0])*n_thread, mem_bamGetQnameId1, 1);
  int i;
  for (i=0; i<n_thread; i++){
    qname_reserved_id[i].first_reserved_id = QNAME_RESERVED_ID_NUM*i;
  }
  g_qnameid = qname_reserved_id[n_thread-1].first_reserved_id + QNAME_RESERVED_ID_NUM;
}

static void free_qnameid_space() {
  qname_space_t *qs, *next;
  int i;
  for (i=0; i<qname_space_head_n; i++){
  for (qs=qname_space_head[i]; qs; qs=next){
    next = qs->next;
	md_free(qs, sizeof(*qs), mem_bamGetQnameId2);
  }
  }
  md_free((void*)qname_space_head, sizeof(qname_space_head[0])*qname_space_head_n, mem_bamGetQnameId1);
  md_free((void*)qname_reserved_id, sizeof(qname_reserved_id[0])*qname_space_head_n, mem_bamGetQnameId1);
  md_free((void*)qname_hash_bucket, sizeof(*qname_hash_bucket)*QNAME_HASH_SIZE, mem_bamGetQnameId1);
}


static void initialize_qname_convention(uint8_t * qn, int l_qname) {
   if(is_illumina((char*)qn)){
     fprintf(stderr, "(I) read naming convention: Illumina\n");
     naming = nc_illumina;
   } else if(is_illumina18((char*)qn)){
     fprintf(stderr, "(I) read naming convention: Illumina 1.8\n");
     naming = nc_illumina18;
   } else if (is_SRR((char*)qn)) {
     fprintf(stderr, "(I) read naming convention: SRR\n");
     naming = nc_SRR;
   } else {
     fprintf(stderr, "(I) read naming convention: Unsupported (the performance could be not good) (%.*s)\n", l_qname, qn);
     naming = nc_unknown;
   }
}

static inline void get_qname_SRR(uint64_t qnameid, char *str, long l_qname, long *len) {
  uint64_t const spot = (qnameid & ((1UL<<SRA_BITS_SPOT) - 1)); 
  uint64_t const acc =  (qnameid & (((1UL<<SRA_BITS_ACC) - 1)<<SRA_BITS_SPOT))>>SRA_BITS_SPOT; 
  uint64_t const org =  (qnameid & (((1UL<<SRA_BITS_ORG) - 1)<<(SRA_BITS_ACC+SRA_BITS_SPOT)))>>(SRA_BITS_ACC+SRA_BITS_SPOT); 
  //               12345678901234567890		20 digits
  //  ULONG_MAX    18446744073709551615 UL
  int n = 3;
  str[0] = (org==0 ? 'S' : (org==1 ? 'E' : 'D'));
  str[1] = 'R';
  str[2] = 'R';
  n += snprintf(&str[n], l_qname-n, "%06lu", acc);
  str[n++] = '.';
  n += snprintf(&str[n], l_qname-n, "%lu", spot);
  *len = n;
  if (n + 1/*\0*/ > l_qname) {
    fprintf(stderr, "(E) length of string buffer %ld must be >=%d\n", l_qname, n + 1);
    exit(-1);
  }
}

static inline void get_qname_illumina(uint64_t qnameid, char *str, long l_qname, long *len) {
  uint64_t const y = (qnameid & ((1UL<<ILM_BITS_Y) - 1)); 
  uint64_t const x = (qnameid & (((1UL<<ILM_BITS_X) - 1)<<ILM_BITS_Y))>>ILM_BITS_Y; 
  uint64_t const tile = (qnameid & (((1UL<<ILM_BITS_TILE) - 1)<<(ILM_BITS_X+ILM_BITS_Y)))>>(ILM_BITS_X+ILM_BITS_Y); 
  uint64_t const lane = (qnameid & (((1UL<<ILM_BITS_LANE) - 1)<<(ILM_BITS_TILE+ILM_BITS_X+ILM_BITS_Y)))>>(ILM_BITS_TILE+ILM_BITS_X+ILM_BITS_Y); 
  uint64_t const ix = (qnameid & (((1UL<<ILM_BITS_INST) - 1)<<(ILM_BITS_LANE+ILM_BITS_TILE+ILM_BITS_X+ILM_BITS_Y)))>>(ILM_BITS_LANE+ILM_BITS_TILE+ILM_BITS_X+ILM_BITS_Y); 
  //               12345678901234567890		20 digits
  //  ULONG_MAX    18446744073709551615 UL
  int n = strlen(inst[ix]);
  memcpy(str, inst[ix], n);
  str[n++] = ':';
  n += snprintf(&str[n], l_qname-n, "%lu", lane+1);
  str[n++] = ':';
  n += snprintf(&str[n], l_qname-n, "%lu", tile);
  str[n++] = ':';
  n += snprintf(&str[n], l_qname-n, "%lu", x);
  str[n++] = ':';
  n += snprintf(&str[n], l_qname-n, "%lu", y);
  *len = n;
  if (n + 1/*\0*/ > l_qname) {
    fprintf(stderr, "(E) length of string buffer %ld must be >=%d\n", l_qname, n + 1);
    exit(-1);
  }
}

static inline void get_qname_illumina18(uint64_t qnameid, char *str, long l_qname, long *len) {
  uint64_t const y = (qnameid & ((1UL<<ILM18_BITS_Y) - 1)); 
  uint64_t const x = (qnameid & (((1UL<<ILM18_BITS_X) - 1)<<ILM18_BITS_Y))>>ILM18_BITS_Y; 
  uint64_t const tile = (qnameid & (((1UL<<ILM18_BITS_TILE) - 1)<<(ILM18_BITS_X+ILM18_BITS_Y)))
  								>>(ILM18_BITS_X+ILM18_BITS_Y); 
  uint64_t const lane = (qnameid & (((1UL<<ILM18_BITS_LANE) - 1)<<(ILM18_BITS_TILE+ILM18_BITS_X+ILM18_BITS_Y)))
  								>>(ILM18_BITS_TILE+ILM18_BITS_X+ILM18_BITS_Y); 
  uint64_t const   fx = (qnameid & (((1UL<<ILM18_BITS_FLOW) - 1)<<(ILM18_BITS_LANE+ILM18_BITS_TILE+ILM18_BITS_X+ILM18_BITS_Y)))
  								>>(ILM18_BITS_LANE+ILM18_BITS_TILE+ILM18_BITS_X+ILM18_BITS_Y); 
  uint64_t const   run = (qnameid & (((1UL<<ILM18_BITS_RUN) - 1)<<(ILM18_BITS_FLOW+ILM18_BITS_LANE+ILM18_BITS_TILE+ILM18_BITS_X+ILM18_BITS_Y)))
  								>>(ILM18_BITS_FLOW+ILM18_BITS_LANE+ILM18_BITS_TILE+ILM18_BITS_X+ILM18_BITS_Y); 
  uint64_t const   ix = (qnameid & (((1UL<<ILM18_BITS_INST) - 1)<<(ILM18_BITS_RUN+ILM18_BITS_FLOW+ILM18_BITS_LANE+ILM18_BITS_TILE+ILM18_BITS_X+ILM18_BITS_Y)))
  								>>(ILM18_BITS_RUN+ILM18_BITS_FLOW+ILM18_BITS_LANE+ILM18_BITS_TILE+ILM18_BITS_X+ILM18_BITS_Y); 
  //               12345678901234567890		20 digits
  //  ULONG_MAX    18446744073709551615 UL
  int n = strlen(inst18[ix]);
  memcpy(str, inst18[ix], n);
  str[n++] = ':';
  n += snprintf(&str[n], l_qname-n, "%lu", run);
  str[n++] = ':';
  int const nf = strlen(flowcell18[fx]);
  memcpy(&str[n], flowcell18[fx], nf);
  n += nf;
  str[n++] = ':';
  n += snprintf(&str[n], l_qname-n, "%lu", lane+1);
  str[n++] = ':';
  n += snprintf(&str[n], l_qname-n, "%lu", tile);
  str[n++] = ':';
  n += snprintf(&str[n], l_qname-n, "%lu", x);
  str[n++] = ':';
  n += snprintf(&str[n], l_qname-n, "%lu", y);
  *len = n;
  if (n + 1/*\0*/ > l_qname) {
    fprintf(stderr, "(E) length of string buffer %ld must be >=%d\n", l_qname, n + 1);
    exit(-1);
  }
}

static void get_qname(uint64_t qnameid, char *qname, long l_qname, long *len) {
  switch(naming){
  case nc_SRR:
    get_qname_SRR(qnameid, qname, l_qname, len);
    break;
  case nc_illumina:
    get_qname_illumina(qnameid, qname, l_qname, len);
    break;
  case nc_illumina18:
    get_qname_illumina18(qnameid, qname, l_qname, len);
    break;
  default:
    //  TODO
    fprintf(stderr, "(E) not implemented get_qname(qnameid) for naming convention is general\n");
    break;
  }
}

static inline uint64_t bam_set_qnameid(bam1_t *b, int myid){
  uint8_t * qn = (uint8_t*)bam_get_qname(b);
  //const int l_qname = b->core.l_qname;
  const int l_qname = b->core.l_qname /* including NULL */ - 1;
  uint64_t v;
  if (naming == nc_uninitialized) {
    initialize_qname_convention(qn, l_qname);
  }

  switch(naming){
  case nc_SRR:
  case nc_illumina:
  case nc_illumina18:
    if (qname_reserved_id[myid].i == QNAME_RESERVED_ID_NUM) {
      qname_reserved_id[myid].first_reserved_id = __atomic_fetch_add(&g_qnameid, QNAME_RESERVED_ID_NUM, __ATOMIC_RELAXED);
      qname_reserved_id[myid].i = 0;
    }
    if (qname_reserved_id[myid].first_reserved_id + qname_reserved_id[myid].i == UINT32_MAX){
      fprintf(stderr, "(E) %u record limit is reached\n", UINT32_MAX);
      exit(-1);
    }
    qname_reserved_id[myid].i++;
    break;
  default:
    // qname_reserved_id[myid].* are updated later
    break;
  }

  switch(naming){
  case nc_SRR:
    v = hashv_SRR((char*)qn, l_qname);
    return v;
    break;
  case nc_illumina:
    v = hashv_illumina((char*)qn, l_qname);
    return v;
    break;
  case nc_illumina18:
    v = hashv_illumina18((char*)qn, l_qname);
    return v;
    break;
  default:
    v = hashv_general((char*)qn, l_qname);
    break;
  }
  const int c = v & QNAME_HASH_MASK;

  PREFETCH_BAMCOPY_R_LOCALITY0((void*)&qname_hash_bucket[c]);

  qnameid_t *t;

/*
  TODO: Performance
  Access to qname_hash_bucket[c], t->next, and t->l_qname frequently causes d-cache misses.
  ~50% of the cpu time could be spent for the access for bam copier threads.
*/
  // 1st search
  //int path=-1;
  qnameid_t * list = qname_hash_bucket[c];
  if (naming == nc_SRR) {
    qn = (uint8_t *)v;
  }
  //n_qnameid_search++;
  for(t=list; t; t=t->next){
    //n_qnameid_equals++;
    if(qname_equals(t, qn, l_qname)){
      break;
    }
  }

    if (t==NULL){
      size_t const sz = sizeof(*t) + ( naming == nc_SRR ? -sizeof(t->l_qname)+sizeof(int64_t) : l_qname );
      if (qname_space_head[myid] && qname_space_head[myid]->curr + sz <= qname_space_head[myid]->space + QNAME_SPACE_SIZE) {
      } else {
	  qname_space_t * const qs =md_malloc(sizeof(*qname_space_head[myid]), mem_bamGetQnameId2, 0);
	  qs->next = qname_space_head[myid];
	  qs->curr = qs->space;
	  qname_space_head[myid] = qs;
      }
      t = (qnameid_t*)qname_space_head[myid]->curr;
      qname_space_head[myid]->curr += sz;

      if (qname_reserved_id[myid].i == QNAME_RESERVED_ID_NUM) {
	qname_reserved_id[myid].first_reserved_id = __atomic_fetch_add(&g_qnameid, QNAME_RESERVED_ID_NUM, __ATOMIC_RELAXED);
	qname_reserved_id[myid].i = 0;
	//fprintf(stderr, "(I) qnameid %u\n", qname_reserved_id[myid].first_reserved_id);
      }
      if (qname_reserved_id[myid].first_reserved_id + qname_reserved_id[myid].i == UINT32_MAX){
	fprintf(stderr, "(E) %u record limit is reached\n", UINT32_MAX);
	exit(-1);
      }
      t->id = qname_reserved_id[myid].first_reserved_id + qname_reserved_id[myid].i++;
      if (naming == nc_SRR) {
        uint64_t * const p = (uint64_t*)&t->l_qname;
	*p = v;
      } else {
        t->l_qname = l_qname;
        memcpy(t->qname, qn, l_qname);
      }
      __atomic_thread_fence(__ATOMIC_RELEASE);
      //qname_hash_bucket[c] = t;

      while(1) {
        t->next = list;
	if (__sync_bool_compare_and_swap(&qname_hash_bucket[c],list,t)) {
	  //path = 2;
	  break;
	}
	list = qname_hash_bucket[c];

	// 2nd search
  	//n_qnameid_search++;
	qnameid_t* t2;
  	for(t2=list; t2; t2=t2->next){
	  //n_qnameid_equals++;
	  if(qname_equals(t2, qn, l_qname)){
	    break;
    	  }
	}
	if(t2) {
	  // an entry for the current qname was added after the 1st search
	  // t is no longer needed to be added
	  //TODO reuse t if garbage t consume a lot of memory spaces
	  t = t2;
	  //path = 3;
	  break; // the entry added by another thread
	}
      }
    }else{
      //path=1;
//fprintf(stderr, "existing qnameid %d %.*s hash %d\n", t->id, l_qname, t->qname, c);
    }
/*
const char *s1 = "20GAVAAXX100126:1:21:8747:157961";
const char *s2 = "20FUKAAXX100202:8:46:5163:157961";
const int equal1 = (0==memcmp(s1,t->qname,strlen(s1)));
const int equal2 = (0==memcmp(s2,t->qname,strlen(s2)));
if (equal1 || equal2){
fprintf(stderr, "path %d s %s t %s qnameid %d myid %d\n", path, bam_get_qname(b), t->qname, t->id, myid);
}
*/
    return t->id;
}
#if 0
{{{
static inline void mt_bam_set_qnameid(bam1_t *b) {
    /* CRITICAL SECTION start */
    static int const unlock = 0;
    static volatile int mutex = 0;
    while(1){
      if(//SEGV __atomic_compare_exchange(&mutex,&unlock,&lock,0,__ATOMIC_ACQUIRE,__ATOMIC_ACQUIRE)
        __sync_bool_compare_and_swap(&mutex, 0, 1)
      ) break;
      IN_SPIN_WAIT_LOOP();
    }
    FINISH_SPIN_WAIT_LOOP();
    //if (list != qname_hash_bucket[c]) { // new nodes were added since the search
    bam_set_qnameid(b);
    __atomic_store(&mutex, &unlock, __ATOMIC_RELEASE);
    /* CRITICAL SECTION end */
}
}}}
#endif

static uint8_t get_libid(const char *lib) {
  int i;
  int libid = -1;
  const int l = strlen(lib);
  for(i=0; i<dict_rg2lib.n; i++) {
    if(0 == memcmp(lib, dict_rg2lib.lib_name[i], l)){
      libid = dict_rg2lib.lib_id[i];
      break;
    }
  }
  if (libid + 1 > UINT8_MAX) {
    fprintf(stderr, "(E) %d LIBs are supported, but %d-th LIB is referenced\n", UINT8_MAX, libid+1);
	exit(-1);
  }
  return libid + 1;
}

static inline uint8_t bam_get_libid(bam_hdr_t *h, bam1_t *b) {
  const char *rg = (char*)bam_aux_get(b, "RG"); // RG:20FUK.6
  int lib = -1;

  if (rg) {
    rg += 1; // skip first "Z"
    const int l = strlen(rg);
    int i;
    for(i=0; i<dict_rg2lib.n; i++) {
      if(0 == memcmp(rg, dict_rg2lib.rg_name[i], l)){
        lib = dict_rg2lib.lib_id[i];
        break;
      }
    }
  }
  if (lib + 1 > UINT8_MAX) {
    fprintf(stderr, "(E) %d LIBs are supported, but %d-th LIB is referenced\n", UINT8_MAX, lib+1);
	exit(-1);
  }
  return lib + 1;
}

// TODO: sizeof(bucket_t) can be halved by using bamptr_FILO. In that case, reversing is needed if the order of reads in the file is sensitive.
#if defined(FIFO)
typedef struct bamptr_FIFO {
  bamptr_t * top;
  bamptr_t * last;
} bamptr_FIFO_t;
struct bucket {
  bamptr_FIFO_t fifo[BUCKET_SIZE];
};
static inline void add_to_bucket(bucket_t *bucket, int index, bamptr_t *bamp){
  if (bucket->fifo[index].top == NULL){
    bucket->fifo[index].top = bamp;
  }
  bamp->clipped_next = NULL;
  if (bucket->fifo[index].last != NULL){
    bucket->fifo[index].last->clipped_next = bamp;
  }
  bucket->fifo[index].last = bamp;
}
static inline void add_to_bucketU(bucket_t *bucket, int index, bamptr_t *bamp){
  if (bucket->fifo[index].top == NULL){
    bucket->fifo[index].top = bamp;
  }
  bamp->unclipped_next = NULL;
  if (bucket->fifo[index].last != NULL){
    bucket->fifo[index].last->unclipped_next = bamp;
  }
  bucket->fifo[index].last = bamp;
}
inline bamptr_t *get_fifo_list_from_bucket(bucket_t *bucket, int idx2, int unclipped){
  return (bucket ?  bucket->fifo[idx2].top : NULL);
}
#else

#if defined(BAMP_ARRAY)

struct bam_space;

typedef struct bam_ctl {
  struct bam_space *head; // list of bam_space_t
  struct {	  	// bamid array
    int size;
    int curr;
    uint64_t *arr;	  // bamid array
  } bamid;
} bam_ctl_t;

#define BAMPTR_SPACE_SIZE 4096
typedef struct bamptr_space {
  struct bamptr_space *next;
  char *curr;
  char space[BAMPTR_SPACE_SIZE];
} bamptr_space_t;

typedef struct bamptr_space_ctl {
  struct bamptr_space *head; // list of bamptr_space_t
  bamptr_array_t *free[N_BAMPTR_SPACE_FREE_LIST];
} bamptr_space_ctl_t;

struct bucket {
  int min, max;
  bam_ctl_t bamctl;
  bamptr_space_ctl_t *bsctl;
  bamptr_array_t *ba[BUCKET_SIZE];
};
static inline int bucket_get_min(bucket_t *bu) {
  return bu->min;
}
static inline int bucket_get_max(bucket_t *bu) {
  return bu->max;
}
static inline bamptr_array_t **bucket_get_bamptr_array_top(bucket_t *bu) {
  return bu->ba;
}

typedef struct bam_free_list {
  struct bam_free_list *next;
} bam_free_list_t;

typedef struct bam_free_list2 {
  bam_free_list_t **bam_free_list1 __attribute__((aligned(128)));
  bam_free_list_t *bam_free_list2 __attribute__((aligned(128))); 
} bam_free_list2_t;

static void bam_free_list2_init(bam_free_list2_t *l2) {
  l2->bam_free_list1 = md_malloc(sizeof(l2->bam_free_list1[0])*n_sam_parser, mem_bamFreeList2Init, 1);
  l2->bam_free_list2 = NULL;
}

static inline bam1_t *md_bam_alloc(bam_free_list2_t *l2, bam_free_list_t ** my_fl) {
  //bam_free_list_t ** const my_fl = &l2->bam_free_list1[myid];
  bam_free_list_t * f = *my_fl;
  bam1_t *b = NULL;
  if (f) {
    b = (bam1_t*)f;
    *my_fl = f->next;
    PREFETCH_SAMPARSE_RW_LOCALITY3(f->next);
  } else {
    f = l2->bam_free_list2;
    if (f) {
      if (__sync_bool_compare_and_swap(&l2->bam_free_list2,f,NULL)) {
        *my_fl = f->next;
        b = (bam1_t*)f;
      }
    }
  }
//static long p[2]={0,0}, po[2]={0,0};
  if (NULL == b) {
    b = bam_init1();
//p[0]++;
  }
//else p[1]++;
//if (0==((p[0]+p[1])&0xfffff)) { fprintf(stderr, "alloc %ld new %ld reuse %ld new %ld\n", p[0], p[0]-po[0], p[1], p[1]-po[1]); po[0]=p[0]; po[1]=p[1];}
  return b;
}

static void md_bam_free(bam_free_list2_t *l2, bam_free_list_t *top, bam_free_list_t *bottom){
if (top) { 
  while(1){
    bam_free_list_t * const oldf = l2->bam_free_list2;
    bottom->next = oldf;
    if (__sync_bool_compare_and_swap(&l2->bam_free_list2,oldf,top)) {
   	  break;
    }
  }
}
}

static inline void bamptr_space_malloc(bucket_t *bu) {
  if(bu->bsctl == NULL){
	bu->bsctl = md_malloc(sizeof(bamptr_space_ctl_t), mem_bamptrSpaceMalloc, 1);
  }
  bamptr_space_t *p = md_malloc(sizeof(bamptr_space_t), mem_bamptrSpaceMalloc2, 0);
  //FIXME: if bu->bsctl->head != NULL && bu->bsctl->head->curr has a free space, put the free space to the free list
  p->next = bu->bsctl->head;
  p->curr = &p->space[0];
  bu->bsctl->head = p;
}
static void bamptr_space_free(bucket_t *bu){
    bamptr_space_t *bs, *next;
	for(bs=bu->bsctl->head; bs; bs=next){
	  next=bs->next; 
      md_free(bs, sizeof(bamptr_space_t), mem_bamptrSpaceMalloc2);
	}
    md_free(bu->bsctl, sizeof(bamptr_space_ctl_t), mem_bamptrSpaceMalloc);
}

static inline void copy_bamptr_array(bamptr_array_t* newba, bamptr_array_t* oldba, int oldsz, int unclipped) {
  // copy
  memcpy(newba->bp, oldba->bp, sizeof_bamptr(unclipped)*oldsz);

#if defined(MATE_CACHE)
  if (unclipped) {
    // update mate pointers
    int i;
    for (i=0; i<oldsz; i++) {
      bamptr_t * const old_bamp = get_bamptr_at(oldba, i, unclipped); 
      bamptr_t * const new_bamp = get_bamptr_at(newba, i, unclipped);
      bamptr_t * const mate = bamptr_get_mate(old_bamp);
      if (mate) {
        // FIXME - lock 'mate' (or bamptr_array_t including it) to update its mate
        bamptr_set_mate(mate, new_bamp);
      }
    }
  }
#endif
}

static inline void free_bamptr_array(bamptr_array_t* oldba, int oldsz, bamptr_space_ctl_t *bsctl) {
  // free the old bamptr array - link the old one to the free list
  oldba->free_next = bsctl->free[oldsz-1];
  bsctl->free[oldsz-1] = oldba;
}

static inline void add_to_bamptr_array(bucket_t * bu, int index, baminfo1_t *baminfo1, int unclipped DCL_ARG_QNAMEID_CACHE(p_cache)){
  bamptr_array_t *ba;
  bamptr_space_ctl_t * const bsctl = bu->bsctl;
  {
    bamptr_array_t* const oldba = bu->ba[index];
    const int oldsz = oldba ? oldba->size : 0;
    int newsz = oldsz + 1;
    if(oldba && newsz <= oldba->max){
      ba = oldba;
      ba->size = newsz;
    } else if(newsz <= N_BAMPTR_SPACE_FREE_LIST){
      ba = bsctl->free[newsz-1];
      if (ba){
	// unlink
	bsctl->free[newsz-1] = ba->free_next;
      } else {
        // no free node
        const size_t sz = sizeof(bamptr_array_t) +(newsz)*sizeof_bamptr(unclipped);
	bamptr_space_t * bs = bsctl->head;
        if(bs->curr + sz > bs->space + BAMPTR_SPACE_SIZE){
	  bamptr_space_malloc(bu);
	  bs = bsctl->head;
	}
	ba = (bamptr_array_t*)bs->curr;
	bs->curr += sz;
      }
      ba->max  = newsz;
      ba->size = newsz;
      if (oldsz > 0) {
        // add to an existing array 
	copy_bamptr_array(ba, oldba, oldsz, unclipped);
        free_bamptr_array(oldba, oldsz, bsctl);
      }
    } else if(oldsz == N_BAMPTR_SPACE_FREE_LIST){ // oldsz == N_BAMPTR_SPACE_FREE_LIST < newsz
      // a new array cannot be allocated from the the bamptr space
      const int allocsz = oldsz + 10;
      ba = md_malloc(sizeof(bamptr_array_t)+(allocsz)*sizeof_bamptr(unclipped), mem_addToBamptrArray, 0);
      ba->max = allocsz;
      ba->size = newsz;
      copy_bamptr_array(ba, oldba, oldsz, unclipped);
      free_bamptr_array(oldba, oldsz, bsctl);
   } else{ // N_BAMPTR_SPACE_FREE_LIST < oldsz < newsz
      int allocsz;
      if (oldsz < 100) {
        allocsz = oldsz + 10;
      } else if (oldsz < 1000) {
        allocsz = oldsz + 100;
      } else if (oldsz < 10000) {
        allocsz = oldsz + 1000;
      } else {
        allocsz = oldsz + 10000;
      }
      //ba = md_realloc(oldba, sizeof(bamptr_array_t)+(allocsz)*sizeof_bamptr(unclipped), mem_addToBamptrArray);
      ba = md_malloc(sizeof(bamptr_array_t)+(allocsz)*sizeof_bamptr(unclipped), mem_addToBamptrArray, 0);
      ba->max = allocsz;
      ba->size = newsz;
      copy_bamptr_array(ba, oldba, oldsz, unclipped);
      // oldba is not maintained in the free list
      md_free(oldba, sizeof(bamptr_array_t)+(oldsz)*sizeof_bamptr(unclipped), mem_addToBamptrArray);
   }
  }

  bamptr_init(get_bamptr_at(ba, ba->size-1, unclipped), baminfo1, unclipped ARG_QNAMEID_CACHE(p_cache));

  bu->ba[index] = ba;
  if (index < bu->min) {
    bu->min = index; 
  }
  if (bu->max < index) {
    bu->max = index; 
  }
}
static inline void add_to_bucket(bucket_t *bucket, int index, baminfo1_t *baminfo1){
  add_to_bamptr_array(bucket, index, baminfo1, 0/*clipped*/ ARG_QNAMEID_CACHE(NULL));
}

static inline void add_to_bucketU(bucket_t *bucket, int index, baminfo1_t *baminfo1 DCL_ARG_QNAMEID_CACHE(p_cache)){
  add_to_bamptr_array(bucket, index, baminfo1, 1/*unclipped*/ ARG_QNAMEID_CACHE(p_cache));
}
static inline bamptr_t *get_fifo_list_from_bucket(bucket_t *bu, int idx2, int unclipped){
  bamptr_t *bamptr = (bu ? (bu->ba[idx2] ? bu->ba[idx2]->bp : NULL) : NULL);
  return bamptr;
}


#else

#define BAMPTR_SPACE_SIZE 128
typedef struct bamptr_space {
  struct bamptr_space *next; // list of bamptr_space_t
  int i; // index of bp array
  bamptr_t bp[BAMPTR_SPACE_SIZE];
} bamptr_space_t;

static inline void bamptr_space_malloc(bucket_t *bu) {
	bamptr_space_t *p = md_malloc(sizeof(bamptr_space_t), mem_bamptrSpaceMalloc, 0);
	p->next = bu->bs;
	p->i = 0;
	bu->bs = p;
 }
static void bamptr_space_free(bucket_t *bu){
    bamptr_space_t *bs, *next;
	for(bs=bu->bs; bs; bs=next){
	  next=bs->next; 
      md_free(bs, sizeof(bamptr_space_t), mem_bamptrSpaceMalloc);
	}
}
typedef struct bamptr_LIFO {
  bamptr_t * last;
} bamptr_LIFO_t;
typedef struct bucket {
  bamptr_space_t *bs;
  bamptr_LIFO_t lifo[BUCKET_SIZE];
} bucket_t;
static inline void add_to_bucket(bucket_t *bucket, int index, bamptr_t *bamp){
  bamp->clipped_next = bucket->lifo[index].last;
  bucket->lifo[index].last = bamp;
}
static inline void add_to_bucketU(bucket_t *bucket, int index, bamptr_t *bamp, bamptr_t **p_cache){
  bamp->unclipped_next = bucket->lifo[index].last;
  bucket->lifo[index].last = bamp;
}
static inline bamptr_t *get_fifo_list_from_bucket(bucket_t *bucket, int idx2, int unclipped){
  bamptr_t *p;
  if (bucket) {
    if (0 != ((intptr_t)bucket->lifo[idx2].last & (intptr_t)0x1)){
      // already reversed
      p = (bamptr_t*)(((intptr_t)bucket->lifo[idx2].last)^(intptr_t)0x1);
    }else{
      if (bucket->lifo[idx2].last){
	bamptr_t *t, *next, *prev=NULL;
	if (unclipped){
	  for(t=bucket->lifo[idx2].last; t; t=next){
	    next = t->unclipped_next;
	    t->unclipped_next = prev;
	    prev = t;
	  }
	}else{
	  for(t=bucket->lifo[idx2].last; t; t=next){
	    next = t->clipped_next;
	    t->clipped_next = prev;
	    prev = t;
	  }
	}
	p = bucket->lifo[idx2].last = prev;
	bucket->lifo[idx2].last = (bamptr_t*)((intptr_t)bucket->lifo[idx2].last | (intptr_t)0x1);
      } else {
	p = NULL;
      }
    }
  } else {
    p = NULL;
  }
  return p;
}
static inline bamptr_t *get_bamp(bucket_t *bu) {
	bamptr_space_t *bs = bu->bs;
	if (bs->i == BAMPTR_SPACE_SIZE) {
	  bamptr_space_malloc(bu);
	  bs = bu->bs;
	}
	bamptr_t *bamp = bs->bp[bs->i];
	bs->i++;
	return bamp;
}
#endif
#endif

#if 0
    /** Calculates a score for the read which is the sum of scores over Q15. */
    private static short getSumOfBaseQualities(final SAMRecord rec) {
        short score = 0;
        for (final byte b : rec.getBaseQualities()) {
            if (b >= 15) score += b;
        }

        return score;
    }
#endif

struct bucket_array {
  int64_t min, max;
  bucket_t **ba;
};
static inline uint64_t bucket_array_get_min(struct bucket_array *ba) {
  return ba->min;
}
static inline uint64_t bucket_array_get_max(struct bucket_array *ba) {
  return ba->max;
}

static inline bucket_t *get_bucket(int bucket_index, bucket_array_t *bucket_array, int alloc) {
	bucket_t *bucket = bucket_array->ba[bucket_index];
	if (alloc && bucket == NULL) {
	  bucket = md_malloc(sizeof(bucket_t), mem_getBucket, 1);
	  bucket->min = BUCKET_SIZE;
	  bucket->max = -1;
	  bamptr_space_malloc(bucket);
	  bucket_array->ba[bucket_index] = bucket;
	  if(bucket_index < bucket_array->min) {
	    bucket_array->min = bucket_index;
	  }
	  if(bucket_array->max < bucket_index) {
	    bucket_array->max = bucket_index;
	  }
	}
	return bucket;
}
static void free_bucket(bucket_t *bu){
	bamptr_space_free(bu);
    md_free(bu, sizeof(*bu), mem_getBucket);
}


#if defined(MT_HASH)
#define HASH_QUEUE_SIZE 256
typedef struct hash_queue {
  struct hash_queue *next;
  struct {
    baminfo1_t baminfo1;
    vid_t vid;
  } bam[HASH_QUEUE_SIZE];
  char eoq[HASH_QUEUE_SIZE];
} hash_queue_t;

static hash_queue_t *hash_queue_malloc(hash_queue_t* volatile * p_free_list) {
#if 1
  hash_queue_t * q = *p_free_list;
  while(q) {
    if (__sync_bool_compare_and_swap(p_free_list,q,q->next)){
      break;
    }
    q = *p_free_list;
    IN_SPIN_WAIT_LOOP();
  }
  FINISH_SPIN_WAIT_LOOP();

  if (NULL == q) {
    q = md_malloc(sizeof(hash_queue_t), mem_hashQueueMalloc, 0);
  }
#else
  q = md_malloc(sizeof(hash_queue_t), mem_hashQueueMalloc, 0);
#endif
  memset(q->eoq, 0, sizeof(q->eoq));
  q->next = NULL;
  return q;
}
static void hash_queue_free(hash_queue_t *p, hash_queue_t* volatile *p_free_list) {
#if 1
  while(1) {
    hash_queue_t * q = *p_free_list;
    p->next = q;
    if (__sync_bool_compare_and_swap(p_free_list,q,p)){
      break;
    }
    IN_SPIN_WAIT_LOOP();
  }
  FINISH_SPIN_WAIT_LOOP();
#else
  md_free(p, sizeof(hash_queue_t), mem_hashQueueMalloc);
#endif
}
static void hash_queue_reclaim(hash_queue_t* volatile *p_free_list) {
#if 1
  hash_queue_t *q, *next;
  for(q=*p_free_list; q; q=next){
    next = q->next;
    md_free(q, sizeof(hash_queue_t), mem_hashQueueMalloc);
  }
#else
#endif
}

typedef struct {
    int i;			// current index of queue
    hash_queue_t *q;	// queue
} hash_enq_t;

typedef struct {
  hash_enq_t enq;
  hash_enq_t enqU;
} hash_enq2_t;

typedef struct {
  pthread_t tid;
  struct mt_hash *mt;
  hash_queue_t * volatile q;	// submitted queue w_q[0] w_q[1] ...
  hash_queue_t * volatile qU;	// submitted queue
  volatile size_t n_enq, n_deq, n_spn;
  hash_queue_t * volatile hash_queue_free_list;
#if defined(MATE_CACHE)
  cache_for_qnameid_t cache;
#endif
} hash_work_t;

typedef struct mt_hash {
  bucket_array_t *bucket_array;
  bucket_array_t *bucketU_array;
  bam_hdr_t *header;
  hash_work_t *w;
  hash_enq2_t **q_buf;		// submitter's local queue
  int done;
  long prof_n_spn;
} mt_hash_t;

#define MAX_HASH_QLEN 1000
#define HASH_THROTTLE

static void hash_queue_flush(mt_hash_t *hash, int is_unclipped, int thread, int myid){
  hash_queue_t * const q = (is_unclipped ? hash->q_buf[myid][thread].enqU.q : hash->q_buf[myid][thread].enq.q);
  hash_queue_t * volatile * const p_w_q = (is_unclipped ? &hash->w[thread].qU : &hash->w[thread].q);
  uint64_t spn = 0;

  __atomic_thread_fence(__ATOMIC_RELEASE);

  while(1){
#if defined(HASH_THROTTLE) 
    if(hash->w[thread].n_enq - hash->w[thread].n_deq > MAX_HASH_QLEN) {
      IN_SPIN_WAIT_LOOP();
      spn++;
      continue;
    }
    FINISH_SPIN_WAIT_LOOP();
#endif

    hash_queue_t * const oldq = *p_w_q;
    q->next = oldq;
    if (__sync_bool_compare_and_swap(p_w_q,oldq,q)) {
	    break;
    }
  }
  __atomic_fetch_add(&hash->w[thread].n_enq, 1, __ATOMIC_RELAXED);
  //if (hash->prof_n_spn) 
  __atomic_fetch_add(&hash->w[thread].n_spn, spn, __ATOMIC_RELAXED);
}


/*
 * Adds a bam1_t record to two virtual address spaces (bucket_array and bucketU_array)
 */
#if defined(BAMP_ARRAY)
static inline void read_pool_add_clipped(baminfo1_t *baminfo1, bucket_array_t *bucket_array, vid_t id) {
  const int bucket_index = get_index1(id);
  bucket_t * const bucket = get_bucket(bucket_index, bucket_array, 1);
  const int bucket_index2 = get_index2(id);
  PREFETCH_HASHADD_RW_LOCALITY0(bucket->ba[bucket_index2]);

  add_to_bucket(bucket, bucket_index2, baminfo1);
#if 0
{{{
bam1_t * const b = bam_vaddr_get_ptr(&baminfo1->c.bamva);
int flag = b->core.flag;
if ((0 == (flag & BAM_FUNMAP)) &&
    (0 == (flag & BAM_FSECONDARY)) &&
    (0 == (flag & BAM_FSUPPLEMENTARY))) {
fprintf(stderr, "add: gen_id(b,0) %ld index1 %d index2 %d %s bam_flag %d bam_tid %d bam_pos %d\n", id, bucket_index, bucket_index2, bam_get_qname(b), flag, b->core.tid, b->core.pos);
}
}}}
#endif
}

static inline void read_pool_add_unclipped(baminfo1_t *baminfo1, bucket_array_t *bucketU_array, vid_t u_id DCL_ARG_QNAMEID_CACHE(p_cache)) {
  /* unclipped position */
  const int u_bucket_index = get_index1(u_id);
  bucket_t * const u_bucket = get_bucket(u_bucket_index, bucketU_array, 1);
  const int u_bucket_index2 = get_index2(u_id);
  PREFETCH_HASHADD_RW_LOCALITY0(u_bucket->ba[u_bucket_index2]);

  /* unclipped position */
  add_to_bucketU(u_bucket, u_bucket_index2, baminfo1 ARG_QNAMEID_CACHE(p_cache));
}

#endif

/*
 * A single thread receives multiple bam1_t records from the main (reader) thread. 
 * It adds them to the two virtual address spaces: the alignment position space and the unclipped position space.
 */
static void *mt_hash_adder(void *data){
  hash_work_t * const w = (hash_work_t*)data;
  mt_hash_t * const mt = w->mt;
  hash_queue_t * volatile * const p_w_q[2] = { &w->q, &w->qU };

  register bucket_array_t * bucket_array = mt->bucket_array;
  register bucket_array_t * bucketU_array = mt->bucketU_array;


  while(1){
    long both_null = 1;
    // obtain a new queue if it was added
    int k;
    hash_queue_t * q[2] = { *p_w_q[0], *p_w_q[1] };
    IN_SPIN_WAIT_LOOP();
    for (k=0; k<2; k++){
      if(q[k]){
        both_null = 0;
        while(1){
          q[k] = *p_w_q[k];
          if (__sync_bool_compare_and_swap(p_w_q[k], q[k], NULL)) {
            break;
          }
        }
      }
    }
    if (both_null) {
      if (mt->done)
        goto no_work;
      continue;
    }
    FINISH_SPIN_WAIT_LOOP();

    // change the order of hash_queue_t entries from LIFO to FIFO so that reads can be traversed as they appear in the input file
    for (k=0; k<2; k++){
    if(q[k]){
      hash_queue_t * q2, *prev = NULL, *next;
      for(q2=q[k]; q2; q2=next){
	  next = q2->next;
	  q2->next = prev;
	  prev = q2;
      }

      // add bam reads to the hash
      for(q2=prev; q2; q2=next){
	int i;
	for(i=0; i<HASH_QUEUE_SIZE; i++) {
	  baminfo1_t * const baminfo1 = &q2->bam[i].baminfo1;
	  vid_t const vid = q2->bam[i].vid;
	  if(!q2->eoq[i]){
/*
FIXME
	    if(i+1<HASH_QUEUE_SIZE && !q2->eoq[i+1]){
	      PREFETCH_HASHADD_R_LOCALITY0(q2->bam[i+1].baminfo1.bamva.p_bam);
	      PREFETCH_HASHADD_R_LOCALITY0(q2->bam[i+1].baminfo1.bamva.p_bam->data);
	    }
*/
	    if (k==0){
	      read_pool_add_clipped(baminfo1, bucket_array, vid);
	    } else {
	      read_pool_add_unclipped(baminfo1, bucketU_array, vid ARG_QNAMEID_CACHE(&w->cache));
	    }
	  }else{
	    // end of the queue
	    break;
	  }
	}
	next = q2->next;
	hash_queue_free(q2, &w->hash_queue_free_list);
	w->n_deq++;
      } /* for q2 */
    } /* q[k] != NULL */
    } /* for k */
  } /* while 1 */
no_work:
  return NULL;
}

static mt_hash_t mt_hash;
static mt_hash_t * mt_hash_init(bucket_array_t * bucket_array, bucket_array_t * bucketU_array, bam_hdr_t *header) {

  mt_hash_t * const hash = &mt_hash;
  hash->bucket_array = bucket_array;
  hash->bucketU_array = bucketU_array;
  hash->header = header;
  /* initialized by the submitter
  hash->enq = md_malloc(sizeof(hash->enq[0])*n_hash_adder, mem_hashInit, 0);
  hash->enqU = md_malloc(sizeof(hash->enqU[0])*n_hash_adder, mem_hashInit, 0);
  for(i=0; i<n_hash_adder; i++){
    hash->enq[i].i = 0;
    hash->enq[i].q = hash_queue_malloc();
    hash->enqU[i].i = 0;
    hash->enqU[i].q = hash_queue_malloc();
  }
  */
  hash->w = md_malloc(sizeof(hash->w[0])*n_hash_adder, mem_hashInit, 1);

  pthread_attr_t attr;
  pthread_attr_init(&attr);
  int i;
  const int cpu_vec_len = (n_physical_cores*SMT+63)/64;
  uint64_t cpu_vec[cpu_vec_len];
  memset(cpu_vec, 0, sizeof(uint64_t)*cpu_vec_len);
  for(i=0; i<n_hash_adder; i++){
    hash->w[i].mt = hash;
    pthread_create(&hash->w[i].tid, &attr, mt_hash_adder, &hash->w[i]);
    char tn[32];
    sprintf(tn, "db_builder-%d-%d\n", i, CPU_HASH(i));
    if(pthread_setname_np(hash->w[i].tid, tn)){}
#if defined(CPU_SET_AFFINITY)
{
      cpu_set_t cpuset;
      CPU_ZERO(&cpuset);
      CPU_SET2(CPU_HASH(i), &cpuset);
      cpu_vec[CPU_HASH(i)/64] |= 1LL<<(63-CPU_HASH(i)%64);
      pthread_setaffinity_np(hash->w[i].tid, sizeof(cpu_set_t), &cpuset);
}
#endif
  }
  fprintf(stderr, "(I) CPU %15s ", "db_builder");
  for(i=0; i<cpu_vec_len; i++) {
    int j;
    for(j=0; j<64; j++) {
      fprintf(stderr, "%s%c", j%SMT ? "" : " ", (cpu_vec[i]&(1LL<<(63-j))) ? 'o' : '-');
    }
  }
  fprintf(stderr, "\n");
  return &mt_hash;
}

static inline int getThreadIndexForBucket(int bucket_index, int n_thread) {
  return ( bucket_index ) % n_thread; // workload distribution - cyclic
}
static inline int getFirstBucketIndex(int start, int myid, int n_thread) {
  int i;
  i = (start / n_thread) * n_thread;
  if (start <= i + myid) {
    i += myid;
  } else {
    i += myid + n_thread;
  }
  return i;
}
static inline int getNextBucketIndex(int bucket_index, int myid, int n_thread) {
  return bucket_index + n_thread;	// cyclic
}

static inline void mt_hash_enqueue(mt_hash_t *hash, vid_t id, vid_t u_id, baminfo1_t *baminfo1, int myid) {
  const int bucket_index = get_index1(id);

  const int thd_idx = getThreadIndexForBucket(bucket_index, n_hash_adder);

  hash_enq2_t* const enq2 = &hash->q_buf[myid][thd_idx];

  // check if the queue is full
  if(HASH_QUEUE_SIZE == enq2->enq.i) {
    // send the queue to the worker thread and allocate a new queue
    hash_queue_flush(hash, 0, thd_idx, myid);
    enq2->enq.i = 0;
    enq2->enq.q = hash_queue_malloc(&hash->w[thd_idx].hash_queue_free_list);
  }
  // add the bam to the queue
  enq2->enq.q->bam[enq2->enq.i].baminfo1 = *baminfo1;
  enq2->enq.q->bam[enq2->enq.i].vid = id;
  enq2->enq.i++;

  if (0 == (baminfo1->u.flag & BAM_FUNMAP)){
    const int u_bucket_index = get_index1(u_id);
    const int u_thd_idx = u_bucket_index % n_hash_adder;
    hash_enq2_t* const u_enq2 = &hash->q_buf[myid][u_thd_idx];

    // check if the queue is full
    if(HASH_QUEUE_SIZE == u_enq2->enqU.i) {
      // send the queue to the worker thread and allocate a new queue
      hash_queue_flush(hash, 1, u_thd_idx, myid);
      u_enq2->enqU.i = 0;
      u_enq2->enqU.q = hash_queue_malloc(&hash->w[u_thd_idx].hash_queue_free_list);
    }
    // add the bam to the queue
    u_enq2->enqU.q->bam[u_enq2->enqU.i].baminfo1 = *baminfo1;
    u_enq2->enqU.q->bam[u_enq2->enqU.i].vid = u_id;
    u_enq2->enqU.i++;
  }
}

static void mt_hash_end(mt_hash_t *hash) {
  int i,j;
  for(i=0;  i<n_hash_adder; i++) {
  for(j=0;  j<n_bam_copier; j++) {
    // add the bams that are not added yet to the queue
    if (hash->q_buf[j][i].enq.i < HASH_QUEUE_SIZE) {
	hash->q_buf[j][i].enq.q->eoq[hash->q_buf[j][i].enq.i] = 1;
    }
    hash_queue_flush(hash, 0, i, j);

    if (hash->q_buf[j][i].enqU.i < HASH_QUEUE_SIZE) {
	hash->q_buf[j][i].enqU.q->eoq[hash->q_buf[j][i].enqU.i] = 1;
    }
    hash_queue_flush(hash, 1, i, j);
  }
  }
  __sync_synchronize();

  hash->done = 1;

  uint64_t n_miss = 0; // the number alignments whose flag shows paired & map & mate map but mate was not found by a simple cache mechanism
  for (i=0; i<n_hash_adder; i++) {
    pthread_join(hash->w[i].tid, 0);
    hash_queue_reclaim(&hash->w[i].hash_queue_free_list);
#if defined(MATE_CACHE)
    n_miss += hash->w[i].cache.n_miss;
#endif
  }
  g_alignments_without_mate_info = n_miss;

  free_qnameid_space();
}

typedef struct bamcopy_queue {
  struct bamcopy_queue *next;
  struct {
    bam1_t *b;
    uint64_t id;
  } bam[HASH_QUEUE_SIZE];
} bamcopy_queue_t;

static bamcopy_queue_t *bamcopy_queue_malloc(bamcopy_queue_t* volatile *p_free_list) {
#if 1
  bamcopy_queue_t * q = *p_free_list;
  while(q) {
    if (__sync_bool_compare_and_swap(p_free_list,q,q->next)){
      break;
    }
    q = *p_free_list;
    IN_SPIN_WAIT_LOOP();
  }
  FINISH_SPIN_WAIT_LOOP();

  if (NULL == q) {
    q = md_malloc(sizeof(hash_queue_t), mem_bamcopyQueueMalloc, 0);
  }
#else
  bamcopy_queue_t * const q = md_malloc(sizeof(bamcopy_queue_t), mem_bamcopyQueueMalloc, 0);
#endif
  q->next = NULL;
  return q;
}
static void bamcopy_queue_free(bamcopy_queue_t *p, bamcopy_queue_t* volatile *p_free_list) {
#if 1
  while(1) {
    bamcopy_queue_t * q = *p_free_list;
    p->next = q;
    if (__sync_bool_compare_and_swap(p_free_list,q,p)){
      break;
    }
    IN_SPIN_WAIT_LOOP();
  }
  FINISH_SPIN_WAIT_LOOP();
#else
  md_free(p, sizeof(bamcopy_queue_t), mem_bamcopyQueueMalloc);
#endif
}
static void bamcopy_queue_reclaim( bamcopy_queue_t* volatile *p_free_list) {
#if 1
  bamcopy_queue_t *q, *next;
  for(q=*p_free_list; q; q=next){
    next = q->next;
    md_free(q, sizeof(bamcopy_queue_t), mem_bamcopyQueueMalloc);
  }
#else
#endif
}

typedef struct {
    int i;			// current index of queue
    bamcopy_queue_t *q;	// queue
} bamcopy_enq_t;

typedef struct {
  pthread_t tid;
  struct mt_bamcopy *mt;
  volatile size_t n_enq, n_deq, n_spn;
  bamcopy_queue_t * volatile q;	// submitted queue
  uint32_t bam_vaddr_fd;
  uint32_t bam_vaddr_seqid;
  bamcopy_queue_t * volatile bamcopy_queue_free_list;
} bamcopy_work_t;

typedef struct mt_bamcopy {
  struct mt_page_write *page_write;
  mt_hash_t *hash;
  bam_hdr_t *header;
  bucket_array_t *bucket_array;
  bamcopy_work_t *w;
  bamcopy_enq_t **q_buf;	// submitter's local queue
  int done;
  bam_free_list2_t bam_free_list2;

  int page_out;
//  size_t max_cache_size;	// max bytes to keep bam1_t data in the main memory
//  size_t cur_cache_size;
//  volatile uint32_t bam_vaddr_seqid;
  long prof_n_spn;
} mt_bamcopy_t;

#define MAX_BAMCOPY_QLEN 1000
#define BAMCOPY_THROTTLE

/*
 * Page the BAM records out/in from/to the physical memory
 */
typedef struct bam_space {
  struct bam_space *next;
  int bam_vaddr_fd; // paging-mode only
  uint32_t bam_vaddr_seqid; // paging-mode omly
  char *curr;
  char space[BAM_SPACE_SIZE];
} bam_space_t;

typedef struct {
  pthread_t tid;
  struct mt_page_write *mt;
} page_write_work_t;

#define N_PAGEWRITER 1 // 16
typedef struct mt_page_write {
  int done;
  int n_files_per_bamcopier;
  int *page_file_fd;
#if defined(PAGEFILE_SEQUENTIAL_WRITE)
  int *page_file_fd_seqid;
  uint32_t *offset_translation_table_size;
  uint32_t **offset_translation_table;
#endif
  page_write_work_t w[N_PAGEWRITER];

  bam_space_t * volatile q; // submitted queue

  bam_space_t * volatile free_list;
} mt_page_write_t;

typedef struct {
#if defined(PAGEFILE_CACHE_PERFILE)
  struct page_cache *cache;
#endif
  mt_page_write_t *mt;
  volatile int lock;
  int fd;
} pagefile_t;

static inline void bam_space_init(mt_bamcopy_t *bamcopy, bam_space_t *bams, int myid) {
  int const n = (BGZF_MAX_BLOCK_SIZE * BGZF_BLOCK_COUNT_PER_WORKER /* chunk size read by each bgzf thread (default 4 MB) */ / BAM_SPACE_SIZE);
  //bams->bam_vaddr_seqid = __atomic_fetch_add(&bamcopy->bam_vaddr_seqid, 1, __ATOMIC_RELAXED);
  bams->curr = &bams->space[0];
  if (bamcopy->page_out) {
    bams->bam_vaddr_fd    = bamcopy->w[myid].bam_vaddr_fd;
    bams->bam_vaddr_seqid = bamcopy->w[myid].bam_vaddr_seqid;

    // update the file position
    // bamcopy->page_write->n_files_per_bamcopier 4
    // copier 0  0  0  0  	1  1  1  1
    // fd     0  1  2  3  	4  5  6  7
    // seqid  0  0  0  0	0  0  0  0
    //        :  :  :  :	:  :  :  :
    //       n-1n-1n-1n-1      n-1n-1n-1n-1
    if ((bamcopy->w[myid].bam_vaddr_seqid + 1) % n != 0) {
      bamcopy->w[myid].bam_vaddr_seqid ++;
    } else {
      bamcopy->w[myid].bam_vaddr_seqid -= n - 1;
      if ((bamcopy->w[myid].bam_vaddr_fd + 1) % bamcopy->page_write->n_files_per_bamcopier != 0) {
        bamcopy->w[myid].bam_vaddr_fd ++;
      } else {
        bamcopy->w[myid].bam_vaddr_fd = myid * bamcopy->page_write->n_files_per_bamcopier;
        bamcopy->w[myid].bam_vaddr_seqid += n;
      }
    }
  }
}

static bam_space_t *bam_space_alloc(bam_space_t * volatile * const p_fl, int myid) {
  bam_space_t *free = p_fl ? *p_fl : NULL;
  while (free) {
    if (__sync_bool_compare_and_swap(p_fl, free, free->next)) {
      break;
    }
    free = *p_fl;
    IN_SPIN_WAIT_LOOP();
  }
  FINISH_SPIN_WAIT_LOOP();

  if(NULL == free ) {
    free = md_malloc(sizeof(bam_space_t), mem_bamSpaceAlloc, 0);
//fprintf(stderr, "(%d) bam_space_alloc malloc %p\n", myid, free);fflush(stderr);
//  } else {
//fprintf(stderr, "(%d) bam_space_alloc reuse %p\n", myid, free);fflush(stderr);
  }
  return free;
}

#if !defined(PAGEOUT_BY_BAMCOPIER)
static void bam_space_free(mt_page_write_t *mt, bam_space_t *p) {
  bam_space_t * volatile * const p_fl = &mt->free_list;
  while (1) {
    bam_space_t *p2 = *p_fl;
    p->next = p2;
    if (__sync_bool_compare_and_swap(p_fl, p2, p)) {
//fprintf(stderr, "bam_space_free %p\n", p);fflush(stderr);
      break;
    }
    IN_SPIN_WAIT_LOOP();
  }
  FINISH_SPIN_WAIT_LOOP();
  //  md_free(p, sizeof(bam_space_t), mem_bamSpaceAlloc);
}
#endif

static mt_page_write_t mt_page_write;

#if !defined(PAGEOUT_BY_BAMCOPIER)
static void *mt_page_writer(void *data){ 
  page_write_work_t * const w = (page_write_work_t*)data;
  mt_page_write_t * const mt = w->mt;
  bam_space_t * volatile * const p_w_q = &mt->q;
  int const myid = w - mt->w;

  struct timeval page_write_start;
  static ssize_t total, last;
  if (myid == 0) {
    total = last = 0;
    gettimeofday(&page_write_start, NULL);
  }
  while(1){
    // obtain a new queue if it was added
    bam_space_t * bams;
    while(1){
      bams = *p_w_q;
      if (bams) {
        if (__sync_bool_compare_and_swap(p_w_q, bams, NULL)) {
	  break;
	}
      } else {
	// if there is no queue and w->done then there is no work.
	if(mt->done) goto no_work;
        IN_SPIN_WAIT_LOOP();
      }
    }
    FINISH_SPIN_WAIT_LOOP();
    bam_space_t * q2, *prev = NULL, *next;
    // change the order of samparse_queue_t entries from LIFO to FIFO so that reads can be traversed as they appear in the input file
    for(q2=bams; q2; q2=next){
	next = q2->next;
	q2->next = prev;
	prev = q2;
    }

    for(q2=prev; q2; q2=next){
      size_t count = q2->curr - &q2->space[0]; 
#if 0
{{{
bam1_t *b;
for(b=(bam1_t*)q2->space; (char*)b < q2->curr; b=(bam1_t*)((char*)b + sizeof(bam1_t) + b->l_data)){
fprintf(stderr, "pwrite bamva %d %d %ld l_data %d %s pos=%d\n", q2->am_copier_id, q2->bam_vaddr_seqid, (char*)b-q2->space, b->l_data, bam_get_qname(b), b->core.pos); 
}
}}}
#endif
//fprintf(stderr, "writer: fileId %d seqid %d count %ld\n", q2->am_copier_id, q2->bam_vaddr_seqid, count);

#if defined(PAGEFILE_SEQUENTIAL_WRITE)
      // translate bam_vaddr_seqid (random) to a page file seqid (sequential) so that pwrite can access a file sequentially
      if (NULL == mt->offset_translation_table[q2->bam_vaddr_fd]) {
        const uint32_t size = (1+q2->bam_vaddr_seqid)*2;
        mt->offset_translation_table_size[q2->bam_vaddr_fd] = size;
        mt->offset_translation_table[q2->bam_vaddr_fd] = md_malloc(size*sizeof(mt->offset_translation_table[0][0]), mem_mtPageWriter, 1);
      } else if (mt->offset_translation_table_size[q2->bam_vaddr_fd] <= q2->bam_vaddr_seqid) {
	uint32_t * const old_ptr = mt->offset_translation_table[q2->bam_vaddr_fd]; 
	uint32_t const old_size = mt->offset_translation_table_size[q2->bam_vaddr_fd];
        const uint32_t new_size = (1+q2->bam_vaddr_seqid)*2;
	uint32_t * const new_ptr = md_realloc(old_ptr, new_size*sizeof(mt->offset_translation_table[0][0]), mem_mtPageWriter);
	memset(new_ptr+old_size, 0, (new_size - old_size)*sizeof(mt->offset_translation_table[0][0]));
        mt->offset_translation_table_size[q2->bam_vaddr_fd] = new_size;
        mt->offset_translation_table[q2->bam_vaddr_fd]      = new_ptr;
      }
      const uint32_t fd_seqid = mt->page_file_fd_seqid[q2->bam_vaddr_fd]++; 
      mt->offset_translation_table[q2->bam_vaddr_fd][q2->bam_vaddr_seqid] = fd_seqid;

      off_t const offset = (off_t)fd_seqid * sizeof(q2->space);
#else
      off_t const offset = (off_t)q2->bam_vaddr_seqid * sizeof(q2->space);
#endif
      ssize_t rc = pwrite(mt->page_file_fd[q2->bam_vaddr_fd], &q2->space[0], count, offset);
      if (-1 == rc) {
        perror("mt_page_writer: pwrite() failed");
        exit(-1);
      }
      __atomic_fetch_add(&total, rc, __ATOMIC_RELAXED);
      if (myid == 0) {
        if (total/1024/1024/1024 != last/1024/1024/1024) {
	  static struct timeval last_t = {0, 0};
          struct timeval t;
          gettimeofday(&t, NULL);
	  double d0 = (t.tv_sec - last_t.tv_sec) + (t.tv_usec - last_t.tv_usec)/1000000;
	  double d = (t.tv_sec - page_write_start.tv_sec) + (t.tv_usec - page_write_start.tv_usec)/1000000;
	  fprintf(stderr, "%ld GB %.3f GB/s (total %.3f GB/s\n", last/1024/1024/1024, (total-last)/d0/1024/1024/1024, total/d/1024/1024/1024);
	  last_t = t;
          last = total;
        }
      }
//fprintf(stderr, "mt_page_writer pwrite id %d seqid %d \n", q2->am_copier_id, q2->bam_vaddr_seqid);
      next = q2->next;
      //w->n_deq ++;

      bam_space_free(mt, q2);
//fprintf(stderr, "mt_page_writer freed prev %p q2 %p\n", prev, q2);fflush(stderr);
    } /* for q2 */
  }
no_work:
  return NULL;
}
#endif

static const char *pagefile_name = "./bam_pagefile"; 
static mt_page_write_t* pagefile_init(void){
  /* set up a page file */
  char *str = getenv("BAM_PAGEFILE");
  if (str) {
    pagefile_name = str;
  }

  mt_page_write_t *mt = &mt_page_write;
  int i;
#if !defined(PAGEOUT_BY_BAMCOPIER)
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  mt->q = NULL;
  for (i=0; i<N_PAGEWRITER; i++) {
    mt->w[i].mt = mt;
    pthread_create(&mt->w[i].tid, &attr, mt_page_writer, &mt->w[i]);
    char tn[64];
    sprintf(tn, "page_writer-%d", i);
    if(pthread_setname_np(mt->w[i].tid, tn)){}
#if 0 && defined(CPU_SET_AFFINITY)
{
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET2(CPU_PAGEWR(i), &cpuset);
    pthread_setaffinity_np(mt->w[i].tid, sizeof(cpu_set_t), &cpuset);
}
#endif
  }
#endif

  char fn[strlen(pagefile_name)+32];
  mt->n_files_per_bamcopier = 1; // ( n_threads + n_bam_copier - 1) / n_bam_copier;
  int const n_page_files = n_bam_copier * mt->n_files_per_bamcopier;
  if (str) {
    fprintf(stderr, "(I) ** NEW pagefile: %s-0~%d\n", pagefile_name, n_page_files);
  } else {
    fprintf(stderr, "(I) default pagefile: %s-0~%d\n", pagefile_name, n_page_files);
  }
  mt->page_file_fd = md_malloc(n_page_files * sizeof(mt->page_file_fd[0]), mem_pagefileInit, 0);
  for(i=0; i<n_page_files; i++) {
    sprintf(fn, "%s-%d", pagefile_name, i);
    int fd = open(fn, O_LARGEFILE | O_CREAT | O_WRONLY | O_TRUNC, S_IRUSR | S_IWUSR);
    if (-1 == fd) {
      perror("pagefile_init: write open() for the page file failed");
      exit(-1);
    }
    mt->page_file_fd[i] = fd;
  }
#if defined(PAGEFILE_SEQUENTIAL_WRITE)
  mt->page_file_fd_seqid = md_malloc(n_page_files * sizeof(mt->page_file_fd_seqid[0]), mem_pagefileInit, 1);
  mt->offset_translation_table_size = md_malloc(n_page_files * sizeof(mt->offset_translation_table_size[0]), mem_pagefileInit, 1);
  mt->offset_translation_table = md_malloc(n_page_files * sizeof(mt->offset_translation_table[0]), mem_pagefileInit, 1);
#endif
  return mt;
}
static void pagefile_end() {
  int i;
#if !defined(PAGEOUT_BY_BAMCOPIER)
  for (i=0; i<N_PAGEWRITER; i++) {
    pthread_join(mt_page_write.w[i].tid, NULL);
  }
#endif
  for (i=0; i<n_bam_copier; i++) {
    int i2;
    for (i2=0; i2<mt_page_write.n_files_per_bamcopier; i2++) {
      close(mt_page_write.page_file_fd[mt_page_write.n_files_per_bamcopier * i + i2]);
    }
  }
}
static void pagefile_init2(mt_bamcopy_t *bamcopy) {
  int i;
  for(i=0; i<n_bam_copier; i++) {
    bamcopy->w[i].bam_vaddr_fd    =i * bamcopy->page_write->n_files_per_bamcopier;
    bamcopy->w[i].bam_vaddr_seqid =0;
  }
}

static void pageout_bam_space(mt_bamcopy_t *bamcopy, bam_space_t *bams, int myid) {
#if defined(PAGEOUT_BY_BAMCOPIER)
  size_t count = bams->curr - &bams->space[0]; 
  mt_page_write_t *mt = bamcopy->page_write;
#if defined(PAGEFILE_SEQUENTIAL_WRITE)
  // translate bam_vaddr_seqid (random) to a page file seqid (sequential) so that pwrite can access a file sequentially
  if (NULL == mt->offset_translation_table[bams->bam_vaddr_fd]) {
    const uint32_t size = (1+bams->bam_vaddr_seqid)*2;
    mt->offset_translation_table_size[bams->bam_vaddr_fd] = size;
    mt->offset_translation_table[bams->bam_vaddr_fd] = md_malloc(size*sizeof(mt->offset_translation_table[0][0]), mem_mtPageWriter, 1);
  } else if (mt->offset_translation_table_size[bams->bam_vaddr_fd] <= bams->bam_vaddr_seqid) {
    uint32_t * const old_ptr = mt->offset_translation_table[bams->bam_vaddr_fd]; 
    uint32_t const old_size = mt->offset_translation_table_size[bams->bam_vaddr_fd];
    const uint32_t new_size = (1+bams->bam_vaddr_seqid)*2;
    uint32_t * const new_ptr = md_realloc(old_ptr, new_size*sizeof(mt->offset_translation_table[0][0]), mem_mtPageWriter);
    memset(new_ptr+old_size, 0, (new_size - old_size)*sizeof(mt->offset_translation_table[0][0]));
    mt->offset_translation_table_size[bams->bam_vaddr_fd] = new_size;
    mt->offset_translation_table[bams->bam_vaddr_fd]      = new_ptr;
  }
  const uint32_t fd_seqid = mt->page_file_fd_seqid[bams->bam_vaddr_fd]++; 
  mt->offset_translation_table[bams->bam_vaddr_fd][bams->bam_vaddr_seqid] = fd_seqid;

  off_t const offset = (off_t)fd_seqid * sizeof(bams->space);
#else
  off_t const offset = (off_t)bams->bam_vaddr_seqid * sizeof(bams->space);
#endif
  ssize_t rc = pwrite(mt->page_file_fd[bams->bam_vaddr_fd], &bams->space[0], count, offset);
  if (-1 == rc) {
    fprintf(stderr, "mt_page_writer: pwrite: mt->page_file_fd[bams->bam_vaddr_fd %d] %d &bams->space[0] %p count %ld offset %ld\n", bams->bam_vaddr_fd, mt->page_file_fd[bams->bam_vaddr_fd], &bams->space[0], count, offset);
    perror("mt_page_writer: pwrite() failed");
    exit(-1);
  }
#else
//fprintf(stderr, "pageout: myid %d fileId %d seqid %d count %ld\n", myid, bams->am_copier_id, bams->bam_vaddr_seqid, bams->curr-bams->space);
  __atomic_thread_fence(__ATOMIC_RELEASE);

  bam_space_t * volatile * const p_w_q = &mt_page_write.q;

  while(1){
    bam_space_t * const oldq = *p_w_q;
    bams->next = oldq;
    if (__sync_bool_compare_and_swap(p_w_q,oldq,bams)) {
      break;
    }
    IN_SPIN_WAIT_LOOP();
  }
  FINISH_SPIN_WAIT_LOOP();
#endif
}


static void bamcopy_queue_flush(mt_bamcopy_t *bamcopy, int thread, int myid){
  bamcopy_queue_t * const q = bamcopy->q_buf[myid][thread].q;
  bamcopy_queue_t * volatile * const p_w_q = &bamcopy->w[thread].q;
  uint64_t spn = 0;

  __atomic_thread_fence(__ATOMIC_RELEASE);

  while(1){
#if defined(BAMCOPY_THROTTLE) 
    if(bamcopy->w[thread].n_enq - bamcopy->w[thread].n_deq > MAX_BAMCOPY_QLEN) {
      IN_SPIN_WAIT_LOOP();
      spn++;
      continue;
    }
#endif
    FINISH_SPIN_WAIT_LOOP();

    bamcopy_queue_t * const oldq = *p_w_q;
    q->next = oldq;
    if (__sync_bool_compare_and_swap(p_w_q,oldq,q)) {
	    break;
    }
  }
  __atomic_fetch_add(&bamcopy->w[thread].n_enq, 1, __ATOMIC_RELAXED);
  //if (bamcopy->prof_n_spn) 
  __atomic_fetch_add(&bamcopy->w[thread].n_spn, spn, __ATOMIC_RELAXED);
}

#if defined(PROFILE_BAM_SPACE_CACHE)
#define DCL_PROFILE_CACHE_HIT_S uint64_t hit;
#define DCL_PROFILE_CACHE_REP_S uint64_t rep;
#define PROFILE_CACHE_HIT_S()	cache->hit++
#define PROFILE_CACHE_REP_S()	cache->rep++
#define PROFILE_CACHE_UPDATE()	{ bam_space_cache_hit += cache->hit; bam_space_cache_rep += cache->rep; }
#define PROFILE_CACHE_INIT()	{atexit(dump_prof_cache);}
static volatile int64_t bam_space_cache_hit = 0UL;
static volatile int64_t bam_space_cache_rep = 0UL;
static void dump_prof_cache() {
  fprintf(stderr, "(I) profile bam_space_cache hit ratio %.1f %%\n", bam_space_cache_hit*100.0/(bam_space_cache_hit+bam_space_cache_rep));
}
#else
#define DCL_PROFILE_CACHE_HIT_S
#define DCL_PROFILE_CACHE_REP_S
#define PROFILE_CACHE_HIT_S()
#define PROFILE_CACHE_REP_S()
#define PROFILE_CACHE_UPDATE()
#define PROFILE_CACHE_INIT()
#endif
#define CACHE_AGE_TYPE uint64_t
#define CACHE_AGE_BITS 64
#if defined(PAGEFILE_CACHE_PERFILE)
#define N_CACHE_SLOT 32*32
#elif defined(PAGEFILE_CACHE_PERTHREAD)
#define N_CACHE_SLOT 64
#endif
#if defined(PAGEFILE_CACHE_PERFILE) || defined(PAGEFILE_CACHE_PERTHREAD)
typedef union {
  uint64_t id;
  struct {
    uint32_t bamva_id;
    uint32_t bamva_seqid;
  };
} cache_id_t;

typedef struct page_cache {
  int last_hit_slot;
  DCL_PROFILE_CACHE_HIT_S
  DCL_PROFILE_CACHE_REP_S
  CACHE_AGE_TYPE age[N_CACHE_SLOT];
  struct {
    cache_id_t cache_id;
    char data[BAM_SPACE_SIZE];
  } slot[N_CACHE_SLOT];
} page_cache_t;
#endif


void *get_bam_space(int n_workers) {
  if (g_paging) {
    int const n_page_files = n_bam_copier * mt_page_write.n_files_per_bamcopier;
    int i;
    pagefile_t *pf = md_malloc(sizeof(*pf) * n_page_files, mem_getBamSpace, 0);
    pf->mt = &mt_page_write;
    char fn[strlen(pagefile_name)+32];
    for(i=0; i<n_page_files; i++){
      sprintf(fn, "%s-%d", pagefile_name, i);
      int fd = open(fn, O_LARGEFILE | O_RDONLY);
      if (-1 == fd) {
        perror("(E) get_bam_space: read open() for page files ");
        exit(-1);
      }
      off_t const orig = lseek(fd, 0, SEEK_CUR);
      off_t const file_size = lseek(fd, 0, SEEK_END);
      lseek(fd, orig, SEEK_SET);
      posix_fadvise(fd, 0, file_size, POSIX_FADV_SEQUENTIAL);

#if defined(PAGEFILE_CACHE_PERFILE)
      pf[ i].cache = NULL;
#endif
      pf[ i].lock = 0;
      pf[ i].fd = fd;
    }

    PROFILE_CACHE_INIT();
    return pf;
  } else {
    return NULL;
  }
}

void remove_bam_space(void) {
  if (g_paging) {
    int const n_page_files = n_bam_copier * mt_page_write.n_files_per_bamcopier;
    int i;
    char fn[strlen(pagefile_name)+32];
    for(i=0; i<n_page_files; i++){
      sprintf(fn, "%s-%d", pagefile_name, i);
      int rc = remove(fn);
      if (-1 == rc) {
        perror("(W) remove_bam_space: page files not removed");
      }
    }
  }
}

static volatile int bam_space_reader_lock = 0;

static inline bam1_t* read_from_bam_space(void **p_bam_space_cache, void *bam_space_id, bam_vaddr_t bamva, int len, int myid) {
if (is_bam_vaddr_external(&bamva)) {
  uint32_t const bamva_id = bam_vaddr_get_fd(&bamva);
  uint32_t const bamva_seqid = bam_vaddr_get_seqid(&bamva);
  uint32_t const bamva_offset = bam_vaddr_get_offset(&bamva);
  pagefile_t * const pf = (pagefile_t*)bam_space_id;

#if defined(PAGEFILE_SEQUENTIAL_WRITE)
  off_t const offset = (off_t)pf->mt->offset_translation_table[bamva_id][bamva_seqid] * sizeof(((bam_space_t*)0)->space);
#else
  off_t const offset = (off_t)bamva_seqid * sizeof(((bam_space_t*)0)->space);
#endif

  int const fd = pf[bamva_id].fd;
  //volatile int * const p_lock = &bam_space_reader_lock;
#if defined(PAGEFILE_CACHE_PERFILE) || defined(PAGEFILE_CACHE_PERTHREAD)
  int i;
  int ref = -1;
#endif
#if defined(PAGEFILE_CACHE_PERFILE)
  static int const unlock = 0;
  volatile int * const p_lock = &pf[bamva_id].lock;

  if (NULL == *p_bam_space_cache) {
    // allocate a per-thread buffer
    *p_bam_space_cache = md_malloc(4096, mem_readFromBamSpace, 0);
  }
  page_cache_t * cache = pf[bamva_id].cache;
  while(NULL == cache) {
    if (__sync_bool_compare_and_swap(p_lock,0,1)) {
      /* critical region start */
      cache = md_malloc(sizeof(page_cache_t), mem_readFromBamSpace, 1);
      cache->last_hit_slot = -1;
      for(i=0; i<N_CACHE_SLOT; i++) {
        cache->slot[i].cache_id.bamva_id = -1; // invalid
      }
      pf[bamva_id].cache = cache;
      __atomic_store(p_lock, &unlock, __ATOMIC_RELEASE);
      /* critical region end */
    }
    cache = pf[bamva_id].cache;
  }
#elif defined(PAGEFILE_CACHE_PERTHREAD)
  page_cache_t * cache = (page_cache_t*)*p_bam_space_cache;
  if(NULL == cache) {
    cache = md_malloc(sizeof(page_cache_t), mem_readFromBamSpace, 1);
    cache->last_hit_slot = -1;
    for(i=0; i<N_CACHE_SLOT; i++) {
      cache->slot[i].cache_id.bamva_id = -1; // invalid
    }
    *p_bam_space_cache = cache;
  }
#endif

  bam1_t *b;
#if defined(PAGEFILE_CACHE_PERFILE)
do {
if (__sync_bool_compare_and_swap(p_lock,0,1)) {
/* critical region start */
#endif

#if defined(PAGEFILE_CACHE_PERFILE) || defined(PAGEFILE_CACHE_PERTHREAD)
  // search for the entry for [bamva_id, bamva_seqid] 
  cache_id_t cache_id;
  cache_id.bamva_id = bamva_id;
  cache_id.bamva_seqid = bamva_seqid;
  if (-1 != cache->last_hit_slot) {
    for(i=cache->last_hit_slot; i<N_CACHE_SLOT; i++) {
      if (cache->slot[i].cache_id.id == cache_id.id) {
        ref = i;
        break;
      }
    }
    if (-1 == ref) {
      for(i=0; i<cache->last_hit_slot; i++) {
        if (cache->slot[i].cache_id.id == cache_id.id) {
          ref = i;
          break;
        }
      }
    }
  }
  if (-1 != ref){    
    PROFILE_CACHE_HIT_S();
  } else { // -1 == ref
    /* replace the oldest cache */
    PROFILE_CACHE_REP_S();
    int min=0;
    uint64_t min_val=cache->age[0];

    for(i=1; i<N_CACHE_SLOT; i++) {
      if (0 == min_val) break;

      if (cache->age[i]< min_val) {
        min=i;
        min_val= cache->age[i];
      }
    }
    ssize_t rc;
//CS    while (1) {
//CS      if (__sync_bool_compare_and_swap(p_lock,0,1)) {
//CS        /* critical region start */
	PROFILE_CACHE_UPDATE();
        rc = pread(fd, cache->slot[min].data, sizeof(((bam_space_t*)0)->space), offset);
//CS	__atomic_store(p_lock, &unlock, __ATOMIC_RELEASE);
//CS	/* critical region end */
//CS	break;
//CS      }
//CS      IN_SPIN_WAIT_LOOP();
//CS    }
//CS    FINISH_SPIN_WAIT_LOOP();
    if (-1 == rc) {
      perror("read_from_bam_space: read() failed");
      exit(-1);
    } 

    cache->slot[min].cache_id.bamva_id = bamva_id;
    cache->slot[min].cache_id.bamva_seqid = bamva_seqid;
    ref = min;
  }

  /* fix bam1_t */
  b = (bam1_t*)(cache->slot[ref].data + bamva_offset);
#if defined(PAGEFILE_CACHE_PERFILE)
  memcpy(*p_bam_space_cache, b, sizeof(*b)+b->l_data); 
  b = (bam1_t*)*p_bam_space_cache;
#endif
  b->data = (uint8_t*)b + sizeof(bam1_t);

if (b->l_data == 0) {
  fprintf(stderr, "(E) read_from_bam_space(): l_data == 0 len %d bamva fd %d seqid %d offset %d\n", len, bam_vaddr_get_fd(&bamva), bam_vaddr_get_seqid(&bamva), bam_vaddr_get_offset(&bamva));
  fflush(stderr);
  exit(-1);
}
  /* aging */
  if (cache->last_hit_slot != ref) {
    for(i=0; i<N_CACHE_SLOT; i++) {
      cache->age[i] >>= 1;
    }
    cache->age[ref] = 1UL<<(CACHE_AGE_BITS-1)| cache->age[ref];
    cache->last_hit_slot = ref;
  }

#if defined(PAGEFILE_CACHE_PERFILE)
__atomic_store(p_lock, &unlock, __ATOMIC_RELEASE);
/* critical region end */
}
} while (-1 == ref);
#endif

#else
  if (NULL == *p_bam_space_cache) {
    // allocate a per-thread buffer
    *p_bam_space_cache = md_malloc(4096, mem_readFromBamSpace, 0);
  }
  int rc = pread(fd, *p_bam_space_cache, len, offset + bamva_offset);
  if (-1 == rc) {
      perror("read_from_bam_space: pread() failed");
      exit(-1);
  } 
  b = (bam1_t*)*p_bam_space_cache;
  b->data = (uint8_t*)b + sizeof(bam1_t);
#endif

  return b;
} else {
  bam1_t * const b = bam_vaddr_get_ptr(&bamva);
  return  b;
}
}

void bam_copy(void **p_bam_space_cache, void *bam_space_id, memcpy_info_array_t *mia, char *blk, int len, int bigendian, int myid) {
  int total_len = 0;
  memcpy_info_t *mi;
  int dbg_ldata_zero = 0;
  bam_free_list_t * top = NULL, *bottom = NULL;
  for (mi=mia->info; !mi->is_end; mi=(memcpy_info_t*)((char*)mi + sizeof(memcpy_info_ptr_t))) {
    int const mi_len = mi->len;
    int const l_data = mi_len - 4 - 32;
    if ( 0 ) { // TODO	prefetch didn't improve the performance
      memcpy_info_t *mi2 =(memcpy_info_t*)((char*)mi + sizeof(memcpy_info_ptr_t) * 5 );
      bam_vaddr_t bamva2 = ((memcpy_info_ptr_t*)mi2)->ptr;
      bam1_t * const b2 = bam_vaddr_get_ptr(&bamva2);
      int i;
      for (i=0; i < (l_data + sizeof(bam1_t*))/128; i++) {
        PREFETCH_BAMCOPY_R_LOCALITY3(b2+128*i);
      }
    }
    char * const dest = blk + mi->block_offset;
    bam_vaddr_t bamva = ((memcpy_info_ptr_t*)mi)->ptr;
    bam1_t *b = read_from_bam_space(p_bam_space_cache, bam_space_id, bamva, sizeof(bam1_t) + l_data, myid);
    const bam1_core_t *c = &b->core;
    const uint32_t block_len = b->l_data + 32;
if (mi_len != 4+32+b->l_data) {
fprintf(stderr, "(E) [%d] bamva id %d seqid %d offset %d (mi_len %d - 4 - 32) %d != b->l_data %d %s %d %d\n", myid, bam_vaddr_get_fd(&bamva), bam_vaddr_get_seqid(&bamva), bam_vaddr_get_offset(&bamva), mi_len, mi_len-4-32, b->l_data, bam_get_qname(b), b->core.flag, b->core.pos);
break;
}

    if (b->l_data == 0) {
	dbg_ldata_zero++;
    }

    if (mi->dup) {
      b->core.flag |= BAM_FDUP; /* mark dup */
    }

    /* save info for creating .bai */
    mi->tid = b->core.tid;
    mi->pos = b->core.pos;
    mi->endpos = bam_endpos(b);
    mi->unmap = (0 != (b->core.flag&BAM_FUNMAP));

    /* copy the data as bam_write1() does */
    uint32_t x[8]; 
    x[0] = c->tid;
    x[1] = c->pos;
    x[2] = (uint32_t)c->bin<<16 | c->qual<<8 | c->l_qname;
    x[3] = (uint32_t)c->flag<<16 | c->n_cigar; 
    x[4] = c->l_qseq;
    x[5] = c->mtid;
    x[6] = c->mpos;
    x[7] = c->isize;
    if (bigendian) {
      int i;
      for (i = 0; i < 8; ++i) ed_swap_4p(x + i);
      uint32_t y = block_len;
      ed_swap_4p(&y);
      *(uint32_t*)dest = y;
      swap_data(c, b->l_data, b->data, 1);
    } else {
      *(int32_t*)dest = block_len;
    }
    memcpy(dest + 4, x, 32);
    memcpy(dest + 4 + 32, b->data, b->l_data);
    if (bigendian) swap_data(c, b->l_data, b->data, 0);
    total_len += (4 + 32 + b->l_data);

    if (NULL == bottom) bottom = (bam_free_list_t*)b;
    ((bam_free_list_t*)b)->next = top;
    top = (bam_free_list_t*)b;
  }

  if (!filters.use_baminfo) { 
    md_bam_free(filters.p_bam_free_list2, top, bottom);
  }

  if (total_len != len) {
    static int dump = 0;
    fprintf(stderr, "(E) %p memcopy len %d must be %d (%d l_data==0) \n", mia, total_len, len, dbg_ldata_zero); 
    if (__sync_bool_compare_and_swap(&dump, 0, 1)) {
      memcpy_info_t *mi;
      total_len = 0;
      for (mi=mia->info; !mi->is_end; mi=(memcpy_info_t*)((char*)mi + sizeof(memcpy_info_ptr_t))) {
        char * const dest = blk + mi->block_offset;
	int32_t l_data = *(int32_t*)dest/*block_len*/ - 32;
        bam_vaddr_t bamva = ((memcpy_info_ptr_t*)mi)->ptr;
        total_len += mi->len;
        fprintf(stderr, "  len %d (+%d) -4+32+l_data= %d bamva id %d seqid %d offset %d\n", total_len, mi->len, mi->len-(l_data+36),
	    bam_vaddr_get_fd(&bamva), bam_vaddr_get_seqid(&bamva), bam_vaddr_get_offset(&bamva));
	if (mi->len != l_data+36){
	  uint32_t *x = (uint32_t*)dest;
          fprintf(stderr, "    tid %d pos %d flag %d %.*s\n", x[0], x[1], x[3]>>16, x[2]&0xff, dest+36);
	}
      }
      exit(-1);
    }
  }
}

static inline void mt_hash_enqueue_0(mt_bamcopy_t *bamcopy, bam1_t *b, bam_vaddr_t bamva, uint64_t qnameid, uint64_t bamid, int myid) {
  const vid_t id = gen_id(b, 0);
  const uint32_t offset_unclipped_pos = (0 == (b->core.flag & BAM_FUNMAP) ? get_offset_unclipped_pos(b) : -1);
  const vid_t u_id = (0 == (b->core.flag & BAM_FUNMAP) ? gen_idU(b, 0, offset_unclipped_pos) : 0/*not used*/);

  baminfo1_t baminfo1;
  init_baminfo1(&baminfo1, b, qnameid, bamcopy->header, bamva, bamid, offset_unclipped_pos);
  mt_hash_enqueue(bamcopy->hash, id, u_id, &baminfo1, myid);
}

static void mt_hash_enqueue_per_bams(mt_bamcopy_t *bamcopy, bucket_t *bu, bam_space_t *bams, int myid) {
  bam1_t *b;
  int i = 0;
  for (b=(bam1_t*)&bams->space[0]; (char*)b < bams->curr; b=(bam1_t*)((char*)b+sizeof(*b)+b->m_data)) {
    bam_vaddr_t bamva;
    bam_vaddr_init(&bamva, bams->bam_vaddr_fd, bams->bam_vaddr_seqid, (char*)b - &bams->space[0]);

    mt_hash_enqueue_0(bamcopy, b, bamva, bam_set_qnameid(b, myid), bu->bamctl.bamid.arr[i++], myid);
  }
}

static bam_vaddr_t copy_to_bam_space(mt_bamcopy_t *bamcopy, bucket_t *bu, bam1_t *b, uint64_t bamid, int myid) {
  size_t const b_sz = sizeof(bam1_t) + b->l_data;
  bam_space_t * bams = bu->bamctl.head;
  int const page_out = bamcopy->page_out;

  if (bams == NULL || 
    bams->curr + b_sz > bams->space + sizeof(bams->space) ){
    if (bams && page_out) {
      pageout_bam_space(bamcopy, bams, myid);

      mt_hash_enqueue_per_bams(bamcopy, bu, bams, myid);

      bu->bamctl.bamid.curr = 0;
    }
    if (
#if defined(PAGEOUT_BY_BAMCOPIER)
        NULL == bams || !page_out
#else
        1
#endif
	) {
      bams = bam_space_alloc(bamcopy->page_write ? &bamcopy->page_write->free_list : NULL, myid);
    }
    if (page_out) {
      bams->next = NULL;
    } else {
      bams->next = bu->bamctl.head;
    }
    bu->bamctl.head = bams;

    bam_space_init(bamcopy, bams, myid);

  }

  if (NULL == bu->bamctl.bamid.arr) {
    bu->bamctl.bamid.size = 1;
    bu->bamctl.bamid.curr = 0;
    bu->bamctl.bamid.arr  = md_malloc(bu->bamctl.bamid.size * sizeof(bu->bamctl.bamid.arr[0]), mem_copyToBamSpace, 0);
  } else if (bu->bamctl.bamid.size <= bu->bamctl.bamid.curr) {
    bu->bamctl.bamid.size += 2;
    bu->bamctl.bamid.arr = md_realloc(bu->bamctl.bamid.arr, bu->bamctl.bamid.size * sizeof(bu->bamctl.bamid.arr[0]), mem_copyToBamSpace);
  }
  bu->bamctl.bamid.arr[bu->bamctl.bamid.curr++] = bamid;

  bam1_t * const new_b = (bam1_t*)bams->curr;
  memcpy(new_b, b, sizeof(*b));
  memcpy((uint8_t*)new_b + sizeof(*b), b->data, b_sz - sizeof(*b));
  new_b->data = (uint8_t*)new_b + sizeof(*b);
  new_b->m_data = new_b->l_data;
  // If bam1_t is allocated by bam_init1() and is freed by bam_destroy1(),
  // the cost of realloc() in bam_parse1() is significantly increased.
  // bam_destroy1(b);

  bam_vaddr_t bam_vaddr;
  bam_vaddr_init2(&bam_vaddr, new_b);

  bams->curr += b_sz;

  return bam_vaddr;
}

/*
 * A single thread receives multiple bam1_t records from the main (reader) thread. 
 * It adds them to the two virtual address spaces: the alignment position space and the unclipped position space.
 */
static void *mt_bam_copier(void *data){
  bamcopy_work_t * const w = (bamcopy_work_t*)data;
  mt_bamcopy_t * const mt = w->mt;
  int const thd_idx = w - mt->w;
  bamcopy_queue_t * volatile * const p_w_q = &mt->w[thd_idx].q;

  while(1){
    // obtain a new queue if it was added
    bamcopy_queue_t *q = *p_w_q;
    if (NULL == q) {
      // if there is no queue and w->done then there is no work.
      if(mt->done) goto no_work_for_bam_copier;
      IN_SPIN_WAIT_LOOP();
      continue;
    }
    FINISH_SPIN_WAIT_LOOP();

    if (!__sync_bool_compare_and_swap(p_w_q, q, NULL)) {
      IN_SPIN_WAIT_LOOP();
      continue;
    }
    FINISH_SPIN_WAIT_LOOP();

    bamcopy_queue_t * q2, *prev = NULL, *next;
    // change the order of bamcopy_queue_t entries from LIFO to FIFO so that reads can be traversed as they appear in the input file
    for(q2=q; q2; q2=next){
      next = q2->next;
      q2->next = prev;
      prev = q2;
    }
    q = prev;

    // copy bam reads to the bam space
    for(q2=q; q2; q2=next){
	int i;
	bam_free_list_t * top = NULL, *bottom = (bam_free_list_t*)q2->bam[0].b;
	for(i=0; i<HASH_QUEUE_SIZE; i++) {
	  bam1_t * const orig_b = q2->bam[i].b;
	  if(orig_b){
	    if(i+1<HASH_QUEUE_SIZE && q2->bam[i+1].b){
	      PREFETCH_BAMCOPY_R_LOCALITY0(q2->bam[i+1].b);
	      PREFETCH_BAMCOPY_R_LOCALITY0(q2->bam[i+1].b->data);
	    }

	    const vid_t id = gen_id(orig_b, 0);
	    const int bucket_index = get_index1(id);
	    bucket_t * const bucket = get_bucket(bucket_index, mt->bucket_array, 1);

	    bam_vaddr_t const bamva = copy_to_bam_space(mt, bucket, orig_b, q2->bam[i].id, thd_idx);

	    if (! mt->page_out) {
	      mt_hash_enqueue_0(mt, orig_b, bamva, bam_set_qnameid(orig_b, thd_idx), q2->bam[i].id, thd_idx);
	    }

	    ((bam_free_list_t*)orig_b)->next = top;
	    top = (bam_free_list_t*)orig_b;
	  }else{
	    // end of the queue
	    break;
	  }
	}
	next = q2->next;
	bamcopy_queue_free(q2, &w->bamcopy_queue_free_list);
	md_bam_free(&mt->bam_free_list2, top, bottom);
	mt->w[thd_idx].n_deq++;
    } /* for q2 */
  } /* while 1 */
no_work_for_bam_copier:
{
if (mt->page_out) {
  // page out bam_space_t to which the thread copied bam1_t but did not page out
  int i;
  for(i=getFirstBucketIndex(mt->bucket_array->min, thd_idx, n_bam_copier);
      i <= mt->bucket_array->max; 
      i=getNextBucketIndex(i, thd_idx, n_bam_copier)) {
    bucket_t *bu = get_bucket(i, mt->bucket_array, 0);
    if (bu) {
      // We paged out only the bam_space_t blocks to limit the total amount of the memory consumed by bam_space_t.
      // Keep the bam_space_t blocks that are not paged out on the memory and call mt_hash_enequeue() for them
      bam1_t *b;
      int j = 0;
      bam_space_t * const bams = bu->bamctl.head;
      if (bams) {
        for (b=(bam1_t*)&bams->space[0]; (char*)b < bams->curr; b=(bam1_t*)((char*)b+sizeof(*b)+b->m_data)) {
          bam_vaddr_t bamva;
          bam_vaddr_init2(&bamva, b);

          mt_hash_enqueue_0(mt, b, bamva, bam_set_qnameid(b, thd_idx), bu->bamctl.bamid.arr[j++], thd_idx);
        }
      }
    }
  }
} /* mt->page_out */
}
  return NULL;
}

static mt_bamcopy_t mt_bamcopy;
static mt_bamcopy_t * mt_bamcopy_init(mt_page_write_t *page_write, mt_hash_t *hash, bucket_array_t * bucket_array, bam_hdr_t *header, int paging) { 

  malloc_qnameid_space(n_bam_copier);

  hash->q_buf = md_malloc(sizeof(hash->q_buf[0])*n_bam_copier, mem_bamcopyInit, 0);
  int k;
  for(k=0; k<n_bam_copier; k++){
    hash->q_buf[k] = md_malloc(sizeof(*hash->q_buf[0])*n_hash_adder, mem_bamcopyInit, 0);
    int j;
    for(j=0; j<n_hash_adder; j++){
      hash->q_buf[k][j].enq.i = 0;
      hash->q_buf[k][j].enq.q = hash_queue_malloc(&hash->w[j].hash_queue_free_list);
      hash->q_buf[k][j].enqU.i = 0;
      hash->q_buf[k][j].enqU.q = hash_queue_malloc(&hash->w[j].hash_queue_free_list);
    }
  }

  pthread_attr_t attr;
  pthread_attr_init(&attr);
  mt_bamcopy_t * const bamcopy = &mt_bamcopy;
  bamcopy->page_write = page_write; 
  bamcopy->hash = hash;
  bamcopy->header = header;
  bamcopy->bucket_array = bucket_array;
  bamcopy->page_out = paging;
//  bamcopy->max_cache_size = _max_mem;
//  bamcopy->cur_cache_size = 0;
  bam_free_list2_init(&bamcopy->bam_free_list2);
  /*
  * initialized by the submitter
  bamcopy->enq = md_malloc(sizeof(bamcopy->enq[0])*n_bam_copier, mem_bamcopyInit, 0);
  for(i=0; i<n_bam_copier; i++){
    bamcopy->enq[i].i = 0;
    bamcopy->enq[i].q = bamcopy_queue_malloc();
  }
  */
  int i;
  bamcopy->w = md_malloc(sizeof(bamcopy->w[0])*n_bam_copier, mem_bamcopyInit, 1);

  if (paging) {
    pagefile_init2(bamcopy);
  }

  const int cpu_vec_len = (n_physical_cores*SMT+63)/64;
  uint64_t cpu_vec[cpu_vec_len];
  memset(cpu_vec, 0, sizeof(uint64_t)*cpu_vec_len);
  for(i=0; i<n_bam_copier; i++){
    bamcopy->w[i].mt = bamcopy;
    pthread_create(&bamcopy->w[i].tid, &attr, mt_bam_copier, &bamcopy->w[i]);
    char tn[32];
    sprintf(tn, "bam_virt-%d-%d\n", i, CPU_BAMCOPY(i));
    if(pthread_setname_np(bamcopy->w[i].tid, tn)){}
#if defined(CPU_SET_AFFINITY)
{
      cpu_set_t cpuset;
      CPU_ZERO(&cpuset);
      CPU_SET2(CPU_BAMCOPY(i), &cpuset);
      cpu_vec[CPU_BAMCOPY(i)/64] |= 1LL<<(63-CPU_BAMCOPY(i)%64);
      pthread_setaffinity_np(bamcopy->w[i].tid, sizeof(cpu_set_t), &cpuset);
}
#endif
  }
  fprintf(stderr, "(I) CPU %15s ", "bam_virt");
  for(i=0; i<cpu_vec_len; i++) {
    int j;
    for(j=0; j<64; j++) {
      fprintf(stderr, "%s%c", j%SMT ? "" : " ", (cpu_vec[i]&(1LL<<(63-j))) ? 'o' : '-');
    }
  }
  fprintf(stderr, "\n");

  return &mt_bamcopy;
}

static inline void mt_bamcopy_enqueue(mt_bamcopy_t *bamcopy, bam1_t *b, uint64_t bamid, int myid) {
  const vid_t id = gen_id(b, 0);
  const int bucket_index = get_index1(id);

  const int thd_idx = getThreadIndexForBucket(bucket_index, n_bam_copier);

  bamcopy_enq_t * const enq = &bamcopy->q_buf[myid][thd_idx];

  // check if the queue is full
  if(HASH_QUEUE_SIZE == enq->i) {
    // send the queue to the worker thread and allocate a new queue
    bamcopy_queue_flush(bamcopy, thd_idx, myid);
    enq->i = 0;
    enq->q = bamcopy_queue_malloc(&bamcopy->w[thd_idx].bamcopy_queue_free_list);
  }
  // add the bam to the queue
  enq->q->bam[enq->i].b = b;
  enq->q->bam[enq->i].id = bamid;
  enq->i++;
}

static void mt_bamcopy_end(mt_bamcopy_t *bamcopy) {
  int i,j;
  for(i=0;  i<n_bam_copier; i++) {
  for(j=0;  j<n_sam_parser; j++) {
    // add the bams that are not added yet to the queue
    if (bamcopy->q_buf[j][i].i < HASH_QUEUE_SIZE) {
	bamcopy->q_buf[j][i].q->bam[bamcopy->q_buf[j][i].i].b = NULL; // null terminated
    }
    bamcopy_queue_flush(bamcopy, i, j);
  }
  }
  __sync_synchronize();

  bamcopy->done = 1;

  for (i=0; i<n_bam_copier; i++) {
    pthread_join(bamcopy->w[i].tid, 0);
    bamcopy_queue_reclaim(&bamcopy->w[i].bamcopy_queue_free_list);
  }
  __sync_synchronize();

  mt_hash_end(bamcopy->hash);

  mt_page_write.done = 1;
  pagefile_end();

}

#define DEFAULT_BAMWRITE_QUEUE_SIZE 64
typedef struct bamwrite_queue {
  struct bamwrite_queue *next;
  int size;
  int len;
  struct {
    bam1_t *b;
    uint64_t id;
  } bam[];
} bamwrite_queue_t;

typedef struct {
  bamwrite_queue_t *q; // queue
} bamwrite_enq_t;

typedef struct {
  pthread_t tid;
  struct mt_bamwrite *mt;
  volatile size_t n_enq, n_deq, n_spn;
  bamwrite_queue_t * volatile bamwrite_queue_free_list;
  bamwrite_queue_t * volatile q[]; 	// submitted queue
} bamwrite_work_t;

#define BAMWRITE_QUEUE_ARRAY_SIZE (1024*1024)
typedef struct {
  volatile long first;
  volatile long last;
  long size;
  bamwrite_queue_t *arr[];
} bamwrite_queue_array_t;

typedef struct mt_bamwrite {
  bamwrite_queue_array_t *ordered_q;
  samFile *out_fp;
  bam_hdr_t *header;
  bamwrite_work_t *w;
  bamwrite_enq_t **q_buf;		// submitter's local queue
  int done;
  bam_free_list2_t bam_free_list2;
  long prof_n_spn;
} mt_bamwrite_t;

static bamwrite_queue_t *bamwrite_queue_malloc(bamwrite_queue_t* volatile *p_free_list) {
#if 1
  bamwrite_queue_t * q = *p_free_list;
  while(q) {
    if (__sync_bool_compare_and_swap(p_free_list,q,q->next)){
      break;
    }
    q = *p_free_list;
    IN_SPIN_WAIT_LOOP();
  }
  FINISH_SPIN_WAIT_LOOP();

  if (NULL == q) {
    size_t const s = DEFAULT_BAMWRITE_QUEUE_SIZE;
    q = md_malloc(sizeof(bamwrite_queue_t)+sizeof(q->bam[1])*s, mem_bamwriteQueueMalloc, 0);
    q->size = s;
  }
#else
  bamwrite_queue_t * const q = md_malloc(sizeof(bamwrite_queue_t), mem_bamwriteQueueMalloc, 0);
#endif
  q->next = NULL;
  q->len = 0;
  return q;
}
static void bamwrite_queue_free(bamwrite_queue_t *p, bamwrite_queue_t* volatile *p_free_list) {
#if 1
  while(1) {
    bamwrite_queue_t * q = *p_free_list;
    p->next = q;
    if (__sync_bool_compare_and_swap(p_free_list,q,p)){
      break;
    }
    IN_SPIN_WAIT_LOOP();
  }
  FINISH_SPIN_WAIT_LOOP();
#else
  md_free(p, sizeof(bamwrite_queue_t), mem_bamwriteQueueMalloc);
#endif
}
static void bamwrite_queue_reclaim( bamwrite_queue_t* volatile *p_free_list) {
#if 1
  bamwrite_queue_t *q, *next;
  for(q=*p_free_list; q; q=next){
    next = q->next;
    md_free(q, sizeof(bamwrite_queue_t), mem_bamwriteQueueMalloc);
  }
#else
#endif
}

#define MAX_BAMWRITE_QLEN (1000*1000)
#define BAMWRITE_THROTTLE

static void bamwrite_queue_flush(mt_bamwrite_t *bamwrite, int thread, int myid){
  bamwrite_queue_t * const q = bamwrite->q_buf[myid][thread].q;
  //bamwrite_queue_t * volatile * const p_w_q = &bamwrite->w[thread].q[myid];
  long volatile * const p_first = &bamwrite->ordered_q->first;
  long volatile * const p_last = &bamwrite->ordered_q->last;
  uint64_t spn = 0;
  uint32_t const block_seqid = (uint32_t)(q->bam[0].id>>32);
//fprintf(stderr, "bamwrt_q_flush: myid %d %p [id %d] #enq %ld #deq %ld...\n", myid, q, block_seqid, bamwrite->w[thread].n_enq, bamwrite->w[thread].n_deq );

  __atomic_thread_fence(__ATOMIC_RELEASE);

  long const ordered_q_size = bamwrite->ordered_q->size;

#if defined(BAMWRITE_THROTTLE) 
  while(1){
    long const nq = bamwrite->w[thread].n_enq - bamwrite->w[thread].n_deq;
    if(nq > MAX_BAMWRITE_QLEN) {
      IN_SPIN_WAIT_LOOP();
      spn++;
      continue;
    }
    break;
  }
  FINISH_SPIN_WAIT_LOOP();
#endif
  {
    long last = *p_last;
    long first = *p_first;
    if (block_seqid < last) {
//fprintf(stderr, "flush [quick] %d q %p id %d first %ld last %ld [local id %ld first %ld last %ld]\n", myid, q, block_seqid, first, last, block_seqid%ordered_q_size, first%ordered_q_size, last%ordered_q_size);
      bamwrite->ordered_q->arr[block_seqid % ordered_q_size] = q;
    } else {
      // check if we can add q by updating last
//fprintf(stderr, "flush [loop1] %d q %p id %d first %ld last %ld [local id %ld first %ld last %ld]\n", myid, q, block_seqid, first, last, block_seqid%ordered_q_size, first%ordered_q_size, last%ordered_q_size);
      while(1) {
        if (block_seqid < first + ordered_q_size) {
	  // block_seqid can be in the array. update last.
	  while(1){
            if (__sync_bool_compare_and_swap(p_last,last,block_seqid)) {
              break;
            }
	    last = *p_last;
            IN_SPIN_WAIT_LOOP();
	  }
          FINISH_SPIN_WAIT_LOOP();
          bamwrite->ordered_q->arr[block_seqid % ordered_q_size] = q;
//fprintf(stderr, "flush [loop2] %d q %p id %d first %ld last %ld [local id %ld first %ld last %ld]\n", myid, q, block_seqid, first, last, block_seqid%ordered_q_size, first%ordered_q_size, last%ordered_q_size);
	  break;
	}
	first = *p_first;
//fprintf(stderr, "flush [loop*] %d q %p id %d first %ld last %ld [local id %ld first %ld last %ld]\n", myid, q, block_seqid, first, last, block_seqid%ordered_q_size, first%ordered_q_size, last%ordered_q_size);
      }/*while 1*/
    }
  }
  __atomic_fetch_add(&bamwrite->w[thread].n_enq, 1, __ATOMIC_RELAXED);
  //if (bamwrite->prof_n_spn) 
  __atomic_fetch_add(&bamwrite->w[thread].n_spn, spn, __ATOMIC_RELAXED);
//  fprintf(stderr, "DONE bamwrt_q_flush: myid %d %p [id %d] #enq %ld #deq %ld...\n", myid, q, block_seqid, bamwrite->w[thread].n_enq, bamwrite->w[thread].n_deq );
}

static inline void mt_bamwrite_enqueue(mt_bamwrite_t *bamwrite, bam1_t *b, uint64_t bamid, int myid) {
  uint32_t const block_seqid = (uint32_t)(bamid>>32);

  bamwrite_enq_t * const enq = &bamwrite->q_buf[myid][0]; // n_bam_writer must be 1

  if (enq->q->len > 0 && block_seqid != (uint32_t)(enq->q->bam[0].id>>32)) {
    // send the queue to the worker thread and allocate a new queue
    bamwrite_queue_flush(bamwrite, 0, myid);
    enq->q = bamwrite_queue_malloc(&bamwrite->w[0].bamwrite_queue_free_list);
    enq->q->len = 0;
  }

  // ensure the queue size
  if(enq->q->size == enq->q->len) {
    enq->q->size += 32;
    enq->q = md_realloc(enq->q, sizeof(bamwrite_queue_t)+sizeof(enq->q->bam[1])*enq->q->size, mem_bamwriteQueueMalloc);
  }

  // add the bam to the queue
  enq->q->bam[enq->q->len].b = b;
  enq->q->bam[enq->q->len].id = bamid;
  enq->q->len++;
}

static void mt_bamwrite_end(mt_bamwrite_t *bamwrite) {
  int i,j;
  for(i=0;  i<n_bam_writer; i++) {
  for(j=0;  j<n_sam_parser; j++) {
    // add the bams that are not added yet to the queue
    bamwrite_queue_flush(bamwrite, i, j);
  }
  }
  __sync_synchronize();

  bamwrite->done = 1;

  for(i=0; i<n_bam_writer; i++) {
    pthread_join(bamwrite->w[i].tid, 0);
    bamwrite_queue_reclaim(&bamwrite->w[i].bamwrite_queue_free_list);
  }
  __sync_synchronize();

}


typedef struct {
  uint32_t max;
  uint32_t n_lines;
  uint32_t pos[];
} line_pos_t;

#define DEFAULT_BLKRD_SIZE 	(BGZF_MAX_BLOCK_SIZE+MAX_LINE_LENGTH) // bytes
typedef struct blkrd {
  struct blkrd *q_next;
  line_pos_t *line_pos;
  uint32_t block_len;
  uint32_t block_seqid;
  char block[DEFAULT_BLKRD_SIZE];
} blkrd_t;

typedef struct blkrd_free_list {
  struct blkrd_free_list *next;
} blkrd_free_list_t;

typedef struct {
  blkrd_free_list_t **blkrd_free_list1 __attribute__((aligned(128))); 
  blkrd_free_list_t *blkrd_free_list2 __attribute__((aligned(128))); 
} blkrd_free_list2_t;

static void md_blkrd_free(blkrd_free_list2_t *l2, blkrd_free_list_t *top, blkrd_free_list_t *bottom);

#define SAMPARSE_QUEUE_SIZE 1
typedef struct samparse_queue {
  struct samparse_queue *next;
  int line_splitter_id;
  struct {
    blkrd_t *blkrd;
  } sam2bam[SAMPARSE_QUEUE_SIZE];
} samparse_queue_t;

static samparse_queue_t *samparse_queue_malloc() {
  samparse_queue_t * const q = md_malloc(sizeof(samparse_queue_t), mem_samparseQueueMalloc, 0);
  q->next = NULL;
  return q;
}

static void samparse_queue_free(samparse_queue_t *p) {
  md_free(p, sizeof(samparse_queue_t), mem_samparseQueueMalloc);
}

typedef struct {
  pthread_t tid;
  struct mt_samparse *mt;
  samparse_queue_t * volatile q;
  volatile size_t n_enq, n_deq, n_spn;
  struct unmap_reads *ur;
  size_t ur_count;
} samparse_work_t;

typedef struct {
  int i;		// current index of queue
  samparse_queue_t *q;	// queue
} samparse_enq_t;

typedef struct mt_bamwrite mt_bamwrite_t;

typedef struct mt_samparse {
  mt_bamcopy_t *bamcopy;
  mt_bamwrite_t *bamwrite;
  samparse_enq_t *my_q;
  samparse_work_t *w;
  bam_free_list2_t *bam_free_list2;
  blkrd_free_list2_t *blkrd_free_list2;
  int done;
  samFile *fp_out;
  bam_hdr_t *header;
  long prof_n_spn;
} mt_samparse_t;

#define MAX_SAMPARSE_QLEN 1000
#define SAMPARSE_THROTTLE
static void samparse_queue_flush(mt_samparse_t *samparse, int myid){
  static int thread = 0; //FIXME
  samparse_queue_t * const q = samparse->my_q[myid].q;
  int tx = thread;
  samparse_queue_t * volatile * p_w_q = &samparse->w[tx].q;
  uint64_t spn = 0;
  q->line_splitter_id = myid; // used for creating the FIFO list of unmapped reads 

  __atomic_thread_fence(__ATOMIC_RELEASE);

  while(1){
#if defined(SAMPARSE_THROTTLE) 
    if(samparse->w[tx].n_enq - samparse->w[tx].n_deq > MAX_SAMPARSE_QLEN) {
      tx = (tx + 1 == n_sam_parser) ? 0 : tx + 1;
      p_w_q = &samparse->w[tx].q;
      IN_SPIN_WAIT_LOOP();
      spn++;
      continue;
    }
    FINISH_SPIN_WAIT_LOOP();
#endif
    samparse_queue_t * const oldq = *p_w_q;
    q->next = oldq;
    if (__sync_bool_compare_and_swap(p_w_q,oldq,q)) {
      break;
    }
  }
  thread = tx;
  __atomic_fetch_add(&samparse->w[tx].n_enq, 1, __ATOMIC_RELAXED);
  //if (samparse->prof_n_spn) 
  __atomic_fetch_add(&samparse->w[tx].n_spn, spn, __ATOMIC_RELAXED);
}

#define UNMAP_READ_ARRAY_LEN 4096
typedef struct unmap_reads {
  struct unmap_reads *next;
  long index;
  bam1_t *reads[];
} unmap_reads_t;

typedef struct {
  long samparser_idx;
  unmap_reads_t *unmap_reads_ptr;
  long unmap_reads_idx;
  unmap_reads_t *unmap_reads[];
} usi_unsorted_t;

typedef struct {
  size_t curr;
  size_t size;
  bam1_t *b[];
} usi_sorted_t;

typedef struct unmap_space_iter {
  long is_sorted;
  union {
    usi_unsorted_t unsorted[1];
    usi_sorted_t   sorted[1];
  };
} unmap_space_iter_t;

#define iter_samparser_idx(iter)	((iter)->unsorted[0].samparser_idx)
#define iter_unmap_reads_ptr(iter)	((iter)->unsorted[0].unmap_reads_ptr)
#define iter_unmap_reads_idx(iter)	((iter)->unsorted[0].unmap_reads_idx)
#define iter_unmap_reads(iter)	((iter)->unsorted[0].unmap_reads)

#define iter_curr(iter)		((iter)->sorted[0].curr)
#define iter_size(iter)		((iter)->sorted[0].size)
#define iter_b(iter)		((iter)->sorted[0].b)

static int mt_wrt_throughput_offset = 0;

static void
save_unmapped_reads(samparse_work_t *w, bam1_t *b) {
  if(NULL == w->ur || 				// no unmap_reads_t allocated yet since b is the first unmap read for the current sam parser
    w->ur->index >= UNMAP_READ_ARRAY_LEN) 	// no space in the current unmap_reads_t
  {
    unmap_reads_t *t = md_malloc(sizeof(*w->ur)+sizeof(w->ur->reads[0])*UNMAP_READ_ARRAY_LEN, mem_saveUnmapReads, 1);
    t->next = w->ur;
    w->ur = t;
  }
  w->ur->reads[w->ur->index++] = b;
  w->ur_count++;
}


static int unmap_cmpfunc(const void *p1, const void *p2){
  bam1_t * const b1 = *(bam1_t **)p1;
  bam1_t * const b2 = *(bam1_t **)p2;
//
// Emulate SortSam (SAMRecordCoordinateComparator)
//
//			Value
//	1. RNAME index	"*"
//	2. POS		0
//	3. FLAG reverse	0
//	4. QNAME
//	5. FLAG
//	6. MAPQ		0
//	7. RNEXT index	"*"
//	8. PNEXT 	0
//	9. TLEN 	0
//

//	4. QNAME
  int r_qname = memcmp(bam_get_qname(b1), bam_get_qname(b2), b1->core.l_qname < b2->core.l_qname ? b1->core.l_qname : b2->core.l_qname);
  if (0 == r_qname) {
    r_qname = (b1->core.l_qname - b2->core.l_qname);
  }
#if 0 && defined(DEBUG_POS)
  if (dump) {
    fprintf(stderr, "(I) cmpfunc read %.*s %c %.*s\n",  (int)len1, str1, r_qname==0?'=':(r_qname>0?'<':'>'), (int)len2, str2);
  }
#endif
  if (r_qname) return r_qname;

//	5. FLAG
  int const r_flag = b1->core.flag - b2->core.flag;
#if 0 && defined(DEBUG_POS)
  if (dump) {
    fprintf(stderr, "(I) cmpfunc flag %u %c %u\n", flag1, r_flag==0?'=':(r_flag>0?'<':'>'), flag2);
  }
#endif
  if (r_flag) return r_flag;

  return 0;
}


static unmap_space_iter_t *
init_unmap_space_iter(mt_samparse_t *samparse) {
  unmap_space_iter_t *iter = NULL;
  size_t count = 0;

  size_t i;
  for (i=0; i<n_sam_parser; i++) {
    count += samparse->w[i].ur_count;
  }

  if (count > 0) {
    if (g_unsort) {
      iter = md_malloc(sizeof(unmap_space_iter_t)+sizeof(unmap_reads_t*)*n_sam_parser, mem_initUnmapSpaceIter, 0);
      iter->is_sorted = 0;
      for (i=0; i<n_sam_parser; i++) {
        iter_unmap_reads(iter)[i] = samparse->w[i].ur;
      }
      iter_samparser_idx(iter) = 0;						// 1st parser
      iter_unmap_reads_ptr(iter) = iter_unmap_reads(iter)[iter_samparser_idx(iter)];	// 1st unmap_reads_t (or NULL if there are no unmap reads)
      iter_unmap_reads_idx(iter) = 0;						// unmap_reads[0]
    } else {
      // !g_unsort
      iter = md_malloc(sizeof(unmap_space_iter_t)+sizeof(bam1_t*)*count, mem_initUnmapSpaceIter, 0);
      iter->is_sorted = 1;
      iter_curr(iter) = 0;
      iter_size(iter) = count;
      size_t k = 0;
      for (i=0; i<n_sam_parser; i++) {
        unmap_reads_t *ur;
        for (ur=samparse->w[i].ur; ur; ur=ur->next) {
          long j;
          for (j=0; j<ur->index; j++) {
            iter_b(iter)[k++] = ur->reads[j];
          }
        }
      }
      if (count != k) {
        fprintf(stderr, "(E) init_unmap_space_iter: k %ld must be %ld\n", k, count);
        exit(-1);
      }
      struct timeval sort_start;
      gettimeofday(&sort_start, NULL);
      // sort the list of unmap_list_t nodes using block_seqid as the key
      qsort(iter_b(iter), count, sizeof(iter_b(iter)[0]), unmap_cmpfunc);

      struct timeval sort_end;
      gettimeofday(&sort_end, NULL);
      double sort_fin = sort_end.tv_sec-sort_start.tv_sec +(sort_end.tv_usec-sort_start.tv_usec)/1000000.0;
      fprintf(stderr, "(I) sort unmap POS=0 reads (#reads %ld time %.1f sec)\n", count, sort_fin);
    }
    mt_wrt_throughput_offset ++;
  }
  return iter;
}

static bam1_t *
get_unmapped_reads(unmap_space_iter_t *iter) {
  if (NULL == iter) return NULL;

  bam1_t *b = NULL;
  if (0 == iter->is_sorted) {
    while (1) {
      unmap_reads_t * ur_ptr = iter_unmap_reads_ptr(iter);
      if (NULL != ur_ptr) {
        if (iter_unmap_reads_idx(iter) < ur_ptr->index) {
          b = ur_ptr->reads[iter_unmap_reads_idx(iter)++];
          break;
        } else {
          iter_unmap_reads_ptr(iter) = ur_ptr->next;
          iter_unmap_reads_idx(iter) = 0;
        }
      } else {
        // ur_ptr == null
        iter_samparser_idx(iter)++;
        if (iter_samparser_idx(iter) < n_sam_parser) {
          iter_unmap_reads_ptr(iter) = iter_unmap_reads(iter)[iter_samparser_idx(iter)];
          iter_unmap_reads_idx(iter) = 0;
        } else {
          // no next bam1_t
          break;
        }
      }
    }
  } else {
    if (iter_curr(iter) < iter_size(iter)) {
      b = iter_b(iter)[iter_curr(iter)++];
    }
  }
  return b;
}


#if 0
#define UNMAP_SPACE_SIZE	BAM_SPACE_SIZE	// must be the same size as bam_space_t space[] to be addressed by bam_vaddr_t seqid 
typedef struct unmap_space {
  struct unmap_space *next;
  char *curr;
  char space[UNMAP_SPACE_SIZE];
} unmap_space_t;

typedef struct unmap_list {
  struct unmap_list *next;
  //unmap_space_t *us;
  uint32_t block_seqid;
  int count;
  bam1_t bam[];
} unmap_list_t;

typedef struct unmap_fifo {
  unmap_space_t *us_head, *us_tail;
  struct unmap_list  *last_unmap;
} unmap_fifo_t;

/*
 * Create a list of bam records so that they can be appended to the output
 * (1) Each sam parser receives a sequence of lines in a 64-KB block
 * (2) Each sam parser parses them and finds the BAM records whose POSs are -1
 * (3) Each sam parser saves the BAM records in a 64-KB block in the local samparse_work_t with block_seqid of the block
 * (4) Each sam parser creates a list of the block_seqids described in 3 (their block_seqids appear unsorted)
 *     parser 1: block_seqid 1 -> 5 -> 4 -> ..
 *     parser 2: block_seqid 3 -> 2 -> 6 -> ..
 * (5) When the sam parsers finish, create a single list of bam records that is sorted by bamid
 *     This merge sort can be done as a background task.
 *
 *   unmap_fifo_t
 *   .last_unmap----------------------------------------------------------+
 *   .us_tail-----------------------------------------------------------+ |
 *   .us_head---+							| |
 *   		|      							| |
 *   +----------+							| |
 *   v									v v
 *   unmap_space_t							unmap_space_t
 *   .next ------------------------------------------------------------>.next=NULL
 *   .space								.space
 *   unmap_list_t	bam0 ... bam19	unmap_list_t	bam0 bam1	unmap_list_t	bam0 ... bam11
 *   .next ---------------------------->.next ------------------------->.next=NULL
 *   .block_seqid=10			.block_seqid=11			.block_seqid=11
 *   .count=20				.count=2			.count=12
 */
static void
save_unmapped_reads(samparse_work_t *w, bam1_t *b, uint32_t block_seqid, int line_splitter_id) {
  unmap_fifo_t * const fifo = &w->us[line_splitter_id];
  long new_unmap_space = 0;
  long new_unmap_list_node = 0;
  unmap_space_t * us = fifo->us_tail;
  size_t b_sz = sizeof(bam1_t) + b->l_data;

  if (NULL == us /* empty fifo */|| us->curr + b_sz > us->space + sizeof(us->space) /* no space in the current unmap_space_t */) {
    new_unmap_space = 1;
  }
  if (new_unmap_space || NULL == fifo->last_unmap /* no unmap_list created yet */|| block_seqid != fifo->last_unmap->block_seqid /* new block_seqid */){
    new_unmap_list_node = 1;
    b_sz += sizeof(unmap_list_t);
    // check size again
    if (NULL != us && us->curr + b_sz > us->space + sizeof(us->space)){
      new_unmap_space = 1;
    }
  }
  if (new_unmap_space){
    // allocate a new unmap_space_t
    us = md_malloc(sizeof(*us), mem_saveUnmappedReads, 0);
    us->curr = &us->space[0];
    us->next = NULL;
    if (NULL != fifo->us_tail) fifo->us_tail->next = us;
    fifo->us_tail = us;
    if (NULL == fifo->us_head) fifo->us_head = us;
  }
  bam1_t *new_b;
  if (new_unmap_list_node) {
    unmap_list_t *new_last = (unmap_list_t*)us->curr;
    if (fifo->last_unmap)
      fifo->last_unmap->next = new_last;
    new_last->next = NULL;
    //new_last->us = u;
    new_last->block_seqid = block_seqid;
    new_last->count = 0;
    fifo->last_unmap = new_last;
    new_b = (bam1_t*)(us->curr + sizeof(unmap_list_t));
  } else {
    new_b = (bam1_t*)us->curr;
  }
  fifo->last_unmap->count++;
  memcpy(new_b, b, sizeof(*b));
  memcpy((uint8_t*)new_b + sizeof(*b), b->data, b->l_data);
  new_b->data = (uint8_t*)new_b + sizeof(*b);
  new_b->m_data = new_b->l_data;
  us->curr += b_sz;

}

typedef struct {
  long total;
  long size;
  long i;
  long size2;
  long i2;
  bam1_t *b;
  unmap_list_t *top[];
} unmap_space_iter_t;

static int unmap_cmpfunc(const void *p1, const void *p2){
  unmap_list_t * const u1 = *(unmap_list_t**)p1;
  unmap_list_t * const u2 = *(unmap_list_t**)p2;
  return u1->block_seqid - u2->block_seqid;
}

static int picard_sortsam_cmpfunc(const void * v1, const void * v2, void * args) ;

/*
 * create an iterator
 */
static unmap_space_iter_t *
init_unmap_space_iter(mt_samparse_t *samparse) {
  // count the total number of unmap_list_t nodes
  samparse_work_t * const w = samparse->w;
  long count = 0;
  int i;
  for (i=0; i<n_sam_parser; i++) {
    int j;
    for (j=0; j<n_line_splitter; j++) {
      if(w[i].us[j].us_head){
        unmap_list_t *p;
        for(p=(unmap_list_t*)w[i].us[j].us_head->space; p; p=p->next) {
          count++;
        }
      }
    }
  }
if (count) {
  // create an list of the unmap_list_t nodes
  unmap_space_iter_t *iter = md_malloc(sizeof(*iter)+sizeof(iter->top[0])*count, mem_initUnmapSpaceIter, 0);
  iter->total = 0;
  iter->size = count;
  iter->i = 0;
  int k=0;
  long total=0;
  for (i=0; i<n_sam_parser; i++) {
    int j;
    for (j=0; j<n_line_splitter; j++) {
      if(w[i].us[j].us_head){
        unmap_list_t *p;
        for(p=(unmap_list_t*)w[i].us[j].us_head->space; p; p=p->next) {
          iter->top[k++] = p;
	  total += p->count;
        }
      }
    }
  }
if (!g_unsort) {
  struct timeval sort_start;
  gettimeofday(&sort_start, NULL);
  // sort the list of unmap_list_t nodes using block_seqid as the key
  qsort(iter->top, count, sizeof(iter->top[0]), unmap_cmpfunc);

  struct timeval sort_end;
  gettimeofday(&sort_end, NULL);
  double sort_fin = sort_end.tv_sec-sort_start.tv_sec +(sort_end.tv_usec-sort_start.tv_usec)/1000000.0;
  fprintf(stderr, "(I) sort unmap POS=0 reads (#block_seqid %ld #reads %ld time %.1f sec)\n", count, total, sort_fin);
}
  mt_wrt_throughput_offset ++;

  // initialize an iterator with the node information that has the smallest block_seqid
  unmap_list_t *p = iter->top[0];
  iter->b = &p->bam[0];
  iter->i2 = 0;
  iter->size2 = p->count;
  return iter;
} else {
  return NULL;
}
}

static bam1_t *
get_unmapped_reads(unmap_space_iter_t *iter) {
  if (NULL == iter) return NULL;

  bam1_t *b = iter->b;
  if (b) {
    iter->total++;
    iter->i2++;
    if (iter->i2 < iter->size2) {
      iter->b = (bam1_t*)((char*)b + sizeof(*b) + b->l_data);
    } else {
      iter->i++;
      if (iter->i < iter->size) {
	unmap_list_t *p = iter->top[iter->i];
	iter->b = &p->bam[0];
	iter->i2 = 0;
	iter->size2 = p->count;
      } else {
        iter->b = NULL;
      }
    }
  }
  return b;
}
#endif


/*
 * A single thread receives multiple bam1_t records from the main (reader) thread. 
 * It adds them to the two virtual address spaces: the alignment position space and the unclipped position space.
 */
static void *mt_sam_parser(void *data){
  samparse_work_t * const w = (samparse_work_t*)data;
  mt_samparse_t * const mt = w->mt;
  int const myid = w - mt->w;
  register bam_free_list_t ** const my_bamfl = &mt->bam_free_list2->bam_free_list1[myid];

  while(1){
    // obtain a new queue if it was added
    samparse_queue_t * q;
    while(1){
      q = w->q;
      if (q) {
        if (__sync_bool_compare_and_swap(&w->q, q, NULL)) {
	  break;
	}
      } else {
	// if there is no queue and w->done then there is no work.
	if(mt->done) goto no_work;
        IN_SPIN_WAIT_LOOP();
      }
    }
    FINISH_SPIN_WAIT_LOOP();
    samparse_queue_t * q2, *prev = NULL, *next;
    // change the order of samparse_queue_t entries from LIFO to FIFO so that reads can be traversed as they appear in the input file
    for(q2=q; q2; q2=next){
	next = q2->next;
	q2->next = prev;
	prev = q2;
    }
    q = prev;
    // add bam reads to the samparse

    for(q2=q; q2; q2=next){
      int i; 
      blkrd_free_list_t * top = NULL, *bottom = (blkrd_free_list_t*)q2->sam2bam[0].blkrd;
      for(i=0; i<SAMPARSE_QUEUE_SIZE; i++) {
	blkrd_t * const blkrd = q2->sam2bam[i].blkrd;
	if(blkrd){
	  line_pos_t * const line_pos = blkrd->line_pos;
	  uint32_t bamid_low = 0;
	  int j;
	  /* sam -> bam */
	  for(j=0; j<line_pos->n_lines; j++, bamid_low++) {
	    bam1_t * const b = md_bam_alloc(mt->bam_free_list2, my_bamfl);
	    kstring_t line;
	    line.s = blkrd->block + line_pos->pos[j];
	    line.m = line.l = (j < line_pos->n_lines-1 ? line_pos->pos[j+1] : blkrd->block_len) - line_pos->pos[j] - 1 /* not include a delimiter */;
	    int ret = sam_parse1(&line, mt->bamcopy ? mt->bamcopy->header : mt->bamwrite->header, b);
	    if (ret < 0) {
	      if (hts_verbose >= 1) fprintf(stderr, "[W::%s] parse error %.*s\n", __func__, (int)line.l, line.s); 
	      if (mt->bamcopy->header->ignore_sam_err) continue;
	    }
	    uint64_t const bamid = ((uint64_t)blkrd->block_seqid)<<32 | bamid_low; 

// perform read filters
            long include = 1;
{
  int ii;
  for(ii=0; ii<filters.funcs[api_pre_filter].n; ii++) {
    if ( 0 == (*filters.funcs[api_pre_filter].funcs[ii].func)(b) ) {
      // exclude
      include = 0;
      break;
    }
  }
}
if (filters.use_baminfo) { 
            if (include) {
	      // exclude unmapped reads that have POS 0 since they are not analyzed and are apppended to the sorted list of reads
	      if (0 != (b->core.flag & BAM_FUNMAP) && -1 == b->core.pos) {
	        save_unmapped_reads(w, b);
	        //save_unmapped_reads(w, b, blkrd->block_seqid, q2->line_splitter_id);
	      } else {
	        mt_bamcopy_enqueue(mt->bamcopy, b, bamid, myid);
	      }
	    }
} else {
            if (include) {
              mt_bamwrite_enqueue(mt->bamwrite, b, bamid, myid);
	    } else {
              mt_bamwrite_enqueue(mt->bamwrite, NULL, bamid, myid);
	    }
            //sam_write3(mt->fp_out, mt->header, b);
}
	  }

	  ((blkrd_free_list_t*)blkrd)->next = top; 
	  top = (blkrd_free_list_t*)blkrd;
	}else{
	  // end of the queue
	  break;
	}
      }
      next = q2->next;
      samparse_queue_free(q2);
      md_blkrd_free(mt->blkrd_free_list2, top, bottom);
      w->n_deq ++;
    }
  }
no_work:
  return NULL;
}

static mt_samparse_t mt_samparse;
static mt_samparse_t * mt_samparse_init(mt_bamcopy_t *p_bamcopy, mt_bamwrite_t *p_bamwrite, bam_hdr_t *header, samFile *fp_out) {
if (p_bamcopy) {
  p_bamcopy->q_buf = md_malloc(sizeof(p_bamcopy->q_buf[0])*n_sam_parser, mem_samparseInit, 0);
  int k;
  for(k=0; k<n_sam_parser; k++){
    p_bamcopy->q_buf[k] = md_malloc(sizeof(*p_bamcopy->q_buf[0])*n_bam_copier, mem_samparseInit, 0);
    int j;
    for(j=0; j<n_bam_copier; j++){
      p_bamcopy->q_buf[k][j].i = 0;
      p_bamcopy->q_buf[k][j].q = bamcopy_queue_malloc(&p_bamcopy->w[j].bamcopy_queue_free_list);
    }
  }
}
if (p_bamwrite) {
  p_bamwrite->q_buf = md_malloc(sizeof(p_bamwrite->q_buf[0])*n_sam_parser, mem_samparseInit, 0);
  int k;
  for(k=0; k<n_sam_parser; k++){
    p_bamwrite->q_buf[k] = md_malloc(sizeof(*p_bamwrite->q_buf[0])*n_bam_writer, mem_samparseInit, 0);
    int j;
    for(j=0; j<n_bam_writer; j++){
      p_bamwrite->q_buf[k][j].q = bamwrite_queue_malloc(&p_bamwrite->w[j].bamwrite_queue_free_list);
    }
  }
}

  mt_samparse_t * const samparse = &mt_samparse;
  samparse->bamcopy = p_bamcopy;
  samparse->bamwrite = p_bamwrite;
  samparse->w = md_malloc(sizeof(samparse->w[0])*n_sam_parser, mem_samparseInit, 1);
  samparse->bam_free_list2 = (p_bamcopy ? &p_bamcopy->bam_free_list2 : &p_bamwrite->bam_free_list2);
  //samparse->blkrd_free_list2 = p_bamcopy->blkrd_free_list2;
  samparse->header = header;
  samparse->fp_out = fp_out;

  pthread_attr_t attr;
  pthread_attr_init(&attr);
  int i;
  const int cpu_vec_len = (n_physical_cores*SMT+63)/64;
  uint64_t cpu_vec[cpu_vec_len];
  memset(cpu_vec, 0, sizeof(uint64_t)*cpu_vec_len);
  for(i=0; i<n_sam_parser; i++){
    //samparse->w[i].us = md_malloc(sizeof(samparse->w[i].us[0])*n_line_splitter, mem_samparseInit, 1);
    samparse->w[i].mt = samparse;
    pthread_create(&samparse->w[i].tid, &attr, mt_sam_parser, &samparse->w[i]);
    char tn[32];
    sprintf(tn, "parser-%d-%d", i, CPU_SAMPARSE(i));
    if(pthread_setname_np(samparse->w[i].tid, tn)){}
#if defined(CPU_SET_AFFINITY)
{
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
//fprintf(stderr, "CPU_SAMPARSE(%d)=%d n_phys_cores %d use_nproc %d N_CORES %d\n", i, CPU_SAMPARSE(i), n_physical_cores, cpu_map->use_nproc, N_CORES());
    CPU_SET2(CPU_SAMPARSE(i), &cpuset);
    cpu_vec[CPU_SAMPARSE(i)/64] |= 1LL<<(63-CPU_SAMPARSE(i)%64);
    pthread_setaffinity_np(samparse->w[i].tid, sizeof(cpu_set_t), &cpuset);
}
#endif
  }
  fprintf(stderr, "(I) CPU %15s ", "sam_parser");
  for(i=0; i<cpu_vec_len; i++) {
    int j;
    for(j=0; j<64; j++) {
      fprintf(stderr, "%s%c", j%SMT ? "" : " ", (cpu_vec[i]&(1LL<<(63-j))) ? 'o' : '-');
    }
  }
  fprintf(stderr, "\n");
  return &mt_samparse;
}

static inline void mt_samparse_enqueue(mt_samparse_t *samparse, blkrd_t * blkrd, int myid) {
  samparse_enq_t * const myq = &samparse->my_q[myid];
  samparse_queue_t * myq_q = myq->q;
  int myq_i = myq->i;

  // check if the queue is full
  if(SAMPARSE_QUEUE_SIZE == myq_i) {
    // send the queue to the worker thread and allocate a new queue
    samparse_queue_flush(samparse, myid);
    myq_i = 0;
    myq_q = myq->q = samparse_queue_malloc();
  }
  // add the bam to the queue
  myq_q->sam2bam[myq_i].blkrd = blkrd;
  myq->i = myq_i + 1;
}

static void md_blkrd_free2(blkrd_free_list2_t *l2);

static void mt_samparse_end(mt_samparse_t *samparse) {
  int i;
  for(i=0; i<n_line_splitter; i++) {
    // add the bams that are not added yet to the queue
    if (samparse->my_q[i].i < SAMPARSE_QUEUE_SIZE) {
	samparse->my_q[i].q->sam2bam[samparse->my_q[i].i].blkrd = NULL; // null terminated
    }
    samparse_queue_flush(samparse, i);
  }
  __sync_synchronize();

  samparse->done = 1;

  for(i=0; i<n_sam_parser; i++) {
    pthread_join(samparse->w[i].tid, 0);
  }
  md_blkrd_free2(samparse->blkrd_free_list2);

  if (samparse->bamcopy) {
    mt_bamcopy_end(samparse->bamcopy);
  }
  if (samparse->bamwrite) {
    mt_bamwrite_end(samparse->bamwrite);
  }
}

typedef struct mt_find_mate {
  pthread_t *tid;
  struct fm_worker *w;
  bucket_array_t *bucket_array;
  bucket_array_t *bucketU_array;
  // bamptr_t **mate_array_clipped;
  bamptr_t **mate_array_unclipped;
  int n_threads;
  uint64_t chunk_size;
  size_t hash_size;
} mt_find_mate_t;

typedef struct fm_worker {
  mt_find_mate_t *fm;
} fm_worker_t;

#if 0
static int bamid_cmpfuncU(const void *p1, const void *p2){
  bamptr_t * const bamp1 = (bamptr_t*)p1;
  bamptr_t * const bamp2 = (bamptr_t*)p2;

  const int b1_rev = bamptr_get_reverse(bamp1); /* 0 or 1 */
  const int b2_rev = bamptr_get_reverse(bamp2); /* 0 or 1 */
  const int64_t v1 = bamptrU_get_bamid(bamp1) | ((int64_t)b1_rev)<<62;
  const int64_t v2 = bamptrU_get_bamid(bamp2) | ((int64_t)b2_rev)<<62;
  const int64_t cmp = v1 - v2;
  const int ret = (cmp < 0 ? -1 : (cmp > 0 ? 1 : 0));
#if 0
{{{
if(bamptr_get_pos(bamp1,0) == 72516-1 || bamptr_get_pos(bamp2,0) == 72516-1){
fprintf(stderr, "%s pos %d (rev %d bamid_low %ld) bamid %ld (0x%lx) %c %s pos %d (rev %d bamid_low %ld) bamid %ld (0x%lx) cmp %ld (0x%lx) ret %d (0x%x)\n", 
dump_bamptr_get_qname(bamp1,0), bamptr_get_pos(bamp1,0), b1_rev,  bamptrU_get_bamid(bamp1), v1, v1,
ret < 0 ? '<' : (ret > 0 ? '>' : '='),
dump_bamptr_get_qname(bamp2,0), bamptr_get_pos(bamp2,0), b2_rev,  bamptrU_get_bamid(bamp2), v2, v2,
cmp, cmp, ret, ret);
}
}}}
#endif
  return ret;
}
#endif

static volatile long g_mate_finder_barrier = 0;
#if 0
static int qnameid_cmpfunc(const void *p1, const void *p2){
  bamptr_t * const bp1 = *(bamptr_t**)p1;
  bamptr_t * const bp2 = *(bamptr_t**)p2;
  uint64_t const id1 = bamptr_get_qnameid(bp1);
  uint64_t const id2 = bamptr_get_qnameid(bp2);
  int r;
  if (id1 < id2) r = -1;
  else if (id1 == id2) r = 0;
  else /* id1 > id2 */ r = 1;
  return r;
}
#endif

static void mt_mate_finder__(int tidx, mt_find_mate_t * fm) {

  //bucket_array_t * const ba = is_unclipped ? fm->bucketU_array : fm->bucket_array;
  bucket_array_t * const ba = fm->bucketU_array;
  int const n_threads = fm->n_threads;
  size_t const hash_size = fm->hash_size;
  const uint64_t st = ba->min + tidx;
  const uint64_t ed = ba->max;
  //bamptr_t ** const ma = is_unclipped ? fm->mate_array_unclipped : fm->mate_array_clipped; 
  bamptr_t ** const ma = fm->mate_array_unclipped;
  int i;

  for(i=st; i<=ed; i+=n_threads) {
    int j;
    bucket_t * const bu = get_bucket(i, ba, 0);
    if (bu){
    for (j=bu->min; j<=bu->max; j++){
      bamptr_t * bamp;
      bamptr_t * const top = get_fifo_list_from_bucket(bu, j, 1/*unclip*/);
#if defined(BAMP_ARRAY)
      int fmx;
      bamptr_array_t * const fma = container_of(top, bamptr_array_t, bp[0]);

      if (top){
      for(fmx=0,bamp=top; fmx<fma->size; fmx++, bamp=get_bamptr_at(fma, fmx, 1/*unclipped*/))
#else
      for(bamp=top; bamp; bamp=(is_unclipped ? bamp->unclipped_next : bamp->clipped_next))
#endif
      {
  short const flag = bamptr_get_flag(bamp);
  if(has_mate(flag)
    && (0 == (flag & BAM_FSECONDARY)) // skip not-primary alignments (e.g., if there are pri1, pri2, and sec1 alignments, either pri1-pri2 or sec1-pri2 are paired, but sec1-pri2 is ignored) 
    && (0 == (flag & BAM_FSUPPLEMENTARY)) // skip supplimentary alignments
    ){
	uint64_t const qnameid = bamptr_get_qnameid(bamp);
	uint32_t const idx0 = qnameid % hash_size;
        bamptr_t * volatile * ma_p = &ma[idx0];
	bamptr_t * hash_ent;
	do {
	  hash_ent = *ma_p;
	  bamptr_set_mate(bamp, hash_ent); // use mate as next ptr on the chain
          if (__sync_bool_compare_and_swap(ma_p,hash_ent,bamp)) {
	    break;
          }
        } while(1);
//{
//char qname[128],qname2[128];
//bamptr_get_qname(bamp, qname, sizeof(qname));
//if(hash_ent){
//bamptr_get_qname(hash_ent, qname2, sizeof(qname2));
//fprintf(stderr, "%d %s %d -> %s %d\n", tidx, qname, flag, qname2, bamptr_get_flag(hash_ent));
//} else {
//fprintf(stderr, "%d %s %d -> NULL\n", tidx, qname, flag);
//}
//}
  }
      } // for (bamp)
#if defined(BAMP_ARRAY)
      } // if (top)
#endif
    } // for (j)
    } // if (bu)
  } // for (i)

  // barrier
  __atomic_thread_fence(__ATOMIC_RELEASE);
  __atomic_fetch_add(&g_mate_finder_barrier, 1, __ATOMIC_RELAXED);
  while (n_threads != g_mate_finder_barrier) {
    IN_SPIN_WAIT_LOOP();
  }
  FINISH_SPIN_WAIT_LOOP();
if (0==tidx) {
fprintf(stderr, "(I) hash done\n");
}

  size_t arr_sz = 128;
  bamptr_t **arr = md_malloc(arr_sz*sizeof(arr[0]), mem_mtMateFinder__, 0);

  for(i=tidx; i<hash_size; i+=n_threads) {
#if 0
    bamptr_t *bp, *next;
    size_t arr_idx = 0;
    for(bp=ma[i]; bp; bp=next) {
      if (arr_idx == arr_sz) {
        size_t new_sz = arr_sz*2;
        arr = md_realloc(arr, new_sz*sizeof(arr[0]), mem_mtMateFinder__);
        arr_sz = new_sz;
      }
      next = bamptr_get_mate(bp);
__builtin_prefetch(&bamptr_get_mate(next), 1 /* rw */, 3 /* temporal locality*/);
      bamptr_set_mate(bp, NULL);
      arr[arr_idx++] = bp;
//{
//char qname[128];
//bamptr_get_qname(arr[arr_idx-1], qname, sizeof(qname));
//fprintf(stderr, "%d arr[%d] %s %d\n", tidx, arr_idx-1, qname, bamptr_get_flag(arr[arr_idx-1]));
//}
    }

    if (arr_idx >= use_qsort) {
      qsort(arr, arr_idx, sizeof(arr[0]), qnameid_cmpfunc);
      int j;
      for(j=0; j<arr_idx; j++) {
        if (j+1 < arr_idx &&
            bamptr_get_qnameid(arr[j]) == bamptr_get_qnameid(arr[j+1])) {
          bamptr_set_mate(arr[j], arr[j+1]);
          bamptr_set_mate(arr[j+1], arr[j]);
          j++;
        } else {
          short const flag = bamptr_get_flag(arr[j]);
          if ((flag & BAM_FPAIRED) && !(flag & BAM_FMUNMAP)) {
            char qname[128];
            bamptr_get_qname(arr[j], qname, sizeof(qname));
            fprintf(stderr, "(W) %s (qnameid %ld) is paired (shown in FLAG) but has no pair read in inputs (j %d arr_idx %ld flag %d)\n", qname, bamptr_get_qnameid(arr[j]), j, arr_idx, flag);
          }
        }
      } // for j
    } else 
#endif
    { // qsort
#if 0
int found=0;
bamptr_t *arr[2];
{
bamptr_t *bp;
char qname[128];
for(bp=ma[i]; bp; bp=bamptr_get_mate(bp)){
bamptr_get_qname(bp, qname, sizeof(qname));
if (0==strcmp("SRR002276.40596309",qname)) {
  found=1;
}
}
if(found){
int ii=0;
fprintf(stderr, "ma[%d] ", i);
for(bp=ma[i]; bp; bp=bamptr_get_mate(bp)){
arr[ii++]=bp;
bamptr_get_qname(bp, qname, sizeof(qname));
fprintf(stderr, "%s (%ld) - ", qname, bamptr_get_qnameid(bp));
}
fprintf(stderr, "\n");
}
}
#endif
#if 1
      bamptr_t *bp, *next;
      for(bp=ma[i]; bp; bp=next) {
        next = bamptr_get_mate(bp);
        const uint64_t qnameid = bamptr_get_qnameid(bp);
        bamptr_t *bp2, *next2, *prev2;
        for(prev2=bp,bp2=next; bp2; prev2=bp2,bp2=next2) {
          next2 = bamptr_get_mate(bp2);
__builtin_prefetch(&(next2->baminfoU[0].mate_bamp), 1 /* rw */, 3 /* temporal locality*/);
          if(qnameid == bamptr_get_qnameid(bp2)) {
            // unlink bp and bp2
            if (prev2 == bp) {
              next = next2;
            } else {
              bamptr_set_mate(prev2, next2);
            }
            bamptr_set_mate(bp, bp2);
            bamptr_set_mate(bp2, bp);
            break;
          } // if qnameid == qnameid2
        } // for prev2,bp2
        if (NULL == bp2) {
          bamptr_set_mate(bp, NULL);

          short const flag = bamptr_get_flag(bp);
          if ((flag & BAM_FPAIRED) && !(flag & BAM_FMUNMAP)) {
            char qname[128];
            bamptr_get_qname(bp, qname, sizeof(qname));
            fprintf(stderr, "(W) %s (qnameid %ld) is paired (shown in FLAG) but has no pair read in inputs (flag %d)\n", qname, qnameid, flag);
          }
        }
      } // for bp
#if 0
if(found){
int ii;
for(ii=0; ii<2; ii++) {
fprintf(stderr, "%p -> %p\n", arr[ii], bamptr_get_mate(arr[ii]));
}
}
#endif
#else
      int j;
      for(j=0; j<arr_idx; j++) {
        if(NULL == bamptr_get_mate(arr[j])) {
          int k;
          const uint64_t qnameid_j = bamptr_get_qnameid(arr[j]);
          for (k=j+1; k<arr_idx; k++) {
            if(qnameid_j == bamptr_get_qnameid(arr[k])) {
              bamptr_set_mate(arr[j], arr[k]);
              bamptr_set_mate(arr[k], arr[j]);
              break;
            }
          } // for k
          if(NULL == bamptr_get_mate(arr[j])) {
            short const flag = bamptr_get_flag(arr[j]);
            if ((flag & BAM_FPAIRED) && !(flag & BAM_FMUNMAP)) {
              char qname[128];
              bamptr_get_qname(arr[j], qname, sizeof(qname));
              fprintf(stderr, "(W) %s (qnameid %ld) is paired (shown in FLAG) but has no pair read in inputs (j %d arr_idx %ld flag %d)\n", qname, qnameid_j, j, arr_idx, flag);
            }
          } // if mate == NULL
        } // if mate == NULL
      } // for j
#endif
    }
  } // for i
  md_free(arr, arr_sz*sizeof(arr[0]), mem_mtMateFinder__);
}

static void *mt_mate_finder(void *data) {
  fm_worker_t * const w = (fm_worker_t *)data;
  mt_find_mate_t * const fm = w->fm;
  const int tidx = w - &fm->w[0];
#if defined(CPU_SET_AFFINITY)
{
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET2(CPU_MATEFIND(tidx), &cpuset);
    pthread_setaffinity_np(fm->tid[tidx], sizeof(cpu_set_t), &cpuset);
}
#endif

//  mt_mate_finder__(tidx, fm, 0); 
//  mt_mate_finder__(tidx, fm, 1);
  mt_mate_finder__(tidx, fm);

  return NULL;
}

static void mt_find_mates(int n_threads, bucket_array_t * bucket_array, bucket_array_t * bucketU_array) {
  int i;
  if (n_threads <= 0) {
    fprintf(stderr, "fm: n_threads %d must be >0\n", n_threads);
    exit(-1);
  }
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  static mt_find_mate_t fm;
  fm.n_threads = n_threads;
  fm.bucket_array = bucket_array;
  fm.bucketU_array = bucketU_array;
  fm.tid = md_malloc( n_threads*sizeof(fm.tid[0]), mem_findMates1, 0);
  const uint64_t length = fm.bucketU_array->max - fm.bucketU_array->min + 1;
  fm.chunk_size = (length + n_threads - 1) / n_threads;
  fm.w = md_malloc( n_threads*sizeof(fm.w[0]), mem_findMates2, 0);
  size_t const hash_size = (g_alignments_without_mate_info > g_qnameid ? g_alignments_without_mate_info : g_qnameid);
  fm.hash_size = hash_size;
  fprintf(stderr, "fm: %ld indices per thread (min %ld max %ld n_threads %d) g_qnameid %ld hash_size %ld\n", fm.chunk_size, fm.bucketU_array->min, fm.bucketU_array->max, fm.n_threads, g_qnameid, hash_size);
  // bamptr_t **clipped = md_malloc( hash_size * sizeof(bamptr_t*), mem_findMates3, 1);
  bamptr_t **unclipped = md_malloc( hash_size * sizeof(bamptr_t*), mem_findMates4, 1);
  // fm.mate_array_clipped = clipped;
  fm.mate_array_unclipped = unclipped;
  for (i = 0; i < fm.n_threads; ++i) {
    fm.w[i].fm = &fm;
    pthread_create(&fm.tid[i], &attr, mt_mate_finder, &fm.w[i]);
    char tn[32];
    sprintf(tn, "matefinder-%d\n", i);
    if(pthread_setname_np(fm.tid[i], tn)){}
  }

  for (i = 0; i < n_threads; ++i) pthread_join(fm.tid[i], 0);
  md_free(fm.tid, n_threads*sizeof(fm.tid[0]), mem_findMates1); 
  md_free(fm.w, n_threads*sizeof(fm.w[0]), mem_findMates2); 
  // md_free(clipped, hash_size*sizeof(bamptr_t*), mem_findMates3); 
  md_free(unclipped, hash_size*sizeof(bamptr_t*), mem_findMates4); 
}

#endif

#if defined(SORT_SAM)
typedef struct mt_sort_sam {
  pthread_t *tid;
  struct ss_worker *w;
  bucket_array_t *bucket_array;
  bucket_array_t *bucketU_array;
  int n_threads;
  uint64_t chunk_size;
} mt_sort_sam_t;

typedef struct ss_worker {
  mt_sort_sam_t *ss;
} ss_worker_t;

static int picard_sortsam_cmpfunc_clip(const void * v1, const void * v2, void * args) ;

static void mt_sam_sorter__(int tidx, mt_sort_sam_t * ss) {
  bucket_array_t * const ba = ss->bucket_array;
  int const n_threads = ss->n_threads;
  const uint64_t st = ba->min + tidx;
  const uint64_t ed = ba->max;

  int i;

  for(i=st; i<=ed; i+=n_threads) {
    int j;
    bucket_t * const bu = get_bucket(i, ba, 0);
    if (bu){
    for (j=bu->min; j<=bu->max; j++){
      bamptr_t * const top = get_fifo_list_from_bucket(bu, j, 0);
#if defined(BAMP_ARRAY)
      bamptr_array_t * const ssa = container_of(top, bamptr_array_t, bp[0]);
      if (top){
      if (ssa->size >= 2) {
        uint64_t param[4];
        param[0] = i;
        param[1] = j;
        param[2] = (uint64_t)ss->bucketU_array;
        param[3] = 0;	// dump
#if defined(DEBUG_POS)
        if (get_pos_from_index(i,j) == debug_pos-1) {
          param[2] = 1;
        }
#endif
	qsort_r(get_bamptr_at(ssa, 0, 0/*clip*/), ssa->size, sizeof_bamptr(0/*clip*/), picard_sortsam_cmpfunc_clip, param);
      }
      }
#else
#error "no implemented"
#endif
    } /* j */
    }
  } /* i */
}

static void *mt_sam_sorter(void *data) {
  ss_worker_t * const w = (ss_worker_t *)data;
  mt_sort_sam_t * const ss = w->ss;
  const int tidx = w - &ss->w[0];
#if defined(CPU_SET_AFFINITY)
{
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET2(CPU_SAMSORT(tidx), &cpuset);
    pthread_setaffinity_np(ss->tid[tidx], sizeof(cpu_set_t), &cpuset);
}
#endif

  mt_sam_sorter__(tidx, ss);

  return NULL;
}

static void mt_sort_sam(int n_threads, bucket_array_t * bucket_array, bucket_array_t * bucketU_array) {
  int i;
  if (n_threads <= 0) {
    fprintf(stderr, "ss: n_threads %d must be >0\n", n_threads);
    exit(-1);
  }
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  static mt_sort_sam_t ss;
  ss.n_threads = n_threads;
  ss.bucket_array = bucket_array;
  ss.bucketU_array = bucketU_array;
  ss.tid = md_malloc( n_threads*sizeof(ss.tid[0]), mem_sortSam1, 0);
  const uint64_t length = ss.bucketU_array->max - ss.bucketU_array->min + 1;
  ss.chunk_size = (length + n_threads - 1) / n_threads;
  fprintf(stderr, "ss: %ld indices per thread (min %ld max %ld n_threads %d) g_qnameid %ld\n", ss.chunk_size, ss.bucketU_array->min, ss.bucketU_array->max, ss.n_threads, g_qnameid);
  ss.w = md_malloc( n_threads*sizeof(ss.w[0]), mem_sortSam2, 0);

  for (i = 0; i < ss.n_threads; ++i) {
    ss.w[i].ss = &ss;
    pthread_create(&ss.tid[i], &attr, mt_sam_sorter, &ss.w[i]);
    char tn[32];
    sprintf(tn, "samsorter-%d\n", i);
    if(pthread_setname_np(ss.tid[i], tn)){}
  }

  for (i = 0; i < n_threads; ++i) pthread_join(ss.tid[i], 0);
  md_free(ss.tid, n_threads*sizeof(ss.tid[0]), mem_sortSam1); 
  md_free(ss.w, n_threads*sizeof(ss.w[0]), mem_sortSam2); 
}
#endif

#define DEFAULT_N_LINES		(128) // #lines per DEFAULT_BLKRD_SIZE 
static void blkrd_free_list2_init(blkrd_free_list2_t *l2) {
  l2->blkrd_free_list1 = md_malloc(sizeof(l2->blkrd_free_list1[0])*n_block_reader, mem_blkrdFreeList2Init, 1);
  l2->blkrd_free_list2 = NULL;
}

static blkrd_t *md_blkrd_alloc(blkrd_free_list2_t *l2, blkrd_free_list_t ** my_fl) {
  //blkrd_free_list_t * f = l2->blkrd_free_list1;
  blkrd_free_list_t * f = *my_fl;
  blkrd_t *b = NULL;
  if (f) {
    b = (blkrd_t*)f;
    *my_fl = f->next;
    PREFETCH_BLKREAD_RW_LOCALITY3(f->next);
//fprintf(stderr, "alloc1 %p\n", b);
  } else {
    f = l2->blkrd_free_list2;
    if (f) {
      if (__sync_bool_compare_and_swap(&l2->blkrd_free_list2,f,NULL)) {
        *my_fl = f->next;
        b = (blkrd_t*)f;
//fprintf(stderr, "alloc2 %p\n", b);
      }
    }
  }
  if (NULL == b) {
    b = md_malloc(sizeof(blkrd_t), mem_blkrdAlloc, 0);
    b->line_pos = md_malloc(sizeof(line_pos_t)+sizeof(b->line_pos->pos[0])*DEFAULT_N_LINES, mem_blkrdAlloc, 0);
    b->line_pos->max = DEFAULT_N_LINES;
  }
  b->line_pos->n_lines = 0;
  return b;
}

static void md_blkrd_free(blkrd_free_list2_t *l2, blkrd_free_list_t *top, blkrd_free_list_t *bottom){
if (top) {
  while(1){
    blkrd_free_list_t * const oldf = l2->blkrd_free_list2;
    bottom->next = oldf;
    if (__sync_bool_compare_and_swap(&l2->blkrd_free_list2,oldf,top)) {
   	  break;
    }
  }
}
}

static void md_blkrd_free2(blkrd_free_list2_t *l2){
  blkrd_free_list_t * p, *next;
  p = l2->blkrd_free_list2;
  while(p){
    next = p->next;
    md_free(((blkrd_t*)p)->line_pos, sizeof(line_pos_t)+sizeof(((blkrd_t*)p)->line_pos->pos[0])*DEFAULT_N_LINES, mem_blkrdAlloc);
    md_free(p, sizeof(blkrd_t), mem_blkrdAlloc);
    p = next;
  }
  int i;
  for(i=0; i<n_block_reader; i++) {
    p = l2->blkrd_free_list1[i];
    while(p){
      next = p->next;
      md_free(((blkrd_t*)p)->line_pos, sizeof(line_pos_t)+sizeof(((blkrd_t*)p)->line_pos->pos[0])*DEFAULT_N_LINES, mem_blkrdAlloc);
      md_free(p, sizeof(blkrd_t), mem_blkrdAlloc);
      p = next;
    }
  }
}

typedef struct {
  pthread_t tid;
  struct mt_linesplit *mt;
  struct blkrd * volatile q;
  volatile size_t n_enq, n_deq, n_spn;
} linesplit_work_t;

typedef struct mt_linesplit {
  mt_samparse_t *samparse;
  bam_free_list2_t *bam_free_list2;
  struct blkrd ** my_q;		// submitter's queue
  linesplit_work_t *w;
  int done;
  long prof_n_spn;
} mt_linesplit_t;

static void *mt_line_splitter(void *data){ 
  linesplit_work_t * const w = (linesplit_work_t*)data;
  mt_linesplit_t * const mt = w->mt;
  int const myid = w - mt->w;

  while(1){
    // obtain a new queue if it was added
    blkrd_t * q;
    while(1){
      q = w->q;
      if (q) {
        if (__sync_bool_compare_and_swap(&w->q, q, NULL)) {
	  break;
	}
      } else {
	// if there is no queue and w->done then there is no work.
	if(mt->done) goto no_work;
        IN_SPIN_WAIT_LOOP();
      }
    }
    FINISH_SPIN_WAIT_LOOP();
    blkrd_t * q2, *prev = NULL, *next;
    // change the order of samparse_queue_t entries from LIFO to FIFO so that reads can be traversed as they appear in the input file
    for(q2=q; q2; q2=next){
	next = q2->q_next;
	q2->q_next = prev;
	prev = q2;
    }

    for(q2=prev; q2; q2=next){
      char *p;
      char * const start = q2->block;
      char * const end = q2->block + q2->block_len;
      line_pos_t * line_pos = q2->line_pos;
//fprintf(stderr, "%d: blkrd_t %p len %d start %p %.*s\n", myid, q2, q2->block_len, start, 32, start);

      // find new line chars
      for(p=start; p < end; ) {
        char * const nl = memchr(p, '\n', end - p);
	if (nl) {
	  if (line_pos->n_lines > line_pos->max-1) {
	    // extend the array
	    line_pos->max *= 2; 
	    line_pos = md_realloc(line_pos, sizeof(*line_pos)+sizeof(line_pos->pos[0])*line_pos->max, mem_blkrdAlloc);
	    q2->line_pos = line_pos;
	  }
	  line_pos->pos[line_pos->n_lines++] = p - start;
	  *nl = 0; // sam_parse1() supposes '\0' or '\t' as delimiters of tokens
	  p = nl + 1;
	} else {
	  fprintf(stderr, "(E) a block (%p len %d) ...", start, q2->block_len);
          int i;
          for(i=0; i<32; i++) {
	    fprintf(stderr, "%c", end[-1-31+i]);
          }
          fprintf(stderr, " is terminated by a non-newline (%c 0x%x)\n", *(end-1), *(end-1));
	  exit(-1);
	}
      } /* for p */

      // enqueue a line to be parsed
      mt_samparse_enqueue(mt->samparse, q2, myid);

      next = q2->q_next;
      w->n_deq ++;
    } /* for q2 */
  }
no_work:
  return NULL;
}

static mt_linesplit_t mt_linesplit;
static mt_linesplit_t * mt_linesplit_init(mt_samparse_t *samparse){
  int i;
  samparse->my_q = md_malloc(sizeof(samparse->my_q[0])*n_line_splitter, mem_linesplitInit, 0);
  for(i=0; i<n_line_splitter; i++){
    samparse->my_q[i].i = 0;
    samparse->my_q[i].q = samparse_queue_malloc();
  }

  pthread_attr_t attr;
  pthread_attr_init(&attr);
  mt_linesplit_t *linesplit = &mt_linesplit;
  linesplit->samparse = samparse;
  linesplit->bam_free_list2 = samparse->bam_free_list2;
  linesplit->w = md_malloc(sizeof(linesplit->w[0])*n_line_splitter, mem_linesplitInit, 1);
  linesplit->my_q = md_malloc(sizeof(linesplit->my_q[0])*n_block_reader, mem_linesplitInit, 1);

  int j;
  const int cpu_vec_len = (n_physical_cores*SMT+63)/64;
  uint64_t cpu_vec[cpu_vec_len];
  memset(cpu_vec, 0, sizeof(uint64_t)*cpu_vec_len);
  for(j=0; j<n_line_splitter; j++){
    linesplit->w[j].mt = linesplit;
    pthread_create(&linesplit->w[j].tid, &attr, mt_line_splitter, &linesplit->w[j]);
    char tn[32];
    sprintf(tn, "line_split-%d-%d", j, CPU_LINESPLIT(j));
    if(pthread_setname_np(linesplit->w[j].tid, tn)){}
#if defined(CPU_SET_AFFINITY)
{
      cpu_set_t cpuset;
      CPU_ZERO(&cpuset);
//fprintf(stderr, "CPU_LINESPLIT(%d)=%d\n", j, CPU_LINESPLIT(j));
      CPU_SET2(CPU_LINESPLIT(j), &cpuset);
      cpu_vec[CPU_LINESPLIT(j)/64] |= 1LL<<(63-CPU_LINESPLIT(j)%64);
      pthread_setaffinity_np(linesplit->w[j].tid, sizeof(cpu_set_t), &cpuset);
}
#endif
  }
  fprintf(stderr, "(I) CPU %15s ", "line_splitter");
  for(i=0; i<cpu_vec_len; i++) {
    int j;
    for(j=0; j<64; j++) {
      fprintf(stderr, "%s%c", j%SMT ? "" : " ", (cpu_vec[i]&(1LL<<(63-j))) ? 'o' : '-');
    }
  }
  fprintf(stderr, "\n");
  return &mt_linesplit;
}

static void linesplit_queue_flush(mt_linesplit_t *linesplit, int thread, int myid){
//TODO  static int thread = 0;
  blkrd_t * const q = linesplit->my_q[myid];
  blkrd_t * volatile * p_w_q = &linesplit->w[thread].q;
  uint64_t spn = 0;

  __atomic_thread_fence(__ATOMIC_RELEASE);

  while(1){
#if defined(SAMPARSE_THROTTLE) 
    if(linesplit->w[thread].n_enq - linesplit->w[thread].n_deq > MAX_SAMPARSE_QLEN) {
      thread = (thread + 1 == n_line_splitter) ? 0 : thread + 1;
      p_w_q = &linesplit->w[thread].q;
      IN_SPIN_WAIT_LOOP();
      spn++;
      continue;
    }
    FINISH_SPIN_WAIT_LOOP();
#endif
    blkrd_t * const oldq = *p_w_q;
    q->q_next = oldq;
    if (__sync_bool_compare_and_swap(p_w_q,oldq,q)) {
      break;
    }
  }
  __atomic_fetch_add(&linesplit->w[thread].n_enq, 1, __ATOMIC_RELAXED);
  //if (linesplit->prof_n_spn) 
  __atomic_fetch_add(&linesplit->w[thread].n_spn, spn, __ATOMIC_RELAXED);
}

static inline void mt_linesplit_enqueue(mt_linesplit_t *linesplit, blkrd_t *blkrd, int myid){
  const int thd_idx = blkrd->block_seqid % n_line_splitter;
  linesplit->my_q[myid] = blkrd;
  linesplit_queue_flush(linesplit, thd_idx, myid);
}

static void mt_linesplit_end(mt_linesplit_t *linesplit) {
  int i;

  linesplit->done = 1;

  for(i=0; i<n_line_splitter; i++) {
    pthread_join(linesplit->w[i].tid, 0);
  }

  mt_samparse_end(linesplit->samparse);
}

typedef struct input_fps {
  int n;
  samFile *fp[];
} input_fps_t;

typedef struct input_files {
  int n;
  char *fn[];
} input_files_t;

typedef struct {
  pthread_t tid;
  struct mt_blkrd *mt;
} blkrd_work_t;

typedef struct mt_blkrd {
  mt_linesplit_t *linesplit;
  input_fps_t   *fps;
  input_files_t *input_files;
  blkrd_work_t *w;
  void * orig_uncompressed_block;
  size_t read_bytes;
  blkrd_free_list2_t blkrd_free_list2;
  //mt_drop_cache_t *drop_cache;
  struct mt_progress *progress;
  struct mt_drop_cache *drop_cache;

  // profile
  double time_open;
  long   n_open;
} mt_blkrd_t;

typedef struct mt_drop_cache {
  mt_blkrd_t *blkrd;
  pthread_t tid;
  long count;
  volatile int done;
  struct timeval begin;
} mt_drop_cache_t;

static void *mt_drop_cache(void *data) {
  mt_drop_cache_t * const mt = (mt_drop_cache_t*)data;
  static off_t last = 0;
  mt_blkrd_t * const blkrd = mt->blkrd;

int i;
for (i=0; i < blkrd->fps->n; i++) {
//  BGZF * const bgzf = ((kstream_t*)fp->fp.voidp)->f;
//  int const fd = ((hFILE_fd*)(bgzf->fp))->fd;
  volatile samFile ** const p_fp = (volatile samFile **)(&blkrd->fps->fp[i]);
  while(NULL == *p_fp) {
    sleep(1);
  }
  samFile * const fp = (samFile *)*p_fp;
  const int fd = ((hFILE_fd*)((kstream_t*)fp->fp.voidp)->f->fp)->fd;

  struct stat st_buf;
  st_buf.st_size = 0;
  if (-1 == fstat(fd, &st_buf)) {
    fprintf(stderr, "(W) DROP CACHE: fstat failed\n");
    continue;
  }
  const size_t file_size = st_buf.st_size;
  
  last = 0;

  while(1) {
    off_t const curr = lseek(fd, 0, SEEK_CUR);
    if (-1 == curr) {
      fprintf(stderr, "(W) DROP CACHE: not drop the cache for %s skip since lseek SEEK_CUR failed\n", blkrd->input_files->fn[i]);
      break;
    } 
    posix_fadvise(fd, last, curr - last, POSIX_FADV_DONTNEED);
#if defined(VERBOSE_DROP_CACHE)
    fprintf(stderr, "(I) DROP CACHE: drop region %ld - %ld of %s\n\n\n", last, curr, blkrd->input_files->fn[i]);
#endif
    if (file_size == curr) {
#if defined(VERBOSE_DROP_CACHE)
      fprintf(stderr, "(I) DROP CACHE: drop file cache done: %s\n\n\n", blkrd->input_files->fn[i]);
#endif
      break;
    }
    last = curr;

    mt->count++;

    sleep(1);

    if (mt->done) break;
  }
}
  return NULL;
}

mt_drop_cache_t drop_cache;
static mt_drop_cache_t *mt_drop_cache_init(mt_blkrd_t *blkrd) {
  mt_drop_cache_t *mt = &drop_cache;
  mt->blkrd = blkrd;
  pthread_attr_t attr;
  pthread_attr_init(&attr);

  mt->count = 0;
  mt->done = 0;

  pthread_create(&mt->tid, &attr, mt_drop_cache, mt);
  if(pthread_setname_np(mt->tid, "drop_cache")){}

  fprintf(stderr, "(I) drop the input data from the OS file cache\n");
  gettimeofday(&mt->begin, NULL);

  return mt;
}

static void mt_drop_cache_end(mt_drop_cache_t *mt) {
  mt->done = 1;

  pthread_join(mt->tid, NULL);
  struct timeval time_end;
  gettimeofday(&time_end, NULL);
  double drop_time = time_end.tv_sec-mt->begin.tv_sec +(time_end.tv_usec-mt->begin.tv_usec)/1000000.0;

  fprintf(stderr, "(I) %ld cache drops (%.1f /s)\n", mt->count, mt->count/drop_time);
}

typedef struct mt_progress {
  pthread_t tid;
  mt_blkrd_t *blkrd;
  size_t size;
  volatile int done;
} mt_progress_t;

static int rd_progress_initialized = 0;
static int wr_progress_initialized = 0;
static int rd_progress_percent = 0;
static size_t rd_progress_bytes = 0;

static void* mt_progress(void *data) {
  mt_progress_t * const prog = (mt_progress_t*)data;
  mt_blkrd_t *p_blkrd = prog->blkrd;

  size_t last_rd = p_blkrd->read_bytes;
  struct timeval read_start;
  gettimeofday(&read_start, NULL);
  struct timeval last_lap = read_start;

  int print_scale = 1;
  while(1) {
      sleep(1);
      int i;
      if (print_scale) {
      	print_scale = 0;
	fprintf(stderr, "(I) 0                       50                      100 %%\n");
	fprintf(stderr, "(I) +----+----+----+----+----+----+----+----+----+----+\n");
  	for (i=0; i<50; i++) fprintf(stderr, ".");
  	fprintf(stderr, "\n");
        while (0 == (rd_progress_bytes = p_blkrd->read_bytes)) {
          sleep(1);
        }
        rd_progress_initialized = 1;
	if (!filters.use_baminfo && !wr_progress_initialized) {
	  sleep(1);
	}
      }
      struct timeval lap_time;
      gettimeofday(&lap_time, NULL);
      double time = lap_time.tv_sec - last_lap.tv_sec + (lap_time.tv_usec-last_lap.tv_usec)/1000000.0;
      last_lap = lap_time;

      const int percent = p_blkrd->read_bytes*100/prog->size + 1;
      rd_progress_percent = percent;
      rd_progress_bytes = p_blkrd->read_bytes;
      fprintf(stderr, "\x1b[%dA", filters.use_baminfo ? 1 : 4); // move the cursor -N lines
      fprintf(stderr, 
    		   	"\x1b[K"	// clear line
      			"(I)  ");
      for (i=0; i<percent/2; i++) fprintf(stderr, "#");
      for (i=percent/2; i<50; i++) fprintf(stderr, ".");
      fprintf(stderr, " %3d %% %ld MB %s%.1f%s MB/s", percent,
	      p_blkrd->read_bytes/1024/1024, "\33[34;1m", (p_blkrd->read_bytes - last_rd)/1024/1024/time, "\33[0m");
      fprintf(stderr, "\r\x1b[%dB", filters.use_baminfo ? 1 : 4);

      last_rd = p_blkrd->read_bytes;

//if (p_hash) {
//      if (p_hash->done) break;
//}
      if (prog->done) break;
  }

  struct timeval read_end;
  gettimeofday(&read_end, NULL);
  double rd_fin = read_end.tv_sec-time_begin.tv_sec +(read_end.tv_usec-time_begin.tv_usec)/1000000.0;
  double rd_fin2 = read_end.tv_sec-read_start.tv_sec +(read_end.tv_usec-read_start.tv_usec)/1000000.0;
  fprintf(stderr, "%s[%8.2f] read finished (%.1f MB/s)%s  (open time %.1f sec count %ld)\n", "\33[30;1m", rd_fin, p_blkrd->read_bytes/rd_fin2/1024/1024, "\33[0m",
	p_blkrd->time_open, p_blkrd->n_open);
  if (!filters.use_baminfo) mt_wrt_throughput_offset ++;
  return NULL;
}
static mt_progress_t progress;
static mt_progress_t *mt_progress_init(mt_blkrd_t *blkrd, size_t size) {
  mt_progress_t *mt = &progress;
  mt->blkrd = blkrd;
  mt->size = size;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_create(&mt->tid, &attr, mt_progress, mt);
  if(pthread_setname_np(mt->tid, "prog_thr")){}
  return mt;
}

static void mt_progress_end(mt_progress_t *mt) {
  mt->done = 1;

  pthread_join(mt->tid, NULL);
}

static void *mt_block_reader(void *data){ 
  blkrd_work_t * const w = (blkrd_work_t*)data;
  mt_blkrd_t * const mt = w->mt;
  const int myid = w - w->mt->w;
  register blkrd_free_list_t ** const my_fl = &mt->blkrd_free_list2.blkrd_free_list1[myid];

  int partial_line_len = 0;
  char partial_line[MAX_LINE_LENGTH];
  uint32_t __block_seqid = 0;
static double total_open = 0.0;
int n_open = 0;

int fn_i;
for (fn_i=myid; fn_i < mt->input_files->n; fn_i+=n_block_reader) {
  char * const fn = mt->input_files->fn[fn_i];

struct timeval tm_open;
gettimeofday(&tm_open, NULL);

  samFile *const fp = sam_open(fn, "r");
  if (fp == NULL) {
    // in case that the target file was removed after its header was checked
    fprintf(stderr, "(E) fail to open file %s\n", fn);
    return NULL;
  }
  mt->fps->fp[fn_i] = fp;
  // skip the header
  bam_hdr_t * const tmp_hdr = sam_hdr_read(fp);
  if (0 != fn_i) {
    bam_hdr_t * const header = mt->linesplit->samparse->header;
    // compare the current header to the header for the 1st SAM file 
    // @PG lines are different since they usually include file names.
    size_t offset=0;
    size_t cmp_len = header->l_text;

    // compare the chunks between the 1st and current headers (optimization)
    size_t const cmp_chunk_size = 1024;
    while (cmp_len > 0) {
      if (cmp_len >= cmp_chunk_size) {
        if (0 == memcmp(header->text + offset, tmp_hdr->text + offset, cmp_chunk_size)) {
          // same
	  offset  += cmp_chunk_size;
	  cmp_len -= cmp_chunk_size;
          continue;
        } else {
	  // different lines exist in the chunk at offset
	  break;
	}
      } else {
        // smaller than the chunk size
	break;
      }
    }

    // compare the lines 
    int error=0;
    char *p1 = header->text + offset;
    char *p2 = tmp_hdr->text + offset;
    while(1) {
      char * const ln1 = strchr(p1, (int)'\n');
      char * const ln2 = strchr(p2, (int)'\n');
      if (ln1 && ln2) {
        // lines exist
        if (ln1-p1 == ln2-p2 && // same length
	    0 == strncmp(p1, p2, ln1-p1)) {
	  // same
	} else if (p1[0]=='@' && p2[0]=='@' &&
	           p1[1]=='P' && p2[1]=='P' &&
	           p1[2]=='G' && p2[2]=='G') {
	  // ignore differences in @PG lines
	} else {
          fprintf(stderr, "(E)     The first difference between the first and current files appears\n");
          fprintf(stderr, "(E)     current file: %s\n", mt->input_files->fn[fn_i]);
          fprintf(stderr, "(E)         %s\n", p2);
          fprintf(stderr, "(E)     first file: %s\n", mt->input_files->fn[0]);
          fprintf(stderr, "(E)         %s\n", p1);
	  error = 1;
          break;
	}
      } else if (NULL==ln1 && NULL==ln2) {
        // cmp done
        break;
      } else if (ln1 /* ln2 is null */) {
        fprintf(stderr, "(E)     The first file has the following line, but the current file does not have it at the same position\n");
        fprintf(stderr, "(E)         %s\n", p1);
	error = 1;
        break;
      } else /* ln1 is null but ln2 is not null */ {
        fprintf(stderr, "(E)     The current file has the following line, but the first file does not have it at the same position\n");
        fprintf(stderr, "(E)         %s\n", p2);
	error = 1;
        break;
      }
      p1 = ln1 + 1;
      p2 = ln2 + 1;
    } // while
    if (error) {
      exit(-1);
    }
  }
  bam_hdr_destroy(tmp_hdr);
struct timeval tm_open2;
gettimeofday(&tm_open2, NULL);
double t = tm_open2.tv_sec-tm_open.tv_sec+(tm_open2.tv_usec-tm_open.tv_usec)/1000000.0;
total_open += t;
n_open++;

/*
  After sam_hdr_read(), some data were already read in the internal buffers:
  fp->line.s			- 1st line
  ks->buf[ks->begin:ks->end]	- 2nd line, etc. (ks=(kstream_t*)fp->fp.voidp)

  The data should be processed in the block reader thread.
*/
  kstream_t * const ks = (kstream_t*)fp->fp.voidp;
  unsigned char * const ks_start = ks->buf + ks->begin;
  int const ks_len = ks->end - ks->begin;
  blkrd_t *b = md_blkrd_alloc(&mt->blkrd_free_list2, my_fl);
  memcpy(b->block, fp->line.s, fp->line.l);
  b->block[fp->line.l] = '\n';
  int read_len;
  BGZF * const bgzf = ks->f;
  mt->orig_uncompressed_block = bgzf->uncompressed_block;
  {
    int fd = ((hFILE_fd*)bgzf->fp)->fd;
    struct stat st_buf;
    st_buf.st_size = 0;
    if (-1 == fstat(fd, &st_buf)) {
      fprintf(stderr, "(I) mt_block_reader: fstat failed\n");
    } else {
      posix_fadvise(fd, 0, st_buf.st_size, POSIX_FADV_SEQUENTIAL);
    }
  }

  unsigned char * const last = memrchr(ks_start, '\n', ks_len);
  if (last) {
    partial_line_len = ks->buf + ks->end - (last + 1);
    memcpy(partial_line, last + 1, partial_line_len);
    read_len = last + 1 - ks_start;
  } else {
    partial_line_len = ks_len;
    memcpy(partial_line, ks_start, partial_line_len);
    read_len = 0;
  }
  memcpy(b->block + fp->line.l + 1, ks_start, read_len);

  b->block_len = fp->line.l + 1 + read_len;
  const uint32_t block_seqid_m = my_block_seqid(__block_seqid, myid);
  if (block_seqid_m > MAX_BLOCK_SEQID) {
    fprintf(stderr, "(E) the number of 64-KB blocks exceeded the limit %d\n", MAX_BLOCK_SEQID);
    exit(-1);
  }
  b->block_seqid = block_seqid_m;
  __block_seqid++;
  mt_linesplit_enqueue(mt->linesplit, b, myid); 
  mt->read_bytes += b->block_len;

  while(1){
    blkrd_t *b = md_blkrd_alloc(&mt->blkrd_free_list2, my_fl);
    b->block_len = 0;
    if(partial_line_len){
      if (partial_line_len > MAX_LINE_LENGTH) {
        fprintf(stderr, "(E) the length of a line exceeded the limit %d\n", MAX_LINE_LENGTH);
	exit(-1);
      }
      memcpy(b->block, partial_line, partial_line_len);
      b->block_len += partial_line_len;
    }
    char * const start = b->block + partial_line_len;
    bgzf->uncompressed_block = start;
    bgzf_read_block(bgzf);
    int read_len = bgzf->block_length;

#if 0 && defined(__powerpc64__) 
// FIXME: 20 MB/s file read on GPFS/x86_64, if enabled
    {
      static off_t last = 0;
      int fd = ((hFILE_fd*)(bgzf->fp))->fd;
      off_t const curr = lseek(fd, 0, SEEK_CUR);
      if (-1 == curr) {
	perror("mt_block_reader: lseek ");
        exit(-1);
      }
      posix_fadvise(fd, last, curr - last, POSIX_FADV_DONTNEED);
      last = curr;
    }
#endif

    /* search \n from the end */
    if (read_len) {
      char * const last = memrchr(start, '\n', read_len);
      if (last) {
	// ensure that b->block[0] is the first char of a line and b->block[read_len-1] is NL
	partial_line_len = start + read_len - (last + 1);
	memcpy(partial_line, last + 1, partial_line_len);
        read_len = last + 1 - start;
      }
    }

    b->block_len += read_len;

    /* submit a line split request */
    if (b->block_len) {
      const uint32_t block_seqid_m = my_block_seqid(__block_seqid, myid);
      if (block_seqid_m > UINT32_MAX) {
        fprintf(stderr, "(E) the block count exceeded the limit %d\n", UINT32_MAX);
        exit(-1);
      }
      b->block_seqid = block_seqid_m;
      __block_seqid++;
//fprintf(stderr, "R%d: blkrd_t %p len %d start %p ..%.*s\n", myid, b, b->block_len, b->block, 32, b->block+b->block_len-32);
      mt_linesplit_enqueue(mt->linesplit, b, myid); 

      mt->read_bytes += b->block_len;
    }
#if 0
{
static int last=0;
if(mt->read_bytes/(10LL*1024*1024*1024) > last){
  last = mt->read_bytes/(10LL*1024*1024*1024);
  PROFILE_MEM_DUMP();
}
}
#endif

    /* exit if the input is EOF */
    if (read_len == 0) {
      if (partial_line_len != 0) {
        fprintf(stderr, "(W) the last line was truncated since it is not terminated by a new-line\n");
      }
      break;
    }
  } // while(1)

  bgzf->uncompressed_block = mt->orig_uncompressed_block;

  // sam_close(fp);
} /* multiple input SAM files */

  w->mt->time_open = total_open;
  w->mt->n_open = n_open;

  return NULL;
}

static mt_blkrd_t mt_blkrd;
static mt_blkrd_t * mt_blkrd_init(mt_linesplit_t *linesplit, input_files_t *input_files, size_t total_size) {
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  mt_blkrd_t *blkrd = &mt_blkrd;
  blkrd->fps = md_malloc(sizeof(*blkrd->fps)+sizeof(blkrd->fps->fp[0])*input_files->n, mem_core_ext, 1/* zero clear*/);
  blkrd->fps->n = input_files->n;
  blkrd->input_files = input_files;
  blkrd->linesplit = linesplit;
  blkrd->w = md_malloc(sizeof(blkrd->w[0])*n_block_reader, mem_blkrdInit, 1);
  blkrd->read_bytes = 0;
  blkrd_free_list2_init(&blkrd->blkrd_free_list2);
  linesplit->samparse->blkrd_free_list2 = &blkrd->blkrd_free_list2;

  int j;
  const int cpu_vec_len = (n_physical_cores*SMT+63)/64;
  uint64_t cpu_vec[cpu_vec_len];
  memset(cpu_vec, 0, sizeof(uint64_t)*cpu_vec_len);
  for(j=0; j<n_block_reader; j++){
    blkrd->w[j].mt = blkrd;
    pthread_create(&blkrd->w[j].tid, &attr, mt_block_reader, &blkrd->w[j]);
    char tn[32];
    sprintf(tn, "blk_reader-%d-%d\n", j, CPU_BLKRD(j));
    if(pthread_setname_np(blkrd->w[j].tid, tn)){}
#if defined(CPU_SET_AFFINITY)
{
      cpu_set_t cpuset;
      CPU_ZERO(&cpuset);
      CPU_SET2(CPU_BLKRD(j), &cpuset);
      cpu_vec[CPU_BLKRD(j)/64] |= 1LL<<(63-CPU_BLKRD(j)%64);
      pthread_setaffinity_np(blkrd->w[j].tid, sizeof(cpu_set_t), &cpuset);
}
#endif
  }
  fprintf(stderr, "(I) CPU %15s ", "block_reader");
  int i;
  for(i=0; i<cpu_vec_len; i++) {
    int j;
    for(j=0; j<64; j++) {
      fprintf(stderr, "%s%c", j%SMT ? "" : " ", (cpu_vec[i]&(1LL<<(63-j))) ? 'o' : '-');
    }
  }
  fprintf(stderr, "\n");

  if (g_drop_cache) {
    blkrd->drop_cache = mt_drop_cache_init(blkrd);
  }
  if (NULL == getenv("DUMP_THROUGHPUT")){
    blkrd->progress = mt_progress_init(blkrd, total_size);
  }

  return &mt_blkrd;
}

static void mt_blkrd_end(mt_blkrd_t *blkrd) {
  int i;

  for(i=0; i<n_block_reader; i++) {
    pthread_join(blkrd->w[i].tid, 0);
  }
  if (blkrd->progress) {
    mt_progress_end(blkrd->progress);
  }

  mt_linesplit_end(blkrd->linesplit);

  if (g_drop_cache) {
    mt_drop_cache_end(blkrd->drop_cache);
  }
  for(i=0; i<blkrd->fps->n; i++) {
    sam_close(blkrd->fps->fp[i]);
  }
}

static bamwrite_queue_array_t * bamwrite_queue_array_init(void) {
  bamwrite_queue_array_t * const p = md_malloc(sizeof(bamwrite_queue_array_t) + sizeof(bamwrite_queue_t*)*BAMWRITE_QUEUE_ARRAY_SIZE, mem_bamwriteQueueArrayInit, 1);
  p->size = BAMWRITE_QUEUE_ARRAY_SIZE;
  p->first = 0;
  p->last = -1;
  return p;
}

static void *mt_bam_writer(void *data){
  bamwrite_work_t * const w = (bamwrite_work_t*)data;
  mt_bamwrite_t * const mt = w->mt;
  int const thd_idx = w - mt->w;
  bamwrite_queue_array_t * const qarr = mt->ordered_q;

//fprintf(stderr, "#### bam_writer first %ld last %ld\n", qarr->first, qarr->last);

  while(1){
    bamwrite_queue_t * const q = qarr->arr[qarr->first];
    if (NULL != q) {
      FINISH_SPIN_WAIT_LOOP();
//fprintf(stderr, "#### sam_write3() len %d first %ld last %ld\n", q->len, qarr->first, qarr->last);
      int k;
      for(k=0; k < q->len; k++) {
        if (NULL != q->bam[k].b) {
	  // not excluded by api_pre_filter
          sam_write3(mt->out_fp, mt->header, q->bam[k].b);
	}
      }
      bamwrite_queue_free(q, &w->bamwrite_queue_free_list);
      qarr->first++;
      mt->w[thd_idx].n_deq ++;
    } else {
      IN_SPIN_WAIT_LOOP();
      if (mt->done) {
        break;
      }
//fprintf(stderr, "#### empty first %ld last %ld\n", qarr->first, qarr->last);
    }
  } /* while 1 */

  return NULL;
}

static mt_bamwrite_t mt_bamwrite;
static mt_bamwrite_t * mt_bamwrite_init(samFile *out_fp, bam_hdr_t *header) {
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  mt_bamwrite_t *bamwrite = &mt_bamwrite;
  bamwrite->ordered_q = bamwrite_queue_array_init();
  bamwrite->out_fp = out_fp;
  bamwrite->header = header;
  //bamwrite->linesplit = linesplit;
  bamwrite->w = md_malloc((sizeof(bamwrite->w[0]) + sizeof(bamwrite->w[0].q[0])*n_sam_parser)*n_bam_writer, mem_bamwriteInit, 1);

  //bamwrite->read_bytes = 0;
  //bamwrite_free_list2_init(&bamwrite->bamwrite_free_list2);
  bam_free_list2_init(&bamwrite->bam_free_list2);
  filters.p_bam_free_list2 = &bamwrite->bam_free_list2;
  //linesplit->samparse->bamwrite_free_list2 = &bamwrite->bamwrite_free_list2;

  int j;
  const int cpu_vec_len = (n_physical_cores*SMT+63)/64;
  uint64_t cpu_vec[cpu_vec_len];
  memset(cpu_vec, 0, sizeof(uint64_t)*cpu_vec_len);
  for(j=0; j<n_bam_writer; j++){
    bamwrite->w[j].mt = bamwrite;
    pthread_create(&bamwrite->w[j].tid, &attr, mt_bam_writer, &bamwrite->w[j]);
    if(pthread_setname_np(bamwrite->w[j].tid, "bam_writer")){}
#if defined(CPU_SET_AFFINITY)
{
      cpu_set_t cpuset;
      CPU_ZERO(&cpuset);
      CPU_SET2(CPU_BAMWT(j), &cpuset);
      cpu_vec[CPU_BAMWT(j)/64] |= 1LL<<(63-CPU_BAMWT(j)%64);
      pthread_setaffinity_np(bamwrite->w[j].tid, sizeof(cpu_set_t), &cpuset);
}
#endif
  }
  fprintf(stderr, "(I) CPU %15s ", "bam_writer");
  int i;
  for(i=0; i<cpu_vec_len; i++) {
    int j;
    for(j=0; j<64; j++) {
      fprintf(stderr, "%s%c", j%SMT ? "" : " ", (cpu_vec[i]&(1LL<<(63-j))) ? 'o' : '-');
    }
  }
  fprintf(stderr, "\n");
  return &mt_bamwrite;
}


#if defined(PROFILE_MEM)
static void dump_bucket_stat0(bucket_array_t *ba, const char *s, int is_unclipped) {
  int i;
  fprintf(stderr, "[%s] backet_array min %ld max %ld\n", s, ba->min, ba->max);
  size_t bu_exist = 0, bu_not_exist = 0, bu_filled = 0, bu_not_filled = 0;
  int n_mapped = 16;
  size_t *mapped =malloc(sizeof(mapped[0])*n_mapped);
  size_t unmapped = 0;
  memset(mapped, 0, sizeof(mapped[0])*n_mapped);
  typedef struct {
      int i, j;
      int len;
  } many_reads_t;
  int n_many_reads = 0;
  many_reads_t *many_reads = NULL;

  size_t free_size = 0;
  for(i=ba->min; i<=ba->max; i++){
    bucket_t *bu = get_bucket(i, ba, 0);
    if (bu) {
      bamptr_space_ctl_t * const bsctl = bu->bsctl;
      int j;
      for(j=0; j<N_BAMPTR_SPACE_FREE_LIST; j++){
        bamptr_array_t *ba;
        for(ba=bsctl->free[j]; ba; ba=ba->free_next) {
          free_size += sizeof(ba[0])+(j+1)*sizeof_bamptr(is_unclipped);
        }
      }
      bu_exist++;
    } else {
      bu_not_exist++;
    }
    int j;
    if (bu){
    for(j=bu->min; j<=bu->max; j++){
      bamptr_t * const top = get_fifo_list_from_bucket(bu, j, 0);
      int cnt = 0;
      if (top) {
        bu_filled++;
#if defined(BAMP_ARRAY)
	bamptr_array_t * const bwa = container_of(top, bamptr_array_t, bp[0]);
	cnt = bwa->size;
	if (cnt > 1000) {
	  int k;
	  int found = 0;
	  for (k=0; k<n_many_reads; k++){
	    if (i==many_reads[k].i && j==many_reads[k].j) {
	      found = 1;
	      break;
	    }
	  }
	  if (found) many_reads[k].len = cnt;
	  else {
	    n_many_reads++;
	    many_reads = (NULL == many_reads ? malloc(sizeof(many_reads[0])) : realloc(many_reads, sizeof(many_reads[0])*n_many_reads));
	    many_reads[n_many_reads-1].i=i;
	    many_reads[n_many_reads-1].j=j;
	    many_reads[n_many_reads-1].len=cnt;
	  }
	}
#else
	for(bamp=top; bamp; bamp=bamp->clipped_next) { cnt++; }
#endif
	if(i==0 && j==0){
	  // unmapped
	  unmapped=cnt;
	} else {
	  if (cnt > n_mapped-1) {
	    int new = cnt > n_mapped*2-1 ? cnt+1 : n_mapped*2;
	    mapped=realloc(mapped, sizeof(mapped[0])*new);
	    memset(mapped+n_mapped, 0, sizeof(mapped[0])*(new-n_mapped));
	    n_mapped = new;
	  }
	  mapped[cnt]++;
	}
      } else {
        bu_not_filled++;
      }
    }
    }
  }
  for(i=0; i<n_many_reads; i++){
    fprintf(stderr, "[%s] i %d j %d #reads %d\n", s, many_reads[i].i, many_reads[i].j, many_reads[i].len);
  }
  fprintf(stderr, "[%s] bytes managed in the free lists of bamptr arrays %ld\n", s, free_size);
  fprintf(stderr, "[%s] unmapped count %ld\n", s, unmapped);
  for(i=0; i<n_mapped; i++){
    if (mapped[i]) fprintf(stderr, "bamptr array size %d count %ld\n", i, mapped[i]);
  }
  fprintf(stderr, "[%s] array filled %.2f exist %ld not %ld bucket filled %.2f exist %ld not %ld\n",  s,
	  bu_exist*100.0/(bu_exist+bu_not_exist), bu_exist, bu_not_exist,
	  bu_filled*100.0/(bu_filled+bu_not_filled), bu_filled, bu_not_filled);
  fflush(stderr);
  free(mapped);
  if (many_reads) free(many_reads);
}
static void dump_bucket_stat() {
  bucket_array_t *ba = (bucket_array_t*)stat_bucket_array;
  dump_bucket_stat0(ba, "clipped", 0);
  ba = (bucket_array_t*)stat_bucketU_array;
  dump_bucket_stat0(ba, "unclipped", 1);
}
#endif

#if defined(DUMP_THROUGHPUT)
typedef struct {
  pthread_t tid;
  mt_blkrd_t *blkrd;
  mt_linesplit_t *linesplit;
  mt_samparse_t *samparse;
  mt_bamcopy_t *bamcopy;
  mt_hash_t *hash;
  volatile int done;
} mt_prof_t;

static void* mt_throughput(void *data) {
  mt_prof_t * const prof = (mt_prof_t*)data;
  mt_blkrd_t *p_blkrd = prof->blkrd;
  mt_linesplit_t *p_linesplit = prof->linesplit;
  mt_samparse_t *p_samparse = prof->samparse;
  mt_bamcopy_t *p_bamcopy = prof->bamcopy;
  mt_hash_t *p_hash = prof->hash;

  size_t last_rd = p_blkrd->read_bytes;
  size_t last_deq_split[n_line_splitter], last_deq_parse[n_sam_parser], last_deq_copy[n_bam_copier], last_deq_hash[n_hash_adder];
  memset(last_deq_split, 0, sizeof(last_deq_split));
  memset(last_deq_parse, 0, sizeof(last_deq_parse));
  memset(last_deq_copy, 0, sizeof(last_deq_copy));
  memset(last_deq_hash, 0, sizeof(last_deq_hash));
  size_t last_spn_split[n_line_splitter], last_spn_parse[n_sam_parser], last_spn_copy[n_bam_copier], last_spn_hash[n_hash_adder];
  memset(last_spn_split, 0, sizeof(last_spn_split));
  memset(last_spn_parse, 0, sizeof(last_spn_parse));
  memset(last_spn_copy, 0, sizeof(last_spn_copy));
  memset(last_spn_hash, 0, sizeof(last_spn_hash));
  struct timeval time_last;
  gettimeofday(&time_last, NULL);

rd_progress_initialized = 1;

  while(1) {
      struct timeval time_end;
      sleep(1);
      gettimeofday(&time_end, NULL);
      double time = time_end.tv_sec - time_last.tv_sec + (time_end.tv_usec-time_last.tv_usec)/1000000.0;
      time_last = time_end;

      fprintf(stderr, "(I) [read] %ld MB %.1f s %s%.1f%s MB/s",
	      p_blkrd->read_bytes/1024/1024, time, "\33[34;1m", (p_blkrd->read_bytes - last_rd)/1024/1024/time, "\33[0m");
      int i;
      /*split*/
      fprintf(stderr, "\n    [splt] ");
      for(i=0; i<n_line_splitter; i++) {
	int spn = (int)((p_linesplit->w[i].n_spn - last_spn_split[i])/1000/1000);
	fprintf(stderr, "(%d %s%1dM%s %4d %4.0f)",i, spn>9?"\33[31;1m":"\33[0m", spn, spn>9?"\33[0m":"", (int)(p_linesplit->w[i].n_enq - p_linesplit->w[i].n_deq), (p_linesplit->w[i].n_deq -last_deq_split[i])/time);
        last_deq_split[i] = p_linesplit->w[i].n_deq;
        last_spn_split[i] = p_linesplit->w[i].n_spn;
      }
      /*parse*/
      fprintf(stderr, "\n    [pars] ");
      for(i=0; i<n_sam_parser; i++) {
	int spn = (int)((p_samparse->w[i].n_spn - last_spn_parse[i])/1000/1000);
	fprintf(stderr, "(%d %s%1dM%s %4d %4.0f)",i, spn>9?"\33[31;1m":"\33[0m", spn, spn>9?"\33[0m":"", (int)(p_samparse->w[i].n_enq - p_samparse->w[i].n_deq), (p_samparse->w[i].n_deq -last_deq_parse[i])/time);
        last_deq_parse[i] = p_samparse->w[i].n_deq;
        last_spn_parse[i] = p_samparse->w[i].n_spn;
      }
if (p_bamcopy) {
      /*copy*/
      fprintf(stderr, "\n    [copy] ");
      for(i=0; i<n_bam_copier; i++) {
	int spn = (int)((p_bamcopy->w[i].n_spn - last_spn_copy[i])/1000/1000);
	fprintf(stderr, "(%d %s%1dM%s %4d %4.0f)",i, spn>9?"\33[31;1m":"\33[0m", spn, spn>9?"\33[0m":"", (int)(p_bamcopy->w[i].n_enq - p_bamcopy->w[i].n_deq), (p_bamcopy->w[i].n_deq -last_deq_copy[i])/time);
        last_deq_copy[i] = p_bamcopy->w[i].n_deq;
        last_spn_copy[i] = p_bamcopy->w[i].n_spn;
      }
}
if (p_hash) {
      /*hash*/
      fprintf(stderr, "\n    [hash] ");
      for(i=0; i<n_hash_adder; i++) {
	int spn = (int)((p_hash->w[i].n_spn - last_spn_hash[i])/1000/1000);
	fprintf(stderr, "(%d %s%1dM%s %4d %4.0f)",i, spn>9?"\33[31;1m":"\33[0m", spn, spn>9?"\33[0m":"", (int)(p_hash->w[i].n_enq - p_hash->w[i].n_deq), (p_hash->w[i].n_deq -last_deq_hash[i])/time);
        last_deq_hash[i] = p_hash->w[i].n_deq;
        last_spn_hash[i] = p_hash->w[i].n_spn;
      }
}
      fprintf(stderr, "\n");

      last_rd = p_blkrd->read_bytes;

//if (p_hash) {
//      if (p_hash->done) break;
//}
      if (prof->done) break;
  }
  return NULL;
}

mt_prof_t prof;
static mt_prof_t *mt_throughput_init(mt_blkrd_t *blkrd, mt_linesplit_t *linesplit, mt_samparse_t *samparse, mt_bamcopy_t *bamcopy, mt_hash_t *hash) {
  mt_prof_t *mt = &prof;
  mt->blkrd = blkrd;
  mt->linesplit = linesplit;		linesplit->prof_n_spn = 1;
  mt->samparse = samparse;		samparse->prof_n_spn = 1;
  mt->bamcopy = bamcopy;		bamcopy->prof_n_spn = 1;
  mt->hash = hash;			hash->prof_n_spn = 1;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_create(&mt->tid, &attr, mt_throughput, mt);
  if(pthread_setname_np(mt->tid, "dump_thr")){}
  return mt;
}

static void mt_throughput_end(mt_prof_t *mt) {
  mt->done = 1;

  pthread_join(mt->tid, NULL);
}

#endif

extern void bgzf_progress(int *, int*, size_t*, int*, int*, long*, double*);

typedef struct {
  pthread_t tid;
  volatile int done;
} mt_prof2_t;

static int prof_bucket_array_min;
static int prof_bucket_array_max;
static volatile int prof_bucket_array_idx;
#define min(a,b) ((a)>(b) ? (b) : (a))

static void* mt_wrt_throughput(void *data) {
  mt_prof2_t * const prof = (mt_prof2_t*)data;

  size_t last_mem = 0;
  struct timeval last_time;
  size_t mem = 0;
  struct timeval now;
  gettimeofday(&now, NULL);
  last_time = now;
  int writer_done = 0;
  int sync_done = 0;
  long sync_count;
  double sync_time;
  const int ba_length = prof_bucket_array_max - prof_bucket_array_min + 1; 
  const int ba_min = prof_bucket_array_min;

  while(!rd_progress_initialized){
    sleep(1);
  }

  fprintf(stderr, "(I) 0                       50                      100 %% (wr)\n");
  fprintf(stderr, "(I) +----+----+----+----+----+----+----+----+----+----+ (wr)\n");
  fprintf(stderr, "(I)  ");
  int i;
  for (i=0; i<50; i++) fprintf(stderr, ".");
  fprintf(stderr, " (wr)\n");
  wr_progress_initialized= 1;
  mt_wrt_throughput_offset = 1;

  while(1) {
    sleep(1);
#if 1 // throughput
    gettimeofday(&now, NULL);
    double t = now.tv_sec-last_time.tv_sec+(now.tv_usec-last_time.tv_usec)/1000000.0;
    int to_be_dfl, being_dfl;
    bgzf_progress(&to_be_dfl, &being_dfl, &mem, &writer_done, &sync_done, &sync_count, &sync_time);
    const int percent = (filters.use_baminfo ?
    			(prof_bucket_array_idx-ba_min)*100/ba_length :
			min(100,rd_progress_percent*mem*100/68/rd_progress_bytes)) + 1;
    fprintf(stderr, "\x1b[%dA", mt_wrt_throughput_offset); // move the cursor -N lines
    fprintf(stderr,
    		   "\x1b[K"	// clear line
    		   "(I)  ");
    for (i=0; i<percent/2; i++) fprintf(stderr, "#");
    for (i=percent/2; i<50; i++) fprintf(stderr, ".");
    fprintf(stderr," %3d %% %ld MB %.1f MB/sec (%d to be deflated %d being deflated)" 
		   , percent-1, mem/1024/1024, (mem - last_mem)/1024/1024/t, to_be_dfl, being_dfl);
    fprintf(stderr, "\r\x1b[%dB", mt_wrt_throughput_offset); // move the cursor +N lines
    last_time = now;
    last_mem = mem;
#endif

    if (prof->done) break;
  }
  return NULL;
}

mt_prof2_t prof2;
static mt_prof2_t *mt_wrt_throughput_init() {
  mt_prof2_t *mt = &prof2;

  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_create(&mt->tid, &attr, mt_wrt_throughput, mt);
  if(pthread_setname_np(mt->tid, "dump_thr2")){}
  return mt;
}

static void mt_wrt_throughput_end(mt_prof2_t *mt) {
  mt->done = 1;

  pthread_join(mt->tid, NULL);
}

static void read_pool_init(bucket_array_t** p_bucket_array) {
	const int sz = sizeof(bucket_t*)*BUCKET_ARRAY_LENGTH;
	*p_bucket_array = md_malloc(sizeof(bucket_array_t), mem_readPoolInit1, 0);
	(*p_bucket_array)->min = BUCKET_ARRAY_LENGTH;
	(*p_bucket_array)->max = 0;
	(*p_bucket_array)->ba = md_malloc(sz, mem_readPoolInit2, 1);
}

static int get_n_physical_cores(void) {
  return n_physical_cores;
}

static inline uint64_t gen_id_clipped(vid_t tid, vid_t pos);
static inline bamptr_t *get_fifo_list_from_bucket(bucket_t *bu, int idx2, int unclipped);
static inline uint32_t get_offset_unclipped_pos_from_index(int i, int j);
//
// (i,j) -> (lid,tid,pos)
// offset_unclipped_pos = pos + bamp_c->unclip + OFFSET
// (lid,tid,offset_unclipped_pos) -> (i',j')
// 
static inline bamptr_t *bamptr_get_U(bamptr_t *bamp_c, int i, int j) {
  fprintf(stderr, "(E) bamptr_get_U: not implemented\n");
  exit(-1);
  return *(bamptr_t**)0;
}

static int picard_sortsam_cmpfunc(const void * v1, const void * v2, void * args) {
  bamptr_t *bamp1 = (bamptr_t*)v1; 
  bamptr_t *bamp2 = (bamptr_t*)v2; 
#if defined(DEBUG_POS)
  int const dump = *(int*)args;
#endif
//
// Emulate SortSam (SAMRecordCoordinateComparator)
//
//
//	1. RNAME index	(not needed - bamptrs@(unclip i,unclip j) have the same tid)
//	2. POS		(needed - bamptrs@(unclip i,unclip j) can have different clipped pos)
//	3. FLAG reverse
//	4. QNAME
//	5. FLAG
//	6. MAPQ
//	7. RNEXT index	(NA for frag)
//	8. PNEXT 	(NA for frag)
//	9. TLEN 	(NA for frag)
//


{
//	1. RNAME index
    uint32_t const tid1_read = bamptr_get_tid(bamp1,0);
    uint32_t const tid2_read = bamptr_get_tid(bamp2,0);
    int const r_tid = tid1_read - tid2_read;
#if defined(DEBUG_POS)
    if (dump) {
      fprintf(stderr, "(I) cmpfunc read tid %u %c %u\n", tid1_read, r_tid==0?'=':(r_tid>0?'<':'>'), tid2_read);
    }
#endif
    if (r_tid) return r_tid;

//	2. POS
    uint32_t const pos1_read = bamptr_get_pos(bamp1,0);
    uint32_t const pos2_read = bamptr_get_pos(bamp2,0);
    int const r_pos = pos1_read - pos2_read;
#if defined(DEBUG_POS)
    if (dump) {
      fprintf(stderr, "(I) cmpfunc read pos %u %c %u\n", pos1_read, r_pos==0?'=':(r_pos>0?'<':'>'), pos2_read);
    }
#endif
    if (r_pos) return r_pos;

//	3. FLAG reverse
    uint32_t flag1 = bamptr_get_flag(bamp1);
    uint32_t flag2 = bamptr_get_flag(bamp2);
    uint32_t rev1_read = (flag1 & BAM_FREVERSE);
    uint32_t rev2_read = (flag2 & BAM_FREVERSE);
    int const r_rev = rev1_read - rev2_read;
#if defined(DEBUG_POS)
    if (dump) {
      fprintf(stderr, "(I) cmpfunc read rev %u %c %u\n", rev1_read, r_rev==0?'=':(r_rev>0?'<':'>'), rev2_read);
    }
#endif
    if (r_rev) return r_rev;

//	4. QNAME
    //int const r_qname = strcmp(dump_bamptr_get_qname(bamp1,0), dump_bamptr_get_qname(bamp2,0));
    uint64_t qnid1 = bamptr_get_qnameid(bamp1);
    uint64_t qnid2 = bamptr_get_qnameid(bamp2);
    if (qnid1 != qnid2) {
      char str1[128], str2[128];
      long len1, len2;
      get_qname(qnid1,str1,sizeof(str1),&len1);
      get_qname(qnid2,str2,sizeof(str2),&len2);
      int const diff_len = len1 - len2;
      if (diff_len != 0) {
        memset(diff_len > 0 ? str2+len2 : str1+len1, 0, diff_len > 0 ? diff_len : -diff_len);
      }
      int const longer = (diff_len > 0 ? len1 : len2);
      int const r_qname = memcmp(str1, str2, longer);
#if defined(DEBUG_POS)
      if (dump) {
        fprintf(stderr, "(I) cmpfunc read %.*s %c %.*s\n",  (int)len1, str1, r_qname==0?'=':(r_qname>0?'<':'>'), (int)len2, str2);
      }
#endif
      if (r_qname) return r_qname;
    }

//	5. FLAG
    int const r_flag = flag1 - flag2;
#if defined(DEBUG_POS)
    if (dump) {
      fprintf(stderr, "(I) cmpfunc flag %u %c %u\n", flag1, r_flag==0?'=':(r_flag>0?'<':'>'), flag2);
    }
#endif
    if (r_flag) return r_flag;

//	6. MAPQ
    uint32_t const mapq1_read = bamptr_get_mapq(bamp1);
    uint32_t const mapq2_read = bamptr_get_mapq(bamp2);
    int const r_mapq = mapq1_read - mapq2_read;
#if defined(DEBUG_POS)
    if (dump) {
      fprintf(stderr, "(I) cmpfunc mapq %u %c %u\n", mapq1_read, r_mapq==0?'=':(r_mapq>0?'<':'>'), mapq2_read);
    }
#endif
    if (r_mapq) return r_mapq;
#if 0
    static long debug_mapq = 1;
    if (
#if defined(DEBUG_POS)
	dump ||
#endif
	debug_mapq
       ) {
      debug_mapq = 0;
      char str1[128];
      long len1;
      get_qname(qnid1,str1,sizeof(str1),&len1);
      fprintf(stderr, "(W) cmpfunc read needs MAPQ OR RNEXT (RNAME, POS, QNAME, and FLAG are the same). "
    	"%.*s flag %d tid %d pos %d bamp1 %p bamp2 %p\n", (int)len1, str1, bamptr_get_flag(bamp1), bamptr_get_tid(bamp1,0), bamptr_get_pos(bamp1,0), bamp1, bamp2);
    }
#endif
//	7. RNEXT index	(NA for frag)
    uint32_t const mtid1_read = bamptr_get_tid(bamp1,1);
    uint32_t const mtid2_read = bamptr_get_tid(bamp2,1);
    int const r_mtid = mtid1_read - mtid2_read;
#if defined(DEBUG_POS)
    if (dump) {
      fprintf(stderr, "(I) cmpfunc read mtid %u %c %u\n", mtid1_read, r_mtid==0?'=':(r_mtid>0?'<':'>'), mtid2_read);
    }
#endif
    if (r_mtid) return r_mtid;

//	8. PNEXT 	(NA for frag)
    uint32_t const mpos1_read = bamptr_get_pos(bamp1,1);
    uint32_t const mpos2_read = bamptr_get_pos(bamp2,1);
    int const r_mpos = mpos1_read - mpos2_read;
#if defined(DEBUG_POS)
    if (dump) {
      fprintf(stderr, "(I) cmpfunc read mpos %u %c %u\n",  mpos1_read, r_mpos==0?'=':(r_mpos>0?'<':'>'), mpos2_read);
    }
#endif
    if (r_mpos) return r_mpos;

//	9. TLEN 	(NA for frag)
    int32_t const tlen1_read = bamptr_get_tlen(bamp1);
    int32_t const tlen2_read = bamptr_get_tlen(bamp2);
    int const r_tlen = tlen1_read - tlen2_read;
#if defined(DEBUG_POS)
    if (dump) {
      fprintf(stderr, "(I) cmpfunc read tlen %u %c %u\n",  tlen1_read, r_tlen==0?'=':(r_tlen>0?'<':'>'), tlen2_read);
    }
#endif
    if (r_tlen) return r_tlen;
#if 0
    static long debug_tlen = 1;
    if (
#if defined(DEBUG_POS)
	dump ||
#endif
	debug_tlen
      ) {
      debug_tlen = 0;
      fprintf(stderr, "(W) cmpfunc needs TLEN (RNAME, POS, QNAME, FLAG, (MAPQ, RNEXT,) and PNEXT are the same).\n");
    }
#endif
  }
  return 0;
}

static int picard_sortsam_cmpfunc_clip(const void * v1, const void * v2, void * args) {
  int const i = (int)((uint64_t*)args)[0];
  int const j = (int)((uint64_t*)args)[1];
  bucket_array_t * const bucketU_array = (bucket_array_t*)((uint64_t*)args)[2];
#if defined(DEBUG_POS)
  int param = 0;
  //const vid_t c_tid = get_tid_from_index(i, j);
  //const vid_t c_pos = get_pos_from_index(i, j);
  param = (int)((uint64_t*)args)[3];
  int dump = param;
#endif
//
// Emulate SortSam (SAMRecordCoordinateComparator)
//
//
//	1. RNAME index	(not needed - bamptrs@(clip i,clip j) have the same tid)
//	2. POS		(not needed - bamptrs@(clip i,clip j) can have different clipped pos)
//	3. FLAG reverse
//	4. QNAME
//	5. FLAG
//	6. MAPQ
//	7. RNEXT index	(NA for frag)
//	8. PNEXT 	(NA for frag)
//	9. TLEN 	(NA for frag)
//
  bamptr_t *c_bamp1 = (bamptr_t*)v1; // clipped 
  bamptr_t *c_bamp2 = (bamptr_t*)v2; // clipped
  const uint32_t flag1 = bamptrC_get_flag(c_bamp1);
  const uint32_t flag2 = bamptrC_get_flag(c_bamp2);

//	3. FLAG reverse
  const uint32_t rev1_read = (flag1 & BAM_FREVERSE);
  const uint32_t rev2_read = (flag2 & BAM_FREVERSE);
  int const r_rev = rev1_read - rev2_read;
#if defined(DEBUG_POS)
  if (dump) {
    fprintf(stderr, "(I) cmpfunc_clip read rev %u %c %u\n", rev1_read, r_rev==0?'=':(r_rev>0?'<':'>'), rev2_read);
  }
#endif
  if (r_rev) return r_rev;

//	4. QNAME
  bamptr_t *u_bamp1 = NULL;
  bamptr_t *u_bamp2 = NULL;
  char str1[128], str2[128];
  long len1, len2;
  uint64_t qnid1, qnid2;
  if (0 == (flag1 & BAM_FUNMAP)) {
    u_bamp1 = bamptr_C2U(c_bamp1, bucketU_array, i, j);
    qnid1 = bamptr_get_qnameid(u_bamp1);
  } else {
    qnid1 = bamptrC_get_qnameid(c_bamp1);
  }

  if (0 == (flag2 & BAM_FUNMAP)) {
    u_bamp2 = bamptr_C2U(c_bamp2, bucketU_array, i, j);
    qnid2 = bamptr_get_qnameid(u_bamp2);
  } else {
    qnid2 = bamptrC_get_qnameid(c_bamp2);
  }
  if (qnid1 != qnid2) {
    get_qname(qnid1,str1,sizeof(str1),&len1);
    get_qname(qnid2,str2,sizeof(str2),&len2);
    int const diff_len = len1 - len2;
    if (diff_len != 0) {
      memset(diff_len > 0 ? str2+len2 : str1+len1, 0, diff_len > 0 ? diff_len : -diff_len);
    }
    int const longer = (diff_len > 0 ? len1 : len2);
    int const r_qname = memcmp(str1, str2, longer);
#if defined(DEBUG_POS)
    if (dump) {
      fprintf(stderr, "(I) cmpfunc_clip read %.*s %c %.*s\n",  (int)len1, str1, r_qname==0?'=':(r_qname>0?'<':'>'), (int)len2, str2);
    }
#endif
    if (r_qname) return r_qname;
  }

//	5. FLAG
  int const r_flag = flag1 - flag2;
#if defined(DEBUG_POS)
  if (dump) {
    fprintf(stderr, "(I) cmpfunc flag %u %c %u\n", flag1, r_flag==0?'=':(r_flag>0?'<':'>'), flag2);
  }
#endif
  if (r_flag) return r_flag;

//	6. MAPQ
  uint32_t const mapq1_read = (0 == (flag1 & BAM_FUNMAP) ? bamptr_get_mapq(u_bamp1) : 0);
  uint32_t const mapq2_read = (0 == (flag2 & BAM_FUNMAP) ? bamptr_get_mapq(u_bamp2) : 0);
  int const r_mapq = mapq1_read - mapq2_read;
#if defined(DEBUG_POS)
  if (dump) {
    fprintf(stderr, "(I) cmpfunc mapq %u %c %u\n", mapq1_read, r_mapq==0?'=':(r_mapq>0?'<':'>'), mapq2_read);
  }
#endif
  if (r_mapq) return r_mapq;
#if 0
  static long debug_mapq = 1;
  if (
#if defined(DEBUG_POS)
	dump ||
#endif
	debug_mapq
       ) {
    debug_mapq = 0;
    char str1[128];
    long len1;
    get_qname(qnid1,str1,sizeof(str1),&len1);
#if defined(DEBUG_POS)
    fprintf(stderr, "(W) cmpfunc_clip read needs MAPQ OR RNEXT (RNAME, POS, QNAME, and FLAG are the same). "
    	"%.*s (dump %s) flag %d bamp1 %p bamp2 %p\n", (int)len1, str1, dump_bamptr_get_qname(u_bamp1,0), flag1, c_bamp1, c_bamp2);
#else
    fprintf(stderr, "(W) cmpfunc_clip read needs MAPQ OR RNEXT (RNAME, POS, QNAME, and FLAG are the same). "
    	"%.*s flag %d bamp1 %p bamp2 %p\n", (int)len1, str1, flag1, c_bamp1, c_bamp2);
#endif
  }
  // int const r_mapq = bamptr_get_mapq(bamp1,0) - bamptr_get_mapq(bamp2,0);
  // int const r_mtid = bamptr_get_tid(bamp1,1) - bamptr_get_tid(bamp2,1);
#endif

//	7. RNEXT index	(NA for frag)
  uint32_t const mtid1_read = ((0 == (flag1 & BAM_FUNMAP)) ? bamptr_get_tid(u_bamp1,1) : -1);
  uint32_t const mtid2_read = ((0 == (flag2 & BAM_FUNMAP)) ? bamptr_get_tid(u_bamp2,1) : -1);
  int const r_mtid = mtid1_read - mtid2_read;
#if defined(DEBUG_POS)
  if (dump) {
    fprintf(stderr, "(I) cmpfunc_clip read mtid %u %c %u\n", mtid1_read, r_mtid==0?'=':(r_mtid>0?'<':'>'), mtid2_read);
  }
#endif
  if (r_mtid) return r_mtid;

//	8. PNEXT 	(NA for frag)
  uint32_t const mpos1_read = ((0 == (flag1 & BAM_FUNMAP)) ? bamptr_get_pos(u_bamp1,1) : -1);
  uint32_t const mpos2_read = ((0 == (flag2 & BAM_FUNMAP)) ? bamptr_get_pos(u_bamp2,1) : -1);
  int const r_mpos = mpos1_read - mpos2_read;
#if defined(DEBUG_POS)
  if (dump) {
    fprintf(stderr, "(I) cmpfunc_clip read mpos %u %c %u\n",  mpos1_read, r_mpos==0?'=':(r_mpos>0?'<':'>'), mpos2_read);
  }
#endif
  if (r_mpos) return r_mpos;
 
//	9. TLEN 	(NA for frag)
  int32_t const tlen1_read = (0 == (flag1 & BAM_FUNMAP) ? bamptr_get_tlen(u_bamp1) : 0);
  int32_t const tlen2_read = (0 == (flag2 & BAM_FUNMAP) ? bamptr_get_tlen(u_bamp2) : 0);
  int const r_tlen = tlen1_read - tlen2_read;
#if defined(DEBUG_POS)
  if (dump) {
    fprintf(stderr, "(I) cmpfunc_clip read tlen %u %c %u\n",  tlen1_read, r_tlen==0?'=':(r_tlen>0?'<':'>'), tlen2_read);
  }
#endif
  if (r_tlen) return r_tlen;

#if 0
  static long debug_tlen = 1;
  if (
#if defined(DEBUG_POS)
      dump ||
#endif
      debug_tlen
    ) {
    debug_tlen = 0;
    fprintf(stderr, "(W) cmpfunc_clip needs TLEN (RNAME, POS, QNAME, FLAG, (MAPQ, RNEXT,) and PNEXT are the same).\n");
  }
#endif
  return 0;
}

//
// API Version 1
//
static struct func_vector_v1 fvv1 = {
    &bucket_array_get_max,
    &bucket_array_get_min,
    &bucket_get_bamptr_array_top,
    &bucket_get_max,
    &bucket_get_min,
    &gen_id_clipped,
    &get_bucket,
    &get_cpu_map,
    &get_fifo_list_from_bucket,
    &get_n_physical_cores,
    &get_offset_unclipped_pos_from_index,
    &md_free,
    &md_malloc,
    &md_realloc,
    &md_register_mem_id,

    &get_libid,
    &bam_get_libid,

    &get_qname,
    &bamptr_get_U,
    &picard_sortsam_cmpfunc,

    0, // n_thread
    NULL, // header
    NULL, // bucket_array
    NULL, // bucketU_array
};

static void init_func_vector_v1(rt_val_t *rt) {
   fvv1.n_threads = rt->n_threads;
   fvv1.header = rt->header;
   fvv1.bucket_array = rt->bucket_array;
   fvv1.bucketU_array = rt->bucketU_array;
}


enum { api_v1, n_api_versions };
static const char *api_version_str[n_api_versions] = {
  "1",
};
static const void *api_version_vec[n_api_versions] = {
   &fvv1,
};
static void (*api_initfunc_vec[n_api_versions])(rt_val_t *) = {
   &init_func_vector_v1,
};


//
// returns the function vector for an API version specified by 'ver'
//
static const void *get_func_vector(const char *ver, rt_val_t *rt) {
  const void *ret = NULL;
  int i;
  for(i=0; i<n_api_versions; i++) {
    if (0 == strcmp(api_version_str[i], ver)) {
      fprintf(stderr, "(I) supported API version: %s\n", ver);
      ret = api_version_vec[i];
      (*api_initfunc_vec[i])(rt);
      break;
    }
  }
  if (NULL == ret) {
    fprintf(stderr, "(E) unsupported API version: %s\n", ver);
    exit(-1);
  }
  return ret;
}

static const char* my_filter_args(filter_args_t *args, const char *filter_name) {
  if (NULL != args) {
    long i;
    long len = strlen(filter_name);
    for(i=0; i<args->n; i++) {
      if (0 == strncmp(args->args[i].name, filter_name, len)) {
        return args->args[i].args;
      }
    }
  }
  return NULL;
}

//
// initialize the API functions provided by a filter library specified by 'base''ent'
// also provide the API function vector to the library
//
static int load_filter(const char *base, const struct dirent *ent) {
  if (!(ent->d_name[0] == '.' && ent->d_name[1] == 0) &&
      !(ent->d_name[0] == '.' && ent->d_name[1] == '.' && ent->d_name[2] == 0)) {

    // dlopen the target library
    fprintf(stderr, "(I) FILTER: %s%s\n", base, ent->d_name);
    {
    }
    char buf[256];
    if (sizeof(buf) < strlen(base)+strlen(ent->d_name)+1) {
      fprintf(stderr, "(E) too long path name %s%s\n", base, ent->d_name);
      exit(-1);
    }
    buf[0]=0;
    strcat(buf, base);
    strcat(buf, ent->d_name);
    void *handle = dlopen(buf, RTLD_LAZY);
    if (NULL == handle) {
      fprintf(stderr, "(E) failed to dlopen %s\n", buf);
      exit(-1);
    }
    dlerror();

    // dlsym all of the API functions defined by the library
    char *e;
    void* (*f[n_filter_api])();
    int i;
    for(i=0; i<n_filter_api; i++) {
      f[i] = dlsym(handle, api_filter_name[i]);
      if (NULL != (e = dlerror())) {
        if (0 == strcmp(api_filter_name[i], "pre_filter")
         || 0 == strcmp(api_filter_name[i], "do_filter")
         || 0 == strcmp(api_filter_name[i], "analyze_data")
         || 0 == strcmp(api_filter_name[i], "post_filter")
	    ) {
          fprintf(stderr, "(I) %s provides no %s\n", buf, api_filter_name[i]);
	  f[i] = NULL;
	} else {
          fprintf(stderr, "(E) failed to dlsym %s: %s\n", api_filter_name[i], e);
          exit(-1);
        }
      }
      if (NULL != f[i]) {
        if (0 == filters.funcs[i].n) {
          filters.funcs[i].n = 1;
	  filters.funcs[i].funcs = malloc(sizeof(filters.funcs[i].funcs[1]));
	} else {
          filters.funcs[i].n++;
	  filters.funcs[i].funcs = realloc(filters.funcs[i].funcs, sizeof(filters.funcs[i].funcs[1])*filters.funcs[i].n);
	}
	filters.funcs[i].funcs[filters.funcs[i].n-1].func = f[i];
      }
    }

    // get the API version that the library requests
    const char * const ver = (*f[api_get_api_version])();

    // get the function vector that the library requests
    void * const func_vec = (void *)get_func_vector(ver, &filters.rt);

    const char *name = (*f[api_get_filter_name])();
    if (NULL == name) {
      fprintf(stderr, "(E) %s get_filter_name() failed\n", ent->d_name);
      exit(-1);
    }
    const char *args = my_filter_args(filters.args, name);

    // call init_filter() function of the library
    const char *r = (*f[api_init_filter])(func_vec, args);
    if (NULL == r) {
      fprintf(stderr, "(E) %s init_filter() failed\n", ent->d_name);
      exit(-1);
    }
    fprintf(stderr, "(I) %s init_filter() succeeded and provides \"%s\" filter\n", ent->d_name, r);
    filters.filter[filters.n_filter].p_filter_name = (char*)r;
    for(i=0; i<n_filter_api; i++) {
      filters.filter[filters.n_filter].p_filter_api[i] = f[i];
    }
    filters.n_filter++;
  }
  return 0;
}

//
// load the filter libraries under 'filter.d' directory under the path where the executable is
//
static void load_filters(int n_threads, bam_hdr_t *header, bucket_array_t *bucket_array, bucket_array_t *bucketU_array) {
  filters.n_filter = 0;
  filters.rt.n_threads = n_threads;
  filters.rt.header = header;
  filters.rt.bucket_array = bucket_array;
  filters.rt.bucketU_array = bucketU_array;

  // open the directory <path of executable>/filter.d/ to load filters under it
  char buf[256];
  ssize_t sz = readlink( "/proc/self/exe", buf, sizeof(buf)-1 );
  if (-1 == sz) {
    fprintf(stderr, "could not get the path of this program\n");
    exit(-1);
  }
  if (sz == sizeof(buf)-1) {
    fprintf(stderr, "too long path of this program\n");
    exit(-1);
  }
  buf[sz] = 0;
  rindex(buf, (int)'/')[1] = 0; // remove the program name
  if (strlen(buf)+strlen("filter.d/")+1 > sizeof(buf)) {
    fprintf(stderr, "too long path of this program\n");
    exit(-1);
  }
  strcat(buf, "filter.d/");
  struct dirent *ent;
  DIR * d = opendir(buf);
  if (NULL == d) {
    fprintf(stderr, "(I) no filters used because the directory %s was not opened\n", buf);
  }

  // load the filter libraries
  long count = 0;
  while((ent = readdir(d)) != NULL) {
    count++;
  }
  closedir(d);
  filters.filter = malloc(sizeof(filters.filter[0])*count);
  struct dirent *ent_arr[count];
  d = opendir(buf);
  int i=0;
  while((ent = readdir(d)) != NULL) {
    ent_arr[i++] = ent;
  }
if (NULL != filters.args) {
  for (i=0; i<filters.args->n; i++) {
    // check if the filter specified in the command line exists in the directory
    int const len = strlen(filters.args->args[i].name);
    int found = 0;
    int ii;
    for (ii=0; ii<count; ii++) {
      if (0 == memcmp(ent_arr[ii]->d_name, "lib_", 4) &&
          0 == memcmp(ent_arr[ii]->d_name+4, filters.args->args[i].name, len) &&
	  0 == memcmp(ent_arr[ii]->d_name+4+len, ".so", 3)) {
        found = 1;
        break;
      }
    }
    if (found) {
      load_filter(buf, ent_arr[ii]);
    } else {
      fprintf(stderr, "(E) FILTER: %s not found under %s\n", filters.args->args[i].name, buf);
      exit(1);
    }
  }
  if (filters.funcs[api_analyze_data].n || filters.funcs[api_do_filter].n) {
    filters.use_baminfo = 1;
  } else {
    n_bam_copier = 0;
    n_hash_adder = 0;
  }
}
}

/*!
  @abstract Convert SAM to compressed BAM

  @param  fn       name of the file to be sorted
  @param  prefix   prefix of the temporary files (prefix.NNNN.bam are written)
  @param  fnout    name of the final output file to be written
  @param  modeout  sam_open() mode to be used to create the final output file
  @param  max_mem  approxiate maximum memory (very inaccurate)
  @return 0 for successful sorting, negative on errors

  @discussion It may create multiple temporary subalignment files
  and then merge them by calling bam_merge_core(). This function is
  NOT thread safe.
 */
int bam_sam2bam_core_ext(input_files_t *input_files, const char *prefix, const char *fnout, const char *modeout, int paging)
{
    md_mem_init();
#if defined(DEBUG_POS)
    char *debug_str = getenv("DEBUG_POS");
    if (debug_str){
      char *endp;
      debug_pos = strtol(debug_str, &endp, 10);
      if (*endp != '\0'){
        debug_pos = 0;
      }
    }else{
      debug_pos = 0;
    }
    debug_str = getenv("DEBUG_CO");
    if(debug_str){
      char *endp;
      debug_co = strtol(debug_str, &endp, 10);
      if (*endp != '\0'){
        debug_co = -1;
      } else {
        //debug_co += UNCLIPPED_POS_OFFSET;
      }
    }else{
      debug_co = -1;
    }
    if (debug_pos) fprintf(stderr, "(I) core_ext: debug_pos %d\n", debug_pos);
    if (debug_co!=-1) fprintf(stderr, "(I) core_ext: debug_co %d\n", debug_co);
#endif

    bucket_array_t* bucket_array = NULL;
    bucket_array_t* bucketU_array = NULL;

#if 0
    if (mallopt(
    	M_MMAP_MAX, 0
    	//M_MMAP_THRESHOLD, 4*1024*1024*sizeof(long)
	) != 1) 
    {
      fprintf(stderr, "mallopt() failed");
      exit(-1);
    }
#endif


#if defined(GET_N_PHYSICAL_CORES_FROM_OS)
    n_physical_cores_valid = 0;
    int total_nproc = 0;
    FILE *cpu_online = fopen("/sys/devices/system/cpu/present", "r");
    if (cpu_online) {
      char buf[128];
      int sz = fread(buf, 1, sizeof(buf), cpu_online); 
      if (sz > 0 && buf[sz-1] == 0x0A) {
        buf[sz-1] = 0;
        char *p1 = buf;
        while(1) {
          char *token = strtok(p1, ",");
	  if (token) {
	    char *endp;
  	    int n = strtol(token, &endp, 10);
	    if (*endp == '-'){
  	      int n2 = strtol(endp+1, &endp, 10);
	      total_nproc += n2 - n + 1;
  	    } else {
	      total_nproc += 1;
	    }
  	    p1 = NULL;
	  } else
	    break;
        }
      }
      int use_nproc = total_nproc;
#if defined(__powerpc64__)
      char *use_nproc_str;
      if (NULL != (use_nproc_str = getenv("N_CPU"))) {
        char *endp;
        int n = strtol(use_nproc_str, &endp, 10);
        if (0 < n && n <= total_nproc) {
//	  if (n == 160 && total_nproc == 192) {
	    use_nproc = n;
	    fprintf(stderr, "(I) use %d cpus among %d cpus\n", use_nproc, total_nproc);
	    total_nproc = n;
//	  } else {
//	    fprintf(stderr, "(W) use %d cpus among %d cpus -- not supported and ignored\n", n, total_nproc);
//	  }
	}
      }
#endif
      cpu_map = md_malloc(sizeof(cpu_map_t)+sizeof(cpu_map->map[0])*use_nproc, mem_core_ext, 0);
      cpu_map->use_nproc = use_nproc;
      int i;
      if (use_nproc == total_nproc){
        for(i=0; i<use_nproc; i++){
	  cpu_map->map[i] = i;
        }
      } else {
	if (use_nproc == 160 && total_nproc == 192) {
          for(i=0; i<use_nproc; i++){
	    cpu_map->map[i] = i%(5/*cores*/*8/*smt*/) + i/(5*8)*(6*8);
          }
        }
      }
	{
	  n_physical_cores = use_nproc/SMT; // 0 origin
	  n_physical_cores_valid = 1;
	  fprintf(stderr, "(I) #physical_cores %d\n", n_physical_cores);
	}
    }
#endif
#if 0 && defined(CPU_SET_AFFINITY)
{
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET2(0, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
}
#endif

    gettimeofday(&time_begin, NULL);
{
    throttle.tv_sec = 0;
    throttle.tv_nsec = 500;
    read_pool_init(&bucket_array);
    read_pool_init(&bucketU_array);
    qname_hash_bucket = md_malloc(sizeof(*qname_hash_bucket)*QNAME_HASH_SIZE, mem_bamGetQnameId1, 1);
}
PROFILE_MEM_INIT();
fprintf(stderr, "(I) sam-to-bam $Rev: 91 $\n");
#if !defined(DEBUG_POS)
fprintf(stderr, "(I) **** built with no DEBUG_POS feature\n");
#endif
fprintf(stderr, "(I) main: sizeof(baminfoU_t) %lu sizeof(baminfoC_t) %lu\n", sizeof(baminfoU_t), sizeof(baminfoC_t));

    hts_init_accelerator();

bam_hdr_t * header = NULL;
size_t total_size = 0;

const size_t fps_size = sizeof(input_fps_t)+sizeof(samFile*)*input_files->n;
input_fps_t *fps = md_malloc(fps_size, mem_core_ext, 0);
fps->n = input_files->n;
int fn_i;
for (fn_i=0; fn_i < input_files->n; fn_i++) {
  char * const fn = input_files->fn[fn_i];
  int format;

  samFile *const fp = sam_open(fn, "r");
  if (fp == NULL) {
      fprintf(stderr, "[bam_sort_core] fail to open file %s\n", fn);
      return -1;
  }
  fps->fp[fn_i] = fp;
  format = fp->format.format;
#if 0
  {
      int fd = ((hFILE_fd*)((kstream_t*)fp->fp.voidp)->f->fp)->fd;
      off_t const orig = lseek(fd, 0, SEEK_CUR);
      off_t const sam_size = lseek(fd, 0, SEEK_END);
      lseek(fd, orig, SEEK_SET);
      posix_fadvise(fd, 0, sam_size, POSIX_FADV_SEQUENTIAL);
  }
#endif
  if (0 == fn_i) { // first SAM file
    header = sam_hdr_read(fp);
    cpu_map->n_targets = header->n_targets;
    cpu_map->fnout = fnout;
  }
  struct stat st_buf;
  st_buf.st_size = 0;
  const int fd = ((hFILE_fd*)((kstream_t*)(fps->fp[fn_i])->fp.voidp)->f->fp)->fd;
  if (-1 == fstat(fd, &st_buf)) {
    fprintf(stderr, "(I) mt_progress_init: fstat failed\n");
  } else {
    total_size += st_buf.st_size;
  }
  switch (format) {
  case bam: 
  case cram:
    // enqueue bam read
    //TODO:
    fprintf(stderr, "BAM/CRAM input is not supported yet\n");
    exit(-1);
    //mt_hash_enqueue(p_hash, b, g_bamid, 0, qnameid);
    break;
  case sam:
    break;
  default:
    abort();
  }
  sam_close(fps->fp[fn_i]); // input
} /* check if the headers are the same for multiple SAM file */
md_free(fps, fps_size, mem_core_ext);

    //size_t mem = 0, k = 0;
    // write sub files
#if 1
    create_rg2lib(header);

#if defined(MT_HASH)
    if (g_skip_filter) {
      fprintf(stderr, "(I) disabled filters\n");
      filters.n_filter = 0;
    } else {
      load_filters(cpu_map->use_nproc*6.9/8 /*faster than x1*/, header, bucket_array, bucketU_array);
    }
    if (filters.use_baminfo) {
      cpu_map->hts_proc_offset = 0;
      cpu_map->hts_nproc = cpu_map->use_nproc;
    } else {
      cpu_map->hts_proc_offset = cpu_map->use_nproc*1/4;
      cpu_map->hts_nproc = cpu_map->use_nproc*3/4 + SMT/* core0 */;
fprintf(stderr, "(I) use_nproc %d hts_proc_offset %d hts_nproc %d\n", cpu_map->use_nproc, cpu_map->hts_proc_offset, cpu_map->hts_nproc);
    }

#if defined(__powerpc64__)
    if (g_n_inputs > 1) {
      n_block_reader = 1; // tuned for GPFS
    }
	  switch(n_physical_cores){
	 case 24:  // e.g., power8
	    //fixed n_block_reader = 1; // on core0
	    // NA12878.bwa.sam
	    if (filters.use_baminfo) {
	    n_line_splitter = 2; // 8 threads per core for core1-23 (total 184 threads) -- GB/s
	    n_sam_parser = 70;
	    n_bam_copier = 39;
	    n_hash_adder = 73;
	    } else {
	    n_line_splitter = 4;
	    n_sam_parser = 36;
	    n_bam_copier = 0;
	    n_hash_adder = 0;
	    }
#if 0
	    n_line_splitter = 3; // 4 threads per core for core1-23 (total 92 threads) ~2.7 GB/s
	    n_sam_parser = 29;
	    n_bam_copier = 22;
	    n_hash_adder = 38;
#endif
	    break;
	  case 20: // firestone
	    n_line_splitter = 4; // 4 threads per core for core1-19 (total 76 threads) ~3.0 GB/s
	    n_sam_parser = 57;
	    n_bam_copier = 59;
	    n_hash_adder = 32;
	    break;
	  case 16: // firestone
	    n_line_splitter = 3; // used for WGS dataset in our PLoS ONE paper
	    n_sam_parser = 57;
	    n_bam_copier = 50;
	    n_hash_adder = 10;
#if 0
	    n_line_splitter = 3; // used for WEX dataset in our PLoS ONE paper
	    n_sam_parser = 43;
	    n_bam_copier = 55;
	    n_hash_adder = 19;
#endif
	    break;
	  case 8: // firestone (e.g., SoftLayer C812)
	    n_line_splitter = 1;
	    n_sam_parser = 34;
	    n_bam_copier = 18;
	    n_hash_adder = 3;
	  default:
	    break;
	  }
#elif defined(__x86_64__)
	  if (n_physical_cores == 36) { // e.g., 2 sockets of 18-core Haswell Xeon
	    //fixed n_block_reader = 1;
	    n_line_splitter = 1;
	    n_sam_parser = 8;
	    n_bam_copier = 11;
	    n_hash_adder = 15;
	  }
#endif
    char *read_str = getenv("N_BLOCKREADER");
    char *split_str = getenv("N_LINESPLITTER");
    char *parse_str = getenv("N_SAMPARSER");
    char *copy_str = getenv("N_BAMCOPIER");
    char *hash_str = getenv("N_HASHADDER");
    char *endp;
    if (read_str){
      n_block_reader = strtol(read_str, &endp, 10);
      if (*endp != '\0') n_block_reader = 1;
    }
    if (split_str){
      n_line_splitter = strtol(split_str, &endp, 10);
      if (*endp != '\0') n_line_splitter = 1;
    }
    if (parse_str){
      n_sam_parser = strtol(parse_str, &endp, 10);
      if (*endp != '\0') n_sam_parser = 1;
    }
    if (copy_str){
      n_bam_copier = strtol(copy_str, &endp, 10);
      if (*endp != '\0') n_bam_copier = 1;
    }
    if (hash_str){
      n_hash_adder = strtol(hash_str, &endp, 10);
      if (*endp != '\0') n_hash_adder = 1;
    }
    fprintf(stderr, "(I) #read %d #split %d #parse %d #copy %d #hash %d\n", n_block_reader, n_line_splitter, n_sam_parser, n_bam_copier, n_hash_adder);
    const int n_threads_fm = cpu_map->use_nproc*7/8; // 8/8=23.5s 0.9=21.6s 7/8(0.87)=21.7s 6/8=22.6s 4/8=26.6s
    const int n_threads_ss = cpu_map->use_nproc; //-16;
    fprintf(stderr, "(I) #mate %d #sort %d\n", n_threads_fm, n_threads_ss);

    {
      int k=0;
      int v = n_block_reader - 1;
      while(v) { k++; v >>= 1; }
      g_block_reader_id_bits = k; 
    } 

    //pagefile_t pagefile0[n_bam_copier];
    mt_page_write_t *p_page_write = NULL;
    if (paging) {
      p_page_write = pagefile_init();
    } else {
      fprintf(stderr, "(I) memory mode: no external paging\n");
    }

    //bam_free_list2_t bam_free_list2;
    //line_free_list2_t line_free_list2;
    //blkrd_free_list2_t blkrd_free_list2;
    //bam_free_list2_init(&bam_free_list2);
    //line_free_list2_init(&line_free_list2);
    //blkrd_free_list2_init(&blkrd_free_list2);

    if (filters.use_baminfo) {
      cpu_map->create_bai = 1;
    }

    mt_bamwrite_t *p_bamwrite = NULL;

    samFile *fp_out = NULL;
    mt_hash_t *p_hash = NULL;
    mt_bamcopy_t *p_bamcopy = NULL;
    if (!filters.use_baminfo) {
      fp_out = sam_open(fnout, modeout);
      if (fp_out == NULL) {perror("sam_open failed");exit(-1);};
      // write sam header without multihreads since multithreads can use only the pointers to bams
      sam_hdr_write(fp_out, header);
      hts_set_threads_2(fp_out, 1024 /* default 256 */, cpu_map);

      p_bamwrite = mt_bamwrite_init(fp_out, header);
    } else {
      p_hash = mt_hash_init(bucket_array, bucketU_array, header);
      p_bamcopy = mt_bamcopy_init(p_page_write, p_hash, bucket_array, header, paging);
    }
    mt_samparse_t * const p_samparse = mt_samparse_init(p_bamcopy, p_bamwrite, header, fp_out);
    // register samparse_enq_t * const enq0 = &p_samparse->enq[0];
    mt_linesplit_t * const p_linesplit = mt_linesplit_init(p_samparse);
    mt_blkrd_t * const p_blkrd = mt_blkrd_init(p_linesplit, input_files, total_size);
    mt_prof2_t *p_prof2 = NULL;
    if (!filters.use_baminfo) {
      p_prof2 = mt_wrt_throughput_init();
    }

#endif
    mt_prof_t *p_prof = NULL; 
    if (getenv("DUMP_THROUGHPUT")){
        p_prof = mt_throughput_init(p_blkrd, p_linesplit, p_samparse, p_bamcopy, p_hash);
    }
    //if(pthread_setname_np(pthread_self(), "<<main>>")){}

#if defined(MT_HASH)
    mt_blkrd_end(p_blkrd);
    if (!filters.use_baminfo) {
      hts_set_threads_3(fp_out, 1024 /* default 256 */, cpu_map);
      mt_wrt_throughput_offset ++;
    }
#endif
if (p_prof){
    mt_throughput_end(p_prof);
}
    struct timeval read_fin;
    gettimeofday(&read_fin, NULL);
    double s2b_fin = read_fin.tv_sec-time_begin.tv_sec +(read_fin.tv_usec-time_begin.tv_usec)/1000000.0;
    fprintf(stderr, "[%8.2f] sam-to-bam finished\n", s2b_fin);
    if (!filters.use_baminfo) {
      mt_wrt_throughput_offset ++;
    }

PROFILE_MEM_DUMP();

struct timeval t3;
if (!filters.use_baminfo) {
    sam_close(fp_out); // output
    t3 = read_fin;
    mt_wrt_throughput_end(p_prof2);
} else {
    mt_find_mates(n_threads_fm, bucket_array, bucketU_array); 
    struct timeval fm_fin;
    gettimeofday(&fm_fin, NULL);
    struct timeval a_fin = fm_fin;
    double fm_fin1 = fm_fin.tv_sec-time_begin.tv_sec +(fm_fin.tv_usec-time_begin.tv_usec)/1000000.0;
    double fm_fin2 = fm_fin.tv_sec-read_fin.tv_sec +(fm_fin.tv_usec-read_fin.tv_usec)/1000000.0;
    fprintf(stderr, "[%8.2f] find mates finished (%.1f sec)\n", fm_fin1, fm_fin2);

#if defined(SORT_SAM)
if (!g_unsort) {
    mt_sort_sam(n_threads_ss, bucket_array, bucketU_array);
    struct timeval ss_fin;
    gettimeofday(&ss_fin, NULL);
    a_fin = ss_fin;
    double ss_fin1 = ss_fin.tv_sec-time_begin.tv_sec +(ss_fin.tv_usec-time_begin.tv_usec)/1000000.0;
    double ss_fin2 = ss_fin.tv_sec-fm_fin.tv_sec +(ss_fin.tv_usec-fm_fin.tv_usec)/1000000.0;
    fprintf(stderr, "[%8.2f] sort sam finished (%.1f sec)\n", ss_fin1, ss_fin2);
}
#endif

#if 0 && defined(DEBUG_POS)
{{{
  const uint64_t st = bucketU_array->min;
  const uint64_t ed = bucketU_array->max;
  int i;
  for(i=st; i<=ed; i++){
    bucket_t * const bu = get_bucket(i, bucketU_array, 0); // unclipped pos
    if (bu) {
      int j;
      for(j=bu->min; j<=bu->max; j++){
	bamptr_t * bamp = get_fifo_list_from_bucket(bu, j, 1);
	if(bamp){
	  int bx3;
	  bamptr_t *bamp3;
	  bamptr_array_t * const ba3 = container_of(bamp, bamptr_array_t, bp[0]);
	  for(bx3=0,bamp3=bamp; bx3<ba3->size; bx3++, bamp3=get_bamptr_at(ba3, bx3, 1/*unclipped*/)) {
	    bamptr_t * const mate = bamptr_get_mate(bamp3);
	    if (mate){
	      char * const qn = mate->dump_qname;
	      if (qn[0] != 'S' ||qn[1] != 'R' ||qn[2] != 'R') {
	        fprintf(stderr, "invalid pointers bamptr_get_mate(%p): %p \n", bamp3, bamptr_get_mate(bamp3)); fflush(stderr);
	        *(int*)0;
	      }
	    }
	  }
	}
      }
    }
  }
}}}
#endif


  int ii;
  struct timeval t2;
  for(ii=0; ii<filters.funcs[api_do_filter].n; ii++) {
    struct timeval t1;
    gettimeofday(&t1, NULL);
    double filter_start = t1.tv_sec-time_begin.tv_sec +(t1.tv_usec-time_begin.tv_usec)/1000000.0;
    fprintf(stderr, "[%8.2f] %s%s%s started...\n", filter_start, "\33[32;1m", (char*)(*filters.funcs[api_get_filter_name].funcs[ii].func)(), "\33[0m");

    (*filters.funcs[api_do_filter].funcs[ii].func)();

    gettimeofday(&t2, NULL);
    double filter_fin = t2.tv_sec-time_begin.tv_sec +(t2.tv_usec-time_begin.tv_usec)/1000000.0;
    double filter_fin2 = t2.tv_sec-a_fin.tv_sec +(t2.tv_usec-a_fin.tv_usec)/1000000.0;
    a_fin = t2;
    fprintf(stderr, "[%8.2f] %s%s%s finished (%.1f sec)\n", filter_fin, "\33[32;1m", (char*)(*filters.funcs[api_get_filter_name].funcs[ii].func)(), "\33[0m", filter_fin2);
  }
{
  int i;
  samFile* fp_out = sam_open(fnout, modeout);
  if (fp_out == NULL) {perror("sam_open failed");exit(-1);};
  // write sam header without multihreads since multithreads can use only the pointers to bams
  change_SO(header, "coordinate");
  sam_hdr_write(fp_out, header);
  hts_set_threads_2(fp_out, 1024 /* default 256 */, cpu_map);

#define DEFAULT_SAME_POS 64

#if 0
{{{
  fprintf(stderr, "double-free check start..\n");
  for(i=0; i<BUCKET_ARRAY_LENGTH; i++) {
    bucket_t *bu = bucket_array[i];
    if (bu) {
      int j; 
      for(j=0; j<BUCKET_SIZE; j++){
	bamptr_t * const top = get_fifo_list_from_bucket(bu, j, 0);
	bamptr_t *bamp;
fprintf(stderr, "bucket %d\n", j);
        for(bamp=top; bamp; bamp=bamp->clipped_next){
	  bam1_t * const bam = bamp->bam;
	  int i2;
	  for(i2=i; i2<BUCKET_ARRAY_LENGTH; i2++) {
	    bucket_t *bu2 = bucket_array[i2];
	    if (bu2) {
	      int j2; 
	      for(j2=j; j2<BUCKET_SIZE; j2++){
		bamptr_t * const top2 = (i2==i && j2==j ? bamp->clipped_next: get_fifo_list_from_bucket(bu2, j2, 0));
		bamptr_t *bamp2;
		for(bamp2=top2; bamp2; bamp2=bamp2->clipped_next){
		  bam1_t * const bam2 = bamp2->bam;
		  if (bam == bam2){
fprintf(stderr, "bam %s bam2 %s\n", bam_get_qname(bam), bam_get_qname(bam2));
		  }
		} /* for bamp2 */
	      } /* for j2 */
	    } /* if bu2 */
	  } /* for i2 */
	} /* for bamp */
      } /* for j */
    } /* if bu */
  } /* for i */
  fprintf(stderr, "double-free check done\n");
}}}
#endif
prof_bucket_array_min = bucket_array->min;
prof_bucket_array_max = bucket_array->max;
  if (filters.use_baminfo) {
    p_prof2 = mt_wrt_throughput_init();
  }

fprintf(stderr, "write index range: %lu - %lu\n", bucket_array->min, bucket_array->max);
  size_t mem = 0, n_bam = 0;
  const int unsort = g_unsort;
  for(i=bucket_array->min; i<=bucket_array->max; i++) {
prof_bucket_array_idx = i;
    bucket_t * const bu = get_bucket(i, bucket_array, 0);
    if (bu) {
      if (filters.funcs[api_post_filter].n) {
      // optimization - avoid the overhead of checking post filters
      int j; 
      for(j=bu->min; j<=bu->max; j++)
      {
        bamptr_t * bamp;
	bamptr_t * const top = get_fifo_list_from_bucket(bu, j, 0);

#if defined(BAMP_ARRAY)
#if 0
// disabled prefetch here since the main thread could not provide sufficient data to the workers
// because of a high overhead of this code
	// prefetch bam pointers in bamp's
	int const dist1 = 100;
	if (j+dist1<=bu->max){
	  bamptr_t * const top1 = get_fifo_list_from_bucket(bu, j+dist1, 0);
	  if (top1){
	    //bamptr_array_t * const bwa1 = container_of(top1, bamptr_array_t, bp[0]);
	    PREFETCH_WRITE_R_LOCALITY3(top1 /* array of bamptrs */);
	    PREFETCH_WRITE_R_LOCALITY3(top1+128 /* array of bamptrs */);
	  }
	}
#endif
	if (top){
	int bwx;
	bamptr_array_t * const bwa = container_of(top, bamptr_array_t, bp[0]);
#if defined(SORT_BAM_BY_REV)
	bamptr_t * bamp_rev[bwa->size];
	long bamp_rev_num = 0;
#endif
	for(bwx=0,bamp=top; bwx<bwa->size; bwx++, bamp=get_bamptr_at(bwa, bwx, 0/*clipped*/))
#else
        for(bamp=top; bamp; bamp=bamp->clipped_next)
#endif
        {
	  long include = 1;
{
  int ii;
  for(ii=0; ii<filters.funcs[api_post_filter].n; ii++) {
    if ( 0 == filters.funcs[api_post_filter].funcs[ii].func(bamp) ) {
      // exclude
      include = 0;
      break;
    }
  }
}
	if (include) {
#if defined(SORT_BAM_BY_REV)
	  if (unsort && bamptrC_get_rev(bamp)) {
	    bamp_rev[bamp_rev_num++] = bamp;
	  } else {
#endif
          sam_write2(fp_out, header, bamptr_get_bamva(bamp), bamptr_get_l_data(bamp), bamptr_get_dup(bamp));
          mem += 4 /*block_len*/ + 32 /* fixed size */ + bamptr_get_l_data(bamp);
	  n_bam++;
#if defined(SORT_BAM_BY_REV)
	  }
#endif
	}
	}
#if defined(BAMP_ARRAY)
#if defined(SORT_BAM_BY_REV)
        /* write reverse */
        int ir;
	for(ir=0; ir<bamp_rev_num; ir++) {
	  bamptr_t * bamp_r = bamp_rev[ir];
          sam_write2(fp_out, header, bamptr_get_bamva(bamp_r), bamptr_get_l_data(bamp_r), bamptr_get_dup(bamp_r));
          mem += 4 /*block_len*/ + 32 /* fixed size */ + bamptr_get_l_data(bamp_r);
	  n_bam++;
	}
#endif
	}
#endif
#if 0
//TODO: run in a separate thread
//TODO: is this needed even if the physical pages are not returned to the system
	bamptr_t *next;
        for(bamp=top; bamp; ){
	  next = bamp->clipped_next;
	  bam_destroy1(bamp->bam);
	  bamp = next;
	}
	  free_bucket(bu);
#endif
      } /* for j */
      } else {
      int j; 
      for(j=bu->min; j<=bu->max; j++)
      {
        bamptr_t * bamp;
	bamptr_t * const top = get_fifo_list_from_bucket(bu, j, 0);

#if defined(BAMP_ARRAY)
#if 0
// disabled prefetch here since the main thread could not provide sufficient data to the workers
// because of a high overhead of this code
	// prefetch bam pointers in bamp's
	int const dist1 = 8;
	if (j+dist1<=bu->max){
	  bamptr_t * const top1 = get_fifo_list_from_bucket(bu, j+dist1, 0);
	  if (top1){
	    //bamptr_array_t * const bwa1 = container_of(top1, bamptr_array_t, bp[0]);
	    PREFETCH_WRITE_R_LOCALITY3(top1 /* array of bamptrs */);
	    PREFETCH_WRITE_R_LOCALITY3(top1+128 /* array of bamptrs */);
	  }
	}
#endif
	if (top){
	int bwx;
	bamptr_array_t * const bwa = container_of(top, bamptr_array_t, bp[0]);
#if defined(SORT_BAM_BY_REV)
	bamptr_t * bamp_rev[bwa->size];
	long bamp_rev_num = 0;
#endif
	for(bwx=0,bamp=top; bwx<bwa->size; bwx++, bamp=get_bamptr_at(bwa, bwx, 0/*clipped*/))
#else
        for(bamp=top; bamp; bamp=bamp->clipped_next)
#endif
	{
#if 0
{{{
char buf[400];
bam_vaddr_t bamva = bamptr_get_bamva(bamp);
char fn[strlen(pagefile_name)+32];
sprintf(fn, "%s-%d", pagefile_name, bam_vaddr_get_fd(&bamva));
int fd = open(fn, O_LARGEFILE | O_RDONLY);
pread(fd, buf, 400, bam_vaddr_get_seqid(&bamva)*sizeof(((bam_space_t*)0)->space) + bam_vaddr_get_offset(&bamva));
close(fd);
fprintf(stderr, "bamid %ld bamva %d %d %d bamptr_get_l_data %d b->l_data %d %s pos=%d\n", bamptrC_get_bamid(bamp), bam_vaddr_get_fd(&bamva), bam_vaddr_get_seqid(&bamva), bam_vaddr_get_offset(&bamva), bamptr_get_l_data(bamp), ((bam1_t*)buf)->l_data, bam_get_qname((bam1_t*)buf), ((bam1_t*)buf)->core.pos);
}}}
#endif
#if defined(SORT_BAM_BY_REV)
	  if (unsort && bamptrC_get_rev(bamp)) {
	    bamp_rev[bamp_rev_num++] = bamp;
	  } else {
#endif
          sam_write2(fp_out, header, bamptr_get_bamva(bamp), bamptr_get_l_data(bamp), bamptr_get_dup(bamp));
          mem += 4 /*block_len*/ + 32 /* fixed size */ + bamptr_get_l_data(bamp);
	  n_bam++;
#if defined(SORT_BAM_BY_REV)
	  }
#endif
	}
#if defined(BAMP_ARRAY)
#if defined(SORT_BAM_BY_REV)
        /* write reverse */
        int ir;
	for(ir=0; ir<bamp_rev_num; ir++) {
	  bamptr_t * bamp_r = bamp_rev[ir];
          sam_write2(fp_out, header, bamptr_get_bamva(bamp_r), bamptr_get_l_data(bamp_r), bamptr_get_dup(bamp_r));
          mem += 4 /*block_len*/ + 32 /* fixed size */ + bamptr_get_l_data(bamp_r);
	  n_bam++;
	}
#endif
	}
#endif
#if 0
//TODO: run in a separate thread
//TODO: is this needed even if the physical pages are not returned to the system
	bamptr_t *next;
        for(bamp=top; bamp; ){
	  next = bamp->clipped_next;
	  bam_destroy1(bamp->bam);
	  bamp = next;
	}
	  free_bucket(bu);
#endif
      } /* for j */
      } /* filter */
    } /* if bu */
  } /* for i */
  {
    /* output unmap reads with POS=0 */
    unmap_space_iter_t * const iter = init_unmap_space_iter(p_samparse);
    bam1_t *unmap_bam;
  if (filters.funcs[api_post_filter].n) {
    // filters accept bamptr_t
    bamptr_t *bamp = md_malloc(sizeof_bamptr(0), mem_core_ext, 0);
    while(NULL != (unmap_bam = get_unmapped_reads(iter))){
      bam_vaddr_t bamva;
      bam_vaddr_init2(&bamva, unmap_bam);
      long include = 1;
{
  // bamptr_init uses only baminfo1->c including bamid, bamva, b->l_data, _dup, and unclip
  baminfo1_t baminfo1;
  // TODO some of baminfo1's fields (bamid and unclip) are not initialized
  init_baminfo1(&baminfo1, unmap_bam, 0/*qnameid*/, NULL/*header*/, bamva, -1/*bamid*/, -1/*offset_unclipped_pos*/);
  bamptr_init(bamp, &baminfo1, 0/*clipped*/ ARG_QNAMEID_CACHE(p_cache));
  int ii;
  for(ii=0; ii<filters.funcs[api_post_filter].n; ii++) {
    if ( 0 == filters.funcs[api_post_filter].funcs[ii].func(bamp) ) {
      // exclude
      include = 0;
      break;
    }
  }
}
    if (include) {
      sam_write2(fp_out, header, bamva, unmap_bam->l_data, 0/*!dup*/);
      mem += 4 /*block_len*/ + 32 /* fixed size */ + unmap_bam->l_data;
      n_bam++;
    }
    } /* while */
  } else {
    while(NULL != (unmap_bam = get_unmapped_reads(iter))){
      bam_vaddr_t bamva;
      bam_vaddr_init2(&bamva, unmap_bam);
      sam_write2(fp_out, header, bamva, unmap_bam->l_data, 0/*!dup*/);
      mem += 4 /*block_len*/ + 32 /* fixed size */ + unmap_bam->l_data;
      n_bam++;
    } /* while */
  } /* filter */
  }
struct timeval t3x;
gettimeofday(&t3x, NULL);
double awr_fin2 = t3x.tv_sec-t2.tv_sec +(t3x.tv_usec-t2.tv_usec)/1000000.0;
double awr_fin = t3x.tv_sec-time_begin.tv_sec +(t3x.tv_usec-time_begin.tv_usec)/1000000.0;
fprintf(stderr, "[%8.2f] BGZF request finished (%.1f MB/s)\n", awr_fin, mem/1024/1024/awr_fin2);
mt_wrt_throughput_offset ++;

struct stat st_buf;
st_buf.st_size = 0;
if (-1 == fstat(((hFILE_fd*)fp_out->fp.bgzf->fp)->fd, &st_buf)) {
fprintf(stderr, "(I) fstat failed before closing the output file\n");
}
  sam_close(fp_out); // output

  if (filters.use_baminfo) {
    mt_wrt_throughput_end(p_prof2);
  }

{
  int ii;
  for(ii=0; ii<filters.funcs[api_end_filter].n; ii++) {
    (*filters.funcs[api_end_filter].funcs[ii].func)();
  }
}

gettimeofday(&t3, NULL);
double wr_fin2 = t3.tv_sec-t2.tv_sec +(t3.tv_usec-t2.tv_usec)/1000000.0;
double wr_fin = t3.tv_sec-time_begin.tv_sec +(t3.tv_usec-time_begin.tv_usec)/1000000.0;
fprintf(stderr, "[%8.2f] write finished (%.1f MB/s)\n", wr_fin, st_buf.st_size/1024/1024/wr_fin2);

}
#else
#endif
}
    bam_hdr_destroy(header);
struct timeval t4;
gettimeofday(&t4, NULL);
double all_fin2 = t4.tv_sec-t3.tv_sec +(t4.tv_usec-t3.tv_usec)/1000000.0;
double all_fin = t4.tv_sec-time_begin.tv_sec +(t4.tv_usec-time_begin.tv_usec)/1000000.0;
fprintf(stderr, "[%8.2f] process finished (%.1f s for closing the output file)\n", all_fin, all_fin2);

    return 0;
}


static int sam2bam_usage(FILE *fp, int status)
{
    fprintf(fp,
"Usage: samtools sam2bam [options...] -o<out.bam> <in.sam>\n"
"Options:\n"
"  -d         Drop the input data from the operating system file cache\n"
"  -e         Emulate SortSam\n"
"  -l INT     Set compression level, from 0 (uncompressed) to 9 (best)\n"
"  -O FORMAT  Write output as FORMAT (currently FORMAT must be bam)\n"
"  -p         Enable paging for a large input file (e.g., WGS)\n"
"  -s         Disable filters\n"
);
    return status;
}

int bam_sam2bam(int argc, char *argv[])
{
    int c, nargs, ret = EXIT_SUCCESS, level = -1;
    char *fnout = "-", *fmtout = "bam", modeout[12], *tmpprefix = NULL;
    kstring_t fnout_buffer = { 0, 0, NULL };

    while ((c = getopt(argc, argv, "l:m:sedpo:O:F:")) >= 0) {
        switch (c) {
	case 'd': g_drop_cache = 1; break;
	case 'p': g_paging = 1; break;
	case 'e': g_unsort = 0; break;
	case 's': g_skip_filter = 1; break;
        case 'o': fnout = optarg; break;
        case 'O': {
	  fmtout = optarg;
	  if(0 != strcmp("bam", fmtout)) {
	    fprintf(stderr, "(E) only bam supported\n");
	    exit(-1);
	  }
	  break;
	}
        case 'F': {
	  // -F filter_name:args
	  const char *args = strchr(optarg, ':'); 
	  if (args) {
	    if (NULL == filters.args) {
	      filters.args = malloc(sizeof(filter_args_t)+sizeof(filters.args->args[0])*1);
	      filters.args->n = 1;
	    } else {
	      filters.args->n++;
	      filters.args = realloc(filters.args, sizeof(filter_args_t)+sizeof(filters.args->args[0])*filters.args->n);
	    }
	    filters.args->args[filters.args->n-1].name = strndup(optarg, args-optarg);
	    filters.args->args[filters.args->n-1].args = args+1; 
	    fprintf(stderr, "(I) filter %s args %s\n", filters.args->args[filters.args->n-1].name, filters.args->args[filters.args->n-1].args);
	  } else {
	    fprintf(stderr, "(E) ignore -F %s since ':' is not found\n", optarg);
            fprintf(stderr, "(E) do you mean -F%s: ?\n", optarg);
       exit(1);
	  }
	  break;
	}
        case 'l': level = atoi(optarg); break;
        default: return sam2bam_usage(stderr, EXIT_FAILURE);
        }
    }

    nargs = argc - optind;
    if (argc == 1)
        return sam2bam_usage(stdout, EXIT_SUCCESS);

    strcpy(modeout, "w");
    if (sam_open_mode(&modeout[1], fnout, fmtout) < 0) {
        if (fmtout) fprintf(stderr, "[bam_sam2bam] can't parse output format \"%s\"\n", fmtout);
        else fprintf(stderr, "[bam_sam2bam] can't determine output format\n");
        ret = EXIT_FAILURE;
        goto sam2bam_end;
    }
    if (level >= 0) sprintf(strchr(modeout, '\0'), "%d", level < 9? level : 9);

    // TODO: md_malloc cannot be called here since md_init is not called
    input_files_t * const input_files = malloc(sizeof(input_files_t)+sizeof(char*)*(0==nargs ? 1 : nargs));
    if (0 == nargs) {
      input_files->n = 1;
      g_n_inputs = 1;
      input_files->fn[0] = "-";
    } else {
      input_files->n = nargs;
      g_n_inputs = nargs;
      int i;
      for (i=0; i<nargs; i++) {
        input_files->fn[i] = argv[optind+i];
      }
    }
    if (bam_sam2bam_core_ext(input_files, tmpprefix, fnout, modeout, g_paging) < 0) ret = EXIT_FAILURE;

sam2bam_end:
    free(fnout_buffer.s);
    return ret;
}

// vim: set foldmethod=marker :


