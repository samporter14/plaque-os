#pragma once
// Force-included into lib/TrustedTime/TrustedTime.cpp only (CMakeLists.txt): its time() and settimeofday() are the fake
// system clock's (FakeWorld.h Clock::timeEpoch), as the rest of the glue code's gettimeofday() already is. The standard
// headers it would otherwise pull in after the macros are included first, so the macros touch the file's own calls
// only.
#include <sys/time.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>
#undef gettimeofday

time_t glue_time(time_t* out);
int glue_settimeofday(const struct timeval* tv, const void* tz);
#define time(arg) glue_time(arg)
#define settimeofday(tv, tz) glue_settimeofday(tv, tz)
