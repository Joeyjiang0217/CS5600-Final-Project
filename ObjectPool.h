//
// Created by Joe on 2025/11/10.
//

#ifndef INC_5600FINALPROJECT_OBJECTPOOL_H
#define INC_5600FINALPROJECT_OBJECTPOOL_H
#include "Common.h"

// Simple thread-safe object pool
// Allocates objects of type T
// Uses a free list to recycle deleted objects
template<class T>
class ObjectPool {
public:
    T* New()
    {
        T* obj = nullptr;
        {
            std::unique_lock<std::mutex> lock(_mtx);
            if (_freeList != nullptr)
            {
                obj = (T*)_freeList;
                _freeList = *((void**)_freeList);
                new(obj)T;
                return obj;
            }

            if (_remainBytes < sizeof(T))
            {
                _remainBytes = 128 * 1024;
                _memory = (char*)SystemAlloc(_remainBytes>> 13);
                if (_memory == nullptr)
                    throw std::bad_alloc();
            }

            obj = (T*)_memory;
            size_t objSize = sizeof(T) < sizeof(void*) ? sizeof(void*) : sizeof(T);
            _memory += objSize;
            _remainBytes -= objSize;
        }

        new(obj)T;
        return obj;
    }

    void Delete(T* obj)
    {
        std::unique_lock<std::mutex> lock(_mtx);
        obj->~T();
        *((void**)obj) = _freeList;
        _freeList = obj;
    }


private:
    char* _memory = nullptr;
    void* _freeList = nullptr;
    size_t _remainBytes = 0;
    std::mutex _mtx;
};

#endif //INC_5600FINALPROJECT_OBJECTPOOL_H
