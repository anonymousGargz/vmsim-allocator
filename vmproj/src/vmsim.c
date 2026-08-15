/* vmsim.c — paged address space over a fixed pool of physical frames.
 *
 * Address layout (single-level page table, flat array):
 *
 *   63                     offset_bits            0
 *   +--------------------------+------------------+
 *   |          VPN             |     offset       |
 *   +--------------------------+------------------+
 *
 * Every policy keeps its bookkeeping O(1) per access except OPT, which scans
 * the resident set on a fault only (it is a baseline, not a real policy).
 */

#include "vmsim.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t vpn;
    bool     occupied;
} frame_t;

struct vm {
    size_t   page_size;
    unsigned offset_bits;
    size_t   npages;
    size_t   nframes;
    policy_t policy;

    pte_t   *ptes;
    frame_t *frames;
    bool    *ever_touched;      /* for compulsory vs capacity classification */

    /* free frame stack */
    uint32_t *free_stack;
    size_t    nfree;

    /* FIFO: circular queue of resident frames in load order */
    uint32_t *fifo;
    size_t    fifo_head, fifo_len;

    /* LRU: intrusive doubly linked list over frame indices, head = MRU */
    int32_t *lru_prev, *lru_next;
    int32_t  lru_head, lru_tail;

    /* CLOCK */
    size_t hand;

    /* OPT */
    const uint64_t *future;   /* next_use[] over the trace, borrowed */
    size_t          future_n, future_pos;
    uint64_t       *frame_next_use;

    vm_stats_t st;
};

static const char *kNames[POLICY_COUNT] = { "FIFO", "LRU", "CLOCK", "OPT" };

const char *policy_name(policy_t p)
{
    return (p >= 0 && p < POLICY_COUNT) ? kNames[p] : "?";
}

bool policy_from_string(const char *s, policy_t *out)
{
    for (int i = 0; i < POLICY_COUNT; i++) {
        const char *n = kNames[i];
        size_t j = 0;
        for (; n[j] && s[j]; j++) {
            char a = n[j], b = s[j];
            if (b >= 'a' && b <= 'z') b = (char)(b - 'a' + 'A');
            if (a != b) break;
        }
        if (!n[j] && !s[j]) { *out = (policy_t)i; return true; }
    }
    return false;
}

static unsigned ilog2_exact(size_t v)
{
    unsigned n = 0;
    while (v > 1) { v >>= 1; n++; }
    return n;
}

vm_t *vm_create(size_t page_size, size_t npages, size_t nframes, policy_t policy)
{
    if (page_size == 0 || (page_size & (page_size - 1)) != 0) return NULL;
    if (npages == 0 || nframes == 0 || nframes > npages) {
        if (nframes > npages) nframes = npages;  /* more frames than pages is legal but pointless */
        if (npages == 0 || nframes == 0) return NULL;
    }

    vm_t *vm = calloc(1, sizeof *vm);
    if (!vm) return NULL;

    vm->page_size   = page_size;
    vm->offset_bits = ilog2_exact(page_size);
    vm->npages      = npages;
    vm->nframes     = nframes;
    vm->policy      = policy;

    vm->ptes           = calloc(npages, sizeof *vm->ptes);
    vm->ever_touched   = calloc(npages, sizeof *vm->ever_touched);
    vm->frames         = calloc(nframes, sizeof *vm->frames);
    vm->free_stack     = calloc(nframes, sizeof *vm->free_stack);
    vm->fifo           = calloc(nframes, sizeof *vm->fifo);
    vm->lru_prev       = calloc(nframes, sizeof *vm->lru_prev);
    vm->lru_next       = calloc(nframes, sizeof *vm->lru_next);
    vm->frame_next_use = calloc(nframes, sizeof *vm->frame_next_use);

    if (!vm->ptes || !vm->ever_touched || !vm->frames || !vm->free_stack ||
        !vm->fifo || !vm->lru_prev || !vm->lru_next || !vm->frame_next_use) {
        vm_destroy(vm);
        return NULL;
    }

    vm_reset(vm);
    return vm;
}

