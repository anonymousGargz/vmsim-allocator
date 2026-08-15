# Virtual Memory & Allocator Simulator

Two systems components in C, both entirely user-space — no kernel modules, no
root, no privileged syscalls:

1. **`vmsim`** — a paged virtual address space over a fixed pool of physical
   frames, with page-table lookup, page-fault servicing, dirty-page writeback
   accounting, and pluggable replacement policies (FIFO, LRU, CLOCK, plus
   Belady's OPT as a lower-bound baseline).
2. **`allocbench`** — a `malloc`/`free`/`realloc`/`calloc` implementation built
   on `mmap`, using boundary tags and an explicit doubly-linked free list with
   block splitting and immediate coalescing, benchmarked against glibc.

## Build

Requires a C11 compiler and GNU make. Nothing else.

```bash
make            # build everything into build/
make test       # known-answer policy tests + allocator torture test
make asan       # rebuild under ASan/UBSan and rerun
make bench      # fault-rate sweeps and allocator timing
make valgrind   # if valgrind is on PATH
make clean
```

## Usage

```bash
# fault rates for every policy at a given configuration
./build/vmsim --workload ws --pages 1024 --ws 128 --frames 64 --refs 300000

# sweep the frame count and print a curve
./build/vmsim --sweep-frames 8 --workload ws --pages 1024 --ws 128

# sweep the working-set size instead, as CSV for plotting
./build/vmsim --sweep-ws 10 --frames 64 --pages 512 --csv > sweep.csv

# allocator: correctness only, then timing against glibc
./build/allocbench --test --no-bench --ops 200000
./build/allocbench --ops 300000 --max 512
```

`vmsim --help` lists every flag. Workloads are `ws`, `phased`, `seq`, `rand`,
`zipf`; `--seed` makes any run reproducible.

## Layout

```
include/vmsim.h        simulator API
include/trace.h        synthetic reference-stream generator
include/alloc.h        allocator API
src/vmsim.c            page table, frame pool, FIFO / LRU / CLOCK / OPT
src/trace.c            working-set, phased, sequential, uniform, Zipf workloads
src/main_vmsim.c       CLI harness and sweeps (table or CSV output)
src/alloc.c            the allocator
src/main_allocbench.c  torture test and head-to-head timing
tests/test_vmsim.c     known-answer tests
WALKTHROUGH.md         annotated build-up of both components
```

## Design — the simulator

A virtual address splits into `VPN | offset` at `log2(page_size)`. The page
table is a flat array of PTEs indexed by VPN (`valid`, `dirty`, `ref`, `frame`);
a reverse map from frame to VPN makes eviction O(1) once a victim is chosen.

Each policy keeps its own bookkeeping, and every access is O(1):

| policy | structure | on hit | on fault |
| --- | --- | --- | --- |
| FIFO | circular queue of frames in load order | nothing | dequeue head |
| LRU | intrusive doubly-linked list, head = MRU | move to head | evict tail |
| CLOCK | reference bits + rotating hand | set `ref` | sweep clearing `ref` until one is 0 |
| OPT | per-frame next-use index | refresh next-use | evict furthest next use |

OPT requires the future, so the trace generator precomputes `next_use[i]` in a
single backwards pass with a last-seen table — O(n + pages) rather than the
naive O(n²) forward scan.

Faults are classified as **compulsory** (first ever touch) or **capacity**
(previously resident, evicted, touched again), and dirty evictions are counted
separately as writebacks, since those are the ones that cost I/O.

## Results

Working-set workload, 1024-page address space, hot set of 128 pages:

```
  frames      FIFO       LRU     CLOCK       OPT
      64    55.87%    54.94%    55.29%    24.04%
     128    22.41%    14.25%    16.44%     4.59%
     192    10.70%     4.19%     4.30%     3.04%
     256     7.33%     3.89%     3.89%     2.48%
     512     3.48%     2.65%     2.67%     1.30%
```

The knee sits exactly where the frame count reaches the working-set size; past
that, additional frames buy very little. CLOCK tracks LRU to within roughly two
percentage points while doing a single bit-write per hit instead of a list
splice — the reason production kernels use an approximation rather than true
LRU.

Sequential scan over 100 pages with 99 frames:

```
FIFO / LRU / CLOCK: 100.00% fault rate     OPT: 1.11%
```

One frame short of the working set and every stack-based policy degenerates
completely, while OPT is unaffected. Locality assumptions are a property of the
workload, not of the policy.

### Correctness

`tests/test_vmsim.c` pins the policies to textbook reference strings (FIFO 15,
LRU 12, OPT 9 faults on the Silberschatz string at three frames) and reproduces
**Belady's anomaly**: FIFO faults 9 times with 3 frames and 10 times with 4 on
`1 2 3 4 1 2 5 1 2 3 4 5`. LRU, being a stack algorithm, is asserted never to
get worse as frames are added. `vm_check()` verifies that the forward and
reverse maps agree and that resident + free frames equal the pool size; the
`--verify` flag runs it after every replay.

## Design — the allocator

```
+--------+-------------------------+--------+
| header |        payload          | footer |
+--------+-------------------------+--------+
  8 B      size - 16, 16-aligned      8 B
```

Header and footer both store `size | alloc`. The footer makes backward
coalescing O(1): from a block you can read the previous block's footer at
`bp - 16` and recover its size without walking the heap. Free blocks store
their list pointers inside their own payload, so the minimum block is 32 bytes
(8 header + 16 pointers + 8 footer) and there is no per-block overhead beyond
the tags.

Each `mmap` region is bracketed by an allocated prologue and a zero-size
allocated epilogue, so coalescing can never walk off either end — no bounds
checks in the hot path.

Three fit policies are selectable at runtime (`FIT_FIRST`, `FIT_BEST`,
`FIT_NEXT`) so the benchmark can quantify the trade-off:

```
fit=first                                  fit=best
workload  mine ms  sys ms  ratio  fitsteps    mine ms  sys ms  ratio  fitsteps
lifo          5.2     5.3  0.99x       1.0        5.4     5.0  1.08x       1.0
random       10.9     8.1  1.34x       3.6       66.1     7.6  8.71x      84.1
churn         7.7     7.4  1.04x       1.0        8.0     7.3  1.09x       1.0
```

Within roughly 5–35% of glibc on LIFO and churn patterns. Best fit is **8.7×
slower** on the random workload for no fragmentation benefit, because it scans
the entire free list — 84 nodes per allocation against 3.6 for first fit. That
measurement is the case for segregated free lists, which is the natural next
step.

### Correctness

`--test` runs a randomized torture test: every allocation is filled with a tag
byte derived from its op index and verified before being freed, so overlapping
blocks, bad splits and lost coalesces all surface as detectable corruption.
`my_heap_check()` walks every block in every region checking header/footer
agreement, alignment, minimum size, and the absence of adjacent free blocks (a
missed coalesce), then verifies that every free-list node is genuinely free and
that the list length matches the counter.

## Limitations

- Not thread safe. A global lock is trivial; per-thread arenas are the real fix.
- Memory is never returned to the OS (`munmap`), so a long-running process holds
  its peak footprint. Releasing a fully-empty region would need a per-region
  live-block count.
- A single free list. Segregated fits by size class would remove the best-fit
  scan cost measured above.
- `realloc` grows in place only by absorbing the *next* block, not by coalescing
  backwards, which would relocate the payload.
- No TLB and a single-level page table, so translation cost is not modelled —
  only fault behaviour is.

## Running without root

Nothing here needs elevated privileges. `mmap` with `MAP_ANONYMOUS` is
unprivileged, and the build uses only libc.

If Valgrind isn't installed and you can't `apt install`, either:

```bash
# build it into your home directory
curl -LO https://sourceware.org/pub/valgrind/valgrind-3.23.0.tar.bz2
tar xf valgrind-3.23.0.tar.bz2 && cd valgrind-3.23.0
./configure --prefix=$HOME/.local && make -j && make install
export PATH=$HOME/.local/bin:$PATH

# or unpack the distro package without installing it
apt-get download valgrind
dpkg -x valgrind_*.deb $HOME/.local/valgrind
export PATH=$HOME/.local/valgrind/usr/bin:$PATH
```

`make asan` is the zero-install alternative and catches most of the same class
of bug. Note that ASan does not instrument the custom allocator's own blocks,
since those come from `mmap` — the torture test's tag verification is what
covers that path.

## Possible extensions

- Multi-level page tables and a TLB with its own hit-rate statistics, giving
  translation a measurable cost model.
- Second-chance / WSClock, or ARC, added to the policy enum.
- Replaying real traces (Valgrind's Lackey tool emits memory traces) rather
  than synthetic ones.
- Routing the allocator's `mmap` calls through the simulator so that allocator
  layout decisions show up as page faults, joining the two halves into one
  experiment.
