// Benchmark harness for the thread-local allocator vs. the system allocator.
//
// Every knob is a command-line flag, so a result can be reproduced from the
// command that produced it. Run with --help for the list.
//
// Two things the original harness got wrong, fixed here:
//   * it summed each thread's elapsed time into one atomic, then labelled the
//     result "runtime". That number is aggregate thread time, not wall clock.
//     Both are reported separately now.
//   * it used clock(), whose resolution on the original platform was coarse
//     enough to swallow the per-round measurements. This uses steady_clock.

#include "ConcurrentAlloc.h"
#include "AllocStats.h"

#include <chrono>
#include <cstring>
#include <random>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>

#if defined(_WIN32)
  #include <windows.h>
  #include <psapi.h>
#else
  #include <sys/resource.h>
#endif

using Clock = std::chrono::steady_clock;

static double MillisSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// ---------------------------------------------------------------- peak memory

// Peak resident set size in KiB.
//
// ru_maxrss is a process-wide high-water mark and never decreases, so running
// both allocators in one process makes whichever goes second look worse no
// matter what. Use --only mine / --only sys to get a figure that means
// something: one allocator per process, then compare across the two runs.
static size_t PeakRssKiB() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        return pmc.PeakWorkingSetSize / 1024;
    return 0;
#else
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
  #if defined(__APPLE__)
    return (size_t)ru.ru_maxrss / 1024;   // darwin reports bytes
  #else
    return (size_t)ru.ru_maxrss;          // linux reports KiB
  #endif
#endif
}

// ------------------------------------------------------------------ workloads

enum class SizeMode { Fixed16, Ramp, Random, Large };
enum class Pattern  { BulkThenFree, Interleaved, CrossThread };

struct Config {
    size_t   ntimes  = 10000;   // allocations per round
    size_t   nworks  = 4;       // worker threads
    size_t   rounds  = 10;      // rounds per thread
    size_t   reps    = 5;       // repetitions of the whole measurement
    SizeMode sizeMode = SizeMode::Ramp;
    Pattern  pattern  = Pattern::BulkThenFree;
    unsigned seed     = 12345;
    size_t   latStride = 64;    // mean gap between latency samples
};

// Returns the request size for iteration `i`.
//
// Ramp is what the original benchmark called "random": (16+i) % 8192 + 1 walks
// sizes in order, which is close to the best case for a size-class allocator.
// Random draws from the same range with a real generator so the two can be
// compared directly.
static inline size_t SizeFor(SizeMode m, size_t i, std::mt19937& rng) {
    switch (m) {
        case SizeMode::Fixed16: return 16;
        case SizeMode::Ramp:    return (16 + i) % 8192 + 1;
        case SizeMode::Random:  return rng() % 8192 + 1;
        // Above MAX_BYTES (256 KB) requests bypass ThreadCache entirely and go
        // straight to PageCache. The original benchmark never reached this path.
        case SizeMode::Large:   return 256 * 1024 + (rng() % (64 * 1024)) + 1;
    }
    return 16;
}

struct Timing {
    double wallMs = 0;        // end-to-end wall clock
    double threadMs = 0;      // summed across threads (the old metric)
    size_t peakRssKiB = 0;
};

// ------------------------------------------------------------ latency sampling

// Per-call latency, as opposed to the throughput numbers everywhere else.
// Throughput answers "how long do 400 000 operations take"; a p99 answers "how
// bad is the worst call in a hundred". A run-to-run standard deviation hints at
// the second but cannot measure it -- 400 000 operations average the spikes away.
struct LatencyData {
    std::vector<uint32_t> allocNs;
    std::vector<uint32_t> freeNs;
};

// Cost of reading the clock, measured rather than assumed. This decides whether
// every call can be timed or whether sampling is required: the fast path is
// around 2 ns, so a timer that costs more than that would be measuring itself.
static double CalibrateTimerNs() {
    const int N = 200000;
    volatile long long sink = 0;
    Clock::time_point t0 = Clock::now();
    for (int i = 0; i < N; ++i) {
        Clock::time_point t = Clock::now();
        sink += t.time_since_epoch().count();
    }
    double total = std::chrono::duration<double, std::nano>(Clock::now() - t0).count();
    (void)sink;
    return total / N;
}

