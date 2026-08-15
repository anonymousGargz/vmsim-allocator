/* main_allocbench.c — correctness torture test + head-to-head timing against
 * the system allocator (glibc ptmalloc). */

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE          /* getopt_long, and consistency with alloc.c */
#endif
#define _POSIX_C_SOURCE 200809L  /* clock_gettime */

#include "alloc.h"

#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef void *(*alloc_fn)(size_t);
typedef void  (*free_fn)(void *);

static uint64_t rng_state = 88172645463325252ull;
static uint64_t rng(void)
{
    uint64_t x = rng_state;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    return rng_state = x;
}
static size_t rnd_size(size_t max) { return 1 + (size_t)(rng() % max); }

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

/* ------------------------------------------------------------------ */
/* Correctness: fill every allocation with a byte derived from its index and
 * verify it later. Catches overlapping blocks, bad splits, lost coalesces. */

static int torture(size_t nops, size_t max_size, size_t live_slots)
{
    void  **ptrs  = calloc(live_slots, sizeof *ptrs);
    size_t *sizes = calloc(live_slots, sizeof *sizes);
    unsigned char *tags = calloc(live_slots, 1);
    int failures = 0;

    for (size_t op = 0; op < nops; op++) {
        size_t slot = rng() % live_slots;

        if (ptrs[slot]) {
            unsigned char *p = ptrs[slot];
            for (size_t i = 0; i < sizes[slot]; i++) {
                if (p[i] != tags[slot]) {
                    fprintf(stderr, "CORRUPTION slot=%zu off=%zu got=%02x want=%02x\n",
                            slot, i, p[i], tags[slot]);
                    failures++;
                    break;
                }
            }
            my_free(ptrs[slot]);
            ptrs[slot] = NULL;
        }

        if (rng() % 4) {                       /* 75% allocate, 25% leave empty */
            size_t sz = rnd_size(max_size);
            void *p = my_malloc(sz);
            if (!p) { fprintf(stderr, "my_malloc(%zu) returned NULL\n", sz); failures++; continue; }
            if ((uintptr_t)p % 16) { fprintf(stderr, "misaligned %p\n", p); failures++; }
            tags[slot]  = (unsigned char)(op & 0xFF);
            sizes[slot] = sz;
            memset(p, tags[slot], sz);
            ptrs[slot] = p;
        }

        if (op % 5000 == 0 && !my_heap_check()) {
            fprintf(stderr, "heap check failed at op %zu\n", op);
            failures++;
            break;
        }
    }

    /* realloc path */
    void *r = my_malloc(64);
    memset(r, 0xAB, 64);
    r = my_realloc(r, 4096);
    for (int i = 0; i < 64; i++)
        if (((unsigned char *)r)[i] != 0xAB) { fprintf(stderr, "realloc lost data\n"); failures++; break; }
    my_free(r);

    void *c = my_calloc(128, 8);
    for (int i = 0; i < 128 * 8; i++)
        if (((unsigned char *)c)[i]) { fprintf(stderr, "calloc not zeroed\n"); failures++; break; }
    my_free(c);

    for (size_t i = 0; i < live_slots; i++) my_free(ptrs[i]);
    if (!my_heap_check()) { fprintf(stderr, "final heap check failed\n"); failures++; }

    free(ptrs); free(sizes); free(tags);
    printf(failures ? "torture: FAIL (%d issues)\n" : "torture: pass (%d issues)\n", failures);
    return failures;
}

/* ------------------------------------------------------------------ */
/* Timing workloads. Each returns ms elapsed. */

static double bench_lifo(alloc_fn af, free_fn ff, size_t nops, size_t max_size)
{
    size_t depth = 1024;
    void **stack = calloc(depth, sizeof *stack);
    double t0 = now_ms();
    size_t top = 0;
    for (size_t i = 0; i < nops; i++) {
        if (top < depth && (rng() % 3)) {
            void *p = af(rnd_size(max_size));
            if (p) { *(char *)p = 1; stack[top++] = p; }
        } else if (top) {
            ff(stack[--top]);
        }
    }
    while (top) ff(stack[--top]);
    double t = now_ms() - t0;
    free(stack);
    return t;
}

