#pragma once
extern int errorLogs;
// Arguments are used, as the firmware's macros use them, so a local that only feeds a log line is not "unused".
inline void logSink(const char*, const char*, ...) {}
#define LOG_ERR(...) (++errorLogs, logSink(__VA_ARGS__))
#define LOG_INF(...) logSink(__VA_ARGS__)
