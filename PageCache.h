//
// Created by Joe on 2025/11/12.
//

#ifndef INC_5600FINALPROJECT_PAGECACHE_H
#define INC_5600FINALPROJECT_PAGECACHE_H
#include "Common.h"
#include "ObjectPool.h"

class PageCache {
public:
    static PageCache* GetInstance()
    {
        return &_sInst;
    }

    // Map the object pointer to its corresponding Span
    Span* MapObjectToSpan(void* obj);

    // Allocate a new Span with k pages
    Span* NewSpan(size_t k);

    // Release free spans back to the PageCache and merge adjacent spans
    void ReleaseSpanToPageCache(Span* span);
public:
    std::mutex _mtx;
private:
    SpanList _spanList[NPAGES];
//    std::unordered_map<PAGE_ID, Span*> _idSpanMap;
#if defined(_WIN64) || defined(__LP64__) || defined(_LP64)
    // 48-bit user virtual addresses is the common ceiling on x86-64 and arm64,
    // so 48 - PAGE_SHIFT page-ID bits covers everything mmap can return.
    TCMalloc_PageMapRadix64<48 - PAGE_SHIFT> _idSpanMap;
#else
    TCMalloc_PageMap1<32 - PAGE_SHIFT> _idSpanMap;
#endif
    ObjectPool<Span> _spanPool;
    PageCache(){};
    PageCache(const PageCache&) = delete;
    PageCache& operator=(const PageCache&) = delete;
    static PageCache _sInst;

};

#endif //INC_5600FINALPROJECT_PAGECACHE_H
