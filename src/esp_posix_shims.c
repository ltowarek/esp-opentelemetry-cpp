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
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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
