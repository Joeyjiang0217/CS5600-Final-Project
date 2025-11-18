//
// Created by Joe on 2025/11/11.
//
#include "CentralCache.h"
#include "PageCache.h"

CentralCache CentralCache::_sInst;

Span* CentralCache::GetOneSpan(SpanList& list, size_t size) {
    // 1. Try to find a span with free objects
    Span* it = list.Begin();
    while (it != list.End()) {
        if (it->_freeList != nullptr) {
            return it;
        }
        it = it->_next;
    }

    list._mtx.unlock(); // unlock the bucket mutex so that other threads can give free objects back

    // 2. No span with free objects, fetch a new span from PageCache
    PageCache::GetInstance()->_mtx.lock();
    Span* newSpan = PageCache::GetInstance()->NewSpan(SizeClass::NumMovePage(size));
    newSpan->_isUse = true;
    newSpan->_objSize = size;
    PageCache::GetInstance()->_mtx.unlock();
    assert(newSpan);

    // Initialize the free list in the new span
    char* start = (char*)(newSpan->_pageId << PAGE_SHIFT);
    char* end = start + (newSpan->_n << PAGE_SHIFT);
    newSpan->_freeList = start;
    start += size;
    void* tail = newSpan->_freeList;
    while (start + size < end) {
        NextObj(tail) = start;
        tail = start;
        start += size;
    }

    NextObj(tail) = nullptr;
    list._mtx.lock();
    list.PushFront(newSpan);
    return newSpan;
}

size_t CentralCache::FetchRangeObj(void*& start, void*& end, size_t batchNum, size_t size) {
    assert(batchNum > 0);
    assert(size <= MAX_BYTES);
    size_t index = SizeClass::Index(size);

    SpanList& spanList = _spanLists[index];
    spanList._mtx.lock();
    Span* span = GetOneSpan(spanList, size);
    assert(span);
    assert(span->_freeList != nullptr);

    // Fetch objects from the span's free list
    size_t actualNum = 1;
    start = span->_freeList;
    end = start;
    size_t leftNum = batchNum - 1;
    while (leftNum > 0 && NextObj(end) != nullptr) {
        end = NextObj(end);
        --leftNum;
        ++actualNum;
    }
    span->_freeList = NextObj(end);
    NextObj(end) = nullptr;
    span->_useCount += actualNum;
    spanList._mtx.unlock();
    return actualNum;
}

void CentralCache::ReleaseListToSpans(void* start, size_t byte_size) {
    assert(start != nullptr);
    assert(byte_size <= MAX_BYTES);

    size_t index = SizeClass::Index(byte_size);
    SpanList& spanList = _spanLists[index];
    spanList._mtx.lock();

    while (start != nullptr) {
        Span* span = PageCache::GetInstance()->MapObjectToSpan(start);
        assert(span);
        void* next = NextObj(start);
        NextObj(start) = span->_freeList;
        span->_freeList = start;
        --span->_useCount;

        // If the span is completely free, release it back to PageCache
        if (span->_useCount == 0) {
            spanList.Erase(span);
            span->_freeList = nullptr;
            span->_prev = nullptr;
            span->_next = nullptr;


            spanList._mtx.unlock();
            PageCache::GetInstance()->_mtx.lock();
            PageCache::GetInstance()->ReleaseSpanToPageCache(span);
            PageCache::GetInstance()->_mtx.unlock();
            spanList._mtx.lock();
        }
        start = next;
    }
    spanList._mtx.unlock();

}



