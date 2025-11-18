//
// Created by Joe on 2025/11/11.
//

#ifndef INC_5600FINALPROJECT_THREADCACHE_H
#define INC_5600FINALPROJECT_THREADCACHE_H
#include "Common.h"

class ThreadCache {
public:
    // Allocate memory of given size
    void* Allocate(size_t size);

    // Deallocate memory of given size
    void Deallocate(void* ptr, size_t size);

    // Fetch objects from the central cache
    void* FetchFromCentralCache(size_t index, size_t size);

    // Handle the case when the free list is too long
    void ListTooLong(FreeList& list, size_t size);
private:
    FreeList _freeList[NFREELISTS];
};

// Thread-local pointer to ThreadCache
static _declspec(thread) ThreadCache* pTLSThreadCache = nullptr;

#endif //INC_5600FINALPROJECT_THREADCACHE_H
