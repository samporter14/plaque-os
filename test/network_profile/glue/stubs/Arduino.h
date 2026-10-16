#pragma once
// The clock of the glue tests: a virtual one. delay() moves it and lets the fake radio catch up with it.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>

unsigned long millis();
void delay(uint32_t ms);

class Stream;  // HttpDownloader.h names it

// What lib/TrustedTime (compiled for real here) takes from the Arduino core: the critical-section lock (no other task
// runs in a glue test) and SNTP's one-call setup, which the fake radio's SNTP model (GlueStubs.cpp) answers.
using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
void configTzTime(const char* tz, const char* server1, const char* server2 = nullptr, const char* server3 = nullptr);
