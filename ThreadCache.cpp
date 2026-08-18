//
// Created by Joe on 2025/11/11.
//
#include <algorithm>
#include "ThreadCache.h"
#include "CentralCache.h"
#include "AllocStats.h"

#ifdef ALLOC_STATS
std::atomic<size_t> AllocStats::fastHits{0};
std::atomic<size_t> AllocStats::slowFetches{0};
std::atomic<size_t> AllocStats::objsFetched{0};
std::atomic<size_t> AllocStats::listReturns{0};
std::atomic<size_t> AllocStats::spansCarved{0};
std::atomic<size_t> AllocStats::maxSizeSum{0};
std::atomic<size_t> AllocStats::distinctClassRefills{0};
std::atomic<size_t> AllocStats::bigRefills{0};
std::atomic<size_t> AllocStats::bigMaxSizeSum{0};
std::atomic<size_t> AllocStats::bigBatchSum{0};
#endif

void* ThreadCache::FetchFromCentralCache(size_t index, size_t size) {
    assert(index < NFREELISTS);
    assert(size <= MAX_BYTES);
    // Slow start feedback adjustment algorithm
    // 1. Initially, it won't request too many objects from the central cache at once, to avoid wasting unused memory.
    // 2. If the demand for this size persists, the batchNum gradually increases until it reaches the maximum limit.
    // 3. The larger the size, the smaller the batchNum requested from the central cache at one time.
    // 4. The smaller the size, the larger the batchNum requested from the central cache at one time.
    STAT_ADD(maxSizeSum, _freeList[index].MaxSize());
    if (size >= 1024) { STAT_INC(bigRefills); STAT_ADD(bigMaxSizeSum, _freeList[index].MaxSize()); }
    size_t batchNum = (std::min)(SizeClass::NumMoveSize(size), _freeList[index].MaxSize());
    if (batchNum == _freeList[index].MaxSize()) {
        // The comment above calls this slow start, but += 1 is additive
        // increase: reaching a batch of N takes N refills. TCP slow start
        // doubles per round trip and reaches N in log2(N). For a size class
        // refilled only a handful of times per round -- which is what an
        // ascending size sweep produces -- additive growth never converges
        // inside the measurement, so the list stays far too small to hold
        // what the workload piles into it.
        //
        // Build with -DMAXSIZE_GROWTH_MULT for the multiplicative version.
#ifdef MAXSIZE_GROWTH_MULT
        size_t cap  = SizeClass::NumMoveSize(size);
        size_t next = _freeList[index].MaxSize() * 2;
        _freeList[index].MaxSize() = next > cap ? cap : next;
#else
        _freeList[index].MaxSize() += 1;
#endif
    }

    void* start = nullptr;
    void* end = nullptr;
    size_t actualNum = CentralCache::GetInstance()->FetchRangeObj(start, end, batchNum, size);
    assert(start);
    assert(end);
    assert(actualNum > 0);
    // The caller keeps the first object; anything beyond that goes on the
    // thread-local free list. Returning unconditionally matters: the old code
    // fell off the end of the function when actualNum was 0, which the assert
    // above only catches while asserts are enabled.
    STAT_ADD(objsFetched, actualNum);
    if (size >= 1024) { STAT_ADD(bigBatchSum, actualNum); }
    if (actualNum > 1) {
        assert(start != end);
        _freeList[index].PushRange(NextObj(start), end, actualNum - 1);
    }
    return start;
}


void* ThreadCache::Allocate(size_t size) {
    assert(size <= MAX_BYTES);
    size_t alignedSize = SizeClass::RoundUp(size);
    size_t index = SizeClass::Index(size);
    if (!_freeList[index].Empty()) {
        STAT_INC(fastHits);
        return _freeList[index].Pop();
    } else {
        STAT_INC(slowFetches);
        return FetchFromCentralCache(index, alignedSize);
    }
    
}

void ThreadCache::Deallocate(void* ptr, size_t size) {
    assert(size <= MAX_BYTES);
    assert(ptr != nullptr);

    // Return the object to the corresponding free list
    size_t index = SizeClass::Index(size);
    size_t alignedSize = SizeClass::RoundUp(size);
    _freeList[index].Push(ptr);

    // Check if the free list is too long
    if (_freeList[index].Size() >= _freeList[index].MaxSize()) {
        ListTooLong(_freeList[index], alignedSize);
    }
}

void ThreadCache::ListTooLong(FreeList& list, size_t size) {
    assert(size <= MAX_BYTES);
    void* start = nullptr;
    void* end = nullptr;
    STAT_INC(listReturns);
    list.PopRange(start, end, list.MaxSize());
    CentralCache::GetInstance()->ReleaseListToSpans(start, size);
}
