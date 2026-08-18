# Benchmark results

Machine: Apple M3 Pro (6 performance + 5 efficiency cores), macOS 15, Apple clang 17,
`-O2 -DNDEBUG`. Baseline is the system allocator, **macOS libmalloc**.

All timings are **wall clock, median of 15 repetitions**, one discarded warm-up pass,
with the two allocators alternating which one runs first. `speedup > 1` means this
allocator is faster than `malloc`.

Reproduce any row with the command shown; every knob is a flag.

```bash
./build/bench --threads 4 --rounds 10 --ntimes 10000 --reps 15 --size ramp --pattern bulk
```

## A. Thread-count sweep — `--size ramp --pattern bulk`

| threads | ours (ms) | malloc (ms) | speedup |
| --- | --- | --- | --- |
| 1 | 5.08 | 19.37 | 3.82x |
| 2 | 5.19 | 20.43 | 3.93x |
| 4 | 6.85 | 31.84 | 4.65x |
| 8 | 9.87 | 49.05 | 4.97x |

These are single runs. Repeated five times each (section F), the 1-thread and
8-thread ranges **overlap** — 3.14-4.60x against 4.01-5.29x — so the apparent
upward trend here is not resolvable. What survives is that **one thread already
shows the full effect**, where there is no contention to remove.

## B. Allocation pattern — `--size ramp --threads 4`

Single runs; see section F for spread.

| pattern | ours (ms) | malloc (ms) | speedup |
| --- | --- | --- | --- |
| `bulk` — allocate 10 000, then free all | 4.46 | 19.70 | 4.42x |
| `interleaved` — bounded 64-object live set | 3.35 | 2.57 | **0.77x** |
| `cross` — thread N frees thread N+1's memory | 6.68 | 16.73 | 2.50x |

## C. Size distribution x pattern — `--threads 4`

Single runs; see section F for spread. The three `interleaved` rows are the ones
that matter, and they are re-measured there.

| size | pattern | ours (ms) | malloc (ms) | speedup |
| --- | --- | --- | --- | --- |
| fixed 16 B | bulk | 6.83 | 7.50 | 1.10x |
| fixed 16 B | interleaved | 0.72 | 1.99 | 2.78x |
| ramp 1 B–8 KB | bulk | 6.17 | 29.83 | 4.83x |
| ramp 1 B–8 KB | interleaved | 4.47 | 3.48 | **0.78x** |
| random 1 B–8 KB | bulk | 7.11 | 30.26 | 4.25x |
| random 1 B–8 KB | interleaved | 1.46 | 5.18 | 3.54x |
| large >256 KB | bulk | 1.54 | 2.19 | 1.42x |

`ramp` walks sizes in order (`(16+i) % 8192 + 1`); `random` draws from the same range
with a real generator. They diverge sharply under `interleaved` — see the README.

## D. Peak RSS — measured one allocator per process (`--only mine` / `--only sys`)

`ru_maxrss` is a process-wide high-water mark that never decreases, so both
allocators in one process is meaningless. These are separate runs.

| size | pattern | ours (KiB) | malloc (KiB) | ratio |
| --- | --- | --- | --- | --- |
| ramp | bulk | 226 256 | 139 152 | 1.63x |
| ramp | interleaved | 131 408 | 11 344 | **11.58x** |
| fixed 16 B | bulk | 4 160 | 4 608 | 0.90x |
| fixed 16 B | interleaved | 2 880 | 2 656 | 1.08x |

## E. Internal counters — why `ramp/interleaved` is the one loss

Build with `-DALLOC_STATS` (off by default, so the measured build carries no
counter overhead) and the allocator reports its own behaviour:

```bash
c++ -std=c++14 -O2 -DNDEBUG -DALLOC_STATS -pthread -o bench_stats \
    main.cpp CentralCache.cpp PageCache.cpp ThreadCache.cpp
./bench_stats --threads 4 --rounds 10 --ntimes 10000 --reps 1 \
    --size ramp --pattern interleaved --only mine
```

