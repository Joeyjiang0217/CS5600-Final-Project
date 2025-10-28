#include "ThreadCache.h"
void* ThreadCache::FetchFromCentralCache(size_t index, size_t size)
{
	// ...
	return nullptr;
}
void* ThreadCache::Allocate(size_t size)
{
	assert(size <= MAX_BYTES);
	size_t alignSize = SizeClass::RoundUp(size);
	size_t index = SizeClass::Index(size);
	if (!_freeLists[index].Empty())
	{
		return _freeLists[index].Pop();
	}
	else
	{
		return FetchFromCentralCache(index, alignSize);
	}
}
void ThreadCache::Deallocate(void* ptr, size_t size)
{
	assert(ptr);
	assert(size <= MAX_BYTES);
	// Find the corresponding free list bucket and insert the object
	size_t index = SizeClass::Index(size);
	_freeLists[index].Push(ptr);
}