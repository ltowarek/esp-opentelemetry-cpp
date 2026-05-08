// POSIX shims for third-party code (Abseil cctz / clock.cc, SpinLock)
// cross-compiled to the Xtensa newlib toolchain.
//
// ESP-IDF newlib ships without `nanosleep()` despite declaring the
// prototype in <time.h>. Abseil's clock/spinlock code calls it
// unconditionally, triggering an unresolved symbol at link time.
//
// This translation unit provides a small, strong implementation backed
// by `vTaskDelay()`. It is a weak symbol so any future ESP-IDF release
// that ships the real thing wins.

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// ---------------------------------------------------------------------------
// Xtensa THREADPTR pre-initializer
//
// Abseil's hash tables (absl::flat_hash_map / raw_hash_set) use a
// `thread_local uint16_t seed` in NextSeed() that is NOT guarded by
// ABSL_HAVE_THREAD_LOCAL.  Protobuf's global static constructors
// (_GLOBAL__sub_I_...) build DescriptorPool tables via this hash map, so
// they fire before vTaskStartScheduler.  On Xtensa, `thread_local` is
// implemented by loading THREADPTR + a link-time offset.  THREADPTR is 0
// before any FreeRTOS task has been scheduled, so every `thread_local`
// access during global ctors triggers a LoadProhibited exception.
//
// Fix: register a .preinit_array function that allocates a small static TLS
// area and initialises THREADPTR before the C++ global constructors run.
// The same calculation is used by FreeRTOS's uxInitialiseStackTLS().

static uint8_t s_startup_tls_area[4096] __attribute__((aligned(16)));

static void esp_init_startup_threadptr(void)
    __attribute__((constructor(101)));  // runs before global ctors (priority 65535)

static void esp_init_startup_threadptr(void)
{
    extern int _thread_local_start, _thread_local_end;
    extern int _flash_rodata_start, _flash_rodata_align;

    uint32_t tls_len = (uint32_t)&_thread_local_end - (uint32_t)&_thread_local_start;
    if (tls_len == 0 || tls_len > sizeof(s_startup_tls_area)) {
        return;  // nothing to set up
    }
    memcpy(s_startup_tls_area, &_thread_local_start, tls_len);

    // Mirror the THREADPTR calculation from uxInitialiseStackTLS in
    // components/freertos/FreeRTOS-Kernel/portable/xtensa/port.c:
    //   threadptr = tls_area_start
    //               - (_thread_local_start - _flash_rodata_start)
    //               - align_up(TCB_SIZE=8, tls_section_align)
    uint32_t tls_section_align = (uint32_t)&_flash_rodata_align;
    uint32_t base = (tls_section_align + 7) & ~(tls_section_align - 1);  // align_up(8, align)
    if (base < 8) base = 8;
    uint32_t threadptr = (uint32_t)s_startup_tls_area
                         - ((uint32_t)&_thread_local_start - (uint32_t)&_flash_rodata_start)
                         - base;

    __asm__ volatile ("wur.THREADPTR %0" :: "a"(threadptr));
}

__attribute__((weak)) int nanosleep(const struct timespec *req,
                                    struct timespec *rem) {
  if (req == NULL || req->tv_nsec < 0 || req->tv_nsec >= 1000000000L ||
      req->tv_sec < 0) {
    errno = EINVAL;
    return -1;
  }
  // Convert to milliseconds, rounding up so very short sleeps still
  // yield the scheduler at least once.
  uint64_t ns    = (uint64_t)req->tv_sec * 1000000000ULL +
                   (uint64_t)req->tv_nsec;
  uint64_t ms    = (ns + 999999ULL) / 1000000ULL;
  TickType_t tks = pdMS_TO_TICKS(ms);
  if (tks == 0 && ms > 0) tks = 1;
  vTaskDelay(tks);
  if (rem) {
    rem->tv_sec  = 0;
    rem->tv_nsec = 0;
  }
  return 0;
}

// ESP-IDF's pthread component omits pthread_atfork. Without a local
// definition the linker satisfies it from newlib's libnosys pthread.o,
// which *also* provides pthread_mutex_*, pthread_exit, ... stubs and
// therefore collides at link time with ESP-IDF's real pthread
// implementations. Defining a trivial no-op here prevents that entire
// object file from being dragged in.
//
// Abseil only calls pthread_atfork to register a "reset CV state
// after fork" handler; since ESP-IDF never forks, dropping the
// registration is a sound no-op.
int pthread_atfork(void (*prepare)(void), void (*parent)(void),
                   void (*child)(void)) {
  (void)prepare;
  (void)parent;
  (void)child;
  return 0;
}

// ESP-IDF's sysconf() returns -1 for _SC_PAGESIZE (default: EINVAL).
// Abseil's LowLevelAlloc stores this as pagesize=0xFFFFFFFF and then
// computes pagesize*16 which overflows, triggering an immediate FATAL abort.
// Intercept via --wrap=sysconf and return 4096 for _SC_PAGESIZE; fall
// through to the real implementation for every other parameter.
#include <unistd.h>
long __real_sysconf(int name);
long __wrap_sysconf(int name) {
  if (name == _SC_PAGESIZE) return 4096;
  return __real_sysconf(name);
}