4 threads, 400 000 allocations, `--only mine`:

| workload | fast path | refills | flushes | central trips /1k allocs | avg batch |
| --- | --- | --- | --- | --- | --- |
| fixed / interleaved | 99.96% | 196 | 136 | 0.8 | 16.5 |
| random / interleaved | 98.89% | 4 900 | 2 424 | 18.3 | 3.6 |
| ramp / interleaved | 95.14% | 21 401 | 13 336 | **86.8** | 12.7 |
| fixed / bulk | 99.27% | 3 206 | 1 412 | 11.5 | 136.8 |
| random / bulk | 93.30% | 29 476 | 15 018 | 111.2 | 14.8 |
| ramp / bulk | 93.62% | 28 061 | 16 532 | 111.5 | 16.2 |

"refills" are `ThreadCache -> CentralCache` fetches; "flushes" are `ListTooLong`
returns in the other direction. A central trip is either one.

**Fast-path hit rate does not explain anything.** `ramp/interleaved` keeps 95% of
allocations local and loses to malloc; `ramp/bulk` keeps 93.6% and beats it 4.8x.

### Decomposing the cost

Fit `time = a x allocations + b x central_trips` using only `fixed/interleaved`
and `ramp/interleaved`, from medians of five independent measurements (0.88 / 1.85 / 3.41 ms):

- **a ~ 2 ns** per fast-path allocation
- **b ~ 70 ns** per CentralCache round trip (~30x a local pop)

Held out the row it was not fitted on:

| workload | predicted | measured median | measured range | error |
| --- | --- | --- | --- | --- |
| random / interleaved | 1.39 ms | 1.85 ms | 1.62-3.34 | **-25%** |

So central trips are the dominant term but not the only one, and this is an
order-of-magnitude decomposition rather than a predictive model. Fitting the same
two points from *single* runs gave a = 1.71 ns, b = 109 ns and a 1.5% error on
the held-out row -- that agreement was a favourable sample and did not reproduce.

The model does not extend to `bulk` at all (it underpredicts `fixed/bulk` by 83%),
because 10 000 simultaneously live objects add cache-miss and page-fault costs
that a per-call model cannot see.

## F. Run-to-run spread

Independent measurements, each already a median of 15 reps:

| workload | n | range | median |
| --- | --- | --- | --- |
| ramp/bulk, 1 thread | 5 | 3.14x - 4.60x | 3.87x |
| ramp/bulk, 8 threads | 5 | 4.01x - 5.29x | 4.43x |
| fixed/interleaved | 5 | 2.06x - 2.71x | 2.60x |
| random/interleaved | 5 | 3.23x - 3.89x | 3.75x |
| ramp/interleaved | 11 | 0.50x - 0.82x | ~0.75x |

Two consequences. The 1-thread and 8-thread ranges **overlap**, so no scaling
trend is resolvable here. And no interleaved range crosses 1.0, so the
win/win/lose split by size distribution is solid even though the magnitudes are
not stable to better than 20-30%. Any single figure in the tables above should be
read as +/- 25%.

## G. Per-call latency

Everything above is throughput: how long 400 000 operations take. This measures
the distribution of individual calls, which is a different question — a run-to-run
standard deviation cannot answer it, because 400 000 operations average the
spikes away.

```bash
./bench --latency --threads 4 --rounds 20 --ntimes 10000 --lat-stride 16 \
        --size ramp --pattern interleaved
```

**Method and its limits.** `steady_clock::now()` costs **16–26 ns** on this
machine, measured at startup — roughly 10x the ~2 ns fast path, so timing every
call would mostly measure the clock. Instead one call in ~16 is timed, with the
gap **randomised** so it cannot alias with periodic slow paths (a refill every
~16 allocations would otherwise be systematically over- or under-sampled). One
clock cost is subtracted from each sample and the result clamped at zero.

