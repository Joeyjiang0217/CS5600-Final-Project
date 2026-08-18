//
// Opt-in counters for explaining *why* a workload is fast or slow.
//
// Compile with -DALLOC_STATS to enable. Without it every macro below expands to
// nothing, so the measured build carries no counter overhead.
//
#ifndef INC_5600FINALPROJECT_ALLOCSTATS_H
#define INC_5600FINALPROJECT_ALLOCSTATS_H

#ifdef ALLOC_STATS

#include <atomic>
#include <cstdio>
#include <cstddef>

struct AllocStats {
    static std::atomic<size_t> fastHits;      // served from the thread-local list
    static std::atomic<size_t> slowFetches;   // trips to CentralCache
    static std::atomic<size_t> objsFetched;   // objects those trips returned
    static std::atomic<size_t> listReturns;   // ListTooLong flushes back to central
    static std::atomic<size_t> spansCarved;   // PageCache::NewSpan calls
    static std::atomic<size_t> maxSizeSum;    // sum of MaxSize() at each refill
    static std::atomic<size_t> distinctClassRefills;
    static std::atomic<size_t> bigRefills;      // refills for size >= 1024
    static std::atomic<size_t> bigMaxSizeSum;   // MaxSize() at those refills
    static std::atomic<size_t> bigBatchSum;     // objects actually returned

    static void Reset() {
        fastHits = 0; slowFetches = 0; objsFetched = 0;
        listReturns = 0; spansCarved = 0; maxSizeSum = 0;
        distinctClassRefills = 0;
        bigRefills = 0; bigMaxSizeSum = 0; bigBatchSum = 0;
    }

    static void Dump(const char* tag) {
        size_t fast = fastHits.load(), slow = slowFetches.load();
        size_t total = fast + slow;
        double hitRate = total ? (100.0 * fast / total) : 0.0;
        double avgBatch = slow ? (double)objsFetched.load() / slow : 0.0;
        double avgMaxSize = slow ? (double)maxSizeSum.load() / slow : 0.0;
        printf("%-28s fastpath %6.2f%%  refills %8zu  avg_batch %7.2f  "
               "avg_MaxSize %7.2f  spans %6zu  flushes %8zu\n",
               tag, hitRate, slow, avgBatch, avgMaxSize,
               spansCarved.load(), listReturns.load());
        size_t br = bigRefills.load();
        if (br) {
            printf("%-28s  size>=1KB: refills %zu  avg_MaxSize %.1f  avg_batch %.1f\n",
                   "", br, (double)bigMaxSizeSum.load()/br, (double)bigBatchSum.load()/br);
        }
    }
};

#define STAT_INC(x)     AllocStats::x.fetch_add(1, std::memory_order_relaxed)
#define STAT_ADD(x, n)  AllocStats::x.fetch_add((size_t)(n), std::memory_order_relaxed)

#else

#define STAT_INC(x)     ((void)0)
#define STAT_ADD(x, n)  ((void)0)

#endif // ALLOC_STATS
#endif // INC_5600FINALPROJECT_ALLOCSTATS_H
