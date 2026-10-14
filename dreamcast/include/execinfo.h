// No stack walker on the Dreamcast: crash reports go without a backtrace.
#ifndef SMS_DC_EXECINFO_H
#define SMS_DC_EXECINFO_H
#include <stddef.h>
static inline int backtrace(void** frames, int capacity) { (void)frames; (void)capacity; return 0; }
static inline void backtrace_symbols_fd(void* const* frames, int count, int fd) { (void)frames; (void)count; (void)fd; }
static inline char** backtrace_symbols(void* const* frames, int count) { (void)frames; (void)count; return NULL; }
#endif
