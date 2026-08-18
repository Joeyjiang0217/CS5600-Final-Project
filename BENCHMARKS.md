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

## K. Decomposing the one loss: it is half us, half libmalloc

`random/interleaved` and `ramp/interleaved` differ only in size distribution, and
the ratio swings 4.8x between them. Medians of five independent measurements:

| workload | ours (ms) | malloc (ms) | speedup |
| --- | --- | --- | --- |
| random / interleaved | 2.39 (1.75-3.26) | 7.95 (6.13-8.33) | 3.33x |
| ramp / interleaved | 5.15 (3.76-7.03) | 3.57 (2.67-4.01) | **0.69x** |

Going from random to ramp: **this allocator gets 2.2x slower and libmalloc gets
2.2x faster.** 2.2 x 2.2 = 4.8, the whole swing. Neither side dominates, so the
loss cannot be explained by looking at this allocator alone.

### Our 2.2x, mostly accounted for

Two things change on our side, and they are collinear across these workloads:

| | random | ramp | factor |
| --- | --- | --- | --- |
| central trips / 1k allocs | 18.3 | 86.8 | 4.7x |
| peak RSS | 53 MB | 134 MB | 2.5x |
| our time | 2.39 ms | 5.15 ms | 2.2x |

At the ~70 ns per trip from section E, the extra 68.5 trips per 1 000 over
400 000 allocations comes to ~1.9 ms against a measured increase of 2.76 ms — so
central trips account for roughly **70%** of it. The remainder is plausibly the
2.5x footprint: 134 MB of spans and page-map leaves does not sit in cache, and the
page-map walk on every `free` pays for that.

