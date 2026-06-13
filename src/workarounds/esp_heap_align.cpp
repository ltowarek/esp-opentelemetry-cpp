// ESP-IDF's heap allocator uses sizeof(void*)=4 as its alignment granularity
// on 32-bit Xtensa, but the ESP32-S3 toolchain reports
// alignof(std::max_align_t)==8.  The C++ standard requires operator new to
// return memory aligned to alignof(max_align_t), so the default malloc-backed
// operator new is non-conforming on this platform.
//
// The practical consequence: protobuf's TaggedAllocationPolicyPtr stores flags
// in the low 3 bits of the AllocationPolicy* (kPtrMask = ~7), requiring an
// 8-byte-aligned pointer.  When operator new returns a 4-byte-aligned block,
// get() computes ptr & ~7 = ptr - 4, reading 4 bytes before the struct.  On
// the next arena block allocation the arena calls what it thinks is block_alloc
// but is actually max_block_size (0x00010000), jumping to that address and
// triggering an InstrFetchProhibited exception.
//
// This file replaces the six standard replaceable allocation operators with
// versions backed by heap_caps_aligned_alloc(alignof(max_align_t), ...), making
// operator new conforming and preventing the misaligned-pointer crash.
//
// The CMakeLists.txt forces this translation unit onto the link line via
// -Wl,--undefined so it is included even before any C++ allocation site is
// encountered by the linker.

#include <cstddef>
#include <exception>
#include <new>

#include "esp_heap_caps.h"

void* operator new(std::size_t size) {
    if (size == 0) {
        size = 1;
    }
    void* p = heap_caps_aligned_alloc(
        alignof(std::max_align_t), size, MALLOC_CAP_DEFAULT);
    if (p == nullptr) {
#if __cpp_exceptions
        throw std::bad_alloc();
#else
        std::terminate();
#endif
    }
    return p;
}

void* operator new[](std::size_t size) {
    return ::operator new(size);
}

void operator delete(void* p) noexcept {
    heap_caps_free(p);
}

void operator delete[](void* p) noexcept {
    heap_caps_free(p);
}

void operator delete(void* p, std::size_t) noexcept {
    heap_caps_free(p);
}

void operator delete[](void* p, std::size_t) noexcept {
    heap_caps_free(p);
}