static double Percentile(const std::vector<uint32_t>& sorted, double q) {
    if (sorted.empty()) return 0;
    double idx = q * (sorted.size() - 1);
    size_t lo = (size_t)idx;
    size_t hi = lo + 1 < sorted.size() ? lo + 1 : lo;
    double frac = idx - lo;
    return sorted[lo] * (1.0 - frac) + sorted[hi] * frac;
}

static void ReportLatency(const char* tag, const char* op,
                          std::vector<uint32_t> xs, double timerNs) {
    if (xs.empty()) { printf("%-18s %-6s (no samples)\n", tag, op); return; }
    std::sort(xs.begin(), xs.end());
    // The clock read brackets the call, so each sample carries one timer cost.
    auto adj = [&](double v) { double r = v - timerNs; return r > 0 ? r : 0.0; };
    printf("%-18s %-6s %9zu %8.0f %8.0f %8.0f %8.0f %9.0f\n",
           tag, op, xs.size(),
           adj(Percentile(xs, 0.50)), adj(Percentile(xs, 0.90)),
           adj(Percentile(xs, 0.99)), adj(Percentile(xs, 0.999)),
           adj((double)xs.back()));
}

// A reusable sense-reversing barrier for `n` threads. Counting up to a
// per-round target breaks as soon as threads drift between rounds, so track a
// generation instead and let each thread wait for the generation to advance.
static void WaitAtBarrier(std::atomic<size_t>& state, size_t n) {
    size_t gen = state.load(std::memory_order_acquire) / n;
    if (state.fetch_add(1, std::memory_order_acq_rel) + 1 == (gen + 1) * n) {
        return;   // last one through, generation has advanced
    }
    while (state.load(std::memory_order_acquire) / n == gen) {
        std::this_thread::yield();
    }
}

