#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE          /* getopt_long is a GNU extension */
#endif

/* main_vmsim.c — harness: replay a synthetic trace under every policy and
 * report page-fault rates, either as a table or as CSV for plotting. */

#include "trace.h"
#include "vmsim.h"

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    size_t     page_size;
    size_t     npages;
    size_t     nframes;
    size_t     ws;
    size_t     nrefs;
    unsigned   write_pct;
    uint64_t   seed;
    workload_t wl;
    int        csv;
    int        sweep_frames;   /* number of sweep points, 0 = single run */
    int        sweep_ws;
    int        verify;
} opts_t;

static void usage(const char *argv0)
{
    fprintf(stderr,
      "usage: %s [options]\n"
      "  --page-size N     bytes, power of two (default 4096)\n"
      "  --pages N         size of the virtual address space in pages (default 1024)\n"
      "  --frames N        physical frames available (default 64)\n"
      "  --ws N            hot working set in pages (default 128)\n"
      "  --refs N          references to generate (default 500000)\n"
      "  --write-pct N     %% of accesses that are writes (default 30)\n"
      "  --workload W      ws | phased | seq | rand | zipf (default ws)\n"
      "  --seed N          PRNG seed (default 1)\n"
      "  --sweep-frames N  sweep frame count over N points and print a curve\n"
      "  --sweep-ws N      sweep working-set size over N points\n"
      "  --csv             machine-readable output\n"
      "  --verify          run the internal invariant check after each replay\n",
      argv0);
}

typedef struct {
    double   fault_rate;
    uint64_t faults, writebacks, compulsory, capacity;
} result_t;

static result_t run_one(const trace_t *t, const opts_t *o, policy_t p,
                        size_t nframes, int verify)
{
    vm_t *vm = vm_create(o->page_size, o->npages, nframes, p);
    if (!vm) { perror("vm_create"); exit(1); }
    if (p == POLICY_OPT) vm_set_future(vm, t->next_use, t->n);

    for (size_t i = 0; i < t->n; i++)
        vm_access(vm, (uint64_t)t->refs[i].vpn * o->page_size, t->refs[i].write);

    if (verify && !vm_check(vm)) {
        fprintf(stderr, "INVARIANT VIOLATION: policy=%s frames=%zu\n",
                policy_name(p), nframes);
        exit(2);
    }

    const vm_stats_t *s = vm_stats(vm);
    result_t r = { vm_fault_rate(vm), s->faults, s->writebacks,
                   s->compulsory, s->capacity };
    vm_destroy(vm);
    return r;
}

static void print_single(const trace_t *t, const opts_t *o)
{
    if (o->csv) printf("policy,frames,accesses,faults,fault_rate,compulsory,capacity,writebacks\n");
    else {
        printf("workload=%s pages=%zu ws=%zu frames=%zu refs=%zu writes=%u%%\n\n",
               workload_name(o->wl), o->npages, o->ws, o->nframes, o->nrefs, o->write_pct);
        printf("%-7s %10s %10s %10s %10s %10s\n",
               "policy", "faults", "fault%", "compuls.", "capacity", "writeback");
        printf("---------------------------------------------------------------\n");
    }

    for (int p = 0; p < POLICY_COUNT; p++) {
        result_t r = run_one(t, o, (policy_t)p, o->nframes, o->verify);
        if (o->csv)
            printf("%s,%zu,%zu,%llu,%.6f,%llu,%llu,%llu\n",
                   policy_name((policy_t)p), o->nframes, o->nrefs,
                   (unsigned long long)r.faults, r.fault_rate,
                   (unsigned long long)r.compulsory,
                   (unsigned long long)r.capacity,
                   (unsigned long long)r.writebacks);
        else
            printf("%-7s %10llu %9.3f%% %10llu %10llu %10llu\n",
                   policy_name((policy_t)p),
                   (unsigned long long)r.faults, 100.0 * r.fault_rate,
                   (unsigned long long)r.compulsory,
                   (unsigned long long)r.capacity,
                   (unsigned long long)r.writebacks);
    }
}

