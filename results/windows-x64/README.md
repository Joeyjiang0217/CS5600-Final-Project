# Windows x64 raw measurements

Backing data for **section U** of [`BENCHMARKS.md`](../../BENCHMARKS.md). Every row is one
independent process; medians and ranges in section U are computed from these files.

Machine: AMD Ryzen 7 7700X (8 physical / 16 logical), 31.2 GB, Windows 10 Pro
19045.6466 x64, MSVC 19.42.34436 (VS 2022 17.12, toolset 14.42.34433),
Windows SDK 10.0.22621.0.

## Files

| file | `sys_*` column is | CRT | override verified |
| --- | --- | --- | --- |
| `ucrt-mt-*.csv` | Windows UCRT malloc | `/MT` (cl default) | n/a |
| `ucrt-md-*.csv` | Windows UCRT malloc | `/MD` | n/a |
| `mimalloc-*.csv` | mimalloc 3.4.5 | `/MD` | yes |
| `tcmalloc-*.csv` | gperftools 2.18.1 `tcmalloc_minimal` | `/MD` | yes |

`ucrt-mt` is the canonical baseline — it is what the documented `cl` command line
produces. `ucrt-md` exists only to show that CRT linkage does not move the UCRT
numbers, which is what makes `ucrt-mt` comparable against the two `/MD` binaries.

`*-timing.csv` covers all five workloads, both allocators in one process (valid
for timing, **not** for peak memory). `*-memory.csv` is the same five workloads
run one allocator per process via `--only`, and is the only valid source for
`peak_rss_kib_*`.

## Columns

`workload` is 1-5 as numbered in section U. `run` is 1-5, one process each.
`allocator` (memory files only) is which `--only` mode produced the row; the
columns belonging to the other allocator are zero. The remaining 15 columns are
the harness's own `--csv` output.

`speedup_wall` is `sys_wall_ms / mine_wall_ms`, so **> 1 means this allocator is
faster than the baseline**. `faults_*` is summed over all `--reps`, so faults per
1000 operations is `faults / (ops * reps) * 1000` — with the parameters below,
`faults / 3600`.

## Reproducing

All four binaries are x64 (`TCMalloc_PageMapRadix64<48 - PAGE_SHIFT>`); a Win32
build selects a different page map and is not comparable.

```
cl /std:c++14 /O2 /DNDEBUG /EHsc main.cpp CentralCache.cpp PageCache.cpp ThreadCache.cpp psapi.lib /Fe:bench.exe
```

Timing, five processes per workload:

```
bench.exe --threads 4 --rounds 10 --ntimes 10000 --reps 9 --size SIZE --pattern PATTERN --csv
```

Peak memory and faults, one allocator per process:

```
bench.exe ... --only mine
bench.exe ... --only sys
```

### The two override builds

**`/MD` is mandatory for both.** Under `cl`'s default `/MT` the CRT is linked
statically into the executable, and neither override reaches it — mimalloc's
redirector patches `ucrtbase.dll`'s exports and gperftools patches the CRT's
malloc in loaded modules, and with a static CRT there is nothing in a module to
patch. Both still start up and report success. Measured that way, the
"mimalloc" and "tcmalloc" columns are UCRT malloc under another name.

```
cl ... /MD ... mimalloc.dll.lib      /link /INCLUDE:mi_version
cl ... /MD ... tcmalloc_minimal.lib  /link /INCLUDE:__tcmalloc
```

Verified per pointer before trusting any of it, with the DLLs beside the exe:

- mimalloc — `mi_is_in_heap_region(malloc(n))`: false under `/MT`, true under `/MD`.
- tcmalloc — `MallocExtension_GetOwnership`, plus pushing 80 MB through plain
  `malloc` and reading `generic.current_allocated_bytes`: unchanged (0.0 MB)
  under `/MT`, exactly 80.0 MB under `/MD`.

The reverse direction was checked too, since `ConcurrentAlloc` must keep
measuring itself and not the injected allocator. During `--only mine` the
injected allocator accounts for 3.5 MB (tcmalloc) and 7.1 MB (mimalloc) against
a ~200 MB process working set; during `--only sys` it accounts for essentially
all of it. `ConcurrentAlloc` calls `VirtualAlloc` directly and is not
intercepted.
