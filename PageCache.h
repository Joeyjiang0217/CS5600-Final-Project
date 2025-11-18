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
    TCMalloc_PageMap1<32-PAGE_SHIFT> _idSpanMap;
    ObjectPool<Span> _spanPool;
    PageCache(){};
    PageCache(const PageCache&) = delete;
    PageCache& operator=(const PageCache&) = delete;
    static PageCache _sInst;

};

#endif //INC_5600FINALPROJECT_PAGECACHE_H
