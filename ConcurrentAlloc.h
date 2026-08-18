//
// Created by Joe on 2025/11/11.
//

#ifndef INC_5600FINALPROJECT_CONCURRENTALLOC_H
#define INC_5600FINALPROJECT_CONCURRENTALLOC_H
#include "Common.h"
#include "ThreadCache.h"
#include "PageCache.h"

// These are static functions in a header, so at -O2 the whole fast path inlines
// into the caller. malloc is a cross-library call that never can. Build with
// -DALLOC_NOINLINE to measure how much of the advantage that accounts for.
#ifdef ALLOC_NOINLINE
  #define ALLOC_ATTR __attribute__((noinline))
#else
  #define ALLOC_ATTR
#endif

static ALLOC_ATTR void* ConcurrentAlloc(size_t size) {
    // Handle large allocations directly through PageCache
    if (size > MAX_BYTES) {
        size_t alignedSize = SizeClass::RoundUp(size);
        size_t npage = alignedSize >> PAGE_SHIFT;
        PageCache::GetInstance()->_mtx.lock();
        Span* span = PageCache::GetInstance()->NewSpan(npage);
        span->_isUse = true;
        span->_objSize = alignedSize;
        PageCache::GetInstance()->_mtx.unlock();
        void* ptr = (void*)(span->_pageId << PAGE_SHIFT);
        return ptr;
    }
    // Use ThreadCache for small allocations

    // Initialize thread-local ThreadCache if not already done
    if (pTLSThreadCache == nullptr) {
        static ObjectPool<ThreadCache> tcPool;
        pTLSThreadCache = tcPool.New();
    }
    return pTLSThreadCache->Allocate(size);
}

static ALLOC_ATTR void ConcurrentFree(void* ptr) {
    Span* span = PageCache::GetInstance()->MapObjectToSpan(ptr);
    size_t size = span->_objSize;

    // Handle large deallocations directly through PageCache
    if (size > MAX_BYTES) {
        PageCache::GetInstance()->_mtx.lock();
        assert(span);
        PageCache::GetInstance()->ReleaseSpanToPageCache(span);
        PageCache::GetInstance()->_mtx.unlock();
        return;
    }
    assert(pTLSThreadCache);
    // Use ThreadCache for small deallocations
    pTLSThreadCache->Deallocate(ptr, size);
}



#endif //INC_5600FINALPROJECT_CONCURRENTALLOC_H
