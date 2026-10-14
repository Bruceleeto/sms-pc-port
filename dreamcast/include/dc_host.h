// Force-included into the platform layer on the Dreamcast: the POSIX calls
// KallistiOS's newlib leaves out (defined in dreamcast/dc_host.cpp).
#ifndef SMS_DC_HOST_H
#define SMS_DC_HOST_H
#include <sys/types.h>
#include <time.h>
#ifdef __cplusplus
extern "C" {
#endif
int clock_nanosleep(clockid_t clock_id, int flags, const struct timespec* rqtp, struct timespec* rmtp);
ssize_t pread(int fd, void* buf, size_t count, off_t offset);
#ifdef __cplusplus
}
#endif
#endif
