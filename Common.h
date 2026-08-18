//
// Created by Joe on 2025/11/11.
//

#ifndef INC_5600FINALPROJECT_COMMON_H
#define INC_5600FINALPROJECT_COMMON_H
#include <iostream>
#include <vector>
#include <thread>
#include <mutex>
#include <unordered_map>
#include <algorithm>
#include <ctime>
#include <cassert>
#include <atomic>

// NOTE ON INCLUDE ORDER
// ObjectPool.h needs SystemAlloc, and PageMap.h needs SystemAlloc, PAGE_SHIFT
// and SizeClass::_RoundUp. Both used to be included up here, which only
// compiled under MSVC because it resolves dependent names in templates far more
// loosely than the standard allows. Clang and GCC reject it. Each include now
// sits directly after the declarations it depends on.

using std::cout;
using std::endl;

static const size_t MAX_BYTES = 256 * 1024;

// EXPERIMENT KNOB -- the alignment used for the 1 KB .. 8 KB range.
//
// 7 (128 bytes) is the original. libmalloc quantises that same range to 512
// bytes -- measured directly with malloc_size, BENCHMARKS.md section K -- and
// that 4x coarser granularity is half of why it beats us on ramp/interleaved:
// the ramp lingers 4x longer in each bucket, so the drain/fill penalty is
// spread over 4x more allocations. Building with -DMID_ALIGN_SHIFT=9 matches
// its granularity, which turns that mechanism from plausible into testable.
#ifndef MID_ALIGN_SHIFT
#define MID_ALIGN_SHIFT 7
#endif
static const size_t MID_ALIGN = (size_t)1 << MID_ALIGN_SHIFT;

// Classes per alignment group: 128/8, (1024-128)/16, (8192-1024)/MID_ALIGN,
// (64K-8K)/1K, (256K-64K)/8K. The knob moves only the third term -- 56 classes
// at 128-byte alignment, 14 at 512 -- so NFREELISTS has to follow it.
static const size_t NCLASS_MID = 7168 >> MID_ALIGN_SHIFT;
static const size_t NFREELISTS = 16 + 56 + NCLASS_MID + 56 + 24;
static const size_t NPAGES = 129;
static const size_t PAGE_SHIFT = 13; // 8KB

#ifdef _WIN32
    // <windows.h> defines min and max as macros unless NOMINMAX is set, which
    // breaks every std::max call that follows it -- under MSVC the peak-RSS
    // tracking in main.cpp fails to compile without this. MinGW's headers do
    // not hit it, so a GCC-only build will not catch the omission.
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#else
    #include <sys/mman.h>
    #include <cstring>
#endif

// Page IDs are (address >> PAGE_SHIFT). On a 64-bit host the address space is
// far wider than 32 bits, so this must be a 64-bit type -- see PageMap.h for
// why the page map itself also has to change.
#if defined(_WIN64) || defined(__LP64__) || defined(_LP64)
    typedef unsigned long long PAGE_ID;
#else
    typedef size_t PAGE_ID;
#endif

// Request `kpage` pages straight from the OS's virtual-memory interface,
// bypassing the C library allocator we are being compared against.
inline static void* SystemAlloc(size_t kpage) {
    void* ptr = nullptr;
#ifdef _WIN32
    ptr = VirtualAlloc(nullptr, kpage * (1 << PAGE_SHIFT), MEM_COMMIT | MEM_RESERVE,
                       PAGE_READWRITE);
#else
    ptr = mmap(nullptr, kpage * (1 << PAGE_SHIFT), PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED)
        ptr = nullptr;
#endif
    if (ptr == nullptr)
        throw std::bad_alloc();
    return ptr;
}

// munmap needs the length, which VirtualFree does not -- so callers pass the
// page count they originally asked for.
inline static void SystemFree(void* ptr, size_t kpage)
{
#ifdef _WIN32
    (void)kpage;
    VirtualFree(ptr, 0, MEM_RELEASE);
#else
    munmap(ptr, kpage * (1 << PAGE_SHIFT));
#endif
}

#include "ObjectPool.h"   // depends on SystemAlloc above

static void*& NextObj(void* obj)
{
    return *((void**)obj);
}

// This class provides methods to push and pop memory blocks to/from a free list,
// as well as operations to handle ranges of memory blocks. It is designed to
// manage memory efficiently by reusing freed memory blocks.
class FreeList {
public:
    void Push(void* obj)
    {
        assert(obj != nullptr);
        NextObj(obj) = _freelist;
        _freelist = obj;
        ++_size;
    }

    void* Pop()
    {
        assert(_freelist != nullptr);
        void* obj = _freelist;
        _freelist = NextObj(_freelist);
        --_size;
        return obj;
    }

    bool Empty()
    {
        return _freelist == nullptr;
    }

    size_t& MaxSize()
    {
        return _maxSize;
    }

    void PushRange(void* start, void* end, size_t n)
    {
        assert(start != nullptr && end != nullptr);
        NextObj(end) = _freelist;
        _freelist = start;
        _size += n;
    }

