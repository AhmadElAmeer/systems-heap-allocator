# systems-heap-allocator

A `malloc` / `free` / `realloc` implementation in C, plus a benchmark that replays allocation traces to compare first-fit, next-fit and best-fit placement on the same workloads.

## Problem

Every program that calls `malloc` relies on an allocator to decide *where* each block goes. That decision is invisible until it goes wrong. Requests start failing even though plenty of memory is free in total, because it is split into holes that are each too small. This is external fragmentation, and placement policy is one of the main levers that controls it.

Textbooks describe the policies in a paragraph each. This project implements them over the same allocator, so the only variable is the policy, and measures what happens.

## Overview

- `src/heap.c` manages one fixed-size arena per heap. It hands out 16-byte-aligned blocks and merges free neighbours immediately.
- `heap_check()` walks the entire heap and the free list and verifies every invariant. The tests call it after every operation.
- `bench/trace_runner` replays a trace (a list of alloc/free/realloc operations) under each policy. It reports failed requests, external fragmentation, internal fragmentation and time per operation.
- `scripts/gen_traces.py` produces four reproducible workloads, each designed to stress the policies differently.

## Features

- `heap_alloc`, `heap_free`, `heap_realloc`, `heap_usable_size` with the same contracts as the standard library: `NULL` on failure, `realloc(NULL, n)` allocates, `realloc(p, 0)` frees.
- Three placement policies selectable per heap: first fit, next fit and best fit.
- Block splitting, and immediate coalescing in O(1) using boundary tags (header + footer).
- `realloc` shrinks in place, grows in place when the next block is free, and moves only when it must. On failure the original block is left intact.
- `heap_free` rejects pointers outside the arena, misaligned pointers and double frees (returns `-1`).
- A heap consistency checker that detects missed coalesces, header/footer mismatches, free-list cycles, broken back-links and stale next-fit state.

## Tech stack

- C11, no dependencies beyond libc
- GNU Make
- GCC/Clang with AddressSanitizer and UndefinedBehaviorSanitizer for the sanitizer test build
- Python 3 (standard library only), used only to regenerate traces

## Architecture

### Arena layout

```
| pad | prologue | block | block | ... | block | epilogue |
  8B     16B                                       8B
```

Each block carries its size and an allocated bit in both a header and a footer:

```
allocated:  | hdr: size|1 | payload ...                       | ftr: size|1 |
free:       | hdr: size|0 | next free | prev free | (unused)   | ftr: size|0 |
```

- Sizes are multiples of 16, so the low bits of the size word are free for the allocated flag.
- Blocks start at addresses ≡ 8 (mod 16), so every payload is 16-byte aligned.
- The footer is what makes coalescing O(1). From any block, the word just before its header is the previous block's footer, so the left neighbour's size and state are one read away.
- The **prologue** and **epilogue** are permanently allocated sentinels. Coalescing never needs a bounds check.
- Free blocks store `next`/`prev` pointers inside their own (unused) payload. That forms an **explicit free list**, so a search only visits free blocks. The cost is a minimum block size of 32 bytes.

### Operations

| Operation | What happens | Cost |
|---|---|---|
| alloc | round the size up, search the free list by policy, split if the remainder is ≥ 32 B | O(free blocks) |
| free | clear the allocated bit, merge with free neighbours, push the result onto the list | O(1) |
| realloc | shrink in place / absorb the next free block / allocate-copy-free | O(1) or O(free blocks) |

Freed blocks are pushed onto the front of the list (LIFO). That choice matters for the results below.

## Installation

Requires a C compiler and `make`. Tested with GCC 13.3 on Ubuntu 24.04.

```bash
git clone https://github.com/AhmadElAmeer/systems-heap-allocator.git
cd systems-heap-allocator
make            # builds build/test_heap and build/trace_runner
```

## Usage

### As a library

```c
#include "heap.h"

heap_t *h = heap_create(1 << 20, FIT_BEST);   /* 1 MiB arena */

int *nums = heap_alloc(h, 100 * sizeof *nums);
nums = heap_realloc(h, nums, 200 * sizeof *nums);

const char *err = heap_check(h);              /* NULL when consistent */
heap_stats st = heap_get_stats(h);
printf("%zu free blocks, largest %zu bytes\n", st.free_blocks, st.largest_free);

heap_free(h, nums);
heap_destroy(h);
```

### Benchmark

```bash
make bench                                    # all traces, 1 MiB heap
./build/trace_runner -s 262144 traces/*.trace # smaller heap = more pressure
./build/trace_runner -c traces/phases.trace   # heap_check after every op
```

Trace format (`#` starts a comment):

```
a 7 120     # allocate 120 bytes, call it block 7
r 7 300     # realloc block 7 to 300 bytes
f 7         # free block 7
```

## Results

Output of `make bench` (1 MiB heap) on a 2-core Intel Xeon @ 2.10 GHz:

