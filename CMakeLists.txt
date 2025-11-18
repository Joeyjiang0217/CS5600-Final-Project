cmake_minimum_required(VERSION 3.30)
project(5600FinalProject)

set(CMAKE_CXX_STANDARD 11)

add_executable(5600FinalProject main.cpp
        ObjectPool.h
        Common.h
        ThreadCache.h
        ThreadCache.cpp
        ConcurrentAlloc.h
        CentralCache.h
        CentralCache.cpp
        PageCache.h
        PageCache.cpp
        PageMap.h)