Two consequences:

- **p50 and p90 are not resolvable.** Both allocators sit at the measurement
  floor. Where the table shows `0` for this allocator and ~20 ns for malloc, that
  means malloc's median call is about one timer-tick slower — directionally
  consistent with Finding 1, but at the edge of what this can see.
- **`max` is a single sample** and mostly reflects OS noise (page faults,
  preemption) that hits both allocators equally. Read p99 and p99.9; treat `max`
  as an anecdote.

All figures in nanoseconds, ~48 000 samples per cell, 4 threads.

### Bounded live set (`--pattern interleaved`)

| size | allocator | op | p99 | p99.9 | max |
| --- | --- | --- | --- | --- | --- |
| fixed 16 B | ours | alloc | 64 | 106 | 13 689 |
| | malloc | alloc | 23 | 106 | 24 522 |
| random | ours | alloc | **67** | 650 | 62 234 |
| | malloc | alloc | **234** | 704 | 38 193 |
| ramp | ours | alloc | **485** | **9 842** | 99 985 |
| | malloc | alloc | **110** | **610** | 53 526 |

### Bulk (`--pattern bulk`)

| size | allocator | op | p90 | p99 | p99.9 | max |
| --- | --- | --- | --- | --- | --- | --- |
| ramp | ours | alloc | **24** | **649** | 6 933 | 65 316 |
| | malloc | alloc | **482** | **1 483** | 9 532 | 100 607 |
| ramp | ours | free | 24 | **315** | 2 891 | 67 399 |
| | malloc | free | 316 | **1 441** | 16 308 | **592 274** |
| fixed | ours | alloc | 19 | **19** | 8 319 | 50 852 |
| | malloc | alloc | 19 | **311** | 1 002 | 48 185 |

### What it says

**Tail latency tracks throughput; it is not an independent advantage.** Where
this allocator stays on the fast path it also has the better tail; where it falls
into the locked cascade it has the worse one. Same mechanism as Finding 2, seen
from a different angle:

| workload | throughput | p99 |
| --- | --- | --- |
| random / interleaved | ours 3.8x | ours 3.5x better |
| ramp / bulk | ours 4.4x | ours 2.3x better (p90 20x better) |
| fixed / interleaved | ours 2.6x | roughly tied |
| **ramp / interleaved** | **ours 0.8x** | **malloc 4.4x better** |

The p99.9 in that last row is **9 842 ns against 610 ns — 16x worse**. Our slow
path is a cascade (free list empty → CentralCache lock → PageCache lock → `mmap`
syscall), and the ramp drives it constantly. libmalloc's refill is shallower and
bounded.

One number worth pulling out: libmalloc's worst `free` in `ramp/bulk` is
**592 µs**. That is the cost of actually returning memory to the OS — which is
the same behaviour that makes it use 11x less memory in section D. This allocator
never returns memory below 1 MB, so it never pays that spike. **Memory footprint
and tail latency are the same trade-off seen from two sides.**

## H. Warm-up sensitivity

The ramp keeps converging for a long time. Counters per round, `--only mine`,
`ramp/interleaved`, 4 threads:

| rounds | refills / round | spans carved / round | fast path |
| --- | --- | --- | --- |
| 1 | 10 287 | 762 | 87.1% |
| 5 | 3 327 | 173 | 93.1% |
| 20 | 1 362 | 46 | 96.8% |
| 50 | 825 | 20 | 98.0% |

Span carving drops **38x** per round between 1 and 50 rounds: `MaxSize()` grows
one step per refill per size class, so batches widen and refills thin out, and
PageCache accumulates spans it can split instead of calling `mmap`.

### Does that mean the ramp deficit is a cold-start artifact?

Partly. Measured work held fixed at 10 rounds, varying only `--warmup`, **each
data point five separate process launches** (the allocator is a global singleton,
so state otherwise accumulates across repetitions and confounds this):

