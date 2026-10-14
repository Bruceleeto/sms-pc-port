// Port runtime on the Dreamcast: logging, stub accounting and the boot
// sequence before SMS_main. Replaces platform/port_runtime.cpp, which is the
// PC's (settings file, crash handler, MEM1 mapped at 0x80000000).
//
// MEM1 is an ordinary heap block here. Game pointers are P1 addresses
// (0x8C000000..), so the GameCube's OSCachedToPhysical (addr - 0x80000000)
// and the GX layer's physical window (base 0x80000000) still round-trip, and
// the few fixed low-memory reads land in boot ROM, which reads harmlessly.
#include "port_compat.h"
#include "port_platform.h"
#include "port_stub.h"
#include "port_os.h"
#include <dolphin/os.h>
#include <kos.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The game reads its files from the CD (sys/ and files/ at its root), or
// from SMS_DISC_ROOT, e.g. /pc/game/files under dcload.
const char* port_disc_root = "/cd/files";
int port_disc_explicit     = 1;
// MOVIES=0: every THP movie reports itself finished as it starts (patch 0016).
extern "C" int port_skip_movies;
int port_skip_movies = !SMS_DC_MOVIES;
extern "C" float port_widescreen;
float port_widescreen = 1.0f;
extern "C" int port_frame_rate;
int port_frame_rate = 30;

u8* port_mem1_base;
u32 port_mem1_size;

extern "C" void port_log(const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

static port_stub_rec* s_stub_list;
static unsigned s_stub_distinct;
extern "C" void port_stub_hit(port_stub_rec* rec)
{
	if (rec->count++ == 0) {
		rec->next   = s_stub_list;
		s_stub_list = rec;
		++s_stub_distinct;
		port_log("[stub] %s (first call; %u distinct stubs hit)\n", rec->name, s_stub_distinct);
	}
}

extern "C" void port_stub_report(void)
{
	port_log("[stub] summary: %u distinct SDK stubs were called\n", s_stub_distinct);
	for (port_stub_rec* r = s_stub_list; r; r = r->next)
		port_log("[stub]   %-32s %lu\n", r->name, r->count);
}

extern "C" void port_ptr32_trap(const void* p)
{
	port_log("[port] PTR32: address %p does not fit a 32-bit slot\n", p);
	abort();
}

void* port_low_alloc(unsigned long size) { return malloc(size); }

// The GameCube's 24 MiB less the ~4.5 MiB its code and data took from MEM1;
// here the program lives outside MEM1, so this leaves the game the same
// arena. SMS_MEM_MB (Makefile.dc MEM_MB=) overrides it.
static void map_mem1()
{
	u32 mb = SMS_DC_MEM_MB;
	if (const char* e = getenv("SMS_MEM_MB"))
		mb = (u32)atoi(e);
	port_mem1_size = mb << 20;
	port_mem1_base = (u8*)memalign(32, port_mem1_size);
	if (!port_mem1_base) {
		port_log("[port] out of memory for a %u MiB MEM1 (%lu MiB RAM; Flycast: enable the 32MB RAM mod)\n",
		         mb, (unsigned long)(HW_MEMSIZE >> 20));
		exit(1);
	}
	memset(port_mem1_base, 0, port_mem1_size);
	port_log("[port] MEM1: %u MiB at %p (RAM %lu MiB)\n", mb, port_mem1_base, (unsigned long)(HW_MEMSIZE >> 20));
}

extern "C" void port_init(int argc, char** argv)
{
	if (const char* d = getenv("SMS_DISC_ROOT"))
		port_disc_root = d;
	for (int i = 1; i < argc; i++)
		if (argv[i][0] != '-')
			port_disc_root = argv[i];
#if !SMS_DC_AUDIO
	setenv("SMS_NO_AUDIO", "1", 1);
#endif
#ifdef SMS_DC_DVD_LOG
	setenv("SMS_DVD_LOG", "1", 1);
#endif
	atexit(port_stub_report);
	map_mem1();
	port_os_init();
	port_dvd_init();
	port_noaudio_init();
	port_vi_init();
	port_log("[port] platform ready\n");
}

// Pieces of the PC platform layer the Dreamcast build leaves out.
extern "C" void port_window_icon_init(void) {}

// HD cutscene packs are a PC option: movies draw at their own resolution.
#include "sms_hd_movies.h"
extern "C" BOOL port_draw_hd_thp(u8*, u8*, u8*, s16, s16, s16, s16, s16, s16) { return FALSE; }
extern "C" const char* port_hd_cutscene_directory(void) { return NULL; }

#if !SMS_DC_MOVIES
// MOVIES=0 leaves out the THP decoder, whose THPInit the player calls first;
// the SDK stub's FALSE would stop every movie screen from ever finishing.
// With nothing to set up, report success: each movie then ends as it starts.
extern "C" BOOL THPInit(void) { return TRUE; }
#endif

// port_runtime.cpp's settings the game reads: 30 fps, as on the GameCube.
extern "C" int port_heat_haze = 0;
extern "C" int port_active_frame_rate = 30;
extern "C" int port_high_fps_active = 0;

// No crash reporter (platform/os/crash_info.cpp is POSIX/Windows).
#include "os/crash.h"
void port_install_crash_handlers() {}
void port_crash_thread_init() {}
void port_crash_emit(PortCrashLine&) {}
bool port_crash_module(uintptr_t, uintptr_t*, const char**) { return false; }
void port_crash_note_file(const char*) {}
void port_crash_set_thread_describer(PortCrashDescriber) {}
