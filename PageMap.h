//
// Created by Joe on 2025/11/17.
//

#ifndef INC_5600FINALPROJECT_PAGEMAP_H
#define INC_5600FINALPROJECT_PAGEMAP_H
#include"Common.h"

// Single-level array
template <int BITS>
class TCMalloc_PageMap1 {
private:
    static const int LENGTH = 1 << BITS;
    void** array_;

public:
    typedef uintptr_t Number;

    //explicit TCMalloc_PageMap1(void* (*allocator)(size_t)) {
    explicit TCMalloc_PageMap1() {
        //array_ = reinterpret_cast<void**>((*allocator)(sizeof(void*) << BITS));
        size_t size = sizeof(void*) << BITS;
        size_t alignSize = SizeClass::_RoundUp(size, 1<<PAGE_SHIFT);
        array_ = (void**)SystemAlloc(alignSize>>PAGE_SHIFT);
        memset(array_, 0, sizeof(void*) << BITS);
    }

    // Return the current value for KEY.  Returns NULL if not yet set,
    // or if k is out of range.
    void* get(Number k) const {
        if ((k >> BITS) > 0) {
            return NULL;
        }
        return array_[k];
    }

    // REQUIRES "k" is in range "[0,2^BITS-1]".
    // REQUIRES "k" has been ensured before.
    //
    // Sets the value 'v' for key 'k'.
    void set(Number k, void* v) {
        array_[k] = v;
    }
};

// Two-level radix tree
template <int BITS>
class TCMalloc_PageMap2 {
private:
    // Put 32 entries in the root and (2^BITS)/32 entries in each leaf.
    static const int ROOT_BITS = 5;
    static const int ROOT_LENGTH = 1 << ROOT_BITS;

    static const int LEAF_BITS = BITS - ROOT_BITS;
    static const int LEAF_LENGTH = 1 << LEAF_BITS;

    // Leaf node
    struct Leaf {
        void* values[LEAF_LENGTH];
    };

    Leaf* root_[ROOT_LENGTH];             // Pointers to 32 child nodes
    void* (*allocator_)(size_t);          // Memory allocator

public:
    typedef uintptr_t Number;

    //explicit TCMalloc_PageMap2(void* (*allocator)(size_t)) {
    explicit TCMalloc_PageMap2() {
        //allocator_ = allocator;
        memset(root_, 0, sizeof(root_));

        PreallocateMoreMemory();
    }

    void* get(Number k) const {
        const Number i1 = k >> LEAF_BITS;
        const Number i2 = k & (LEAF_LENGTH - 1);
        if ((k >> BITS) > 0 || root_[i1] == NULL) {
            return NULL;
        }
        return root_[i1]->values[i2];
    }

    void set(Number k, void* v) {
        const Number i1 = k >> LEAF_BITS;
        const Number i2 = k & (LEAF_LENGTH - 1);
        assert(i1 < ROOT_LENGTH);
        root_[i1]->values[i2] = v;
    }

    bool Ensure(Number start, size_t n) {
        for (Number key = start; key <= start + n - 1;) {
            const Number i1 = key >> LEAF_BITS;

            // Check for overflow
            if (i1 >= ROOT_LENGTH)
                return false;

            // Make 2nd level node if necessary
            if (root_[i1] == NULL) {
                //Leaf* leaf = reinterpret_cast<Leaf*>((*allocator_)(sizeof(Leaf)));
                //if (leaf == NULL) return false;
                static ObjectPool<Leaf>	leafPool;
                Leaf* leaf = (Leaf*)leafPool.New();

                memset(leaf, 0, sizeof(*leaf));
                root_[i1] = leaf;
            }

            // Advance key past whatever is covered by this leaf node
            key = ((key >> LEAF_BITS) + 1) << LEAF_BITS;
        }
        return true;
    }

    void PreallocateMoreMemory() {
        // Allocate enough to keep track of all possible pages
        Ensure(0, 1 << BITS);
    }
};