Note what this rules out: `random/interleaved` *also* has a big footprint (7x
libmalloc's) and *also* takes central trips, and we win it 3.33x. Neither factor
is disqualifying on its own; the ramp is where both are large at once.

### libmalloc's 2.2x, and what it is not

Measured, same workloads: libmalloc's **peak RSS is unchanged** between random and
ramp (7 808 vs 7 696 KiB) and its **page faults are ~0 in both** (0.1 per 1 000).
So its improvement on the ramp is not a memory effect at all — same footprint,
same faults, 2.2x faster.

What remains is the access pattern into its own per-size-class structures: the
ramp walks classes in order, random jumps between ~130 of them. That would be a
cache and prefetch story. **I have not verified it** and cannot without profiling
libmalloc's internals, so it is recorded as the leading hypothesis, not a result.
One piece of weak support: in `bulk` libmalloc shows no random-vs-ramp preference
(30.26 vs 29.83 ms), which is consistent with a locality effect that exists but is
swamped there by the 43-57 faults per 1 000 that `bulk` imposes on it.

### A mechanism I previously overstated

I earlier described the ramp as making allocation and deallocation land on
*different* size classes, so nothing recycles locally. That is right in the
8-byte-alignment region (a class spans 8 consecutive iterations, so a 64-object
window is always ~8 classes away from itself) but **not uniform**: from 1 KB to
8 KB the alignment is 128 bytes, so one class covers 128 consecutive iterations
and a 64-object window frequently allocates and frees within the same class. Since
most of the 1-8192 range lies in that region, the simple story does not carry the
result. The measured trip count (86.8 vs 18.3 per 1 000) is solid; the precise
per-class accounting behind it is not something this harness pins down.

## L. Why the ramp drives central traffic — derived, then tested

### The derivation

Two numbers set everything: the **live-set window** `W` (64 by default) and the
**dwell length** — how many consecutive iterations stay inside one size class,
which is fixed by the alignment of that size region:

| size region | alignment | dwell |
| --- | --- | --- |
| 1-128 B | 8 B | 8 |
| 129-1024 B | 16 B | 16 |
| **1025-8192 B** | **128 B** | **128** (most of the range) |

Under `interleaved`, iteration `i` allocates `size(i)` and frees the object from
`i - W`. Follow a single class C with dwell 128, allocated from during iterations
`[0, 128)`. Frees arrive into C during `[W, 128 + W)` — the same total count,
**shifted later by W**. That splits C's lifetime into three phases:

```
iteration   0        64       128      192
            |--------|--------|--------|
alloc from C ####################          128 allocations, i in [0,128)
free into C           ####################  128 frees,      i in [64,192)

            \--only--/\--both-/\--only--/
             drains    recycles  fills
```

1. **Drains** for `W` iterations: allocations with no frees arriving yet, so the
   list empties and must **fetch from CentralCache**.
2. **Recycles** for `dwell - W` iterations: frees feed allocations locally. Fast
   path.
3. **Fills** for `W` iterations: frees arrive after allocation has moved to the
   next class, the list grows past `MaxSize()`, and `ListTooLong` **flushes back
   to CentralCache**.

So the forced traffic per class is ~`W` objects out and ~`W` objects back, and the
fraction of a class's work that is forced is **`W / dwell`**. With `W = 64` and
dwell 128 that is half. At `MaxSize ~ 15` that is ~4-5 fetches plus ~4-5 flushes
per class, ~9 trips, over ~130 classes per round: **~1 200 trips per thread-round**
against ~870 measured. Right order.

Under `random` the same class has no phases: live objects per class average
`64 / 130 = 0.5`, a free lands in a class that will be allocated from again in
~130 iterations, and the list never reaches `MaxSize` in between — so the object
is reused locally.

**The mechanism is a timing offset inside one class, not allocation and
deallocation landing in different classes.** (That earlier framing was wrong; see
section K.)

### The test

If the driver is `W / dwell`, then sweeping `W` should move central traffic
monotonically and saturate once `W` exceeds the dwell. `--window N`,
`ramp/interleaved`, 4 threads:

| `--window` | central trips /1k | fast path | ours (ms) | malloc (ms) | speedup |
| --- | --- | --- | --- | --- | --- |
| 4 | **18.2** | 98.88% | 1.66 | 2.63 | **1.58x** |
| 16 | 62.5 | 96.63% | 2.08 | 2.28 | 1.10x |
| 64 | 87.6 | 95.10% | 3.17 | 2.53 | **0.80x** |
| 256 | 110.4 | 93.72% | 3.88 | 3.36 | 0.87x |
| 1 024 | 110.2 | 93.73% | 4.06 | 4.55 | 1.12x |

Monotonic in `W`, and **saturating at ~110 trips per thousand — which is what
`bulk` measures (111.5)**. That is the derivation's other prediction: once
`W` exceeds the dwell, allocation and deallocation classes are fully disjoint,
every object must round-trip, and `interleaved` becomes indistinguishable from
`bulk` as far as the allocator is concerned.

At `W = 4` traffic falls to 18.2 — the same as `random` — and this allocator wins
1.58x. **The ramp is not intrinsically bad for it; `W` comparable to the dwell
is.** The losing band is roughly `16 <= W <= 256`, and it closes again at large
`W` only because libmalloc also degrades there (2.53 -> 4.55 ms).

## M. The W/dwell model was wrong. The driver is pile size vs MaxSize

Section L proposed that forced central traffic scales with `W / dwell`, and
predicted that `dwell = 1` -- every allocation in a different size class -- would
be the worst case. **It is very nearly the best case.**

`--size classstep` walks one size-class representative per iteration (8, 16, ...,
128, 144, ..., 1024, 1152, ..., 8192, then repeats), so `dwell = 1`.
`interleaved`, `W = 64`, 4 threads:

| size mode | dwell | central trips /1k | fast path | ours (ms) | speedup |
| --- | --- | --- | --- | --- | --- |
| fixed 16 B | infinite | 0.8 | 99.96% | 1.24 | 2.24x |
| random | n/a | 18.6 | 98.87% | 1.93 | 3.05x |
| ramp | 128 | **87.4** | 95.11% | 4.27 | **0.69x** |
| **classstep** | **1** | **2.6** | 99.77% | 1.11 | **4.05x** |

### The corrected mechanism

What matters is not the ratio `W / dwell`. It is **how many objects of one size
class sit in that class's free list while allocation has moved on -- the "pile" --
compared to `MaxSize()`**:

- **pile <= MaxSize** — the objects wait in the thread-local list and are handed
  back out on the class's next visit. They never leave the thread.
- **pile >> MaxSize** — `ListTooLong` flushes them to CentralCache, and the next
  visit has to fetch them back. Two central trips per `MaxSize` objects.

Pile size per class:

| size mode | pile | vs MaxSize (~19) | trips /1k |
| --- | --- | --- | --- |
| fixed | ~0 (alloc and free are always the same class) | under | 0.8 |
| classstep | ~max(1, W / 128 classes) = 1 | under | 2.6 |
| random | ~W / 130 classes ~ 0.5 | under | 18.6 |
| ramp | ~min(W, dwell) = 64 | **3x over** | 87.4 |

### The test that separates the two models

`W / dwell` predicts `classstep` should get worse with `W` just like `ramp` does.
Pile-vs-MaxSize predicts `classstep` is **flat** in `W` until `W` approaches the
number of classes, because its pile is 1 regardless.

| `--window` | classstep trips /1k | ramp trips /1k |
| --- | --- | --- |
| 4 | 2.6 | 18.2 |
| 16 | 2.6 | 62.4 |
| 64 | **2.6** | 87.1 |
| 256 | 9.0 | 110.3 |
| 1 024 | 31.0 | 110.2 |

Flat at 2.6 across a 16x range of `W`, then rising once `W` exceeds the 128-class
cycle and each class starts holding `W / 128` objects. `ramp` meanwhile climbs
monotonically and saturates at its dwell. The pile model fits all ten points;
`W / dwell` fits neither column.

### What this implies

The loss is not about ascending sizes, and not about the live set. It is about
**`MaxSize()` being too small for the pile the workload creates** — which points
at the same design flaw noted in section E, `MaxSize()` serving as both the refill
batch and the flush threshold. Raising the flush threshold above the pile, or
decoupling the two, is the fix the model predicts. Untested so far.

## N. Testing the fix: multiplicative growth works, and is not worth it

Section M predicted that `MaxSize()`'s additive growth is the root cause, and that
making it multiplicative would collapse the central traffic. Build with
`-DMAXSIZE_GROWTH_MULT` to double instead of incrementing, clamped to
`NumMoveSize(size)`. ASan-clean across all 12 size x pattern combinations.

### The mechanism is confirmed

`interleaved`, W = 64, 4 threads, `--only mine`:

| size | growth | central trips /1k | avg MaxSize | avg batch | fast path |
| --- | --- | --- | --- | --- | --- |
| ramp | additive | 87.2 | 15.5 | 12.7 | 95.11% |
| ramp | **multiplicative** | **30.5** | 24.9 | 12.3 | 97.67% |
| random | additive | 18.5 | 3.8 | 3.7 | 98.88% |
| random | multiplicative | **6.2** | 2.0 | 2.0 | 99.44% |
| fixed | additive | 0.8 | 16.5 | 16.5 | 99.96% |
| fixed | multiplicative | **0.1** | 18.1 | 17.1 | 99.99% |

Traffic falls 2.9x on the ramp and improves on every workload. Note `avg batch`
barely moves (12.7 -> 12.3): the win comes from the **flush threshold**, role 2,
not the refill batch, role 1 — exactly what the pile model says should happen.

### But it does not fix the loss, and it costs memory

Speedups, medians of five independent processes:

| workload | additive | multiplicative |
| --- | --- | --- |
| **ramp/interleaved** | 0.71x | **0.92x** |
| random/interleaved | 3.11x | 3.22x |
| fixed/interleaved | 2.16x | 2.35x |
| ramp/bulk | 4.44x | 4.08x |

Peak RSS, `--only mine`, separate processes:

| workload | additive | multiplicative | change |
| --- | --- | --- | --- |
| ramp/interleaved | 132 MB | **210 MB** | **+59%** |
| ramp/bulk | 223 MB | **308 MB** | +38% |
| random/interleaved | 54 MB | 60 MB | +10% |

So on the workload it was meant to fix: **30% faster, 59% more memory, and still
slower than libmalloc** — which uses 8 MB for the same work. 132 MB against
libmalloc was 16x; 210 MB is 26x.

### What the experiment actually established

Two things, one of which reverses an earlier judgement.

**The two cost terms are in tension.** Section K attributed ~70% of our ramp
degradation to central trips and the rest to footprint. Cutting trips 2.9x bought
only 30% wall-clock, because the same change grew the footprint 59% and the
footprint term pushed back. Additive and multiplicative are two points on a
trade-off curve, not a bug and its fix.

**The decoupling is necessary after all.** Section M downgraded "split the three
roles" in favour of "just grow faster". That was wrong: growing faster raises the
flush threshold, the refill batch, *and* the retained memory together, because one
variable controls all three. Getting the traffic reduction without the footprint
needs role 2 (cache capacity) raised while role 1 (batch) stays modest, plus the
scavenging that defect C describes so idle capacity is given back. That is what
production tcmalloc does, and this experiment is why.

The switch is left in the code as `-DMAXSIZE_GROWTH_MULT`; the default build keeps
additive growth, since on these workloads it is the better memory/speed point.

## O. Decoupling the three roles: a negative result

Section N concluded that the roles must be separated. So separate them:
`-DDECOUPLED_LISTS` gives `FreeList` an independent `_capacity` used as the flush
threshold, doubling on each flush (a flush means the pile did not fit) and capped
at `2 x NumMoveSize`, plus hysteresis -- flush down to `capacity / 2` rather than
emptying the list. `_maxSize` keeps its original additive growth and remains the
transfer batch only. Combined with `-DMAXSIZE_GROWTH_MULT` as a fourth variant.
All four ASan-clean across 12 size x pattern combinations.

**It is worse on both axes.**

| workload | metric | additive | multiplicative | **decoupled** | both |
| --- | --- | --- | --- | --- | --- |
| ramp/interleaved | speedup | 0.70x | **0.94x** | **0.62x** | 0.63x |
| ramp/interleaved | peak RSS | 142 MB | 202 MB | **289 MB** | 299 MB |
| random/interleaved | speedup | 3.12x | 3.40x | 3.35x | 3.24x |
| random/interleaved | peak RSS | 53 MB | 58 MB | 55 MB | 56 MB |
| ramp/bulk | speedup | 4.26x | 4.29x | **3.37x** | 3.30x |
| ramp/bulk | peak RSS | 207 MB | 293 MB | **420 MB** | 429 MB |

The one-line multiplicative change stays the best of the four. The considered
redesign is slower than doing nothing and holds twice the memory.

### Three reasons, all visible in the counters

Central traffic on `ramp/interleaved`, per 1 000 allocations:

| variant | refills | flushes | total | avg batch |
| --- | --- | --- | --- | --- |
| additive | 53.9 | 33.6 | 87.5 | 12.9 |
| multiplicative | 27.1 | **5.3** | **32.4** | 12.2 |
| decoupled | 31.3 | 14.5 | 45.9 | **6.0** |
| both | **22.8** | 14.6 | 37.4 | 11.8 |

**1. The hysteresis backfires.** Flushing down to `capacity / 2` leaves the list
half full, so the next pile has only half the headroom and trips the threshold
again. Emptying the list -- which is what the original does -- leaves *more* room
for the next pile. The drain phase wants objects retained; the fill phase wants
headroom. One flush policy cannot serve both, and I picked the wrong one for the
phase that dominates.

**2. Changing role 2 starved role 1's growth signal.** `_maxSize` grows only on a
refill. Hysteresis makes the list non-empty more often, so refills get rarer, so
`_maxSize` grows more slowly -- average batch falls from 12.9 to **6.0**. Reducing
trips reduced the signal the batch size uses to learn, which then increased trips.
The roles are coupled through the *control loop*, not only through the shared
variable, which is not something splitting the variable fixes.

**3. Capacity ratchets up and never comes down.** `_capacity` only grows. It
climbs to its `2 x NumMoveSize` cap and stays, so every size class the workload
ever touches holds that much forever: 289 MB against additive's 142 MB, for less
speed. This is defect C from section M, which I listed and then did not implement
a fix for -- and the memory result is exactly what that omission predicts.

### What this actually establishes

**Decoupling without scavenging is strictly worse than not decoupling.** Raising a
cache's capacity is only safe if something also lowers it; otherwise capacity
becomes a high-water mark on memory for the life of the process. Production
tcmalloc separates batch from capacity *and* runs periodic scavenging plus
per-CPU caches to bound the total, and this result is a concrete demonstration of
why the second half is not optional.

The default build keeps additive growth. All variants are compile-time switches
and none is shipped as default.

## P. A latent bug this surfaced

`FreeList::PopRange` asserted `n >= _size` -- backwards; you cannot pop more than
the list holds. It never fired because the only caller popped exactly `MaxSize()`
at the moment `Size()` had reached it, so `n == _size` satisfied both directions.
Adding a low-water mark made `n < _size` and it fired immediately, in all 12
workload combinations.

Fixed to `assert(n > 0); assert(n <= _size);`. Note what this means for the
original code: under `-DNDEBUG` the assert is compiled out, so a future change to
the flush amount would have silently corrupted `_size` in release builds while
appearing fine in debug. The loop index was also `int` against a `size_t` count.

## Q. Against real tcmalloc: the ramp weakness is architectural

gperftools tcmalloc 2.18.1, the design this project is modelled on, and the one
that has the two mechanisms sections N and O identified as missing: a **transfer
cache** between thread caches and the central list, and **periodic scavenging**.

### Method, and a trap worth recording

Linking `-ltcmalloc` does not just add `tc_malloc`. On macOS gperftools takes over
the **default malloc zone**, so plain `malloc` routes to tcmalloc as well.
Verified directly: allocating 80 MB through `malloc` grew tcmalloc's own
`generic.current_allocated_bytes` by 78 MB. A first attempt at a three-way
comparison inside one process was therefore measuring tcmalloc twice and calling
one of the columns libmalloc.

The correct shape is two binaries:

```bash
c++ ... -o bench     main.cpp ...              # malloc column = macOS libmalloc
c++ ... -ltcmalloc -o bench_tc main.cpp ...    # malloc column = gperftools tcmalloc
```

`ConcurrentAlloc` goes to `mmap` directly and is unaffected by which malloc is
installed, so it is the fixed point that makes the two runs comparable — and it
measures the same in both (4.33 / 4.20 ms, 1.96 / 1.90 ms, 4.55 / 4.51 ms).

### Results — 4 threads, medians of five runs

| workload | ours | libmalloc | real tcmalloc | ours vs libmalloc | ours vs tcmalloc |
| --- | --- | --- | --- | --- | --- |
| ramp/interleaved | 4.27 ms | **3.09 ms** | 3.60 ms | **0.71x** | 0.86x |
| random/interleaved | 1.93 ms | 6.11 ms | 2.36 ms | 3.12x | 1.24x |
| ramp/bulk | 4.53 ms | 19.21 ms | 13.21 ms | 4.22x | 2.93x |

Peak RSS and page faults, one allocator per process:

| workload | allocator | peak RSS | faults /1k |
| --- | --- | --- | --- |
| ramp/interleaved | ours | 125 MB | 3.1 |
| | libmalloc | **8 MB** | 0.2 |
| | real tcmalloc | **32 MB** | 0.6 |
| ramp/bulk | ours | 212 MB | 1.4 |
| | libmalloc | 134 MB | **40.4** |
| | real tcmalloc | 154 MB | **1.3** |

### Does real tcmalloc fix ramp/interleaved? No.

**libmalloc 3.09 ms, real tcmalloc 3.60 ms, ours 4.27 ms.** Real tcmalloc — with
the transfer cache and the scavenging — closes about 57% of our gap and **still
loses to libmalloc by 17%**.

So the weakness this project spent sections E through O characterising is **a
property of the tcmalloc architecture on this workload, not a defect introduced by
simplifying it**. Per-size-class thread caching has a shape of workload it handles
worse than libmalloc's magazines, and adding the missing machinery narrows the gap
without closing it.

### What scavenging actually buys: 4x less memory

On `ramp/interleaved` real tcmalloc holds **32 MB against our 125 MB** — 3.9x
less — while being slightly *faster*. That is the concrete value of the mechanism
section O showed we cannot omit: it converts our large memory problem into a small
one at no speed cost. Section O's conclusion, that raising capacity without a way
to lower it is strictly worse, is what this measurement looks like from the other
side.

### One result the page-fault story does not explain

On `ramp/bulk` real tcmalloc takes **1.3 faults per thousand, essentially ours
(1.4), not libmalloc's 40.4** — so it is not paying the re-fault cost that section I
attributes libmalloc's slowness to. Yet it is still 2.9x slower than us. So there
is a second reason tcmalloc-family allocators can be slow on a large cyclic live
set, and page faults are not it. Not characterised here.

### Limitations

- **gperftools on Darwin goes through the malloc zone**, adding a dispatch layer
  on every call that a Linux build would not have. Its absolute numbers are
  penalised by that, so "2.9x faster than tcmalloc" should be read with caution.
- **This is not modern google/tcmalloc.** That one needs Bazel and is officially
  Linux-only; its per-CPU caches rely on restartable sequences, which do not exist
  on Darwin, so even a successful build would fall back to per-thread mode and
  would not test the mechanism this README keeps citing.
- Our allocator still has the narrower contract (no `malloc(0)`, no alignment
  guarantee, no `malloc_size`), so it is not a like-for-like replacement for either
  opponent.

## R. On Linux against glibc ptmalloc — the baseline dominates everything

Ubuntu 22.04.5, Linux 5.15 aarch64, 4 cores, **glibc 2.35 (ptmalloc)**, g++ 11.4,
`-O2 -DNDEBUG`. ASan-clean across all 12 size x pattern combinations on this
platform too. Built first try from the same sources.

This is the baseline the original write-up's Related Work discussed at length and
never measured.

### Results, medians of five runs, 4 threads

| workload | vs glibc ptmalloc | range |
| --- | --- | --- |
| classstep/interleaved | **9.27x** | 9.16-9.41 |
| ramp/bulk | 5.40x | 4.93-5.53 |
| random/interleaved | 3.31x | 0.92-3.50 |
| random/bulk | 2.65x | 2.03-3.89 |
| ramp/interleaved | 1.08x | 0.37-1.14 |
| **fixed/interleaved** | **0.59x** | 0.50-0.78 |
| **fixed/bulk** | **0.31x** | 0.27-0.48 |

Thread sweep on `ramp/bulk`: **6.13x at 1 thread, 6.06x at 2, 5.21x at 4** — the
advantage *shrinks* with concurrency here, which is a stronger version of Finding 1
than the macOS data gave. Machine noise is low (repeating one config gives
3.56 3.67 3.58 3.60 3.58, +/-1.5%); the wide ranges above are ptmalloc's own
variance, not the VM's.

### Two results reverse sign relative to macOS

| workload | vs macOS libmalloc | vs glibc ptmalloc |
| --- | --- | --- |
| fixed/bulk | 1.10x | **0.31x** |
| fixed/interleaved | 2.60x | **0.59x** |
| ramp/interleaved | **0.71x** | 1.08x |

`fixed/bulk` is a real loss, not noise: ours 5.3-5.9 ms against ptmalloc's
1.55-2.54 ms. glibc's tcache and fastbins are very good at a single small size
class, which is exactly the case where our per-class list has the least to offer.
And `ramp/interleaved` — the workload sections E through Q are built around — is
**not a loss against ptmalloc at all**.

### Page faults: the mechanism is confirmed and amplified

| workload | allocator | peak RSS | faults /1k |
| --- | --- | --- | --- |
| ramp/interleaved | ours | 122 MB | 12.2 |
| | ptmalloc | **6 MB** | 12.9 |
| ramp/bulk | ours | 187 MB | 7.2 |
| | ptmalloc | 106 MB | **642.5** |

**642.5 faults per thousand operations** — 16x macOS libmalloc's 40.4 on the same
workload. glibc trims and returns memory harder than libmalloc does, re-faults it
every round, and that is the whole 5-6x. Section I's mechanism holds across both
platforms and is the single most portable finding in this file.

The memory side is also consistent: 122 MB against 6 MB on `ramp/interleaved`, a
20x ratio, matching macOS's 16x. Our hoarding is platform-independent.

### The conclusion that supersedes the others

Same code, same workloads, three baselines:

| workload | Windows CRT (Win32) | macOS libmalloc | glibc ptmalloc |
| --- | --- | --- | --- |
| fixed/bulk | 5.3x | 1.10x | **0.31x** |
| ramp/bulk | 22.8x | 4.22x | 5.40x |
| fixed/interleaved | — | 2.60x | **0.59x** |
| ramp/interleaved | — | **0.71x** | 1.08x |

**The spread across baselines is larger than the spread across workloads, and it
flips signs.** `fixed/bulk` goes from a 5.3x win to a 3.2x loss purely by changing
what you compare against. (The Windows column here is the original **Win32**
build; section U re-measures it on x64, where the same row reads 1.35x. Section T
carries the authoritative five-baseline table.) Any statement of the form "this allocator is Nx faster
than malloc" is close to meaningless without naming the malloc, and this table is
why the original 22.8x figure was the least informative number in the project.

### Real tcmalloc on Linux: see section S — it changes the conclusion

## S. Real tcmalloc on Linux — this retracts section Q's conclusion

gperftools 2.18.1 built from source into `$HOME` (no `sudo` needed; the release
tarball ships `configure`, and `--enable-minimal` avoids the libunwind dependency)
and injected with `LD_PRELOAD`, so **one binary is used for both baselines and only
the allocator differs**. Verified by reading `/proc/self/maps`.

### A trap in the opposite direction from macOS

Linking `-ltcmalloc_minimal` the ordinary way **does not work on Ubuntu**. `ldd`
showed no tcmalloc in the binary at all: the default `--as-needed` discards a
shared library whose symbols nothing directly references, and our code only calls
`malloc`, which resolves to libc. Numbers from that binary would have been glibc
ptmalloc labelled as tcmalloc.

macOS had the mirror-image problem — linking silently *did* take over `malloc`.
Two platforms, two opposite failure modes, both invisible without checking.
`LD_PRELOAD` plus a `/proc/self/maps` check is the only version of this experiment
I would trust.

### Results — 4 threads, medians of five runs

| workload | ours | glibc ptmalloc | **real tcmalloc** | vs glibc | vs tcmalloc |
| --- | --- | --- | --- | --- | --- |
| ramp/interleaved | 4.33 ms | 7.50 ms | **1.60 ms** | 0.97x | **0.37x** |
| random/interleaved | 1.64 ms | 5.93 ms | **0.91 ms** | 3.51x | **0.55x** |
| fixed/interleaved | 1.30 ms | 0.78 ms | **0.51 ms** | 0.60x | **0.39x** |
| ramp/bulk | 8.04 ms | 40.66 ms | **10.83 ms** | 5.14x | 1.35x |

| workload | allocator | peak RSS | faults /1k |
| --- | --- | --- | --- |
| ramp/interleaved | ours | 107 MB | 13.2 |
| | glibc | **6 MB** | 12.9 |
| | real tcmalloc | 22 MB | **2.4** |
| ramp/bulk | ours | 185 MB | 10.9 |
| | glibc | 106 MB | **642.5** |
| | real tcmalloc | 151 MB | 8.7 |

### Section Q was wrong: real tcmalloc does solve it

On `ramp/interleaved` real tcmalloc runs in **1.60 ms** — **4.7x faster than glibc
ptmalloc and 2.7x faster than this implementation**. It is the fastest allocator
measured on every `interleaved` workload, and it beats glibc on all four.

Section Q, measuring gperftools **on macOS**, found it at 3.60 ms against
libmalloc's 3.09 ms and concluded "the weakness is architectural, real tcmalloc
does not fix it either". That conclusion is retracted. The macOS figure was
crippled by the malloc-zone dispatch layer that section Q listed as a limitation
and then reasoned past. Same source, same version: 3.60 ms through the zone shim,
1.60 ms native.

So the honest answer to "can the tcmalloc design handle this workload" is **yes,
comfortably** — and the ~2.7x between 1.60 ms and 4.33 ms is the measured price of
this project's simplifications, on the workload that exposes them worst.

tcmalloc's page faults on `ramp/interleaved` are **2.4 per thousand against our
13.2 and glibc's 12.9** — the lowest of the three while holding 22 MB to our
107 MB. It is simultaneously more economical with memory *and* touching the OS
less, which is what a working scavenger plus a transfer cache buys.

### Where we still win, and why

`ramp/bulk` is the one row we take (1.35x), and section I explains it: glibc
re-faults at **642.5 faults per thousand**, tcmalloc at 8.7, us at 10.9. Against
glibc that mechanism is worth 5.14x; against tcmalloc, which does not thrash the
OS, it is worth only 1.35x. The page-fault finding survives, but its *size* was
mostly a statement about glibc's trim policy, not about our design being good.

### Caveat on our own numbers

Under `LD_PRELOAD` the harness's own `std::vector` allocations also go through
tcmalloc, so the "ours" column is not perfectly identical across modes (7.74 ms in
the glibc run against 4.33 ms in the tcmalloc run for the same workload). The
opponent-versus-opponent comparison — same binary, same harness, only the
allocator swapped — is the solid one: **glibc 7.50 ms against tcmalloc 1.60 ms.**

## T. The full cross-baseline table

All five workloads against both Linux baselines, one batch, one binary,
`LD_PRELOAD` switching the allocator. 4 threads, medians of five runs.

### Opponent absolute times — the cleanest comparison

Same binary, same harness, only the allocator swapped:

| workload | ours | glibc ptmalloc | real tcmalloc | tcmalloc vs glibc |
| --- | --- | --- | --- | --- |
| fixed/bulk | 5.72 ms | 1.74 ms | **1.46 ms** | 1.2x |
| ramp/bulk | 8.36 ms | 42.65 ms | 10.78 ms | 4.0x |
| fixed/interleaved | 1.23 ms | 0.77 ms | **0.47 ms** | 1.6x |
| ramp/interleaved | 4.56 ms | 5.19 ms | **2.37 ms** | 2.2x |
| classstep/interleaved | 1.04 ms | 9.75 ms | **0.77 ms** | **12.7x** |

**Real tcmalloc beats glibc on all five**, by 1.2x to 12.7x.

### Us, against five baselines

Windows figures are the x64 measurements from section U; the Win32 numbers this
table used to carry (5.3x, 22.8x) came from a 32-bit build with a different page
map and are not comparable.

| workload | Windows UCRT | macOS libmalloc | glibc ptmalloc | mimalloc | **real tcmalloc** |
| --- | --- | --- | --- | --- | --- |
| fixed/bulk | 1.35x | 1.10x | 0.30x | 0.24x | **0.45x** |
| ramp/bulk | 9.52x | 4.22x | 5.10x | 1.01x | **1.43x** |
| fixed/interleaved | 2.59x | 2.60x | 0.63x | 0.57x | **0.72x** |
| ramp/interleaved | 1.77x | **0.71x** | 1.14x | 0.49x | **0.41x** |
| classstep/interleaved | **22.82x** | 4.05x | **9.38x** | 0.91x | **0.74x** |

Our win count falls monotonically as the baseline gets better: **5 of 5** against
Windows UCRT, 4 of 5 against macOS libmalloc, 3 of 5 against glibc, and **1 of 5**
against each of mimalloc and real tcmalloc — and that one, `ramp/bulk` at 1.01x
against mimalloc, is a tie rather than a win.

### The sharpest form of the cross-baseline result

The same five workloads, measured on two operating systems, once against whatever
malloc ships with the OS and once against the same build of real tcmalloc. How far
does each ratio move between platforms?

| workload | glibc | Windows UCRT | swing | tcmalloc (Linux) | tcmalloc (Win) | swing |
| --- | --- | --- | --- | --- | --- | --- |
| fixed/bulk | 0.30x | 1.35x | 4.5x | 0.26x | 0.45x | 1.7x |
| ramp/bulk | 5.10x | 9.52x | 1.9x | 1.29x | 1.43x | **1.1x** |
| fixed/interleaved | 0.63x | 2.59x | 4.1x | 0.38x | 0.72x | 1.9x |
| ramp/interleaved | 1.14x | 1.77x | 1.6x | 0.52x | 0.41x | 1.3x |
| classstep/interleaved | 9.38x | 22.82x | 2.4x | 0.74x | 0.74x | **1.0x** |
| | | | **median 2.4x** | | | **median 1.3x** |

**Measured against a real peer, this allocator's standing is roughly
platform-independent — 1.3x median swing, and 1.00x on `classstep`. Measured
against the system allocator it swings 2.4x.** The variance was never in this
allocator. It was in how much the shipped mallocs differ from each other.

That is the whole cross-baseline lesson in one table, and it is why every headline
number in this project's history — 22.8x on Win32, 9.38x against glibc, 22.82x
against UCRT — is a statement about the opponent.

`ramp/bulk` is the only row we take from tcmalloc, and section S explains it:
never returning memory below 1 MB. It is worth 5.10x against glibc, which
re-faults at 642 per thousand, and only 1.29x against tcmalloc, which does not.

### The single most misleading number in the project

`classstep/interleaved` is our best result anywhere: **9.38x against glibc, and
22.82x against Windows UCRT**. Read the absolute times and both evaporate:

| | Linux | Windows |
| --- | --- | --- |
| system allocator | glibc 9.75 ms | UCRT 26.22 ms |
| real tcmalloc | **0.77 ms** | **0.94 ms** |
| this allocator | 1.04 ms | 1.27 ms |
| **us vs tcmalloc** | **0.74x** | **0.74x** |

We are 26-35% *slower* than tcmalloc on the workload where we look 9.4x and 22.8x
faster than the system allocators — and our ratio against tcmalloc is **identical
to two decimal places on both platforms** while the system-allocator ratio moves
by 2.4x. The big numbers describe glibc and UCRT handling
one-size-class-per-allocation badly. They say nothing about this allocator.

Same shape as the original 22.8x, on the opposite end of the project.

### Caveat on our own column

Under `LD_PRELOAD` the harness's own `std::vector` allocations also route through
tcmalloc, and our measured time shifts between modes by up to 49%
(`ramp/interleaved`: 4.56 ms without preload against 6.78 ms with). The table
above uses the **non-preloaded** figure as canonical for "ours", on the grounds
that `ConcurrentAlloc` goes to `mmap` and should not depend on which malloc the
process installed. That assumption is not verified, so ratios involving our column
are approximate; the opponent-versus-opponent numbers are not affected.

## U. Windows x64 — three baselines, and the trap that makes two of them fake

AMD Ryzen 7 7700X (8 physical cores, 16 logical), 31.2 GB,
Windows 10 Pro 19045.6466 x64, MSVC 19.42.34436 (VS 2022 17.12, toolset
14.42.34433), Windows SDK 10.0.22621.0, `/std:c++14 /O2 /DNDEBUG /EHsc` with
`psapi.lib`. Raw per-process rows in [`results/windows-x64/`](results/windows-x64/).

Three baselines in one batch: **Windows UCRT malloc**, **mimalloc 3.4.5**, and
**gperftools 2.18.1 `tcmalloc_minimal`** — the same version and the same
`--enable-minimal` configuration used for macOS in section Q and Linux in
section S, so the tcmalloc column is comparable across all three operating
systems.

Everything here is x64. A Win32 build selects `TCMalloc_PageMap1<32 - PAGE_SHIFT>`,
a flat array covering the low 4 GB, instead of `TCMalloc_PageMapRadix64<48 - PAGE_SHIFT>`;
those are different data structures and their numbers do not belong in the same
table. Checked per binary rather than assumed: PE machine `0x8664`, and `_WIN64`
defined, which is what `PageCache.h` switches on.

### The correctness gate, and why ASan alone would not have been one

The 12 `size` x `pattern` combinations run clean under `/fsanitize=address`. That
is worth less than it looks. ASan interposes on `malloc`/`free`, and there is not
one `ASAN_POISON_MEMORY_REGION` anywhere in this project, so the sanitizer cannot
see inside `ConcurrentAlloc`'s free lists at all — it validates the *baseline*
side of the comparison and generic stack and global errors, and very little of
the allocator under test.

`verify.cpp` checks the allocator's own contract instead: never null, aligned to
at least `sizeof(void*)`, the whole requested extent writable, and — the one that
matters — **no two simultaneously live blocks share a byte**. Every block is
stamped with a tag unique to (thread, allocation) and re-verified immediately
before it is freed, so handing the same memory out twice, handing out a block
that overlaps a live one, and returning a block shorter than requested all
surface as a corrupted stamp. The stamp check is lock-free, so the concurrency
profile stays close to the benchmark's; `--registry` adds exact duplicate-pointer
detection behind a global lock as a second pass.

All clean: 12/12 under ASan, 12/12 under the invariant checker, 12/12 with
`--registry`, and 12/12 at 8 threads across three seeds.

A gate that cannot fail is not a gate, so both directions were checked.
`--alloc broken` (an allocator that deliberately hands the same slab out
repeatedly) is caught 59 030 times; `--alloc sys` reports zero, so the checker
does not simply fire on everything.

### A trap: `/MT` silently defeats both overrides

`cl` links the CRT statically by default. mimalloc's redirector patches the
malloc exports of `ucrtbase.dll`, and gperftools patches the CRT's malloc in
loaded modules — with a static CRT there is no module export to patch, and the
program's own `malloc` calls go straight to the CRT code linked into the
executable. Both injections still initialise and report success. mimalloc prints
`mimalloc: malloc is redirected.` while not having redirected this binary's
malloc at all.

The tell was that the first mimalloc batch landed on top of UCRT: 49.6 ms against
52.9 ms on `ramp/bulk`, 25.5 against 26.9 on `classstep/interleaved`. Two
allocators do not agree to that precision. Checked per pointer:

| CRT linkage | mimalloc: `mi_is_in_heap_region(malloc(n))` | tcmalloc: 80 MB via `malloc` moves `generic.current_allocated_bytes` by |
| --- | --- | --- |
| `/MT` (cl default) | no | **0.0 MB** |
| `/MD` | **yes** | **80.0 MB** |

This is a third distinct failure mode. macOS linked tcmalloc and it silently
*did* take over `malloc` (section Q); Ubuntu's `--as-needed` silently dropped the
library (section S); Windows links it, loads it, initialises it, prints that it
worked, and still routes every allocation to the CRT. All three are invisible
without a positive per-pointer check.

Section T left the mirror-image assumption unverified — that under injection our
own column still measures `ConcurrentAlloc`. On Windows it can be measured
directly, and it holds: during `--only mine` the injected allocator accounts for
3.5 MB (tcmalloc) and 7.1 MB (mimalloc) against a ~200 MB process working set,
while during `--only sys` it accounts for essentially all of it. `ConcurrentAlloc`
calls `VirtualAlloc` and is not intercepted.

A `/MD` UCRT control was built for the same reason, and CRT linkage does not move
the UCRT numbers (`ramp/bulk` 47.9 ms against 52.9, `classstep` 26.2 against
26.9), which is what lets the canonical `/MT` UCRT column sit in the same table
as the two `/MD` injected ones.

### Opponent absolute times — the cleanest comparison

Same harness, same machine, only the allocator swapped. Medians of five
independent processes, 4 threads.

| workload | UCRT malloc | mimalloc | real tcmalloc | tcmalloc vs UCRT |
| --- | --- | --- | --- | --- |
| fixed/bulk | 4.31 ms | **0.74 ms** | 1.42 ms | 3.0x |
| ramp/bulk | 47.90 ms | **5.30 ms** | 6.70 ms | 7.1x |
| fixed/interleaved | 2.89 ms | **0.62 ms** | 0.81 ms | 3.6x |
| ramp/interleaved | 7.22 ms | 2.06 ms | **1.51 ms** | 4.8x |
| classstep/interleaved | 26.22 ms | 1.07 ms | **0.94 ms** | **27.9x** |

Both modern allocators beat the Windows CRT on all five, and mimalloc takes three
of the five rows from tcmalloc.

### Us, against three Windows baselines

Ratio is opponent over ours, so **> 1 means we are faster**. Ranges are the five
independent runs, not a standard deviation.

| workload | ours (ms) | UCRT malloc | mimalloc | real tcmalloc |
| --- | --- | --- | --- | --- |
| fixed/bulk | 3.11 (2.97-3.17) | 1.35x (1.25-1.37) | 0.24x (0.22-0.25) | 0.45x (0.44-0.48) |
| ramp/bulk | 5.35 (5.32-5.71) | **9.52x** (9.27-10.51) | 1.01x (0.98-1.12) | **1.43x** (1.25-1.47) |
| fixed/interleaved | 1.11 (1.11-1.14) | 2.59x (2.56-2.60) | 0.57x (0.56-0.58) | 0.72x (0.70-0.76) |
| ramp/interleaved | 4.30 (3.84-4.40) | 1.77x (1.71-2.01) | 0.49x (0.47-0.50) | 0.41x (0.41-0.43) |
| classstep/interleaved | 1.17 (1.15-1.23) | **22.82x** (21.33-23.66) | 0.91x (0.90-0.95) | 0.74x (0.71-0.75) |

**Against real tcmalloc we win one of five** — the same score as Linux in section
T, on the same row, for the same reason: `ramp/bulk` is where never returning
memory below 1 MB pays, and tcmalloc scavenges.

### `classstep/interleaved` gives the same 0.74x on two operating systems

Section T called this the most misleading number in the project. Windows makes
the point harder, because the same row can now be read against two very different
baselines on one machine:

| | ours | system allocator | real tcmalloc | looks like | actually |
| --- | --- | --- | --- | --- | --- |
| Linux (section T) | 1.04 ms | glibc 9.75 ms | 0.77 ms | 9.38x | **0.74x** |
| Windows | 1.29 ms | UCRT 26.22 ms | 0.94 ms | **22.32x** | **0.74x** |

The headline multiplier more than doubles, from 9.4x to 22.3x, and the honest
number does not move at all. Two operating systems, two system allocators, the
same pair of thread-caching designs, **0.74x both times**. What changed between
the two rows is entirely the baseline's handling of "every allocation in a
different size class".

That also puts the original 22.8x in its place. This project's first Windows
number was `ramp/bulk` = 22.8x measured on a Win32 build; on x64 that same
workload measures **9.52x**. The 22.8x-shaped result on this machine is a
different workload against a baseline that happens to be bad at it.

### `ramp/interleaved` across every baseline measured

The one row this allocator loses everywhere except Windows CRT:

| baseline | ratio |
| --- | --- |
| Windows UCRT malloc | **1.77x** |
| glibc ptmalloc | 1.14x |
| macOS libmalloc | 0.71x |
| real tcmalloc (Linux) | 0.52x |
| Windows mimalloc | 0.49x |
| real tcmalloc (Windows) | **0.41x** |

A 4.3x spread across baselines for one fixed workload on fixed hardware, against
a 1.35x-to-22.8x spread across workloads for one fixed baseline. Same tcmalloc
build reads 0.52x on Linux and 0.41x on Windows, which is close enough that the
remaining gap is more plausibly OS paging behaviour than anything in the
allocator.

### Peak RSS and page faults

One allocator per process — `GetProcessMemoryInfo` reports a process-wide
monotonic high-water mark, so a shared process cannot measure this. Faults per
1000 operations.

| workload | metric | ours | UCRT | mimalloc | real tcmalloc |
| --- | --- | --- | --- | --- | --- |
| ramp/bulk | peak | 328.5 MB | 135.9 MB | 161.3 MB | 156.8 MB |
| | faults /1k | 12.27 | **150.88** | 4.06 | **3.30** |
| ramp/interleaved | peak | 246.0 MB | 25.1 MB | **12.5 MB** | 25.1 MB |
| | faults /1k | 11.69 | 7.39 | **0.27** | 0.78 |
| classstep/interleaved | peak | 85.7 MB | 11.1 MB | **7.3 MB** | 10.8 MB |
| | faults /1k | **0.09** | 10.10 | 0.13 | 0.13 |

UCRT's 150.88 faults per thousand on `ramp/bulk` is the same mechanism as glibc's
642 in section S — memory handed back to the OS and faulted in again — and it is
most of where our 9.52x on that row comes from. We fault 12x less and still hold
2.4x the memory.

`ramp/interleaved` remains the one workload where we fault *more* than the
baseline while also holding 9.8x the memory, which is the section E story
unchanged by the platform.

How badly a shared process corrupts this, measured rather than asserted:

| workload | allocator | peak, shared process | peak, own process | inflation |
| --- | --- | --- | --- | --- |
| ramp/interleaved | UCRT | 270.8 MB | 25.1 MB | **10.8x** |
| classstep/interleaved | UCRT | 96.9 MB | 11.1 MB | **8.8x** |
| ramp/bulk | UCRT | 462.5 MB | 135.9 MB | 3.4x |

### Caveats

- **Our column drifts slightly with the opponent binary.** The harness's own
  `std::vector` allocations route through whichever allocator is installed. Across
  the three `/MD` binaries our medians are 5.14 / 5.13 / 4.69 ms on `ramp/bulk`,
  4.18 / 4.24 / 3.68 on `ramp/interleaved`, and 1.16 / 1.18 / 1.29 on
  `classstep/interleaved` — up to 12% either way, and not always in the same
  direction. Far smaller than the 49% section T saw under `LD_PRELOAD`, but
  ratios involving our column carry that much slack. Opponent-versus-opponent
  numbers do not.
- `fixed/bulk` in the tcmalloc binary has the widest single-column spread
  anywhere in this section, 19.2% across five runs (2.95-3.56 ms). Treat that row
  as the least resolved.
- `ramp/interleaved` has the widest ratio spread against UCRT, 16.9%.
- Windows `PageFaultCount` counts soft and hard faults together, unlike Linux
  `ru_minflt`. The Windows fault columns are not strictly the same quantity as
  the Linux ones in sections R and S.
- **Unverified mechanism.** The Windows NT heap activates its Low Fragmentation
  Heap per size bucket only after enough allocations of that size. That would
  explain the shape of the UCRT column — `fixed` (one size, LFH fully active)
  loses by only 1.35x, `ramp` partially defeats activation, and `classstep`
  (every allocation in a different class, dwell = 1) never activates it at all
  and loses by 22.8x. Consistent with all five rows, but LFH activation state was
  not measured, so this is a hypothesis and not a finding.
- gperftools supports only `tcmalloc_minimal` on Windows. The Linux and macOS
  measurements also used the minimal build, so the configuration matches; the
  allocation fast path is the same as full tcmalloc.

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