// Alloc is the allocation function, Free the matching deallocation.
template <class AllocFn, class FreeFn>
static Timing RunOnce(const Config& cfg, AllocFn Alloc, FreeFn Free,
                      std::vector<LatencyData>* lat = nullptr) {
    std::vector<std::thread> threads(cfg.nworks);
    std::atomic<double> threadMsTotal{0.0};

    // CrossThread: every thread allocates into a slot that the *next* thread
    // frees, so no object goes back to the free list it came from. This is the
    // case a thread-local design handles worst, and the original harness never
    // exercised it.
    std::vector<std::vector<void*>> shared(cfg.nworks);
    for (auto& v : shared) v.resize(cfg.ntimes, nullptr);
    std::atomic<size_t> barrier{0};   // generation counter for CrossThread

    Clock::time_point wallStart = Clock::now();

    for (size_t k = 0; k < cfg.nworks; ++k) {
        threads[k] = std::thread([&, k]() {
            std::mt19937 rng(cfg.seed + (unsigned)k);
            std::vector<void*> local;
            local.reserve(cfg.ntimes);

            // Latency sampling. The gap between samples is randomised around
            // cfg.latStride rather than fixed: slow paths recur on a period
            // (a refill every ~16 allocations, say), and a fixed stride can
            // alias with that period and systematically over- or under-sample
            // them.
            LatencyData* L = lat ? &(*lat)[k] : nullptr;
            std::mt19937 srng(cfg.seed * 7919 + (unsigned)k);
            size_t nextA = L ? srng() % (2 * cfg.latStride) : (size_t)-1;
            size_t nextF = L ? srng() % (2 * cfg.latStride) : (size_t)-1;
            size_t ctrA = 0, ctrF = 0;
            auto bump = [&](size_t& next, size_t& ctr) {
                next = ctr + 1 + srng() % (2 * cfg.latStride);
            };
            // Time one call and record it; `ctr` counts calls of that kind.
            auto TimedAlloc = [&](size_t sz) -> void* {
                if (L && ctrA == nextA) {
                    Clock::time_point a = Clock::now();
                    void* p = Alloc(sz);
                    Clock::time_point b = Clock::now();
                    L->allocNs.push_back((uint32_t)std::chrono::duration_cast<
                        std::chrono::nanoseconds>(b - a).count());
                    bump(nextA, ctrA);
                    ++ctrA;
                    return p;
                }
                ++ctrA;
                return Alloc(sz);
            };
            auto TimedFree = [&](void* p) {
                if (L && ctrF == nextF) {
                    Clock::time_point a = Clock::now();
                    Free(p);
                    Clock::time_point b = Clock::now();
                    L->freeNs.push_back((uint32_t)std::chrono::duration_cast<
                        std::chrono::nanoseconds>(b - a).count());
                    bump(nextF, ctrF);
                    ++ctrF;
                    return;
                }
                ++ctrF;
                Free(p);
            };

            Clock::time_point t0 = Clock::now();

            for (size_t r = 0; r < cfg.rounds; ++r) {
                switch (cfg.pattern) {
                    case Pattern::BulkThenFree: {
                        for (size_t i = 0; i < cfg.ntimes; ++i)
                            local.push_back(TimedAlloc(SizeFor(cfg.sizeMode, i, rng)));
                        for (size_t i = 0; i < cfg.ntimes; ++i)
                            TimedFree(local[i]);
                        local.clear();
                        break;
                    }
                    case Pattern::Interleaved: {
                        // Keeps a bounded live set, which is how real programs
                        // behave: allocate, free something older, repeat.
                        // A ring buffer, not erase(begin()) on a vector -- that
                        // is O(n) per step and would swamp the allocator cost
                        // we are trying to measure.
                        const size_t window = 64;
                        std::vector<void*> ring(window, nullptr);
                        size_t pos = 0;
                        for (size_t i = 0; i < cfg.ntimes; ++i) {
                            void* p = TimedAlloc(SizeFor(cfg.sizeMode, i, rng));
                            if (ring[pos]) TimedFree(ring[pos]);
                            ring[pos] = p;
                            pos = (pos + 1) % window;
                        }
                        for (void* p : ring) if (p) TimedFree(p);
                        break;
                    }
                    case Pattern::CrossThread: {
                        for (size_t i = 0; i < cfg.ntimes; ++i)
                            shared[k][i] = Alloc(SizeFor(cfg.sizeMode, i, rng));

                        // Two barriers per round, not one. The first makes sure
                        // every slot is filled before anyone frees a peer's
                        // memory. The second is what the first version was
                        // missing: without it a fast thread loops into the next
                        // round and overwrites slots a slower thread is still
                        // freeing, which is a use-after-free.
                        WaitAtBarrier(barrier, cfg.nworks);

                        size_t victim = (k + 1) % cfg.nworks;
                        for (size_t i = 0; i < cfg.ntimes; ++i) {
                            if (shared[victim][i]) {
                                Free(shared[victim][i]);
                                shared[victim][i] = nullptr;
                            }
                        }

                        WaitAtBarrier(barrier, cfg.nworks);
                        break;
                    }
                }
            }

            double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
            // fetch_add on double needs a CAS loop pre-C++20.
            double cur = threadMsTotal.load();
            while (!threadMsTotal.compare_exchange_weak(cur, cur + ms)) {}
        });
    }

    for (auto& t : threads) t.join();

    Timing out;
    out.wallMs = MillisSince(wallStart);
    out.threadMs = threadMsTotal.load();
    out.peakRssKiB = PeakRssKiB();
    return out;
}

// ------------------------------------------------------------------ reporting

struct Stats { double median = 0, mean = 0, stddev = 0, min = 0, max = 0; };

static Stats Summarize(std::vector<double> xs) {
    Stats s;
    if (xs.empty()) return s;
    std::sort(xs.begin(), xs.end());
    s.min = xs.front();
    s.max = xs.back();
    s.median = xs[xs.size() / 2];
    double sum = 0;
    for (double x : xs) sum += x;
    s.mean = sum / xs.size();
    double acc = 0;
    for (double x : xs) acc += (x - s.mean) * (x - s.mean);
    s.stddev = std::sqrt(acc / xs.size());
    return s;
}

static const char* SizeModeName(SizeMode m) {
    switch (m) {
        case SizeMode::Fixed16: return "fixed-16B";
        case SizeMode::Ramp:    return "ramp-1B..8KB";
        case SizeMode::Random:  return "random-1B..8KB";
        case SizeMode::Large:   return "large-256KB+";
    }
    return "?";
}

static const char* PatternName(Pattern p) {
    switch (p) {
        case Pattern::BulkThenFree: return "bulk-then-free";
        case Pattern::Interleaved:  return "interleaved";
        case Pattern::CrossThread:  return "cross-thread-free";
    }
    return "?";
}

