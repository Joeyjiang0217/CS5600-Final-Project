#pragma once
#include <iostream>
#include <vector>
#include <thread>
#include <time.h>
#include <assert.h>
using std::cout;
using std::endl;
static const size_t MAX_BYTES = 256 * 1024;
static const size_t NFREELIST = 208;
static void*& NextObj(void* obj)
{
	return *(void**)obj;
}
// Manages free lists of small objects that have been carved up
class FreeList
{
public:
	void Push(void* obj)
	{
		assert(obj);
		// Head insertion
		//*(void**)obj = _freeList;
		NextObj(obj) = _freeList;
		_freeList = obj;
	}
	void* Pop()
	{
		assert(_freeList);
		// Head deletion
		void* obj = _freeList;
		_freeList = NextObj(obj);
		return obj;
	}
	bool Empty()
	{
		return _freeList == nullptr;
	}
private:
	void* _freeList = nullptr;
};
// Calculates alignment mapping rules for object sizes
class SizeClass
{
public:
	// Overall control to waste at most around 10% internal fragmentation
	// [1,128]					8byte alignment	    freelist[0,16)
	// [128+1,1024]				16byte alignment    freelist[16,72)
	// [1024+1,8*1024]			128byte alignment   freelist[72,128)
	// [8*1024+1,64*1024]		1024byte alignment  freelist[128,184)
	// [64*1024+1,256*1024]		8*1024byte alignment freelist[184,208)
	/*size_t _RoundUp(size_t size, size_t alignNum)
	{
		size_t alignSize;
		if (size % alignNum != 0)
		{
			alignSize = (size / alignNum + 1)*alignNum;
		}
		else
		{
			alignSize = size;
		}
		return alignSize;
	}*/
	// 1-8 
	static inline size_t _RoundUp(size_t bytes, size_t alignNum)
	{
		return ((bytes + alignNum - 1) & ~(alignNum - 1));
	}
	static inline size_t RoundUp(size_t size)
	{
		if (size <= 128)
		{
			return _RoundUp(size, 8);
		}
		else if (size <= 1024)
		{
			return _RoundUp(size, 16);
		}
		else if (size <= 8*1024)
		{
			return _RoundUp(size, 128);
		}
		else if (size <= 64*1024)
		{
			return _RoundUp(size, 1024);
		}
		else if (size <= 256 * 1024)
		{
			return _RoundUp(size, 8*1024);
		}
		else
		{
			assert(false);
			return -1;
		}
	}
	/*size_t _Index(size_t bytes, size_t alignNum)
	{
	if (bytes % alignNum == 0)
	{
	return bytes / alignNum - 1;
	}
	else
	{
	return bytes / alignNum;
	}
	}*/
	// 1 + 7  8
	// 2      9
	// ...
	// 8      15
	
	// 9 + 7 16
	// 10
	// ...
	// 16    23
	static inline size_t _Index(size_t bytes, size_t align_shift)
	{
		return ((bytes + (1 << align_shift) - 1) >> align_shift) - 1;
	}
	// Calculate which free list bucket to map to
	static inline size_t Index(size_t bytes)
	{
		assert(bytes <= MAX_BYTES);
		// Number of lists in each interval
		static int group_array[4] = { 16, 56, 56, 56 };
		if (bytes <= 128){
			return _Index(bytes, 3);
		}
		else if (bytes <= 1024){
			return _Index(bytes - 128, 4) + group_array[0];
		}
		else if (bytes <= 8 * 1024){
			return _Index(bytes - 1024, 7) + group_array[1] + group_array[0];
		}
		else if (bytes <= 64 * 1024){
			return _Index(bytes - 8 * 1024, 10) + group_array[2] + group_array[1] + group_array[0];
		}
		else if (bytes <= 256 * 1024){
			return _Index(bytes - 64 * 1024, 13) + group_array[3] + group_array[2] + group_array[1] + group_array[0];
		}
		else{
			assert(false);
		}
		return -1;
	}
};