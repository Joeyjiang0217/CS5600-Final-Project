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

**3.82x of the eventual 4.97x is already there at one thread**, where there is no
contention to remove. Scaling from 1 to 8 threads adds only ~30%.

## B. Allocation pattern — `--size ramp --threads 4`

| pattern | ours (ms) | malloc (ms) | speedup |
| --- | --- | --- | --- |
| `bulk` — allocate 10 000, then free all | 4.46 | 19.70 | 4.42x |
| `interleaved` — bounded 64-object live set | 3.35 | 2.57 | **0.77x** |
| `cross` — thread N frees thread N+1's memory | 6.68 | 16.73 | 2.50x |

## C. Size distribution x pattern — `--threads 4`

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
