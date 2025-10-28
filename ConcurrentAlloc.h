#pragma once
#include "Common.h"
#include "ThreadCache.h"
static void* ConcurrentAlloc(size_t size)
{
	// Through TLS, each thread obtains its own exclusive ThreadCache object without locks
	if (pTLSThreadCache == nullptr)
	{
		pTLSThreadCache = new ThreadCache;
	}
	cout << std::this_thread::get_id() << ":"<<pTLSThreadCache<<endl;
	return pTLSThreadCache->Allocate(size);
}
static void ConcurrentFree(void* ptr, size_t size)
{
	assert(pTLSThreadCache);
	pTLSThreadCache->Deallocate(ptr, size);
}