void vm_reset(vm_t *vm)
{
    memset(&vm->st, 0, sizeof vm->st);
    memset(vm->ever_touched, 0, vm->npages * sizeof *vm->ever_touched);

    for (size_t i = 0; i < vm->npages; i++) {
        vm->ptes[i].valid = false;
        vm->ptes[i].dirty = false;
        vm->ptes[i].ref   = false;
        vm->ptes[i].frame = VM_NO_FRAME;
    }
    for (size_t i = 0; i < vm->nframes; i++) {
        vm->frames[i].occupied = false;
        vm->lru_prev[i] = vm->lru_next[i] = -1;
        vm->frame_next_use[i] = VM_NEVER;
        /* push in reverse so frame 0 is handed out first — nicer traces */
        vm->free_stack[i] = (uint32_t)(vm->nframes - 1 - i);
    }
    vm->nfree      = vm->nframes;
    vm->fifo_head  = vm->fifo_len = 0;
    vm->lru_head   = vm->lru_tail = -1;
    vm->hand       = 0;
    vm->future_pos = 0;
}

void vm_destroy(vm_t *vm)
{
    if (!vm) return;
    free(vm->ptes);
    free(vm->ever_touched);
    free(vm->frames);
    free(vm->free_stack);
    free(vm->fifo);
    free(vm->lru_prev);
    free(vm->lru_next);
    free(vm->frame_next_use);
    free(vm);
}

void vm_set_future(vm_t *vm, const uint64_t *next_use, size_t n)
{
    vm->future     = next_use;
    vm->future_n   = n;
    vm->future_pos = 0;
}

/* ---------------- LRU list helpers ---------------- */

static void lru_unlink(vm_t *vm, int32_t f)
{
    int32_t p = vm->lru_prev[f], n = vm->lru_next[f];
    if (p >= 0) vm->lru_next[p] = n; else vm->lru_head = n;
    if (n >= 0) vm->lru_prev[n] = p; else vm->lru_tail = p;
    vm->lru_prev[f] = vm->lru_next[f] = -1;
}

static void lru_push_front(vm_t *vm, int32_t f)
{
    vm->lru_prev[f] = -1;
    vm->lru_next[f] = vm->lru_head;
    if (vm->lru_head >= 0) vm->lru_prev[vm->lru_head] = f;
    vm->lru_head = f;
    if (vm->lru_tail < 0) vm->lru_tail = f;
}

static void lru_touch(vm_t *vm, int32_t f)
{
    if (vm->lru_head == f) return;
    lru_unlink(vm, f);
    lru_push_front(vm, f);
}

/* ---------------- victim selection ---------------- */

static uint32_t pick_victim(vm_t *vm)
{
    switch (vm->policy) {
    case POLICY_FIFO: {
        uint32_t f = vm->fifo[vm->fifo_head];
        vm->fifo_head = (vm->fifo_head + 1) % vm->nframes;
        vm->fifo_len--;
        return f;
    }
    case POLICY_LRU: {
        int32_t f = vm->lru_tail;
        assert(f >= 0);
        lru_unlink(vm, f);
        return (uint32_t)f;
    }
    case POLICY_CLOCK: {
        /* Sweep, clearing reference bits, until a page with ref == 0 is found.
         * Guaranteed to terminate in at most two laps. */
        for (;;) {
            uint32_t f = (uint32_t)vm->hand;
            vm->hand = (vm->hand + 1) % vm->nframes;
            pte_t *e = &vm->ptes[vm->frames[f].vpn];
            if (!e->ref) return f;
            e->ref = false;
        }
    }
    case POLICY_OPT: {
        uint32_t best = 0;
        uint64_t furthest = 0;
        for (size_t f = 0; f < vm->nframes; f++) {
            if (vm->frame_next_use[f] >= furthest) {
                furthest = vm->frame_next_use[f];
                best = (uint32_t)f;
            }
            if (furthest == VM_NEVER) break;   /* can't do better than never */
        }
        return best;
    }
    default:
        abort();
    }
}

