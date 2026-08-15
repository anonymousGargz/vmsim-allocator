/* test_vmsim.c — small known-answer tests. These are the ones worth quoting in
 * an interview: they pin the policies to textbook reference strings. */

#include "trace.h"
#include "vmsim.h"

#include <stdio.h>
#include <stdlib.h>

static int failures;

#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
                   printf("\n"); failures++; } } while (0)

static uint64_t replay(policy_t p, const uint32_t *refs, size_t n, size_t frames)
{
    vm_t *vm = vm_create(4096, 16, frames, p);
    uint64_t *next_use = malloc(n * sizeof *next_use);
    for (size_t i = 0; i < n; i++) {
        next_use[i] = VM_NEVER;
        for (size_t j = i + 1; j < n; j++)
            if (refs[j] == refs[i]) { next_use[i] = j; break; }
    }
    if (p == POLICY_OPT) vm_set_future(vm, next_use, n);
    for (size_t i = 0; i < n; i++) vm_access(vm, (uint64_t)refs[i] * 4096, false);
    CHECK(vm_check(vm), "invariants broken for %s", policy_name(p));
    uint64_t f = vm_stats(vm)->faults;
    vm_destroy(vm);
    free(next_use);
    return f;
}

int main(void)
{
    /* Classic reference string from Silberschatz. 3 frames:
     * FIFO = 15, LRU = 12, OPT = 9 faults. */
    static const uint32_t ref[] = {7,0,1,2,0,3,0,4,2,3,0,3,2,1,2,0,1,7,0,1};
    size_t n = sizeof ref / sizeof ref[0];

    CHECK(replay(POLICY_FIFO, ref, n, 3) == 15, "FIFO got %llu, want 15",
          (unsigned long long)replay(POLICY_FIFO, ref, n, 3));
    CHECK(replay(POLICY_LRU,  ref, n, 3) == 12, "LRU got %llu, want 12",
          (unsigned long long)replay(POLICY_LRU, ref, n, 3));
    CHECK(replay(POLICY_OPT,  ref, n, 3) ==  9, "OPT got %llu, want 9",
          (unsigned long long)replay(POLICY_OPT, ref, n, 3));

    /* Belady's anomaly: FIFO can fault MORE with more frames. */
    static const uint32_t anomaly[] = {1,2,3,4,1,2,5,1,2,3,4,5};
    size_t an = sizeof anomaly / sizeof anomaly[0];
    uint64_t f3 = replay(POLICY_FIFO, anomaly, an, 3);
    uint64_t f4 = replay(POLICY_FIFO, anomaly, an, 4);
    CHECK(f3 == 9 && f4 == 10, "Belady: 3 frames=%llu (want 9), 4 frames=%llu (want 10)",
          (unsigned long long)f3, (unsigned long long)f4);

    /* LRU is a stack algorithm: never anomalous. */
    uint64_t l3 = replay(POLICY_LRU, anomaly, an, 3);
    uint64_t l4 = replay(POLICY_LRU, anomaly, an, 4);
    CHECK(l4 <= l3, "LRU anomalous: %llu -> %llu",
          (unsigned long long)l3, (unsigned long long)l4);

    /* OPT is a lower bound on faults for any policy, on any trace. */
    trace_t *t = trace_generate(WL_WORKING_SET, 256, 64, 20000, 30, 42);
    uint64_t opt = 0, worst = 0;
    for (int p = 0; p < POLICY_COUNT; p++) {
        vm_t *vm = vm_create(4096, 256, 32, (policy_t)p);
        if (p == POLICY_OPT) vm_set_future(vm, t->next_use, t->n);
        for (size_t i = 0; i < t->n; i++)
            vm_access(vm, (uint64_t)t->refs[i].vpn * 4096, t->refs[i].write);
        CHECK(vm_check(vm), "invariants broken for %s on generated trace", policy_name(p));
        uint64_t f = vm_stats(vm)->faults;
        if (p == POLICY_OPT) opt = f;
        if (f > worst) worst = f;
        vm_destroy(vm);
    }
    CHECK(opt <= worst, "OPT (%llu) beaten by another policy (%llu)",
          (unsigned long long)opt, (unsigned long long)worst);

    /* Writebacks only ever happen for dirty evictions. */
    vm_t *vm = vm_create(4096, 8, 2, POLICY_FIFO);
    vm_access(vm, 0, false);
    vm_access(vm, 4096, false);
    vm_access(vm, 8192, false);            /* evicts a clean page */
    CHECK(vm_stats(vm)->writebacks == 0, "clean eviction caused a writeback");
    vm_destroy(vm);

    vm = vm_create(4096, 8, 2, POLICY_FIFO);
    vm_access(vm, 0, true);                /* dirty it */
    vm_access(vm, 4096, false);
    vm_access(vm, 8192, false);            /* evicts the dirty page */
    CHECK(vm_stats(vm)->writebacks == 1, "dirty eviction did not write back");
    vm_destroy(vm);

    trace_free(t);
    printf(failures ? "test_vmsim: %d FAILURES\n" : "test_vmsim: all tests passed\n", failures);
    return failures ? 1 : 0;
}
