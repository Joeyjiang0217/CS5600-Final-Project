#pragma once
#include "Common.h"
class ThreadCache
{
public:
	// Allocate and deallocate memory objects
	void* Allocate(size_t size);
	void Deallocate(void* ptr, size_t size);
	// Fetch objects from central cache
	void* FetchFromCentralCache(size_t index, size_t size);
private:
	FreeList _freeLists[NFREELIST];
};
// TLS thread local storage
static _declspec(thread) ThreadCache* pTLSThreadCache = nullptr;