static double bench_random(alloc_fn af, free_fn ff, size_t nops, size_t max_size)
{
    size_t slots = 4096;
    void **live = calloc(slots, sizeof *live);
    double t0 = now_ms();
    for (size_t i = 0; i < nops; i++) {
        size_t s = rng() % slots;
        if (live[s]) { ff(live[s]); live[s] = NULL; }
        else { void *p = af(rnd_size(max_size)); if (p) { *(char *)p = 1; live[s] = p; } }
    }
    for (size_t s = 0; s < slots; s++) if (live[s]) ff(live[s]);
    double t = now_ms() - t0;
    free(live);
    return t;
}

static double bench_grow(alloc_fn af, free_fn ff, size_t nops, size_t max_size)
{
    /* allocate a long-lived array, interleaved with short-lived churn —
     * the pattern that produces external fragmentation */
    size_t keep = nops / 100 + 1;
    void **held = calloc(keep, sizeof *held);
    double t0 = now_ms();
    size_t h = 0;
    for (size_t i = 0; i < nops; i++) {
        void *tmp = af(rnd_size(max_size));
        if (tmp) { *(char *)tmp = 1; }
        if (i % 100 == 0 && h < keep) { held[h++] = tmp; }
        else if (tmp) ff(tmp);
    }
    for (size_t i = 0; i < h; i++) if (held[i]) ff(held[i]);
    double t = now_ms() - t0;
    free(held);
    return t;
}

typedef double (*bench_fn)(alloc_fn, free_fn, size_t, size_t);

static void run_suite(size_t nops, size_t max_size, fit_policy_t fit, const char *fitname)
{
    struct { const char *name; bench_fn fn; } benches[] = {
        { "lifo",   bench_lifo   },
        { "random", bench_random },
        { "churn",  bench_grow   },
    };

    printf("\nfit=%s  ops=%zu  max_size=%zu\n", fitname, nops, max_size);
    printf("%-8s %10s %10s %8s %9s %9s %9s\n",
           "workload", "mine ms", "sys ms", "ratio", "peakKiB", "unused", "fitsteps");
    printf("---------------------------------------------------------------------\n");

    for (size_t b = 0; b < sizeof benches / sizeof benches[0]; b++) {
        my_set_fit(fit);
        my_reset_stats();

        rng_state = 88172645463325252ull;
        double mine = benches[b].fn(my_malloc, my_free, nops, max_size);
        double frag = my_fragmentation();

        const alloc_stats_t *st = my_stats();
        double peak_kib = (double)st->payload_peak / 1024.0;
        double steps = st->mallocs ? (double)st->search_steps / (double)st->mallocs : 0.0;

        rng_state = 88172645463325252ull;
        double sys = benches[b].fn(malloc, free, nops, max_size);

        printf("%-8s %10.1f %10.1f %7.2fx %9.0f %8.1f%% %9.1f\n",
               benches[b].name, mine, sys, mine / sys, peak_kib, 100.0 * frag, steps);
    }
}

int main(int argc, char **argv)
{
    size_t nops = 200000, max_size = 512;
    int do_test = 0, do_bench = 1, do_dump = 0;

    static struct option lo[] = {
        { "ops",   required_argument, 0, 'n' },
        { "max",   required_argument, 0, 'm' },
        { "test",  no_argument,       0, 't' },
        { "dump",  no_argument,       0, 'd' },
        { "no-bench", no_argument,    0, 'B' },
        { 0, 0, 0, 0 }
    };
    int c;
    while ((c = getopt_long(argc, argv, "", lo, NULL)) != -1) {
        switch (c) {
        case 'n': nops = strtoul(optarg, 0, 0); break;
        case 'm': max_size = strtoul(optarg, 0, 0); break;
        case 't': do_test = 1; break;
        case 'd': do_dump = 1; break;
        case 'B': do_bench = 0; break;
        default: return 1;
        }
    }

    int rc = 0;
    if (do_test) rc = torture(nops, max_size, 2048) ? 1 : 0;

    if (do_bench) {
        run_suite(nops, max_size, FIT_FIRST, "first");
        run_suite(nops, max_size, FIT_BEST,  "best");
        run_suite(nops, max_size, FIT_NEXT,  "next");
    }
    if (do_dump) my_dump();
    return rc;
}