// Three-level radix tree sized for a 64-bit address space.
//
// Why this exists: TCMalloc_PageMap1<32-PAGE_SHIFT> above is a flat array
// covering 2^19 page IDs, i.e. the low 4 GB only. That works for a 32-bit
// build (the original target was -A Win32), but on a 64-bit host mmap returns
// addresses well above 4 GB, every page ID fails the (k >> BITS) bound check,
// get() returns NULL for perfectly valid pointers, and the first free crashes.
//
// Why three levels and not two: with BITS = 48 - 13 = 35, splitting in half
// puts 2^18 entries in a leaf, which is 2 MB per node. ObjectPool refills in
// fixed 128 KB chunks, so a 2 MB object silently overruns it. Splitting three
// ways keeps every node at 16-32 KB. Real tcmalloc switches to a three-level
// map on 64-bit for the same reason.
//
// Nodes come straight from SystemAlloc rather than ObjectPool so this structure
// has no dependency on the pool's chunk size, are created lazily, and are never
// freed -- so a pointer that was once published stays valid, which is what lets
// get() run without taking the PageCache lock.
template <int BITS>
class TCMalloc_PageMapRadix64 {
private:
    static const int LEAF_BITS = BITS / 3;
    static const int MID_BITS  = (BITS - LEAF_BITS) / 2;
    static const int ROOT_BITS = BITS - LEAF_BITS - MID_BITS;

    static const size_t LEAF_LENGTH = (size_t)1 << LEAF_BITS;
    static const size_t MID_LENGTH  = (size_t)1 << MID_BITS;
    static const size_t ROOT_LENGTH = (size_t)1 << ROOT_BITS;

    struct Leaf { std::atomic<void*> values[LEAF_LENGTH]; };
    struct Mid  { std::atomic<Leaf*> leaves[MID_LENGTH]; };

    // Every node is comfortably under ObjectPool's 128 KB chunk, and under any
    // reasonable page-granularity allocation.
    static_assert(sizeof(Leaf) <= 128 * 1024, "leaf node too large");
    static_assert(sizeof(Mid)  <= 128 * 1024, "mid node too large");

    std::atomic<Mid*>* root_;   // ROOT_LENGTH entries

    // Zero-filled pages straight from the OS.
    template <class T>
    static T* NewNode() {
        size_t alignSize = SizeClass::_RoundUp(sizeof(T), 1 << PAGE_SHIFT);
        void* mem = SystemAlloc(alignSize >> PAGE_SHIFT);
        // mmap and VirtualAlloc both hand back zeroed pages, but be explicit --
        // these slots are read without a lock and must not start as garbage.
        memset(mem, 0, alignSize);
        return (T*)mem;
    }

public:
    typedef uintptr_t Number;

    explicit TCMalloc_PageMapRadix64() {
        size_t bytes = sizeof(std::atomic<Mid*>) * ROOT_LENGTH;
        size_t alignSize = SizeClass::_RoundUp(bytes, 1 << PAGE_SHIFT);
        root_ = (std::atomic<Mid*>*)SystemAlloc(alignSize >> PAGE_SHIFT);
        memset(root_, 0, alignSize);
    }

    // Called on every free, without the PageCache lock held. Acquire loads pair
    // with the release stores in set() so a reader that sees a node pointer also
    // sees the zeroed contents behind it.
    void* get(Number k) const {
        if ((k >> BITS) > 0) {
            return nullptr;
        }
        const Number i1 = k >> (LEAF_BITS + MID_BITS);
        const Number i2 = (k >> LEAF_BITS) & (MID_LENGTH - 1);
        const Number i3 = k & (LEAF_LENGTH - 1);

        Mid* mid = root_[i1].load(std::memory_order_acquire);
        if (mid == nullptr) return nullptr;
        Leaf* leaf = mid->leaves[i2].load(std::memory_order_acquire);
        if (leaf == nullptr) return nullptr;
        return leaf->values[i3].load(std::memory_order_acquire);
    }