| `--warmup` | speedup, 5 fresh processes | median |
| --- | --- | --- |
| 0 | 0.24 0.46 0.23 0.32 0.32 | **0.32x** |
| 1 | 0.39 0.83 1.01 0.58 0.57 | 0.58x |
| 5 | 1.06 0.79 1.24 1.01 1.27 | 1.06x |
| 20 | 0.79 0.83 0.78 0.92 0.61 | 0.79x |
| 50 | 0.73 0.67 0.63 0.75 0.88 | 0.73x |
| 100 | 0.72 1.12 0.71 0.85 1.57 | 0.85x |
| 300 | 0.36 0.61 0.70 0.76 0.66 | 0.66x |

- **Truly cold is much worse**: `--warmup 0` spans 0.23–0.46 and does not overlap
  any warmed configuration. Starting from nothing, this allocator is ~3x slower
  than libmalloc on this workload.
- **Beyond one warm-up round the effect is inside the noise.** Every row from 5
  upward overlaps every other. There is no clean convergence past 1.0.

An earlier version of this measurement appeared to show the speedup climbing to
1.42x at 100 rounds. That did not reproduce in fresh processes — it came from
allocator state accumulating across `--reps`, which makes later repetitions
warmer than earlier ones.

**All figures elsewhere in this file use `--warmup 1`.**

### The tail gap is *not* a warm-up artifact

`ramp/interleaved`, alloc, ~48 000 samples:

| `--warmup` | ours p99 | ours p99.9 | malloc p99 | malloc p99.9 |
| --- | --- | --- | --- | --- |
| 0 | 438 | 9 541 | 230 | 938 |
| 1 | 444 | 12 271 | 193 | 694 |
| 20 | 527 | 5 706 | 193 | 735 |
| 100 | 359 | 3 189 | 109 | 525 |

Warming helps p99.9 (12 271 → 3 189) but **malloc has the better p99 at every
warm-up level tested**, and the gap is still 3.3x at `--warmup 100`. Once warm,
~5% of allocations still reach CentralCache, and those calls are what p99 is made
of. That is structural, not transient.

## I. Why `bulk` wins and `ramp/interleaved` loses — two separate mechanisms

`bulk` and `interleaved` differ in **live set size**: 10 000 objects held at once
versus 64. Holding total operations and size distribution fixed (`--size random`,
so request size does not depend on the loop index) and varying only the live set:

| live set | pattern | ours (ms) | malloc (ms) | speedup | page faults /1k ops, ours | malloc |
| --- | --- | --- | --- | --- | --- | --- |
| 100 | bulk | 2.57 | 5.40 | 2.10x | 1.4 | 0.2 |
| 1 000 | bulk | 6.26 | 14.66 | 2.34x | 4.3 | 1.3 |
| 5 000 | bulk | 7.35 | 33.79 | 4.60x | 3.9 | **51.2** |
| 10 000 | bulk | 7.19 | 32.46 | 4.52x | 4.1 | **57.3** |
| 10 000 | interleaved | 1.68 | 5.47 | 3.26x | 1.4 | 0.1 |

**Mechanism A — page faults, which is what `bulk` actually measures.** Between a
1 000- and a 5 000-object live set, libmalloc's soft page faults jump **39x**
(1.3 → 51.2 per 1 000 operations) and the speedup jumps with them. libmalloc
returns freed memory to the OS; a workload that allocates a large set, frees all
of it, and repeats therefore re-faults that memory every round. This allocator
never returns anything below 1 MB, so it faults its pages in once and reuses them
— its fault rate stays flat at ~4 regardless of live set. Note that our peak RSS
is *higher* (221 MB vs 136 MB) while our fault count is *lower*: that combination
is the signature of hoarding.

This has nothing to do with the ramp. `random/bulk` wins by the same 4.5x.

