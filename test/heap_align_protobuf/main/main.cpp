// Regression test for issue #32 using protobuf's Arena, the exact code path
// that crashed in OtlpHttpExporter::Export().
//
// OtlpHttpExporter creates a google::protobuf::Arena with non-default
// ArenaOptions on every Export() call.  A non-default max_block_size causes
// InitializeWithPolicy to embed an AllocationPolicy inside the first arena
// block, at an offset that is a multiple of 8, so the policy pointer has the
// same alignment as the block.  ESP-IDF's heap hands out 4-byte-aligned
// blocks.  Before protobuf 393aac3, TaggedAllocationPolicyPtr reserved 3 tag
// bits (kTagsMask = 7) and get() computed (ptr & ~7) = ptr - 4, reading
// max_block_size (0x00010000) as the block_alloc function pointer; the next
// arena block allocation jumped to 0x00010000 → InstrFetchProhibited.
// protobuf 393aac3 reserves only the tag bit it uses, so the pointer
// round-trips intact without an aligned operator new.
//
// protobuf documents that arena blocks must be 8-byte aligned, and
// AllocateMemory DCHECKs it.  This test breaks that on purpose, the same way
// ESP-IDF's heap does, and passes only because protobuf is built with
// -DNDEBUG (cmake/protobuf_setup.cmake).  Without NDEBUG the DCHECK aborts
// it before the misaligned path runs.
//
// Expected output with the fix:
//   alignof(max_align_t) = 8
//   operator new low7=4 for some blocks   (ESP-IDF heap is 4-byte aligned)
//   first arena block low3=4
//   PASS: Arena survived second-block allocation
//
// Expected output without the fix:
//   first arena block low3=4
//   Guru Meditation Error: Core  0 panic'ed (InstrFetchProhibited)
//   PC: 0x00010000

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <new>

#include "google/protobuf/arena.h"

namespace {

void* first_block_raw = nullptr;
void* first_block = nullptr;

// Places the first block, which holds the AllocationPolicy, at 4 mod 8 the
// way ESP-IDF's heap does for some allocations, so the test does not depend
// on where the heap happens to put it.  Later blocks come from operator new
// unchanged.
void* misaligning_block_alloc(size_t size)
{
    if (first_block != nullptr) {
        return ::operator new(size);
    }
    first_block_raw = ::operator new(size + 4);
    auto raw = reinterpret_cast<uintptr_t>(first_block_raw);
    first_block = reinterpret_cast<void*>((raw & 7) == 0 ? raw + 4 : raw);
    printf("first arena block %p  low3=%u\n", first_block,
           static_cast<unsigned>(reinterpret_cast<uintptr_t>(first_block) & 7));
    return first_block;
}

void misaligning_block_dealloc(void* p, size_t size)
{
    if (p == first_block) {
        ::operator delete(first_block_raw);
    } else {
        ::operator delete(p, size);
    }
}

}  // namespace

extern "C" void app_main()
{
    printf("alignof(max_align_t) = %zu\n\n", alignof(std::max_align_t));

    // ------------------------------------------------------------------ //
    // 1. Alignment survey                                                  //
    // ------------------------------------------------------------------ //
    // Keep all pointers live so each iteration gets a *different* block.
    // Freeing immediately would return the same block each time and hide
    // misalignment.
    printf("=== operator new(256) alignment survey ===\n");
    static void* ptrs[16];
    int bad = 0;
    for (int i = 0; i < 16; ++i) {
        ptrs[i] = ::operator new(256);
        unsigned low = static_cast<unsigned>(
            reinterpret_cast<uintptr_t>(ptrs[i]) & (alignof(std::max_align_t) - 1));
        printf("  [%2d]  %p  low%zu=%u%s\n",
               i, ptrs[i], alignof(std::max_align_t) - 1, low,
               low ? "  <-- NON-CONFORMING" : "");
        if (low) ++bad;
    }
    for (int i = 0; i < 16; ++i) ::operator delete(ptrs[i]);
    printf("Non-conforming: %d / 16\n\n", bad);

    // ------------------------------------------------------------------ //
    // 2. Arena second-block allocation                                     //
    // ------------------------------------------------------------------ //
    // These are the ArenaOptions used by OtlpHttpExporter::Export(), plus a
    // block_alloc that puts the first block at 4 mod 8.  A non-default
    // max_block_size triggers InitializeWithPolicy, which embeds an
    // AllocationPolicy in the first block.  Allocating past the first block's
    // capacity forces the arena to call block_alloc for a second block.
    printf("=== protobuf Arena second-block allocation ===\n");

    google::protobuf::ArenaOptions opts;
    opts.initial_block_size = 1024;   // same as otlp_http_exporter.cc
    opts.max_block_size     = 65536;  // 0x00010000 — the crash address
    opts.block_alloc        = misaligning_block_alloc;
    opts.block_dealloc      = misaligning_block_dealloc;
    google::protobuf::Arena arena(opts);

    // Allocate 20 × 64 = 1280 bytes, exceeding the ~1000 usable bytes in
    // the first block.  The second block allocation calls block_alloc().
    for (int i = 0; i < 20; ++i) {
        (void)google::protobuf::Arena::CreateArray<char>(&arena, 64);
    }

    printf("PASS: Arena survived second-block allocation\n");
}
