/* alloc.c — a malloc/free implementation on top of mmap.
 *
 * Block layout (boundary tags, Knuth-style):
 *
 *      +--------+-------------------------+--------+
 *      | header |        payload          | footer |
 *      +--------+-------------------------+--------+
 *        8 B      size - 16 B, 16-aligned    8 B
 *
 * header/footer both hold (size | alloc_bit). The footer is what makes
 * backwards coalescing O(1): from a block's header we can read the previous
 * block's footer at bp-16 and learn its size without walking the heap.
 *
 * Free blocks additionally store two pointers in the first 16 bytes of their
 * payload, threading them onto an explicit doubly linked free list. That is
 * why the minimum block size is 32 B: 8 header + 16 pointers + 8 footer.
 *
 * Each mmap'd region is bracketed by an allocated prologue and a zero-size
 * allocated epilogue so coalescing never walks off the end of a region.
 *
 * Not thread safe by design — a lock or per-thread arenas would be the next
 * step; see README.
 */

/* MAP_ANONYMOUS is a BSD/Linux extension, not ISO C and not POSIX, so under
 * -std=c11 (which defines __STRICT_ANSI__) glibc hides it unless we ask.
 * _DEFAULT_SOURCE must be defined before any header is included. */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "alloc.h"

#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* Older BSD-derived systems spell it MAP_ANON. */
#if !defined(MAP_ANONYMOUS) && defined(MAP_ANON)
#define MAP_ANONYMOUS MAP_ANON
#endif

#define WSIZE      8u
#define ALIGNMENT  16u
#define MIN_BLOCK  32u
#define CHUNK      (1u << 20)      /* 1 MiB per region grow */

#define PACK(size, alloc)  ((size) | (alloc))
#define GET(p)             (*(size_t *)(p))
#define PUT(p, v)          (*(size_t *)(p) = (v))
#define GET_SIZE(p)        (GET(p) & ~(size_t)0xF)
#define GET_ALLOC(p)       (GET(p) & (size_t)0x1)

#define HDRP(bp)           ((char *)(bp) - WSIZE)
#define FTRP(bp)           ((char *)(bp) + GET_SIZE(HDRP(bp)) - 2 * WSIZE)
#define NEXT_BLK(bp)       ((char *)(bp) + GET_SIZE(HDRP(bp)))
#define PREV_BLK(bp)       ((char *)(bp) - GET_SIZE((char *)(bp) - 2 * WSIZE))

typedef struct free_node {
    struct free_node *prev, *next;
} free_node;

typedef struct region {
    struct region *next;
    void   *base;
    size_t  len;
} region_t;

static free_node    *g_free_head;
static region_t     *g_regions;
static char         *g_next_fit;      /* rover for FIT_NEXT */
static fit_policy_t  g_fit = FIT_FIRST;
static alloc_stats_t g_st;

/* Stats track rounded block sizes rather than requested bytes: storing the
 * caller's exact size would need another word per block. bytes_requested vs
 * payload_live therefore shows internal fragmentation in aggregate. */

static size_t align_up(size_t x, size_t a) { return (x + a - 1) & ~(a - 1); }

/* ---------------- free list ---------------- */

static void fl_insert(void *bp)
{
    free_node *n = (free_node *)bp;
    n->prev = NULL;
    n->next = g_free_head;
    if (g_free_head) g_free_head->prev = n;
    g_free_head = n;
    g_st.free_blocks++;
}

static void fl_remove(void *bp)
{
    free_node *n = (free_node *)bp;
    if (g_next_fit == (char *)n) g_next_fit = (char *)n->next;   /* keep rover valid */
    if (n->prev) n->prev->next = n->next; else g_free_head = n->next;
    if (n->next) n->next->prev = n->prev;
    n->prev = n->next = NULL;
    g_st.free_blocks--;
}

/* ---------------- coalescing ---------------- */

static void *coalesce(void *bp)
{
    size_t size        = GET_SIZE(HDRP(bp));
    bool   prev_alloc  = GET_ALLOC(HDRP(PREV_BLK(bp)));
    bool   next_alloc  = GET_ALLOC(HDRP(NEXT_BLK(bp)));

    if (!next_alloc) {                       /* merge forward */
        void *next = NEXT_BLK(bp);
        fl_remove(next);
        size += GET_SIZE(HDRP(next));
        PUT(HDRP(bp), PACK(size, 0));
        PUT(FTRP(bp), PACK(size, 0));
        g_st.coalesces++;
    }
    if (!prev_alloc) {                       /* merge backward */
        void *prev = PREV_BLK(bp);
        fl_remove(prev);
        size += GET_SIZE(HDRP(prev));
        PUT(HDRP(prev), PACK(size, 0));
        PUT(FTRP(prev), PACK(size, 0));
        bp = prev;
        g_st.coalesces++;
    }
    if (g_next_fit && (char *)g_next_fit > (char *)bp &&
        (char *)g_next_fit < (char *)bp + size)
        g_next_fit = bp;                      /* rover fell inside merged block */

    fl_insert(bp);
    return bp;
}

