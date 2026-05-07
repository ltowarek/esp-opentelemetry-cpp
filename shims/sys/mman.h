// Stub sys/mman.h for ESP-IDF (Xtensa/newlib).
//
// ESP-IDF's newlib does not ship sys/mman.h. Abseil's LowLevelAlloc uses
// mmap to grow its internal arena on every mutex/thread-identity allocation.
// The original stub returned MAP_FAILED unconditionally, which caused
// ABSL_RAW_LOG(FATAL, "mmap error") -> abort() on first Abseil allocation.
// Back mmap with malloc so Abseil's allocator works on the FreeRTOS heap.

#pragma once

#include <stddef.h>   // size_t
#include <stdlib.h>   // malloc, free

#define PROT_READ   0x1
#define PROT_WRITE  0x2
#define MAP_PRIVATE 0x02
#define MAP_ANONYMOUS 0x20
#define MAP_ANON MAP_ANONYMOUS
#define MAP_FAILED ((void *)-1)

#ifdef __cplusplus
extern "C" {
#endif

static inline void *mmap(void *addr, size_t length, int prot, int flags,
                         int fd, long offset) {
  (void)addr; (void)prot; (void)flags; (void)fd; (void)offset;
  void *ptr = malloc(length);
  return ptr ? ptr : MAP_FAILED;
}

static inline int munmap(void *addr, size_t length) {
  (void)length;
  free(addr);
  return 0;
}

#ifdef __cplusplus
}
#endif