int main(int argc, char** argv) {
    Config cfg;
    bool csv = false;
    bool runMine = true, runSys = true;
    bool latency = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : ""; };
        if      (a == "--threads")  cfg.nworks = std::stoul(next());
        else if (a == "--rounds")   cfg.rounds = std::stoul(next());
        else if (a == "--ntimes")   cfg.ntimes = std::stoul(next());
        else if (a == "--reps")     cfg.reps   = std::stoul(next());
        else if (a == "--seed")     cfg.seed   = (unsigned)std::stoul(next());
        else if (a == "--csv")      csv = true;
        else if (a == "--size") {
            std::string v = next();
            if      (v == "fixed")  cfg.sizeMode = SizeMode::Fixed16;
            else if (v == "ramp")   cfg.sizeMode = SizeMode::Ramp;
            else if (v == "random") cfg.sizeMode = SizeMode::Random;
            else if (v == "large")  cfg.sizeMode = SizeMode::Large;
            else { fprintf(stderr, "unknown --size %s\n", v.c_str()); return 2; }
        }
        else if (a == "--pattern") {
            std::string v = next();
            if      (v == "bulk")        cfg.pattern = Pattern::BulkThenFree;
            else if (v == "interleaved") cfg.pattern = Pattern::Interleaved;
            else if (v == "cross")       cfg.pattern = Pattern::CrossThread;
            else { fprintf(stderr, "unknown --pattern %s\n", v.c_str()); return 2; }
        }
        else if (a == "--latency")    latency = true;
        else if (a == "--lat-stride") cfg.latStride = std::stoul(next());
        else if (a == "--only") {
            std::string v = next();
            if      (v == "mine") { runMine = true;  runSys = false; }
            else if (v == "sys")  { runMine = false; runSys = true;  }
            else if (v == "both") { runMine = true;  runSys = true;  }
            else { fprintf(stderr, "unknown --only %s\n", v.c_str()); return 2; }
        }
        else if (a == "--help" || a == "-h") {
            printf("usage: %s [options]\n"
                   "  --threads N     worker threads (default 4)\n"
                   "  --rounds N      rounds per thread (default 10)\n"
                   "  --ntimes N      allocations per round (default 10000)\n"
                   "  --reps N        repetitions for median/stddev (default 5)\n"
                   "  --size MODE     fixed | ramp | random | large (default ramp)\n"
                   "  --pattern MODE  bulk | interleaved | cross (default bulk)\n"
                   "  --seed N        RNG seed (default 12345)\n"
                   "  --latency       per-call latency percentiles instead of throughput\n"
                   "  --lat-stride N  mean gap between latency samples (default 64)\n"
                   "  --csv           machine-readable output\n", argv[0]);
            return 0;
        }
        else { fprintf(stderr, "unknown flag %s (try --help)\n", a.c_str()); return 2; }
    }

    auto Mine = [](size_t n) { return ConcurrentAlloc(n); };
    auto MineFree = [](void* p) { ConcurrentFree(p); };
    auto Sys = [](size_t n) { return malloc(n); };
    auto SysFree = [](void* p) { free(p); };

    if (latency) {
        if (cfg.pattern == Pattern::CrossThread) {
            fprintf(stderr, "--latency does not support --pattern cross: the "
                            "barrier wait would be counted inside a free.\n");
            return 2;
        }
        double timerNs = CalibrateTimerNs();

        // Warm up, then collect. Sampling perturbs throughput, so this mode
        // deliberately reports no speedup -- the two are measured separately.
        Config warm = cfg; warm.rounds = 1;
        RunOnce(warm, Mine, MineFree);
        RunOnce(warm, Sys, SysFree);

        std::vector<LatencyData> lmine(cfg.nworks), lsys(cfg.nworks);
        RunOnce(cfg, Mine, MineFree, &lmine);
        RunOnce(cfg, Sys, SysFree, &lsys);

        auto merge = [&](std::vector<LatencyData>& v, bool allocSide) {
            std::vector<uint32_t> out;
            for (auto& d : v) {
                const std::vector<uint32_t>& s = allocSide ? d.allocNs : d.freeNs;
                out.insert(out.end(), s.begin(), s.end());
            }
            return out;
        };

        printf("========================================================\n");
        printf("PER-CALL LATENCY  size=%s pattern=%s threads=%zu\n",
               SizeModeName(cfg.sizeMode), PatternName(cfg.pattern), cfg.nworks);
        printf("sampled 1 call in ~%zu (randomised stride); "
               "clock read costs %.1f ns, subtracted\n", cfg.latStride, timerNs);
        printf("--------------------------------------------------------\n");
        printf("%-18s %-6s %9s %8s %8s %8s %8s %9s\n",
               "allocator", "op", "samples", "p50", "p90", "p99", "p99.9", "max");
        ReportLatency("ConcurrentAlloc", "alloc", merge(lmine, true),  timerNs);
        ReportLatency("ConcurrentAlloc", "free",  merge(lmine, false), timerNs);
        ReportLatency("malloc/free",     "alloc", merge(lsys, true),   timerNs);
        ReportLatency("malloc/free",     "free",  merge(lsys, false),  timerNs);
        printf("========================================================\n");
        printf("all figures in nanoseconds\n");
        return 0;
    }

    // One discarded warm-up pass each. The first pass pays for lazily created
    // ThreadCaches, page-map nodes, and first-touch page faults, none of which
    // recur; leaving it in the sample is most of why the variance was large.
    {
        Config warm = cfg;
        warm.rounds = 1;
        if (runMine) RunOnce(warm, Mine, MineFree);
        if (runSys)  RunOnce(warm, Sys, SysFree);
    }

    std::vector<double> mineWall, mineThread, sysWall, sysThread;
    size_t minePeak = 0, sysPeak = 0;

    // Alternate which allocator goes first across repetitions. The original
    // harness always ran ours then malloc's, so any drift over the life of the
    // process landed entirely on malloc.
    for (size_t r = 0; r < cfg.reps; ++r) {
        bool mineFirst = (r % 2 == 0);
        for (int phase = 0; phase < 2; ++phase) {
            bool doMine = (phase == 0) == mineFirst;
            if (doMine && runMine) {
                Timing a = RunOnce(cfg, Mine, MineFree);
                mineWall.push_back(a.wallMs);
                mineThread.push_back(a.threadMs);
                minePeak = std::max(minePeak, a.peakRssKiB);
            } else if (!doMine && runSys) {
                Timing b = RunOnce(cfg, Sys, SysFree);
                sysWall.push_back(b.wallMs);
                sysThread.push_back(b.threadMs);
                sysPeak = std::max(sysPeak, b.peakRssKiB);
            }
        }
    }