**Mechanism B — CentralCache round trips, which is what `ramp` measures.** With a
64-object live set neither allocator returns anything (both at ~0 faults), so the
comparison collapses to per-call cost. There, `random/interleaved` still wins
**3.26x** — the fast path is genuinely cheaper. Only `ramp/interleaved` loses, at
0.8x, and section E shows why: the ramp forces 86.8 central trips per 1 000
allocations against random's 18.3.

So the two results are not "best case versus worst case" for one design. They are
**two independent effects that happen to point in opposite directions**:

| | what drives it | ramp involved? | pattern involved? |
| --- | --- | --- | --- |
| `bulk` wins 4.5x | memory-return policy → page faults | no | yes (live set) |
| `ramp/interleaved` loses 0.8x | size-class locality → central trips | yes | no |

**A caveat on an earlier version of this experiment.** Sweeping the live set with
`--size ramp` is confounded: request size is `(16+i) % 8192 + 1` where `i` is the
index *within a round*, so shrinking `ntimes` also shortens the ramp and narrows
the number of size classes touched. The table above uses `--size random`, whose
size does not depend on `i`, which is why it is the one to read.

## J. The baseline per-call advantage, and what it is not

With page faults near zero and central traffic minimal, what is left is the cost
of the fast path itself. The cleanest configuration for that is
`fixed/interleaved`: **99.96% fast path**, 0.8 central trips per 1 000
allocations, no page faults. It runs at roughly **2x** (1.9-2.7x across runs).

`random/bulk` at a 100-object live set gives 2.10x but is not as clean -- 99.25%
fast path and 11.2 central trips per 1 000, 14x more shared-layer traffic.

### It is not inlining

`ConcurrentAlloc` is a `static` function in a header, so at -O2 the entire fast
path can inline into the caller. `malloc` is a cross-library call that cannot.
That seemed likely to account for a chunk of the advantage. Build with
`-DALLOC_NOINLINE` to force `__attribute__((noinline))` on both entry points and
it does not:

| build | speedup, 5 fresh processes | median |
| --- | --- | --- |
| default (inlinable) | 1.99 2.15 2.39 2.10 2.06 | 2.10x |
| `-DALLOC_NOINLINE` | 1.89 1.96 2.13 2.07 2.15 | 2.07x |

A 1.5% difference, well inside the noise. Call overhead is not where the 2x comes
from.

### What is left

Two candidates remain, and this harness cannot separate them without profiling
libmalloc's internals:

- **No atomics.** This allocator's free list is genuinely thread-private, so a
  pop is a plain load, load, store. macOS's nano zone keeps per-CPU magazines,
  which a migrating thread can race, so its fast path needs an atomic
  compare-exchange -- considerably more than a plain store on arm64.
- **A narrower contract.** `malloc` has to handle `malloc(0)`, guarantee 16-byte
  alignment, dispatch through `malloc_zone_t`, support `malloc_size`,
  interposition and debug hooks. `ConcurrentAlloc` does none of that, which is
  also why it is not a drop-in replacement.

**Keep the absolute scale in view.** 2x on an operation that costs about 2 ns is
about 1 ns per call. It only shows up in allocation-dominated loops; a program
that does real work between allocations will not notice it.

## Notes on method

- **Wall clock, not summed thread time.** An earlier harness accumulated each
  thread's elapsed time into one atomic and reported the sum as "runtime". Both
  figures are printed now; only wall clock is used for speedups.
- **`steady_clock`, not `clock()`.** At these durations `clock()`'s resolution was
  comparable to the measurement itself.
- **Warm-up discarded.** The first pass pays for lazily created thread caches,
  page-map nodes, and first-touch page faults. Including it put the standard
  deviation at ~94% of the median; excluding it brings it to ~20%.
- **Order alternated.** Whichever allocator always runs second inherits any drift
  over the life of the process.
- `malloc`'s standard deviation stays high (often 30–50% of its median) even after
  these fixes. That is a statement about **whole-run totals**, not about
  individual calls — see section G, which measures per-call latency directly and
  does not support reading a tail-latency claim into it.
