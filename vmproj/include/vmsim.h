/* vmsim.h — user-space paged virtual memory simulator */
#ifndef VMSIM_H
#define VMSIM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VM_NO_FRAME ((uint32_t)-1)
#define VM_NEVER    UINT64_MAX

typedef enum {
    POLICY_FIFO = 0,
    POLICY_LRU,
    POLICY_CLOCK,
    POLICY_OPT,      /* Belady's optimal — lower bound baseline, needs the future */
    POLICY_COUNT
} policy_t;

/* One page table entry. Real hardware packs this into 8 bytes; kept explicit
 * here so the simulator can report per-field behaviour. */
typedef struct {
    uint32_t frame;      /* physical frame number, VM_NO_FRAME if not resident */
    bool     valid;      /* present bit */
    bool     dirty;      /* written since load — decides writeback on eviction */
    bool     ref;        /* reference bit, used by clock */
} pte_t;

typedef struct {
    uint64_t accesses, reads, writes;
    uint64_t hits, faults;
    uint64_t compulsory;   /* first ever touch of the page */
    uint64_t capacity;     /* seen before, evicted, touched again */
    uint64_t evictions;
    uint64_t writebacks;   /* evicted page was dirty */
} vm_stats_t;

typedef struct vm vm_t;

vm_t *vm_create(size_t page_size, size_t npages, size_t nframes, policy_t policy);
void  vm_destroy(vm_t *vm);
void  vm_reset(vm_t *vm);

/* Translate + service. Returns true if the access faulted. */
bool  vm_access(vm_t *vm, uint64_t vaddr, bool is_write);

/* For POLICY_OPT: next_use[i] is the index of the next reference to the same
 * page after position i, or VM_NEVER. Must be set before replaying the trace. */
void  vm_set_future(vm_t *vm, const uint64_t *next_use, size_t n);

const vm_stats_t *vm_stats(const vm_t *vm);
double vm_fault_rate(const vm_t *vm);
const char *policy_name(policy_t p);
bool policy_from_string(const char *s, policy_t *out);

/* Invariant checker — every resident PTE agrees with its frame, free list
 * count is consistent. Cheap enough to call from tests. */
bool vm_check(const vm_t *vm);

#endif /* VMSIM_H */