#ifdef ALLOC_STATS
    {
        char tag[128];
        snprintf(tag, sizeof(tag), "%s/%s", SizeModeName(cfg.sizeMode), PatternName(cfg.pattern));
        AllocStats::Dump(tag);
    }
#endif
    Stats mw = Summarize(mineWall), sw = Summarize(sysWall);
    Stats mt = Summarize(mineThread), st = Summarize(sysThread);
    double speedupWall = mw.median > 0 ? sw.median / mw.median : 0.0;
    size_t totalOps = cfg.nworks * cfg.rounds * cfg.ntimes;

    if (csv) {
        printf("size,pattern,threads,ops,mine_wall_ms,sys_wall_ms,speedup_wall,"
               "mine_thread_ms,sys_thread_ms,mine_wall_sd,sys_wall_sd,peak_rss_kib_mine,peak_rss_kib_sys\n");
        printf("%s,%s,%zu,%zu,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%zu,%zu\n",
               SizeModeName(cfg.sizeMode), PatternName(cfg.pattern), cfg.nworks, totalOps,
               mw.median, sw.median, speedupWall, mt.median, st.median,
               mw.stddev, sw.stddev, minePeak, sysPeak);
        return 0;
    }

    printf("========================================================\n");
    printf("size=%s  pattern=%s  threads=%zu  rounds=%zu  ntimes=%zu  reps=%zu\n",
           SizeModeName(cfg.sizeMode), PatternName(cfg.pattern),
           cfg.nworks, cfg.rounds, cfg.ntimes, cfg.reps);
    printf("total operations per allocator: %zu alloc + %zu free\n", totalOps, totalOps);
    printf("--------------------------------------------------------\n");
    printf("                 wall ms (median)   sd      aggregate thread ms\n");
    printf("  ConcurrentAlloc %10.2f      %7.2f   %12.2f\n", mw.median, mw.stddev, mt.median);
    printf("  malloc/free     %10.2f      %7.2f   %12.2f\n", sw.median, sw.stddev, st.median);
    printf("--------------------------------------------------------\n");
    printf("  speedup (wall clock, median): %.2fx\n", speedupWall);
    printf("  peak RSS: ours %zu KiB, malloc %zu KiB\n", minePeak, sysPeak);
    printf("========================================================\n");
    return 0;
}
