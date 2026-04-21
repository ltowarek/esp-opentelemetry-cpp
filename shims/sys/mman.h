// Stub sys/mman.h for ESP-IDF (Xtensa/newlib).
//
// Abseil detects __XTENSA__ and defines ABSL_HAVE_MMAP, but ESP-IDF's
// newlib does not ship sys/mman.h.  This stub satisfies the #include
// while leaving mmap/munmap undefined so the linker rejects any actual
// call — which is fine because the code paths guarded by ABSL_HAVE_MMAP
// in absl/debugging are never reached at runtime on our firmware.

#pragma once

#include <stddef.h>   // size_t

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
  (void)addr; (void)length; (void)prot; (void)flags; (void)fd; (void)offset;
  return MAP_FAILED;
}

static inline int munmap(void *addr, size_t length) {
  (void)addr; (void)length;
  return -1;
}

#ifdef __cplusplus
}
#endif
