/*  prefilter.c

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

#define _GNU_SOURCE
#include <memory.h>
#include <pthread.h>
#include <assert.h>
#include <limits.h>
#include "bam_sam2bam.h"

#define FILTER_NAME "pre_filter"

static long mem_readFilter;

#define REGISTER_MEM_ID(id) id = (*api.md_register_mem_id)(#id)
static struct func_vector_v1 api;

const char *get_api_version(void) {
  return "1";
}

const char *get_filter_name(void) {
  return FILTER_NAME;
}

static void mem_init(){
  REGISTER_MEM_ID(mem_readFilter);
}

static struct {
  long enabled;

  const char *r;	// read group name
  size_t     r_len;

  int32_t l;		// library id (assigned by the program)
  int32_t q;		// quality lower limit
  int32_t m;		// #cigar lower limit
  int32_t f;		// set in bits f
  int32_t F;		// not set in bits F
  int32_t s;		// reference sequence id (assgined by the program)
  int32_t s_beg;	// region begin
  int32_t s_end;	// region end
} g_args;


/*
  The argument format:
  pre_filter:{r=STR}*{q=INT}*{l=STR}*{m=INT}*{f=INT}*{F=INT}*

  TODO:
  Read the files and initialize the read targets here
  to support the following options that 'samtools view' provides:

  -L FILE  only include reads overlapping this BED FILE [null]
  -R FILE  only include reads with read group listed in FILE [null]
*/
/*
  The filter supports the options that 'samtools view' provides:

  -r STR   only include reads in read group STR [null]
  -q INT   only include reads with mapping quality >= INT [0]
  -l STR   only include reads in library STR [null]
  -m INT   only include reads with number of CIGAR operations consuming query sequence >= INT [0]
  -f INT   only include reads with all bits set in INT set in FLAG [0]
  -F INT   only include reads with none of the bits set in INT set in FLAG [0]
*/
const char *init_filter(struct func_vector_v1 *p_vec, const char *args) {
  api = *p_vec;
  mem_init();
if (args) {
  const char *p=args;
  while(*p){
    const char **p_s;
    int32_t *p_v;
    size_t *p_len;
    switch(*p){
    case 'r':
      p_s = &g_args.r; p_len = &g_args.r_len; goto parse_str; 
parse_str:
      p++;
      if (*p=='='){
        p++;
        const char *end = index(p, ',');
	if (NULL != end) {
	  g_args.enabled = 1;
	  *p_s = strndup(p, end-p);
	  *p_len = end-p;
	  p = end+1; // skip ,
	  break;
	} else {
	  g_args.enabled = 1;
	  *p_s = p;
	  *p_len = strlen(p);
	  goto parse_done;
	}
      }
      goto parse_error;

    case 's':
      p++;
      if (*p=='='){
        p++;
	const char * const c = index(p, ':');
	const char * const tid_str = (c ? strndup(p, c-p) : p);
	int id = bam_name2id(api.header, tid_str);
	if (id != -1) {
	  // known reference sequence name
	  g_args.s = id + 1; // 1-origin
	} else {
	  fprintf(stderr, "(W) unknown ref seq name %s\n", tid_str);
	}
	if (c) {
	  if (hts_parse_reg(c, &g_args.s_beg, &g_args.s_end)) {
	    g_args.enabled = 1;
	    goto parse_done;
	  } else {
	    fprintf(stderr, "(W) invalid region format %s\n", c+1);
	  }
	} else {
	  g_args.enabled = 1;
	  g_args.s_beg = 0;
	  g_args.s_end = INT_MAX;
	  goto parse_done;
	}
      }
      goto parse_error;

    case 'l': 
      p++;
      if (*p=='='){
        p++;
        uint8_t libid = api.get_libid(p);
	fprintf(stderr, "(I) library %s id %d\n", p, libid);
	if (0 != libid) {
	  // known library
	  g_args.enabled = 1;
	  g_args.l = libid;
	  goto parse_done;
	}
      }
      goto parse_error;

    case 'q': 
      p_v = &g_args.q; goto parse_int; 
    case 'm':
      p_v = &g_args.m; goto parse_int; 
    case 'f':
      p_v = &g_args.f; goto parse_int; 
    case 'F':
      p_v = &g_args.F;
parse_int:
      p++;
      if (*p=='='){
        p++;
        char *endp;
	long v;
        v = strtol(p, &endp, 10);
	if (p != endp) {
	  p = endp;
	  if (0 == *p) {
	    g_args.enabled = 1;
	    *p_v = v;  
	    goto parse_done;
	  } else if (*p == ',') {
	    g_args.enabled = 1;
	    *p_v = v;  
	    p++; // skip ,
	    break;
	  }
	}
      }
      goto parse_error;
    default:
      if (*p == 0) {
        goto parse_done;
      } else {
        goto parse_error;
      }
    }
  }
parse_error:
  fprintf(stderr, "\33[32;1m" FILTER_NAME "\33[0m parse error for arguments: %s\n", args);

parse_done:
  if (g_args.enabled) {
    fprintf(stderr, "\33[32;1m" FILTER_NAME "\33[0m ARGS: %s\n", args);
    fprintf(stderr, "\33[32;1m" FILTER_NAME "\33[0m region    : %d %d - %d\n", g_args.s, g_args.s_beg, g_args.s_end);
    fprintf(stderr, "\33[32;1m" FILTER_NAME "\33[0m read group: %s\n", g_args.r);
    fprintf(stderr, "\33[32;1m" FILTER_NAME "\33[0m library   : %d\n", g_args.l);
    fprintf(stderr, "\33[32;1m" FILTER_NAME "\33[0m quality   : %d\n", g_args.q);
    fprintf(stderr, "\33[32;1m" FILTER_NAME "\33[0m cigar     : %d\n", g_args.m);
    fprintf(stderr, "\33[32;1m" FILTER_NAME "\33[0m flag set  : %d\n", g_args.f);
    fprintf(stderr, "\33[32;1m" FILTER_NAME "\33[0m flag ^set : %d\n", g_args.F);
  }
}

  return "\33[32;1m" FILTER_NAME "\33[0m";

}

extern const char *bam_get_library(bam_hdr_t *header, const bam1_t *b);

long pre_filter(bam1_t *b) {
  if (b->core.qual < g_args.q) { // mapq
    return 0;
  }
  if ((b->core.flag & g_args.f) != g_args.f) { // all on
    return 0;
  }
  if (0 != (b->core.flag & g_args.F)) { // all off
    return 0;
  }
  if (g_args.r) {			// RGZ-----
    const char *p = (const char*)bam_aux_get(b, "RG");
    if (p) {
      p += 1; // skip Z
      if (memcmp(p, g_args.r, g_args.r_len) != 0) return 0;
    }
  }
  if (g_args.l) {
    const uint8_t libid = api.bam_get_libid(api.header, b);
    if (libid && libid != g_args.l) return 0;
  }
  if (g_args.s) {
    if (b->core.tid < 0 
     || b->core.tid != g_args.s - 1
     || g_args.s_end <= b->core.pos 
     || bam_endpos(b) <= g_args.s_beg
     ) return 0;
  }

  return 1;
}

/*
long post_filter(bam1_t *b) { }
*/

void *end_filter(void) {
  return NULL;
}