static void print_sweep_frames(const trace_t *t, const opts_t *o)
{
    int points = o->sweep_frames;
    size_t max_frames = o->npages < 512 ? o->npages : 512;

    if (o->csv) printf("frames,policy,fault_rate,faults\n");
    else {
        printf("fault rate vs frames  (workload=%s ws=%zu pages=%zu)\n\n",
               workload_name(o->wl), o->ws, o->npages);
        printf("%8s", "frames");
        for (int p = 0; p < POLICY_COUNT; p++) printf("%10s", policy_name((policy_t)p));
        printf("\n--------------------------------------------------\n");
    }

    for (int i = 1; i <= points; i++) {
        size_t nf = max_frames * (size_t)i / (size_t)points;
        if (nf == 0) nf = 1;
        if (!o->csv) printf("%8zu", nf);
        for (int p = 0; p < POLICY_COUNT; p++) {
            result_t r = run_one(t, o, (policy_t)p, nf, o->verify);
            if (o->csv)
                printf("%zu,%s,%.6f,%llu\n", nf, policy_name((policy_t)p),
                       r.fault_rate, (unsigned long long)r.faults);
            else
                printf("%9.2f%%", 100.0 * r.fault_rate);
        }
        if (!o->csv) printf("\n");
    }
}

static void print_sweep_ws(const opts_t *o)
{
    int points = o->sweep_ws;

    if (o->csv) printf("ws,policy,fault_rate,faults\n");
    else {
        printf("fault rate vs working-set size  (frames=%zu pages=%zu)\n\n",
               o->nframes, o->npages);
        printf("%8s", "ws");
        for (int p = 0; p < POLICY_COUNT; p++) printf("%10s", policy_name((policy_t)p));
        printf("\n--------------------------------------------------\n");
    }

    for (int i = 1; i <= points; i++) {
        size_t ws = o->npages * (size_t)i / (size_t)points;
        if (ws == 0) ws = 1;
        opts_t local = *o;
        local.ws = ws;
        trace_t *t = trace_generate(o->wl, o->npages, ws, o->nrefs, o->write_pct, o->seed);
        if (!t) { fprintf(stderr, "trace_generate failed\n"); exit(1); }

        if (!o->csv) printf("%8zu", ws);
        for (int p = 0; p < POLICY_COUNT; p++) {
            result_t r = run_one(t, &local, (policy_t)p, o->nframes, o->verify);
            if (o->csv)
                printf("%zu,%s,%.6f,%llu\n", ws, policy_name((policy_t)p),
                       r.fault_rate, (unsigned long long)r.faults);
            else
                printf("%9.2f%%", 100.0 * r.fault_rate);
        }
        if (!o->csv) printf("\n");
        trace_free(t);
    }
}

int main(int argc, char **argv)
{
    opts_t o = { 4096, 1024, 64, 128, 500000, 30, 1, WL_WORKING_SET, 0, 0, 0, 0 };

    static struct option lo[] = {
        { "page-size",    required_argument, 0, 'P' },
        { "pages",        required_argument, 0, 'p' },
        { "frames",       required_argument, 0, 'f' },
        { "ws",           required_argument, 0, 'w' },
        { "refs",         required_argument, 0, 'n' },
        { "write-pct",    required_argument, 0, 'W' },
        { "workload",     required_argument, 0, 'k' },
        { "seed",         required_argument, 0, 's' },
        { "sweep-frames", required_argument, 0, 'F' },
        { "sweep-ws",     required_argument, 0, 'S' },
        { "csv",          no_argument,       0, 'c' },
        { "verify",       no_argument,       0, 'v' },
        { "help",         no_argument,       0, 'h' },
        { 0, 0, 0, 0 }
    };

    int c;
    while ((c = getopt_long(argc, argv, "", lo, NULL)) != -1) {
        switch (c) {
        case 'P': o.page_size = strtoul(optarg, 0, 0); break;
        case 'p': o.npages    = strtoul(optarg, 0, 0); break;
        case 'f': o.nframes   = strtoul(optarg, 0, 0); break;
        case 'w': o.ws        = strtoul(optarg, 0, 0); break;
        case 'n': o.nrefs     = strtoul(optarg, 0, 0); break;
        case 'W': o.write_pct = (unsigned)strtoul(optarg, 0, 0); break;
        case 's': o.seed      = strtoull(optarg, 0, 0); break;
        case 'k':
            if (!workload_from_string(optarg, &o.wl)) {
                fprintf(stderr, "unknown workload '%s'\n", optarg);
                return 1;
            }
            break;
        case 'F': o.sweep_frames = atoi(optarg); break;
        case 'S': o.sweep_ws     = atoi(optarg); break;
        case 'c': o.csv    = 1; break;
        case 'v': o.verify = 1; break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }

    if (o.sweep_ws) { print_sweep_ws(&o); return 0; }

    trace_t *t = trace_generate(o.wl, o.npages, o.ws, o.nrefs, o.write_pct, o.seed);
    if (!t) { fprintf(stderr, "trace_generate failed\n"); return 1; }

    if (o.sweep_frames) print_sweep_frames(t, &o);
    else                print_single(t, &o);

    trace_free(t);
    return 0;
}
