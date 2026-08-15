/* trace.h — synthetic reference streams for the replacement harness */
#ifndef TRACE_H
#define TRACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t vpn;
    bool     write;
} ref_t;

typedef struct {
    ref_t   *refs;
    size_t   n;
    uint64_t *next_use;   /* next_use[i] = index of next ref to same vpn, or VM_NEVER */
} trace_t;

typedef enum {
    WL_WORKING_SET,   /* hot window of `ws` pages, occasional stray reference */
    WL_PHASED,        /* working set that migrates every phase_len refs */
    WL_SEQUENTIAL,    /* linear scan, the classic FIFO/LRU worst case */
    WL_RANDOM,        /* uniform over the whole address space */
    WL_ZIPF,          /* skewed popularity, closer to real programs */
    WL_COUNT
} workload_t;

const char *workload_name(workload_t w);
bool workload_from_string(const char *s, workload_t *out);

/* ws is the hot-set size in pages; write_pct in [0,100]. Caller frees. */
trace_t *trace_generate(workload_t w, size_t npages, size_t ws, size_t nrefs,
                        unsigned write_pct, uint64_t seed);
void trace_free(trace_t *t);

#endif /* TRACE_H */
