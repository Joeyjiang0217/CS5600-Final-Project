//
// Created by Joe on 2025/11/12.
//
#include "PageCache.h"

PageCache PageCache::_sInst;

Span* PageCache::MapObjectToSpan(void* obj) {
    PAGE_ID pageId = (PAGE_ID)obj >> PAGE_SHIFT;
//    std::unique_lock<std::mutex> lock(_mtx);
//    auto it = _idSpanMap.find(pageId);
//    if (it != _idSpanMap.end())
//    {
//        return it->second;
//    }
    auto it = _idSpanMap.get(pageId);
    if (it != nullptr)
    {
        return (Span*)it;
    }
    assert(false);
    return nullptr;
}

Span* PageCache::NewSpan(size_t k) {
    assert(k > 0);

    // 1. If k >= NPAGES, directly allocate from the system
    if (k >= NPAGES)
    {
        void* ptr = SystemAlloc(k);
        Span* span = _spanPool.New();
        span->_pageId = (PAGE_ID)ptr >> PAGE_SHIFT;
        span->_n = k;
//        _idSpanMap[span->_pageId] = span;
        _idSpanMap.set(span->_pageId, span);
        return span;
    }

    // 2. Try to find a span of size k from the spanList
    if (!_spanList[k].Empty())
    {
        Span* span = _spanList[k].PopFront();
        for (PAGE_ID i = 0; i < span->_n; ++i)
        {
//            _idSpanMap[span->_pageId + i] = span;
            _idSpanMap.set(span->_pageId + i, span);
        }
        return span;
    }

    // 3. Try to split a larger span
    for (int i = k + 1; i < NPAGES; ++i)
    {
        if (!_spanList[i].Empty())
        {
            Span* largerSpan = _spanList[i].PopFront();
            assert(largerSpan->_n >= k);
            Span* newSpan = _spanPool.New();
            newSpan->_pageId = largerSpan->_pageId;
            newSpan->_n = k;
            largerSpan->_pageId += k;
            largerSpan->_n -= k;
            if (largerSpan->_n > 0)
            {
                _spanList[largerSpan->_n].PushFront(largerSpan);
            }

//            _idSpanMap[largerSpan->_pageId] = largerSpan;
//            _idSpanMap[largerSpan->_pageId + largerSpan->_n - 1] = largerSpan;
            _idSpanMap.set(largerSpan->_pageId, largerSpan);
            _idSpanMap.set(largerSpan->_pageId + largerSpan->_n - 1, largerSpan);

            for (int j = 0; j < k; ++j)
            {
//                _idSpanMap[newSpan->_pageId + j] = newSpan;
                _idSpanMap.set(newSpan->_pageId + j, newSpan);
            }
            return newSpan;
        }
    }

    // 4. Allocate a new large span from the system and split
    void* ptr = SystemAlloc(NPAGES - 1);
    Span* span = _spanPool.New();
    span->_pageId = (PAGE_ID)ptr >> PAGE_SHIFT;
    span->_n = NPAGES - 1;
    _spanList[NPAGES - 1].PushFront(span);
    return NewSpan(k);
}


void PageCache::ReleaseSpanToPageCache(Span* span) {
    assert(span);

    // 1. If span size >= NPAGES, directly release to the system
    if (span->_n >= NPAGES)
    {
        void* ptr = (void*)(span->_pageId << PAGE_SHIFT);
        SystemFree(ptr);
        _spanPool.Delete(span);
        return;
    }

    // merge from left
    while (true) {
        size_t leftPageId = span->_pageId - 1;
//        auto it = _idSpanMap.find(leftPageId);
//        if (it == _idSpanMap.end()) {
//            break;
//        }
        Span* it = (Span*)_idSpanMap.get(leftPageId);

        // not found
        if (it == nullptr) {
            break;
        }

        // exceed max span size
        if (span->_n + it->_n > NPAGES - 1) {
            break;
        }

        // in use
        if (it->_isUse) {
            break;
        }

        Span *leftSpan = it;
        _spanList[leftSpan->_n].Erase(leftSpan);
        span->_pageId = leftSpan->_pageId;
        span->_n += leftSpan->_n;
        _spanPool.Delete(leftSpan);
    }

    // merge from right
    while (true)
    {
        size_t rightPageId = span->_pageId + span->_n;
//        auto it = _idSpanMap.find(rightPageId);
//        if (it == _idSpanMap.end()) {
//            break;
//        }

        Span* it = (Span*)_idSpanMap.get(rightPageId);
        if (it == nullptr) {
            break;
        }

        if (span->_n + it->_n > NPAGES - 1) {
            break;
        }

        if (it->_isUse) {
            break;
        }

        Span *rightSpan = it;
        _spanList[rightSpan->_n].Erase(rightSpan);
        span->_n += rightSpan->_n;
        _spanPool.Delete(rightSpan);
    }

    span->_isUse = false;
    _spanList[span->_n].PushFront(span);
//    _idSpanMap[span->_pageId] = span;
//    _idSpanMap[span->_pageId + span->_n - 1] = span;
    _idSpanMap.set(span->_pageId, span);
    _idSpanMap.set(span->_pageId + span->_n - 1, span);

}
