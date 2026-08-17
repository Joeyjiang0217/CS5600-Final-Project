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
  these fixes. libmalloc is genuinely more variable than this allocator under
  concurrent load, which is itself a result.
