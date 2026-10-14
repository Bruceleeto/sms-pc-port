// POSIX calls the platform layer uses that KallistiOS's newlib leaves out.
#include "dc_host.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

extern "C" int clock_nanosleep(clockid_t clock_id, int flags, const struct timespec* rqtp, struct timespec* rmtp)
{
	struct timespec rel = *rqtp;
	if (flags & TIMER_ABSTIME) {
		struct timespec now;
		clock_gettime(clock_id, &now);
		rel.tv_sec = rqtp->tv_sec - now.tv_sec;
		rel.tv_nsec = rqtp->tv_nsec - now.tv_nsec;
		if (rel.tv_nsec < 0) {
			rel.tv_sec -= 1;
			rel.tv_nsec += 1000000000L;
		}
		if (rel.tv_sec < 0)
			return 0;
	}
	return nanosleep(&rel, rmtp) == 0 ? 0 : errno;
}

// Seek-then-read, under one lock: game threads read the disc concurrently.
static pthread_mutex_t s_pread_lock = PTHREAD_MUTEX_INITIALIZER;

extern "C" ssize_t pread(int fd, void* buf, size_t count, off_t offset)
{
	pthread_mutex_lock(&s_pread_lock);
	ssize_t r = lseek(fd, offset, SEEK_SET) < 0 ? -1 : read(fd, buf, count);
	pthread_mutex_unlock(&s_pread_lock);
	return r;
}

// KallistiOS's file systems take "a/b/../c" literally; dvd.cpp finds the
// disc's sys/ folder as <root>/../sys. fopen is wrapped at link time
// (-Wl,--wrap=fopen) to fold "x/.." away first.
extern "C" FILE* __real_fopen(const char* path, const char* mode);
extern "C" FILE* __wrap_fopen(const char* path, const char* mode)
{
	if (!strstr(path, "/.."))
		return __real_fopen(path, mode);
	char out[512];
	size_t n = 0;
	for (const char* p = path; *p && n < sizeof out - 1;) {
		if (p[0] == '/' && p[1] == '.' && p[2] == '.' && (p[3] == '/' || !p[3])) {
			while (n > 0 && out[n - 1] != '/')
				n--;
			if (n > 0)
				n--;  // the parent's slash
			p += 3;
			continue;
		}
		out[n++] = *p++;
	}
	out[n] = 0;
	return __real_fopen(out, mode);
}
