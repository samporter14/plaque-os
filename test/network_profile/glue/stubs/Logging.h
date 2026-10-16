#pragma once
// LOG_* of the firmware, captured so that a test can read what was logged (and how often at which level).
void gluelog(int level, const char* tag, const char* format, ...) __attribute__((format(printf, 3, 4)));
#define LOG_ERR(tag, ...) gluelog(2, tag, __VA_ARGS__)
#define LOG_INF(tag, ...) gluelog(1, tag, __VA_ARGS__)
#define LOG_DBG(tag, ...) gluelog(0, tag, __VA_ARGS__)
