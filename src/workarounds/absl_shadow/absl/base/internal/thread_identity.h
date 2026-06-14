#pragma once
// Workaround: Abseil's absl/base/internal/thread_identity.h has an
// unconditional, class-body assertion (line 187 at commit b6c1816):
//
//     static_assert(std::atomic<WaitState>::is_always_lock_free);
//
// where WaitState is `enum class : uint8_t` -- a 1-byte atomic. esp_otel CI
// reproducibly fails this assert when cross-compiling the examples for esp32s3
// (GitHub Actions job 81229264859, 2026-06-13). The C++ standard does not
// require 1-byte atomics to be always-lock-free, so the assert is not
// guaranteed to hold across ESP toolchain configurations. (It does pass on
// esp-15.2.0_20251204 / GCC 15.2, where __GCC_ATOMIC_CHAR_LOCK_FREE == 2.)
//
// This header shadows Abseil's via the dedicated src/workarounds/absl_shadow
// include directory, which is prepended ahead of Abseil's own -I for every
// vendored target (see the target_include_directories(... BEFORE ...) calls in
// CMakeLists.txt and cmake/protobuf_setup.cmake). It includes <atomic> so the
// real
// std::atomic<T>::is_always_lock_free member is defined, then -- bracketed by
// push_macro/pop_macro so the rewrite applies to *only* this one
// #include_next -- rewrites the `is_always_lock_free` token so the failing
// assert becomes `... || true` (always true). The companion
// `static_assert(ABSL_CACHELINE_SIZE >= kToBePaddedSize)` is left intact, and
// no other translation unit is affected (a whole-TU macro would mangle
// is_always_lock_free declarations in, e.g., <bits/shared_ptr_atomic.h>).
// Byte atomics remain fully functional at runtime -- Abseil <= 20250512.1 used
// a std::atomic<bool> field in the same struct with no such assert.
//
// Remove this file once the upstream assert is platform-gated.
// See https://github.com/ltowarek/esp-opentelemetry-cpp/issues/59
#include <atomic>  // define std::atomic<T>::is_always_lock_free before the macro
#pragma push_macro("is_always_lock_free")
#define is_always_lock_free is_always_lock_free || true
#include_next <absl/base/internal/thread_identity.h>
#pragma pop_macro("is_always_lock_free")
