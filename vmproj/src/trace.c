/* trace.c — synthetic workloads.
 *
 * Everything uses a small xorshift PRNG rather than rand() so results are
 * reproducible across machines for a given seed.
 */

#include "trace.h"
#include "vmsim.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint64_t s; } rng_t;

static uint64_t rng_next(rng_t *r)
{
    uint64_t x = r->s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return r->s = x;
}

static uint32_t rng_below(rng_t *r, uint32_t n)
{
    return n ? (uint32_t)(rng_next(r) % n) : 0;
}

static double rng_unit(rng_t *r)
{
    return (double)(rng_next(r) >> 11) / (double)(1ull << 53);
}

static const char *kWl[WL_COUNT] = { "ws", "phased", "seq", "rand", "zipf" };

const char *workload_name(workload_t w)
{
    return (w >= 0 && w < WL_COUNT) ? kWl[w] : "?";
}

bool workload_from_string(const char *s, workload_t *out)
{
    for (int i = 0; i < WL_COUNT; i++)
        if (strcmp(s, kWl[i]) == 0) { *out = (workload_t)i; return true; }
    return false;
}

/* next_use[] is what POLICY_OPT needs. Computed with one backwards pass and a
 * last-seen table: O(n + npages) instead of the naive O(n^2) forward scan. */
static void compute_next_use(trace_t *t, size_t npages)
{
    uint64_t *last = malloc(npages * sizeof *last);
    if (!last) return;
    for (size_t p = 0; p < npages; p++) last[p] = VM_NEVER;

    for (size_t i = t->n; i-- > 0; ) {
        uint32_t v = t->refs[i].vpn;
        t->next_use[i] = last[v];
        last[v] = i;
    }
    free(last);
}

trace_t *trace_generate(workload_t w, size_t npages, size_t ws, size_t nrefs,
                        unsigned write_pct, uint64_t seed)
{
    if (npages == 0 || nrefs == 0) return NULL;
    if (ws == 0 || ws > npages) ws = npages;

    trace_t *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->n        = nrefs;
    t->refs     = malloc(nrefs * sizeof *t->refs);
    t->next_use = malloc(nrefs * sizeof *t->next_use);
    if (!t->refs || !t->next_use) { trace_free(t); return NULL; }

    rng_t rng = { seed ? seed : 0x9E3779B97F4A7C15ull };

    /* Zipf setup: precompute the normalised CDF over page ranks. */
    double *cdf = NULL;
    if (w == WL_ZIPF) {
        cdf = malloc(npages * sizeof *cdf);
        if (!cdf) { trace_free(t); return NULL; }
        double sum = 0.0;
        for (size_t i = 0; i < npages; i++) { sum += 1.0 / (double)(i + 1); cdf[i] = sum; }
        for (size_t i = 0; i < npages; i++) cdf[i] /= sum;
    }

    size_t base = 0;                 /* start of the current hot window */
    const size_t phase_len = nrefs / 8 ? nrefs / 8 : 1;

    for (size_t i = 0; i < nrefs; i++) {
        uint32_t vpn;
        switch (w) {
        case WL_WORKING_SET:
            /* 95% inside the hot set, 5% anywhere — models a program with
             * good locality plus the occasional cold-path touch. */
            vpn = (rng_unit(&rng) < 0.95) ? rng_below(&rng, (uint32_t)ws)
                                          : rng_below(&rng, (uint32_t)npages);
            break;
        case WL_PHASED:
            if (i > 0 && i % phase_len == 0)
                base = (base + ws) % npages;         /* move to a new hot set */
            vpn = (rng_unit(&rng) < 0.95)
                    ? (uint32_t)((base + rng_below(&rng, (uint32_t)ws)) % npages)
                    : rng_below(&rng, (uint32_t)npages);
            break;
        case WL_SEQUENTIAL:
            vpn = (uint32_t)(i % ws);
            break;
        case WL_RANDOM:
            vpn = rng_below(&rng, (uint32_t)npages);
            break;
        case WL_ZIPF: {
            double u = rng_unit(&rng);
            size_t lo = 0, hi = npages - 1;
            while (lo < hi) {                        /* binary search the CDF */
                size_t mid = lo + (hi - lo) / 2;
                if (cdf[mid] < u) lo = mid + 1; else hi = mid;
            }
            vpn = (uint32_t)lo;
            break;
        }
        default:
            vpn = 0;
        }
        t->refs[i].vpn   = vpn;
        t->refs[i].write = (rng_below(&rng, 100) < write_pct);
    }

    free(cdf);
    compute_next_use(t, npages);
    return t;
}

void trace_free(trace_t *t)
{
    if (!t) return;
    free(t->refs);
    free(t->next_use);
    free(t);
}