    void PopRange(void*& start, void*& end, size_t n)
    {
        // This read `assert(n >= _size)`, which is backwards: you cannot pop
        // more than the list holds. It never fired because the only caller
        // popped exactly MaxSize() at the moment Size() had just reached it, so
        // n == _size and both directions happened to hold. Any change to the
        // flush amount trips it -- which is exactly what happened when the
        // flush gained a low-water mark.
        assert(n > 0);
        assert(n <= _size);
        start = _freelist;
        end = start;
        for (size_t i = 0; i < n - 1; ++i)
        {
            end = NextObj(end);
        }
        _freelist = NextObj(end);
        NextObj(end) = nullptr;
        _size -= n;
    }

    size_t Size()
    {
        return _size;
    }

private:
    void* _freelist = nullptr;
    size_t _maxSize = 1;
    size_t _size = 0;
};


// SizeClass handles size rounding and index calculation for memory allocation.
// It provides methods to round up sizes to the nearest alignment and to
// calculate the corresponding index in the free list based on the size.
class SizeClass {
public:
    static inline size_t _RoundUp(size_t bytes, size_t alignNum)
    {
        return (bytes + alignNum - 1) & ~(alignNum - 1);
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
        else if (size <= 8 * 1024)
        {
            return _RoundUp(size, MID_ALIGN);
        }
        else if (size <= 64 * 1024)
        {
            return _RoundUp(size, 8 * 128);
        }
        else if (size <= 256 * 1024)
        {
            return _RoundUp(size,  64 * 128);
        }
        else
        {
            return _RoundUp(size, 1 << PAGE_SHIFT);
        }
    }

    static inline size_t _Index(size_t bytes, size_t align_shift)
    {
        return ((bytes + (1 << align_shift) - 1) >> align_shift) - 1;
    }

    static inline size_t Index(size_t bytes)
    {
        assert(bytes <= MAX_BYTES);

        static const size_t group_array[4] = { 16, 56, NCLASS_MID, 56 };
        if (bytes <= 128){
            return _Index(bytes, 3);
        }

        else if (bytes <= 1024)
        {
            return _Index(bytes - 128, 4) + group_array[0];
        }
        else if (bytes <= 8 * 1024)
        {
            return _Index(bytes - 1024, MID_ALIGN_SHIFT) + group_array[1] + group_array[0];
        }
        else if (bytes <= 64*1024)
        {
            return _Index(bytes - 8*1024, 10) + group_array[2] +
                   group_array[1] + group_array[0];
        }
        else if (bytes <= 256 * 1024){
            return _Index(bytes - 64 * 1024, 13) + group_array[3] +
                   group_array[2] + group_array[1] + group_array[0];
        }
        else
        {
            assert(false);
        }
        return -1;
    }

    static inline size_t NumMoveSize(size_t size)
    {
        assert(size <= MAX_BYTES);
        size_t num = MAX_BYTES / size;
        if (num < 2)
            num = 2;
        if (num > 512)
            num = 512;
        return num;
    }

    static size_t NumMovePage(size_t size)
    {
        size_t num = NumMoveSize(size);
        size_t npage = num*size;
        npage >>= PAGE_SHIFT;
        if (npage == 0)
            npage = 1;
        return npage;
    }
};

#include "PageMap.h"   // depends on SizeClass::_RoundUp above

// Span represents a contiguous block of memory pages.
// It contains metadata about the span, including its page ID, size,
// usage count, object size, and free list of objects within the span.
struct Span
{
    PAGE_ID _pageId = 0;
    size_t _n = 0;
    Span* _next = nullptr;
    Span* _prev = nullptr;
    size_t _useCount = 0;
    size_t _objSize = 0;
    void* _freeList = nullptr;
    bool _isUse = false;
};

// SpanList manages a doubly linked list of Span objects.
// It provides methods to insert, erase, push, and pop spans from the list.
class SpanList
{
public:
    SpanList()
    {
        static ObjectPool<Span> spanPool;
        _head = spanPool.New();
        _head->_next = _head;
        _head->_prev = _head;
    }

    Span* Begin()
    {
        return _head->_next;
    }

    Span* End()
    {
        return _head;
    }

    void PushFront(Span* span)
    {
        Insert(Begin(), span);
    }

    Span* PopFront()
    {
        assert(!Empty());
        Span* first = Begin();
        Erase(first);
        return first;
    }

    bool Empty()
    {
        return Begin() == End();
    }

    void Insert(Span* pos, Span* newSpan)
    {
        assert(pos != nullptr && newSpan != nullptr);
        newSpan->_next = pos;
        newSpan->_prev = pos->_prev;
        pos->_prev->_next = newSpan;
        pos->_prev = newSpan;
    }

    void Erase(Span* span)
    {
        assert(span != nullptr);
        assert(span != _head);
        span->_prev->_next = span->_next;
        span->_next->_prev = span->_prev;
        span->_next = nullptr;
        span->_prev = nullptr;
    }
private:
    Span* _head = nullptr;
public:
    std::mutex _mtx;
};

#endif //INC_5600FINALPROJECT_COMMON_H
