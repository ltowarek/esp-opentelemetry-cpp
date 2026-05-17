// Workaround: ESP-IDF newlib struct tm lacks tm_gmtoff/tm_zone; Abseil cctz
// includes them unconditionally. Upstream: Abseil cctz. ESP-IDF newlib (all versions).
//
// Shim time.h for Abseil on ESP-IDF (Xtensa/newlib).
//
// ESP-IDF's newlib struct tm lacks the BSD/GNU tm_gmtoff and tm_zone
// extensions.  Abseil's cctz time_zone_libc.cc requires these.  This
// wrapper includes the real toolchain time.h and defines tm_gmtoff as
// a macro mapping to tm_isdst (an existing int field).  The value is
// wrong at runtime but Abseil's timezone path is never used on ESP32
// (we use std::chrono for timestamping).

#pragma once

#include_next <time.h>

#if !defined(tm_gmtoff) && !defined(__tm_gmtoff)
#define tm_gmtoff tm_isdst
#endif
