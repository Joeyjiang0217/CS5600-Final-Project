//
// verify.cpp -- correctness gate, run before any timing is believed.
//
// This walks the same `--size` x `--pattern` matrix as the benchmark harness
// but checks invariants instead of measuring. It exists because ASan is a
// weaker tool than it looks for this particular target: ASan works by
// interposing on malloc/free, and ConcurrentAlloc carries no
// ASAN_POISON_MEMORY_REGION annotations anywhere, so the sanitizer cannot see
// inside its free lists. It would validate the malloc side of the comparison
// and generic stack/global errors, and largely not the allocator under test.
//
// The invariants checked here are the allocator's own contract:
//
//   1. never returns null for a satisfiable request
//   2. returns a pointer aligned to at least sizeof(void*) -- the free lists
//      store a `next` pointer in the first word of every free block, so an
//      underaligned or undersized block corrupts the list itself
//   3. the whole requested extent is writable
//   4. no two simultaneously live blocks share a byte. Every block is stamped
//      with a tag unique to (thread, allocation) and re-verified immediately
//      before it is freed, so handing the same memory out twice, or handing
//      out a block that overlaps a live one, or returning a block smaller
//      than requested, all show up as a corrupted stamp.
//   5. cross-thread free works -- under `--pattern cross` the stamp is
//      written by one thread and verified by another.
//
// The stamp check is deliberately lock-free so that the concurrency profile
// stays close to the benchmark's. `--registry` adds an exact duplicate-live-
// pointer check on top, which needs a global mutex and therefore serialises
// the allocator; run it as a second pass, not as the only pass.
//
// Run with `--alloc sys` to check the checker. Any failure reported against
// the system allocator means this file is wrong, not the allocator.
//
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <set>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <random>
#include <unordered_set>
#include <algorithm>

#include "ConcurrentAlloc.h"

enum class VSizeMode { Fixed16, Ramp, Random, ClassStep };
enum class VPattern  { BulkThenFree, Interleaved, CrossThread };
enum class AllocMode { Mine, Sys, Broken };

struct VConfig {
    size_t    ntimes = 500;
    size_t    nworks = 4;
    size_t    rounds = 3;
    VSizeMode sizeMode = VSizeMode::Ramp;
    VPattern  pattern  = VPattern::BulkThenFree;
    unsigned  seed   = 12345;
    size_t    window = 64;
    AllocMode mode   = AllocMode::Mine;
    bool      registry = false;
};

// A deliberately wrong allocator, used as a negative control. It hands the
// same slab out repeatedly, which is precisely the failure this file exists to
// detect. If `--alloc broken` reports PASS, the checks above are not actually
// checking anything and nothing else in this file should be trusted.
static void* BrokenAlloc(size_t n) {
    // The slab has to be large enough that even the boundary check's biggest
    // request (2 x MAX_BYTES) stays inside it. A negative control must be
    // detectably wrong without being memory-unsafe for the harness itself --
    // a 64 KB slab let the boundary check write past the end and abort before
    // reporting anything, which made the control useless rather than failing.
    // The wrap has to stay small so that live blocks start overlapping almost
    // immediately; the slab has to be larger than the wrap plus the biggest
    // request the checks make, so nothing writes past the end. Sizing the slab
    // alone is not enough -- widening the wrap along with it silently stopped
    // small allocations from colliding, and fixed/bulk started passing under
    // the negative control, which is exactly the failure mode this control
    // exists to rule out.
    static const size_t kWrap = 32 * 1024;
    static const size_t kSlab = kWrap + 2 * MAX_BYTES;
    static unsigned char slab[kSlab];
    static std::atomic<size_t> cursor{0};
    // The 64-byte stride means any request above 64 bytes already overlaps.
    size_t off = cursor.fetch_add(64) % kWrap;
    (void)n;
    return slab + off;
}
static void BrokenFree(void*) {}