static void on_place(vm_t *vm, uint32_t frame)
{
    switch (vm->policy) {
    case POLICY_FIFO: {
        size_t tail = (vm->fifo_head + vm->fifo_len) % vm->nframes;
        vm->fifo[tail] = frame;
        vm->fifo_len++;
        break;
    }
    case POLICY_LRU:
        lru_push_front(vm, (int32_t)frame);
        break;
    case POLICY_CLOCK:
        vm->ptes[vm->frames[frame].vpn].ref = true;
        break;
    case POLICY_OPT:
        break;
    default:
        abort();
    }
}

static void on_hit(vm_t *vm, uint32_t frame)
{
    switch (vm->policy) {
    case POLICY_LRU:   lru_touch(vm, (int32_t)frame); break;
    case POLICY_CLOCK: vm->ptes[vm->frames[frame].vpn].ref = true; break;
    default: break;
    }
}

/* ---------------- the access path ---------------- */

bool vm_access(vm_t *vm, uint64_t vaddr, bool is_write)
{
    uint64_t vpn = vaddr >> vm->offset_bits;
    if (vpn >= vm->npages) {
        /* Out of the simulated address space — a real kernel would SIGSEGV. */
        return false;
    }

    vm->st.accesses++;
    if (is_write) vm->st.writes++; else vm->st.reads++;

    pte_t *e = &vm->ptes[vpn];
    bool faulted = false;

    if (e->valid) {
        vm->st.hits++;
        on_hit(vm, e->frame);
    } else {
        faulted = true;
        vm->st.faults++;
        if (vm->ever_touched[vpn]) vm->st.capacity++;
        else                       vm->st.compulsory++;

        uint32_t frame;
        if (vm->nfree > 0) {
            frame = vm->free_stack[--vm->nfree];
        } else {
            frame = pick_victim(vm);
            pte_t *victim = &vm->ptes[vm->frames[frame].vpn];
            vm->st.evictions++;
            if (victim->dirty) vm->st.writebacks++;   /* pay for the write-out */
            victim->valid = false;
            victim->dirty = false;
            victim->ref   = false;
            victim->frame = VM_NO_FRAME;
        }

        vm->frames[frame].vpn      = (uint32_t)vpn;
        vm->frames[frame].occupied = true;
        e->valid = true;
        e->dirty = false;
        e->ref   = false;
        e->frame = frame;
        on_place(vm, frame);
    }

    vm->ever_touched[vpn] = true;
    if (is_write) e->dirty = true;

    if (vm->policy == POLICY_OPT && vm->future && vm->future_pos < vm->future_n)
        vm->frame_next_use[e->frame] = vm->future[vm->future_pos];
    vm->future_pos++;

    return faulted;
}

const vm_stats_t *vm_stats(const vm_t *vm) { return &vm->st; }

double vm_fault_rate(const vm_t *vm)
{
    return vm->st.accesses ? (double)vm->st.faults / (double)vm->st.accesses : 0.0;
}

bool vm_check(const vm_t *vm)
{
    size_t resident = 0;
    for (size_t p = 0; p < vm->npages; p++) {
        const pte_t *e = &vm->ptes[p];
        if (!e->valid) continue;
        resident++;
        if (e->frame >= vm->nframes) return false;
        if (!vm->frames[e->frame].occupied) return false;
        if (vm->frames[e->frame].vpn != p) return false;
    }
    if (resident + vm->nfree != vm->nframes) return false;
    if (vm->policy == POLICY_FIFO && vm->fifo_len != resident) return false;
    return true;
}
