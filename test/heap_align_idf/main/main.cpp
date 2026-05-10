// Reproduction of:
// "ESP-IDF heap: malloc returns 4-byte-aligned memory despite
//  alignof(std::max_align_t)==8 on Xtensa"
//
// No external libraries — pure ESP-IDF only.
//
// The C/C++ standards require malloc() and operator new() to return memory
// aligned to alignof(max_align_t).  On ESP32-S3 the toolchain reports
// alignof(max_align_t)==8, but ESP-IDF's multi_heap uses sizeof(void*)=4 as
// its alignment granularity, so roughly 1 in 4 allocations is only 4-byte
// aligned.
//
// This file also demonstrates the concrete consequence for any code that
// stores flag bits in the low 3 bits of a heap pointer (kPtrMask = ~7),
// e.g. protobuf's TaggedAllocationPolicyPtr.  When the pointer is 4-aligned,
// masking with ~7 shifts it 4 bytes backward; fields are read from the wrong
// offset and a subsequent indirect call jumps to an invalid address.
//
// Expected (conforming) output — all allocations 8-byte aligned:
//   alignof(max_align_t) = 8
//   malloc low3=0  low3=0  ...
//   Non-conforming: 0/16
//   ...
//   PASS
//
// Actual output on unpatched ESP32-S3 + ESP-IDF 5.3.1:
//   alignof(max_align_t) = 8
//   malloc low3=4  low3=0  low3=4  ...
//   Non-conforming: 2/16                  <-- violates C/C++ standard
//   ...
//   tagged.get() shifted -4 bytes from stored pointer
//   field[2] reads 0x00010000 instead of NULL  <-- would call 0x00010000
//                                                   -> InstrFetchProhibited

#include <cstddef>
#include <cstdlib>
#include <cstdio>
#include <new>

// Mirrors the layout of protobuf's AllocationPolicy on 32-bit:
//   offset 0: start_block_size  (size_t, 4 bytes)
//   offset 4: max_block_size    (size_t, 4 bytes) = 0x00010000 = 65536
//   offset 8: block_alloc       (fn ptr, 4 bytes) = NULL by default
//   offset 12: block_dealloc    (fn ptr, 4 bytes)
struct Policy {
    size_t start_block_size        = 256;
    size_t max_block_size          = 65536;   // 0x00010000
    void* (*block_alloc)(size_t)   = nullptr;
    void  (*block_dealloc)(void*, size_t) = nullptr;
};

// Mirrors TaggedAllocationPolicyPtr: stores/retrieves a pointer with the
// low 3 bits reserved for flags (kPtrMask = ~7, requires 8-byte alignment).
static void* tagged_get(void* stored) {
    return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(stored) & ~uintptr_t{7});
}

extern "C" void app_main()
{
    printf("alignof(max_align_t) = %zu\n\n", alignof(std::max_align_t));

    // ------------------------------------------------------------------ //
    // 1. Alignment survey                                                  //
    // ------------------------------------------------------------------ //
    // Keep all pointers live so each iteration gets a *different* block.
    // Freeing immediately would return the same block each time and hide
    // misalignment.
    printf("=== malloc(256) alignment survey ===\n");
    static void* ptrs[16];
    int bad = 0;
    for (int i = 0; i < 16; ++i) {
        ptrs[i] = malloc(256);
        unsigned low = static_cast<unsigned>(
            reinterpret_cast<uintptr_t>(ptrs[i]) & (alignof(std::max_align_t) - 1));
        printf("  [%2d]  %p  low%zu=%u%s\n",
               i, ptrs[i], alignof(std::max_align_t) - 1, low,
               low ? "  <-- NON-CONFORMING" : "");
        if (low) ++bad;
    }
    for (int i = 0; i < 16; ++i) free(ptrs[i]);
    printf("Non-conforming: %d / 16\n\n", bad);

    // ------------------------------------------------------------------ //
    // 2. Consequence: TaggedAllocationPolicyPtr reads wrong field          //
    // ------------------------------------------------------------------ //
    // Allocate a block as protobuf's arena would and place a Policy at
    // ptr() = block_base + kBlockHeaderSize(8).  If block_base is only
    // 4-aligned, ptr() is also 4-aligned, and tagged_get() shifts it 4
    // bytes backward — reading max_block_size(0x00010000) as block_alloc.
    printf("=== TaggedAllocationPolicyPtr simulation ===\n");

    void* block = malloc(256);
    uintptr_t block_base = reinterpret_cast<uintptr_t>(block);
    uintptr_t ptr        = block_base + 8;  // kBlockHeaderSize = 8

    // Place Policy at ptr() via placement-new.
    Policy* stored = new (reinterpret_cast<void*>(ptr)) Policy{};

    // Retrieve via kPtrMask = ~7.
    Policy* retrieved = reinterpret_cast<Policy*>(tagged_get(stored));

    printf("  block_base  = %p  (low3 = %u)\n",
           block, static_cast<unsigned>(block_base & 7u));
    printf("  stored      = %p  (ptr() = block_base + 8)\n", stored);
    printf("  retrieved   = %p  (stored & ~7)\n", retrieved);
    printf("  delta       = %+d bytes\n",
           static_cast<int>(
               reinterpret_cast<char*>(retrieved) -
               reinterpret_cast<char*>(stored)));
    printf("  retrieved->block_alloc = %p",
           reinterpret_cast<void*>(retrieved->block_alloc));
    if (retrieved != stored) {
        printf("  <-- reads max_block_size(0x%zx) as a fn ptr!"
               " Calling it -> InstrFetchProhibited\n",
               retrieved->max_block_size);
    } else {
        printf("  (correct: NULL)\n");
    }

    free(block);

    printf("\n");
    if (bad == 0 && retrieved == stored) {
        printf("PASS: allocator is conforming on this build.\n");
    } else {
        printf("FAIL: allocator returns insufficiently aligned memory"
               " (alignof(max_align_t)=%zu, got 4-byte aligned blocks).\n",
               alignof(std::max_align_t));
    }
}
