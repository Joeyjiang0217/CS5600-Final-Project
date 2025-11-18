//
// Created by Joe on 2025/11/11.
//
#include <algorithm>
#include "ThreadCache.h"
#include "CentralCache.h"

void* ThreadCache::FetchFromCentralCache(size_t index, size_t size) {
    assert(index < NFREELISTS);
    assert(size <= MAX_BYTES);
    // Slow start feedback adjustment algorithm
    // 1. Initially, it won't request too many objects from the central cache at once, to avoid wasting unused memory.
    // 2. If the demand for this size persists, the batchNum gradually increases until it reaches the maximum limit.
    // 3. The larger the size, the smaller the batchNum requested from the central cache at one time.
    // 4. The smaller the size, the larger the batchNum requested from the central cache at one time.
    size_t batchNum = (std::min)(SizeClass::NumMoveSize(size), _freeList[index].MaxSize());
    if (batchNum == _freeList[index].MaxSize()) {
        _freeList[index].MaxSize() += 1;
    }

    void* start = nullptr;
    void* end = nullptr;
    size_t actualNum = CentralCache::GetInstance()->FetchRangeObj(start, end, batchNum, size);
    assert(start);
    assert(end);
    assert(actualNum > 0);
    if (actualNum == 1) {
        return start;
    } else if (actualNum > 1) {
        assert(start != end);
        _freeList[index].PushRange(NextObj(start), end, actualNum - 1);
        return start;
    }
}


void* ThreadCache::Allocate(size_t size) {
    assert(size <= MAX_BYTES);
    size_t alignedSize = SizeClass::RoundUp(size);
    size_t index = SizeClass::Index(size);
    if (!_freeList[index].Empty()) {
        return _freeList[index].Pop();
    } else {
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
    list.PopRange(start, end, list.MaxSize());
    CentralCache::GetInstance()->ReleaseListToSpans(start, size);
}