    // Callers hold the PageCache lock, so the node creation below cannot race
    // with another set(). It can still race with a concurrent get(), which is
    // why the stores are release stores.
    void set(Number k, void* v) {
        assert((k >> BITS) == 0);
        const Number i1 = k >> (LEAF_BITS + MID_BITS);
        const Number i2 = (k >> LEAF_BITS) & (MID_LENGTH - 1);
        const Number i3 = k & (LEAF_LENGTH - 1);

        Mid* mid = root_[i1].load(std::memory_order_relaxed);
        if (mid == nullptr) {
            mid = NewNode<Mid>();
            root_[i1].store(mid, std::memory_order_release);
        }
        Leaf* leaf = mid->leaves[i2].load(std::memory_order_relaxed);
        if (leaf == nullptr) {
            leaf = NewNode<Leaf>();
            mid->leaves[i2].store(leaf, std::memory_order_release);
        }
        leaf->values[i3].store(v, std::memory_order_release);
    }
};

// Three-level radix tree
template <int BITS>
class TCMalloc_PageMap3 {
private:
    // How many bits should we consume at each interior level
    static const int INTERIOR_BITS = (BITS + 2) / 3; // Round-up
    static const int INTERIOR_LENGTH = 1 << INTERIOR_BITS;

    // How many bits should we consume at leaf level
    static const int LEAF_BITS = BITS - 2 * INTERIOR_BITS;
    static const int LEAF_LENGTH = 1 << LEAF_BITS;

    // Interior node
    struct Node {
        Node* ptrs[INTERIOR_LENGTH];
    };

    // Leaf node
    struct Leaf {
        void* values[LEAF_LENGTH];
    };

    Node* root_;                          // Root of radix tree
    void* (*allocator_)(size_t);          // Memory allocator

    Node* NewNode() {
        Node* result = reinterpret_cast<Node*>((*allocator_)(sizeof(Node)));
        if (result != NULL) {
            memset(result, 0, sizeof(*result));
        }
        return result;
    }

public:
    typedef uintptr_t Number;

    explicit TCMalloc_PageMap3(void* (*allocator)(size_t)) {
        allocator_ = allocator;
        root_ = NewNode();
    }

    void* get(Number k) const {
        const Number i1 = k >> (LEAF_BITS + INTERIOR_BITS);
        const Number i2 = (k >> LEAF_BITS) & (INTERIOR_LENGTH - 1);
        const Number i3 = k & (LEAF_LENGTH - 1);
        if ((k >> BITS) > 0 ||
            root_->ptrs[i1] == NULL || root_->ptrs[i1]->ptrs[i2] == NULL) {
            return NULL;
        }
        return reinterpret_cast<Leaf*>(root_->ptrs[i1]->ptrs[i2])->values[i3];
    }

    void set(Number k, void* v) {
        assert((k >> BITS) == 0);
        const Number i1 = k >> (LEAF_BITS + INTERIOR_BITS);
        const Number i2 = (k >> LEAF_BITS) & (INTERIOR_LENGTH - 1);
        const Number i3 = k & (LEAF_LENGTH - 1);
        reinterpret_cast<Leaf*>(root_->ptrs[i1]->ptrs[i2])->values[i3] = v;
    }

    bool Ensure(Number start, size_t n) {
        for (Number key = start; key <= start + n - 1;) {
            const Number i1 = key >> (LEAF_BITS + INTERIOR_BITS);
            const Number i2 = (key >> LEAF_BITS) & (INTERIOR_LENGTH - 1);

            // Check for overflow
            if (i1 >= INTERIOR_LENGTH || i2 >= INTERIOR_LENGTH)
                return false;

            // Make 2nd level node if necessary
            if (root_->ptrs[i1] == NULL) {
                Node* n = NewNode();
                if (n == NULL) return false;
                root_->ptrs[i1] = n;
            }

            // Make leaf node if necessary
            if (root_->ptrs[i1]->ptrs[i2] == NULL) {
                Leaf* leaf = reinterpret_cast<Leaf*>((*allocator_)(sizeof(Leaf)));
                if (leaf == NULL) return false;
                memset(leaf, 0, sizeof(*leaf));
                root_->ptrs[i1]->ptrs[i2] = reinterpret_cast<Node*>(leaf);
            }

            // Advance key past whatever is covered by this leaf node
            key = ((key >> LEAF_BITS) + 1) << LEAF_BITS;
        }
        return true;
    }

    void PreallocateMoreMemory() {
    }
};
#endif //INC_5600FINALPROJECT_PAGEMAP_H
