//
// Created by Joe on 2025/11/11.
//

#ifndef INC_5600FINALPROJECT_CENTRALCACHE_H
#define INC_5600FINALPROJECT_CENTRALCACHE_H
#include "Common.h"

class CentralCache {
public:
    static CentralCache* GetInstance()
    {
        return &_sInst;
    }
    // Get a span that has free objects
    Span* GetOneSpan(SpanList& list, size_t size);

    // Fetch a range of objects from the central cache
    size_t FetchRangeObj(void*& start, void*& end, size_t batchNum, size_t size);

    // Release a list of objects back to spans
    void ReleaseListToSpans(void* start, size_t byte_size);
private:
    SpanList _spanLists[NFREELISTS];
private:
    static CentralCache _sInst; // singleton
    CentralCache() {}
    CentralCache(const CentralCache&) = delete;
    CentralCache& operator=(const CentralCache&) = delete;
};

#endif //INC_5600FINALPROJECT_CENTRALCACHE_H
