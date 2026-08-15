/* alloc.h — explicit free-list allocator with splitting and coalescing */
#ifndef ALLOC_H
#define ALLOC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { FIT_FIRST = 0, FIT_BEST, FIT_NEXT } fit_policy_t;

typedef struct {
    uint64_t mallocs, frees, reallocs;
    uint64_t bytes_requested;    /* what callers asked for */
    uint64_t payload_live;       /* currently handed out (requested bytes) */
    uint64_t payload_peak;
    uint64_t heap_bytes;         /* total mapped from the OS */
    uint64_t heap_peak;
    uint64_t splits, coalesces;
    uint64_t region_grows;
    uint64_t search_steps;       /* free-list nodes examined across all fits */
    uint64_t free_blocks;
} alloc_stats_t;

void *my_malloc(size_t size);
void  my_free(void *ptr);
void *my_calloc(size_t nmemb, size_t size);
void *my_realloc(void *ptr, size_t size);

void my_set_fit(fit_policy_t p);
const alloc_stats_t *my_stats(void);
void my_reset_stats(void);
void my_dump(void);            /* human-readable heap walk, for debugging */
bool my_heap_check(void);      /* full consistency check of every block */
double my_fragmentation(void); /* 1 - live_payload / heap_bytes */

#endif /* ALLOC_H */
