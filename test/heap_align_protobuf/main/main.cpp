// Reproduction of issue #32 using protobuf's Arena, the exact code path
// that crashes in OtlpHttpExporter::Export().
//
// OtlpHttpExporter creates a google::protobuf::Arena with non-default
// ArenaOptions on every Export() call.  A non-default max_block_size causes
// InitializeWithPolicy to embed an AllocationPolicy pointer inside the first
// arena block.  If operator new returned a 4-byte-aligned block (as ESP-IDF's
// heap can), TaggedAllocationPolicyPtr::get() computes (ptr & ~7) = ptr - 4,
// reading max_block_size (0x00010000) as the block_alloc function pointer.
// The next arena block allocation calls block_alloc(size) → jumps to
// 0x00010000 → InstrFetchProhibited.
//
// Expected output (workaround active / conforming allocator):
//   alignof(max_align_t) = 8
//   operator new low3=0  ...
//   PASS: Arena survived second-block allocation
//
// Expected output (bug present, no workaround):
//   alignof(max_align_t) = 8
//   operator new low3=4  ...
//   Guru Meditation Error: Core  0 panic'ed (InstrFetchProhibited)
//   PC: 0x00010000

#include <cstddef>
#include <cstdio>
#include <new>

#include "google/protobuf/arena.h"

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
    // These are the exact ArenaOptions used by OtlpHttpExporter::Export().
    // A non-default max_block_size triggers InitializeWithPolicy, which
    // embeds an AllocationPolicy in the first block.  Allocating past the
    // first block's capacity (initial_block_size bytes minus header overhead)
    // forces the arena to call block_alloc for a second block.
    printf("=== protobuf Arena second-block allocation ===\n");

    // After freeing the survey blocks the heap coalesces them.  The next
    // allocation gets the even-aligned base of that region — which would let
    // the Arena survive.  One dummy allocation consumes that even slot,
    // ensuring the Arena's initial block lands on the next (4-byte-aligned)
    // boundary and reliably triggers the crash.
    void* parity_flip = ::operator new(256);

    google::protobuf::ArenaOptions opts;
    opts.initial_block_size = 1024;   // same as otlp_http_exporter.cc
    opts.max_block_size     = 65536;  // 0x00010000 — the crash address
    google::protobuf::Arena arena(opts);

    // Allocate 20 × 64 = 1280 bytes, exceeding the ~1000 usable bytes in
    // the first block.  The second block allocation calls block_alloc().
    // Without the workaround: block_alloc = 0x00010000 → crash.
    for (int i = 0; i < 20; ++i) {
        (void)google::protobuf::Arena::CreateArray<char>(&arena, 64);
    }

    printf("PASS: Arena survived second-block allocation\n");
    ::operator delete(parity_flip);
}