// Same class-size table and same SizeFor as the harness, so the verified
// workload is the measured workload and not an approximation of it.
static const std::vector<size_t>& ClassSizes() {
    static std::vector<size_t> v = [] {
        std::vector<size_t> t;
        for (size_t s = 8;    s <= 128;  s += 8)   t.push_back(s);
        for (size_t s = 144;  s <= 1024; s += 16)  t.push_back(s);
        for (size_t s = 1152; s <= 8192; s += 128) t.push_back(s);
        return t;
    }();
    return v;
}

static inline size_t SizeFor(VSizeMode m, size_t i, std::mt19937& rng) {
    switch (m) {
        case VSizeMode::Fixed16:   return 16;
        case VSizeMode::Ramp:      return (16 + i) % 8192 + 1;
        case VSizeMode::Random:    return rng() % 8192 + 1;
        case VSizeMode::ClassStep: {
            const std::vector<size_t>& cs = ClassSizes();
            return cs[i % cs.size()];
        }
    }
    return 16;
}

// ------------------------------------------------------------------- stamping

static inline uint64_t Mix(uint64_t x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

// Fill [p, p+n) with a byte sequence derived from `tag`. Two different tags
// disagree in essentially every byte, so an overlap of even one byte between
// two live blocks is caught.
static void FillBlock(void* p, size_t n, uint64_t tag) {
    unsigned char* b = static_cast<unsigned char*>(p);
    size_t chunk = 0;
    for (size_t i = 0; i < n; i += 8, ++chunk) {
        uint64_t v = Mix(tag ^ chunk);
        size_t m = (n - i) < 8 ? (n - i) : 8;
        memcpy(b + i, &v, m);
    }
}

// Returns true if intact; otherwise sets *badOff to the first differing byte.
static bool CheckBlock(const void* p, size_t n, uint64_t tag, size_t* badOff) {
    const unsigned char* b = static_cast<const unsigned char*>(p);
    size_t chunk = 0;
    for (size_t i = 0; i < n; i += 8, ++chunk) {
        uint64_t v = Mix(tag ^ chunk);
        size_t m = (n - i) < 8 ? (n - i) : 8;
        if (memcmp(b + i, &v, m) != 0) {
            const unsigned char* vb = reinterpret_cast<const unsigned char*>(&v);
            for (size_t j = 0; j < m; ++j)
                if (b[i + j] != vb[j]) { *badOff = i + j; return false; }
            *badOff = i;
            return false;
        }
    }
    return true;
}

// ------------------------------------------------------------------ reporting

struct Failures {
    std::atomic<size_t> nullPtr{0};
    std::atomic<size_t> misaligned{0};
    std::atomic<size_t> corrupted{0};
    std::atomic<size_t> duplicate{0};
    std::mutex mu;
    std::vector<std::string> messages;   // capped, first few are enough

    void Note(const std::string& s) {
        std::lock_guard<std::mutex> g(mu);
        if (messages.size() < 12) messages.push_back(s);
    }
    size_t Total() const {
        return nullPtr.load() + misaligned.load()
             + corrupted.load() + duplicate.load();
    }
};

// A live-pointer set, only used under --registry. Exact duplicate detection:
// if an allocation returns a pointer already recorded as live, the allocator
// handed the same block out twice.
struct Registry {
    std::mutex mu;
    std::unordered_set<void*> live;
    bool enabled = false;

    bool Insert(void* p) {
        if (!enabled) return true;
        std::lock_guard<std::mutex> g(mu);
        return live.insert(p).second;
    }
    void Erase(void* p) {
        if (!enabled) return;
        std::lock_guard<std::mutex> g(mu);
        live.erase(p);
    }
};

// ------------------------------------------------------------------ the check

struct Block { void* p; size_t n; uint64_t tag; };

template <class AllocFn, class FreeFn>
static void RunCheck(const VConfig& cfg, AllocFn Alloc, FreeFn Free,
                     Failures& f, Registry& reg) {
    std::vector<std::thread> threads(cfg.nworks);

    // Cross-thread: slot arrays are filled by thread k and freed by thread
    // k-1, so the tag and size travel with the pointer.
    std::vector<std::vector<Block>> shared(cfg.nworks);
    for (auto& v : shared) v.resize(cfg.ntimes, Block{nullptr, 0, 0});
    std::atomic<size_t> barrier{0};

    auto WaitAtBarrier = [&]() {
        size_t n = cfg.nworks;
        size_t gen = barrier.load(std::memory_order_acquire) / n;
        if (barrier.fetch_add(1, std::memory_order_acq_rel) + 1 == (gen + 1) * n)
            return;
        while (barrier.load(std::memory_order_acquire) / n == gen)
            std::this_thread::yield();
    };

    for (size_t k = 0; k < cfg.nworks; ++k) {
        threads[k] = std::thread([&, k]() {
            std::mt19937 rng(cfg.seed + static_cast<unsigned>(k));
            uint64_t counter = 0;

            // Allocate, validate the pointer itself, stamp it.
            auto Take = [&](size_t n) -> Block {
                void* p = Alloc(n);
                uint64_t tag = Mix((static_cast<uint64_t>(k) << 48) | (++counter));
                if (p == nullptr) {
                    f.nullPtr.fetch_add(1);
                    f.Note("null return for size " + std::to_string(n));
                    return Block{nullptr, n, tag};
                }
                if (reinterpret_cast<uintptr_t>(p) % sizeof(void*) != 0) {
                    f.misaligned.fetch_add(1);
                    char buf[128];
                    snprintf(buf, sizeof(buf), "misaligned %p for size %zu", p, n);
                    f.Note(buf);
                }
                if (!reg.Insert(p)) {
                    f.duplicate.fetch_add(1);
                    char buf[128];
                    snprintf(buf, sizeof(buf),
                             "duplicate live pointer %p (size %zu)", p, n);
                    f.Note(buf);
                }
                FillBlock(p, n, tag);   // also proves the extent is writable
                return Block{p, n, tag};
            };

            // Verify the stamp survived, then hand the block back.
            auto Give = [&](const Block& b) {
                if (!b.p) return;
                size_t off = 0;
                if (!CheckBlock(b.p, b.n, b.tag, &off)) {
                    f.corrupted.fetch_add(1);
                    char buf[192];
                    snprintf(buf, sizeof(buf),
                             "stamp corrupted at %p size %zu byte %zu "
                             "(block was overlapped, handed out twice, or short)",
                             b.p, b.n, off);
                    f.Note(buf);
                }
                reg.Erase(b.p);
                Free(b.p);
            };

            for (size_t r = 0; r < cfg.rounds; ++r) {
                switch (cfg.pattern) {
                    case VPattern::BulkThenFree: {
                        std::vector<Block> local;
                        local.reserve(cfg.ntimes);
                        for (size_t i = 0; i < cfg.ntimes; ++i)
                            local.push_back(Take(SizeFor(cfg.sizeMode, i, rng)));
                        // Every block in the batch is live at once here, so
                        // this is the strongest overlap test of the three.
                        for (size_t i = 0; i < cfg.ntimes; ++i)
                            Give(local[i]);
                        break;
                    }
                    case VPattern::Interleaved: {
                        std::vector<Block> ring(cfg.window, Block{nullptr, 0, 0});
                        size_t pos = 0;
                        for (size_t i = 0; i < cfg.ntimes; ++i) {
                            Block b = Take(SizeFor(cfg.sizeMode, i, rng));
                            if (ring[pos].p) Give(ring[pos]);
                            ring[pos] = b;
                            pos = (pos + 1) % cfg.window;
                        }
                        for (size_t i = 0; i < cfg.window; ++i)
                            if (ring[i].p) Give(ring[i]);
                        break;
                    }
                    case VPattern::CrossThread: {
                        for (size_t i = 0; i < cfg.ntimes; ++i)
                            shared[k][i] = Take(SizeFor(cfg.sizeMode, i, rng));

                        WaitAtBarrier();

                        size_t victim = (k + 1) % cfg.nworks;
                        for (size_t i = 0; i < cfg.ntimes; ++i) {
                            if (shared[victim][i].p) {
                                Give(shared[victim][i]);
                                shared[victim][i] = Block{nullptr, 0, 0};
                            }
                        }

                        WaitAtBarrier();
                        break;
                    }
                }
            }
        });
    }

    for (auto& t : threads) t.join();
}

// ------------------------------------------------------------------- driver

// Size-boundary contract check.
//
// The workload loops above sweep sizes but never touch the ends of the range,
// so they cannot see an off-by-one in the size-class table. That is how
// ConcurrentAlloc(0) survived: Index(0) underflowed to SIZE_MAX and indexed
// past _freeList[], caught by an assert in debug builds and by nothing at all
// under -DNDEBUG. This walks both sides of every size-class transition, holds
// every block live at once so an overlap shows up as a corrupted stamp, and
// treats a null or misaligned return as a failure.
template <class AllocFn, class FreeFn>
static void RunBoundaryCheck(AllocFn Alloc, FreeFn Free, Failures& f) {
    std::vector<size_t> sizes;
    sizes.push_back(0);
    // both sides of every alignment-group transition, plus the ends
    const size_t edges[] = { 1, 8, 128, 1024, 8 * 1024, 64 * 1024, MAX_BYTES };
    for (size_t i = 0; i < sizeof(edges) / sizeof(edges[0]); ++i) {
        if (edges[i] > 1) sizes.push_back(edges[i] - 1);
        sizes.push_back(edges[i]);
        sizes.push_back(edges[i] + 1);
    }
    // above MAX_BYTES the request bypasses ThreadCache entirely
    sizes.push_back(MAX_BYTES * 2);

    std::vector<Block> live;
    std::set<void*> seen;
    for (size_t i = 0; i < sizes.size(); ++i) {
        const size_t n = sizes[i];
        void* p = Alloc(n);
        uint64_t tag = Mix(0xB0D1CE5EEDull + (uint64_t)(i + 1));
        if (p == nullptr) {
            f.nullPtr.fetch_add(1);
            f.Note("null return for size " + std::to_string(n));
            continue;
        }
        if (reinterpret_cast<uintptr_t>(p) % sizeof(void*) != 0) {
            f.misaligned.fetch_add(1);
            char buf[128];
            snprintf(buf, sizeof(buf), "misaligned %p for size %zu", p, n);
            f.Note(buf);
        }
        if (!seen.insert(p).second) {
            f.duplicate.fetch_add(1);
            char buf[128];
            snprintf(buf, sizeof(buf), "duplicate live pointer %p (size %zu)", p, n);
            f.Note(buf);
        }
        FillBlock(p, n, tag);
        live.push_back(Block{ p, n, tag });
    }
    // every block is live simultaneously, so any overlap is visible now
    for (size_t i = 0; i < live.size(); ++i) {
        size_t badOff = 0;
        if (!CheckBlock(live[i].p, live[i].n, live[i].tag, &badOff)) {
            f.corrupted.fetch_add(1);
            char buf[160];
            snprintf(buf, sizeof(buf),
                     "stamp corrupted at %p size %zu byte %zu (overlapping or short block)",
                     live[i].p, live[i].n, badOff);
            f.Note(buf);
        }
    }
    for (size_t i = 0; i < live.size(); ++i) Free(live[i].p);
}

static const char* SizeName(VSizeMode m) {
    switch (m) {
        case VSizeMode::Fixed16:   return "fixed";
        case VSizeMode::Ramp:      return "ramp";
        case VSizeMode::Random:    return "random";
        case VSizeMode::ClassStep: return "classstep";
    }
    return "?";
}

static const char* PatternName(VPattern p) {
    switch (p) {
        case VPattern::BulkThenFree: return "bulk";
        case VPattern::Interleaved:  return "interleaved";
        case VPattern::CrossThread:  return "cross";
    }
    return "?";
}

int main(int argc, char** argv) {
    VConfig cfg;
    bool all = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : ""; };
        if      (a == "--threads")  cfg.nworks = std::stoul(next());
        else if (a == "--rounds")   cfg.rounds = std::stoul(next());
        else if (a == "--ntimes")   cfg.ntimes = std::stoul(next());
        else if (a == "--seed")     cfg.seed   = (unsigned)std::stoul(next());
        else if (a == "--window")   cfg.window = std::stoul(next());
        else if (a == "--registry") cfg.registry = true;
        else if (a == "--all")      all = true;
        else if (a == "--alloc") {
            std::string v = next();
            if      (v == "mine")   cfg.mode = AllocMode::Mine;
            else if (v == "sys")    cfg.mode = AllocMode::Sys;
            else if (v == "broken") cfg.mode = AllocMode::Broken;
            else { fprintf(stderr, "unknown --alloc %s\n", v.c_str()); return 2; }
        }
        else if (a == "--size") {
            std::string v = next();
            if      (v == "fixed")     cfg.sizeMode = VSizeMode::Fixed16;
            else if (v == "ramp")      cfg.sizeMode = VSizeMode::Ramp;
            else if (v == "random")    cfg.sizeMode = VSizeMode::Random;
            else if (v == "classstep") cfg.sizeMode = VSizeMode::ClassStep;
            else { fprintf(stderr, "unknown --size %s\n", v.c_str()); return 2; }
        }
        else if (a == "--pattern") {
            std::string v = next();
            if      (v == "bulk")        cfg.pattern = VPattern::BulkThenFree;
            else if (v == "interleaved") cfg.pattern = VPattern::Interleaved;
            else if (v == "cross")       cfg.pattern = VPattern::CrossThread;
            else { fprintf(stderr, "unknown --pattern %s\n", v.c_str()); return 2; }
        }
        else if (a == "--help" || a == "-h") {
            printf("usage: %s [options]\n"
                   "  --all           run all 12 size x pattern combinations\n"
                   "  --size MODE     fixed | ramp | random | classstep\n"
                   "  --pattern MODE  bulk | interleaved | cross\n"
                   "  --threads N     worker threads (default 4)\n"
                   "  --rounds N      rounds per thread (default 3)\n"
                   "  --ntimes N      allocations per round (default 500)\n"
                   "  --window N      live set for interleaved (default 64)\n"
                   "  --alloc WHICH   mine | sys | broken\n"
                   "                  sys    = positive control, must PASS\n"
                   "                  broken = negative control, must FAIL\n"
                   "  --registry      add exact duplicate-live-pointer check\n"
                   "                  (takes a global lock; run as a 2nd pass)\n"
                   "  --seed N        RNG seed (default 12345)\n"
                   "exit status is 0 only if every check passed\n", argv[0]);
            return 0;
        }
        else { fprintf(stderr, "unknown flag %s (try --help)\n", a.c_str()); return 2; }
    }

    std::vector<VSizeMode> sizes;
    std::vector<VPattern>  patterns;
    if (all) {
        sizes    = { VSizeMode::Fixed16, VSizeMode::Ramp,
                     VSizeMode::Random,  VSizeMode::ClassStep };
        patterns = { VPattern::BulkThenFree, VPattern::Interleaved,
                     VPattern::CrossThread };
    } else {
        sizes    = { cfg.sizeMode };
        patterns = { cfg.pattern };
    }

    const char* modeName =
        cfg.mode == AllocMode::Sys    ? "system malloc/free (positive control)" :
        cfg.mode == AllocMode::Broken ? "deliberately broken (negative control -- must FAIL)"
                                      : "ConcurrentAlloc/ConcurrentFree";
    printf("allocator under test: %s%s\n", modeName,
           cfg.registry ? "   [+registry]" : "");
    printf("threads=%zu rounds=%zu ntimes=%zu window=%zu\n",
           cfg.nworks, cfg.rounds, cfg.ntimes, cfg.window);
    printf("%-11s %-12s %10s %10s %10s %10s   %s\n",
           "size", "pattern", "null", "misalign", "corrupt", "dup", "result");

    size_t grandTotal = 0;
    std::vector<std::string> allMessages;

    for (size_t si = 0; si < sizes.size(); ++si) {
        for (size_t pi = 0; pi < patterns.size(); ++pi) {
            VConfig c = cfg;
            c.sizeMode = sizes[si];
            c.pattern  = patterns[pi];

            Failures f;
            Registry reg;
            reg.enabled = c.registry;

            if (c.mode == AllocMode::Sys) {
                RunCheck(c, [](size_t n) { return malloc(n); },
                            [](void* p) { free(p); }, f, reg);
            } else if (c.mode == AllocMode::Broken) {
                RunCheck(c, BrokenAlloc, BrokenFree, f, reg);
            } else {
                RunCheck(c, [](size_t n) { return ConcurrentAlloc(n); },
                            [](void* p) { ConcurrentFree(p); }, f, reg);
            }

            // Anything still in the registry after a full round trip is a leak
            // in the harness, not the allocator -- but it would invalidate the
            // duplicate check, so it is worth knowing about.
            if (reg.enabled && !reg.live.empty()) {
                char buf[128];
                snprintf(buf, sizeof(buf), "%zu pointers still live at end of %s/%s",
                         reg.live.size(), SizeName(c.sizeMode), PatternName(c.pattern));
                f.Note(buf);
            }

            size_t total = f.Total();
            grandTotal += total;
            printf("%-11s %-12s %10zu %10zu %10zu %10zu   %s\n",
                   SizeName(c.sizeMode), PatternName(c.pattern),
                   f.nullPtr.load(), f.misaligned.load(),
                   f.corrupted.load(), f.duplicate.load(),
                   total == 0 ? "PASS" : "*** FAIL ***");
            fflush(stdout);

            for (size_t m = 0; m < f.messages.size(); ++m)
                allMessages.push_back(std::string(SizeName(c.sizeMode)) + "/" +
                                      PatternName(c.pattern) + ": " + f.messages[m]);
        }
    }

    // The boundary check is not a size x pattern combination; it exercises the
    // API contract at the ends of the range instead of in the middle.
    {
        Failures f;
        if (cfg.mode == AllocMode::Sys) {
            RunBoundaryCheck([](size_t n) { return malloc(n); },
                             [](void* p) { free(p); }, f);
        } else if (cfg.mode == AllocMode::Broken) {
            RunBoundaryCheck(BrokenAlloc, BrokenFree, f);
        } else {
            RunBoundaryCheck([](size_t n) { return ConcurrentAlloc(n); },
                             [](void* p) { ConcurrentFree(p); }, f);
        }
        size_t total = f.Total();
        grandTotal += total;
        printf("%-11s %-12s %10zu %10zu %10zu %10zu   %s\n",
               "boundary", "size 0..512K",
               f.nullPtr.load(), f.misaligned.load(),
               f.corrupted.load(), f.duplicate.load(),
               total == 0 ? "PASS" : "*** FAIL ***");
        for (size_t m = 0; m < f.messages.size(); ++m)
            allMessages.push_back("boundary: " + f.messages[m]);
    }

    if (!allMessages.empty()) {
        printf("\nfirst failures:\n");
        for (size_t i = 0; i < allMessages.size() && i < 24; ++i)
            printf("  %s\n", allMessages[i].c_str());
    }

    printf("\n%s (%zu failing check%s)\n",
           grandTotal == 0 ? "ALL CHECKS PASSED" : "*** CORRECTNESS GATE FAILED ***",
           grandTotal, grandTotal == 1 ? "" : "s");
    return grandTotal == 0 ? 0 : 1;
}