/* ---------------- region growth ---------------- */

static void *grow(size_t need)
{
    size_t len = align_up(need + 4 * WSIZE, CHUNK);
    void  *base = mmap(NULL, len, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) return NULL;

    char *p = (char *)base;
    PUT(p + WSIZE,     PACK(2 * WSIZE, 1));   /* prologue header */
    PUT(p + 2 * WSIZE, PACK(2 * WSIZE, 1));   /* prologue footer */
    PUT(p + len - WSIZE, PACK(0, 1));         /* epilogue header */

    char  *bp   = p + 4 * WSIZE;              /* payload of the first block */
    size_t size = len - 5 * WSIZE;
    size &= ~(size_t)0xF;
    PUT(HDRP(bp), PACK(size, 0));
    PUT(FTRP(bp), PACK(size, 0));

    region_t *r = (region_t *)mmap(NULL, sizeof *r, PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (r != MAP_FAILED) {
        r->base = base; r->len = len; r->next = g_regions; g_regions = r;
    }

    g_st.heap_bytes += len;
    if (g_st.heap_bytes > g_st.heap_peak) g_st.heap_peak = g_st.heap_bytes;
    g_st.region_grows++;

    fl_insert(bp);
    return bp;
}

/* ---------------- fit search ---------------- */

static void *find_fit(size_t asize)
{
    if (g_fit == FIT_BEST) {
        void  *best = NULL;
        size_t best_size = (size_t)-1;
        for (free_node *n = g_free_head; n; n = n->next) {
            g_st.search_steps++;
            size_t s = GET_SIZE(HDRP(n));
            if (s >= asize && s < best_size) {
                best = n; best_size = s;
                if (s == asize) break;            /* perfect fit, stop early */
            }
        }
        return best;
    }

    if (g_fit == FIT_NEXT && g_next_fit) {
        /* Resume from the rover, then wrap. Cheaper than first-fit on long
         * lists, at the cost of slightly worse locality of small blocks. */
        free_node *start = (free_node *)g_next_fit;
        for (free_node *n = start; n; n = n->next) {
            g_st.search_steps++;
            if (GET_SIZE(HDRP(n)) >= asize) { g_next_fit = (char *)n->next; return n; }
        }
        for (free_node *n = g_free_head; n && n != start; n = n->next) {
            g_st.search_steps++;
            if (GET_SIZE(HDRP(n)) >= asize) { g_next_fit = (char *)n->next; return n; }
        }
        return NULL;
    }

    for (free_node *n = g_free_head; n; n = n->next) {
        g_st.search_steps++;
        if (GET_SIZE(HDRP(n)) >= asize) return n;
    }
    return NULL;
}

/* Place asize bytes at bp, splitting off the tail if the remainder is big
 * enough to be a legal block. Splitting is what keeps internal fragmentation
 * down; without it a 32 B request could consume a 1 MiB block. */
static void place(void *bp, size_t asize)
{
    size_t csize = GET_SIZE(HDRP(bp));
    fl_remove(bp);

    if (csize - asize >= MIN_BLOCK) {
        PUT(HDRP(bp), PACK(asize, 1));
        PUT(FTRP(bp), PACK(asize, 1));
        void *rest = NEXT_BLK(bp);
        PUT(HDRP(rest), PACK(csize - asize, 0));
        PUT(FTRP(rest), PACK(csize - asize, 0));
        fl_insert(rest);
        g_st.splits++;
    } else {
        PUT(HDRP(bp), PACK(csize, 1));
        PUT(FTRP(bp), PACK(csize, 1));
    }
}

/* ---------------- public API ---------------- */

void my_set_fit(fit_policy_t p) { g_fit = p; g_next_fit = NULL; }

void *my_malloc(size_t size)
{
    if (size == 0) return NULL;
    if (size > (size_t)-1 - 4 * WSIZE) return NULL;   /* overflow guard */

    size_t asize = align_up(size + 2 * WSIZE, ALIGNMENT);
    if (asize < MIN_BLOCK) asize = MIN_BLOCK;

    void *bp = find_fit(asize);
    if (!bp) {
        bp = grow(asize);          /* fresh region: bp is on the free list */
        if (!bp) return NULL;
    }
    place(bp, asize);

    g_st.mallocs++;
    g_st.bytes_requested += size;
    g_st.payload_live    += GET_SIZE(HDRP(bp));
    if (g_st.payload_live > g_st.payload_peak) g_st.payload_peak = g_st.payload_live;
    return bp;
}

void my_free(void *ptr)
{
    if (!ptr) return;
    size_t size = GET_SIZE(HDRP(ptr));
    if (!GET_ALLOC(HDRP(ptr))) {
        fprintf(stderr, "my_free: double free of %p\n", ptr);
        return;
    }
    PUT(HDRP(ptr), PACK(size, 0));
    PUT(FTRP(ptr), PACK(size, 0));
    g_st.payload_live -= size;
    g_st.frees++;
    coalesce(ptr);
}

void *my_calloc(size_t nmemb, size_t size)
{
    if (nmemb && size > (size_t)-1 / nmemb) return NULL;
    size_t total = nmemb * size;
    void *p = my_malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *my_realloc(void *ptr, size_t size)
{
    if (!ptr) return my_malloc(size);
    if (size == 0) { my_free(ptr); return NULL; }

    size_t old = GET_SIZE(HDRP(ptr)) - 2 * WSIZE;
    if (old >= size) return ptr;              /* shrink in place */

    /* Try to absorb the following block if it is free and big enough — avoids
     * a copy in the common grow-an-array pattern. */
    void *next = NEXT_BLK(ptr);
    size_t asize = align_up(size + 2 * WSIZE, ALIGNMENT);
    if (asize < MIN_BLOCK) asize = MIN_BLOCK;
    size_t combined = GET_SIZE(HDRP(ptr)) + GET_SIZE(HDRP(next));
    if (!GET_ALLOC(HDRP(next)) && combined >= asize) {
        size_t before = GET_SIZE(HDRP(ptr));
        fl_remove(next);
        PUT(HDRP(ptr), PACK(combined, 0));
        PUT(FTRP(ptr), PACK(combined, 0));
        fl_insert(ptr);                       /* place() expects a listed block */
        place(ptr, asize);                    /* re-splits the unused tail */
        g_st.payload_live += GET_SIZE(HDRP(ptr)) - before;
        g_st.reallocs++;
        return ptr;
    }

    void *np = my_malloc(size);
    if (!np) return NULL;
    memcpy(np, ptr, old);
    my_free(ptr);
    g_st.reallocs++;
    return np;
}

const alloc_stats_t *my_stats(void) { return &g_st; }

void my_reset_stats(void)
{
    uint64_t heap = g_st.heap_bytes, peak = g_st.heap_peak;
    uint64_t live = g_st.payload_live, fb = g_st.free_blocks;
    memset(&g_st, 0, sizeof g_st);
    g_st.heap_bytes = heap; g_st.heap_peak = peak;
    g_st.payload_live = live; g_st.payload_peak = live; g_st.free_blocks = fb;
}

/* Fraction of the mapped heap that was NOT in use at the moment of peak
 * demand. Measured against payload_peak because measuring after everything is
 * freed would trivially report 100%: this allocator never munmaps. */
double my_fragmentation(void)
{
    if (!g_st.heap_bytes || !g_st.payload_peak) return 0.0;
    return 1.0 - (double)g_st.payload_peak / (double)g_st.heap_bytes;
}

/* place() calls fl_remove() which decrements free_blocks; place() is only
 * reached with bp on the list, so the counter stays honest. */
static void region_walk(region_t *r, bool verbose, bool *ok)
{
    char *bp = (char *)r->base + 4 * WSIZE;
    void *prev = NULL;
    while (GET_SIZE(HDRP(bp)) > 0) {
        size_t sz = GET_SIZE(HDRP(bp));
        bool a = GET_ALLOC(HDRP(bp));
        if (verbose)
            printf("  %p  size=%-8zu %s\n", (void *)bp, sz, a ? "ALLOC" : "free");
        if (GET(HDRP(bp)) != GET(FTRP(bp))) {           /* tag mismatch */
            if (ok) *ok = false;
            if (verbose) printf("    !! header/footer mismatch\n");
        }
        if (sz < MIN_BLOCK || (sz & 0xF)) {
            if (ok) *ok = false;
            if (verbose) printf("    !! bad size\n");
            return;
        }
        if (prev && !GET_ALLOC(HDRP(prev)) && !a) {     /* missed coalesce */
            if (ok) *ok = false;
            if (verbose) printf("    !! adjacent free blocks not coalesced\n");
        }
        prev = bp;
        bp = NEXT_BLK(bp);
    }
}

void my_dump(void)
{
    for (region_t *r = g_regions; r; r = r->next) {
        printf("region %p len=%zu\n", r->base, r->len);
        region_walk(r, true, NULL);
    }
}

bool my_heap_check(void)
{
    bool ok = true;
    for (region_t *r = g_regions; r; r = r->next)
        region_walk(r, false, &ok);

    /* every node on the free list must actually be a free block */
    size_t n = 0;
    for (free_node *f = g_free_head; f; f = f->next) {
        if (GET_ALLOC(HDRP(f))) ok = false;
        if (++n > (1u << 22)) { ok = false; break; }    /* cycle guard */
    }
    if (n != g_st.free_blocks) ok = false;
    return ok;
}