```
trace                  policy         ops     failed  ext-frag  int-frag    ns/op
phases.trace           first-fit    12000          0     51.6%      1.0%     86.4
phases.trace           next-fit     12000          0     51.2%      1.0%     41.0
phases.trace           best-fit     12000          0     32.7%      1.0%    100.3
random-mix.trace       first-fit    20394        123     82.1%      1.1%    134.4
random-mix.trace       next-fit     20394         69     89.4%      1.1%    105.9
random-mix.trace       best-fit     20394          0     35.5%      1.1%    351.2
realloc-growth.trace   first-fit      840          0      3.1%      1.0%    197.8
realloc-growth.trace   next-fit       840          0      6.1%      1.0%    201.8
realloc-growth.trace   best-fit       840          0      1.8%      1.1%    236.5
small-large.trace      first-fit    16800          0     10.6%      1.0%     26.2
small-large.trace      next-fit     16800          0     10.6%      1.0%     13.4
small-large.trace      best-fit     16800          0      5.7%      1.0%     46.8
```

- **failed**: requests that returned `NULL`. Deterministic.
- **ext-frag**: average of `1 - largest_free / total_free` over 200 samples. 0% means all free memory is one block. Deterministic.
- **int-frag**: share of allocated block bytes not requested by the program (headers, footers, rounding), measured at peak usage.
- **ns/op**: noisy. Across repeated runs on the same machine it moved by up to 2×, so read it as a ranking, not a measurement.

What the numbers show:

- **Best fit fragments least on every trace.** On `random-mix`, best fit serves every request, while first fit fails 123 and next fit 69, even though all three had the same total free memory.
- **Best fit is the slowest.** Unless it finds an exact match, it must scan the whole free list. On `random-mix` it was roughly 2.5–4.5× slower per operation than first fit across five runs.
- **Next fit is usually the fastest and the most fragmented.** It resumes searching where the last one stopped, so searches are short. The cost is that allocations spread across the whole heap.
- **First fit here is not textbook first fit.** With a LIFO free list, "first" means "most recently freed", not "lowest address". This version therefore behaves more like next fit than the classic address-ordered first fit, which the literature reports as fragmenting about as little as best fit. Testing that is the first item under future improvements.

## Testing

```bash
make test       # 11 tests × 3 policies, 674 checks
make test-san   # the same tests under AddressSanitizer + UBSan
```

The suite covers:

- alignment
- splitting
- each of the four coalescing cases (free neighbour on the left, right, both, neither)
- reuse of freed memory
- out-of-memory, including `SIZE_MAX` overflow
- invalid and double frees
- every `realloc` path
- which block each policy picks
- a 20,000-operation random stress test

In the stress test, every live block is filled with a slot-specific byte. Any overlap between blocks, or any metadata write into a payload, shows up as corrupted contents. `heap_check` runs after every one of those operations.

To check that the tests can actually fail, six deliberate bugs were planted one at a time and the suite run against each:

- removing right-side coalescing
- leaving a stale next-fit pointer
- allowing 16-byte splits
- breaking best fit's selection
- copying too little in `realloc`
- disabling double-free detection

All six were caught, either by a failed check or by a crash.

## Design decisions and challenges

- **Explicit vs. implicit free list.** An implicit list (walking every block) is simpler, but each allocation becomes O(all blocks). The explicit list makes allocation O(free blocks), at the cost of a 32-byte minimum block.
- **The next-fit rover.** Next fit remembers where its last search ended. The hard part was keeping that pointer valid: when a block is merged into its neighbour or allocated, the rover may point into the middle of another block. The fix is in `list_remove`: any block leaving the free list moves the rover to its successor. `heap_check` verifies the rover never points at an allocated block.
- **Strict aliasing.** All header and pointer reads go through `memcpy` rather than casting `char *` to `size_t *`. That avoids undefined behaviour, and the UBSan build reports none.
- **Validating `free`.** A real `free` can't tell a garbage pointer from a valid one. This version can catch the common mistakes (wrong heap, misaligned, double free) because it knows its arena bounds and checks the header and footer for agreement. It can't catch a pointer into the middle of a live block's payload that happens to look like a header.
- **Measuring without distorting.** Computing fragmentation means walking the heap, which would dominate the timing. The runner therefore replays each trace twice: once timed, once sampled.

## Future improvements

- Address-ordered free list, to test whether first fit then matches best fit, as the literature suggests.
- Segregated free lists (one list per size class), the approach used by real allocators, giving close-to-best-fit placement at close-to-O(1) cost.
- Growing the arena with `mmap`/`sbrk` instead of a fixed size.
- A thread-safe variant (one lock vs. per-thread arenas) with a concurrency benchmark.
- An `LD_PRELOAD` shim to replay traces captured from real programs.

## What this project teaches

- How `malloc` actually works: headers, alignment, splitting and coalescing.
- Pointer arithmetic and data structures that live inside the memory they manage.
- Measuring trade-offs instead of asserting them: speed vs. fragmentation is visible in the table.
- Testing low-level code: invariant checkers, shadow-model stress tests, sanitizers and mutation testing.

## License

MIT — see [LICENSE](LICENSE).
