#pragma once
#pragma GCC system_header  // include_next is a GNU extension; this file is the test's, not the firmware's
// The system's <sys/time.h>, with gettimeofday() answering the fake system clock (FakeWorld.h Clock::timeEpoch).
#include_next <sys/time.h>
int glue_gettimeofday(struct timeval* tv, void* tz);
#define gettimeofday glue_gettimeofday
