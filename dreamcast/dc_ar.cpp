// ARAM on the Dreamcast. Replaces platform/ar/ar.cpp, which keeps all 16 MiB
// in a host buffer.
//
// The game splits ARAM as 0xF40000 bytes for audio and the rest (about
// 0.75 MiB) for JKRAram's archive heap (System/Application.cpp). With
// AUDIO=0 nothing goes in the audio part, so only the top kBacked bytes are
// backed; a transfer below them is logged and dropped.
#include "port_compat.h"
#include "port_os.h"
#include "port_platform.h"
#include <dolphin/ar.h>
#include <dolphin/arq.h>
#include <stdlib.h>
#include <string.h>

namespace {
const u32 kAramSize = 16u << 20;
const u32 kAramBase = 0x4000;
#if SMS_DC_AUDIO
const u32 kBacked = kAramSize;
#else
const u32 kBacked = 1u << 20;
#endif
const u32 kLow = kAramSize - kBacked;  // ARAM address of g_aram[0]
u8* g_aram;
u32 g_alloc = kAramBase;

void copy(u32 type, u32 src, u32 dst, u32 len)
{
	u32 a = type == ARAM_DIR_MRAM_TO_ARAM ? dst : src;
	if (a < kLow || a + len > kAramSize) {
		port_log("[ar] %s outside the backed ARAM 0x%x+0x%x\n",
		         type == ARAM_DIR_MRAM_TO_ARAM ? "write" : "read", a, len);
		return;
	}
	if (type == ARAM_DIR_MRAM_TO_ARAM)
		memcpy(g_aram + (dst - kLow), (void*)(uintptr_t)src, len);
	else
		memcpy((void*)(uintptr_t)dst, g_aram + (src - kLow), len);
}
} // namespace

extern "C" u32 ARInit(u32*, u32)
{
	if (!g_aram)
		g_aram = (u8*)calloc(1, kBacked);
	return kAramBase;
}
extern "C" int ARCheckInit(void) { return g_aram != NULL; }
extern "C" u32 ARAlloc(u32 length)
{
	u32 p = g_alloc;
	g_alloc += length;
	return p;
}
extern "C" u32 ARFree(u32* length) { return g_alloc; }
extern "C" u32 ARGetBaseAddress(void) { return kAramBase; }
extern "C" u32 ARGetSize(void) { return kAramSize; }
extern "C" u32 ARGetDMAStatus(void) { return 0; }
extern "C" void ARStartDMA(u32 type, u32 mainmem, u32 aram, u32 length)
{
	if (type == ARAM_DIR_MRAM_TO_ARAM)
		copy(type, mainmem, aram, length);
	else
		copy(type, aram, mainmem, length);
}
extern "C" void ARQInit(void) {}
extern "C" void ARQPostRequest(ARQRequest* req, u32 owner, u32 type, u32 prio, u32 source, u32 dest, u32 length,
                               ARQCallback callback)
{
	req->owner    = owner;
	req->type     = type;
	req->priority = prio;
	req->source   = source;
	req->dest     = dest;
	req->length   = length;
	req->callback = callback;
	copy(type, source, dest, length);
	if (callback)
		port_irq_run_now([req, callback]() { callback((u32)(uintptr_t)req); });
}
extern "C" u8* port_aram_ptr(u32 addr) { return g_aram && addr >= kLow ? g_aram + (addr - kLow) : NULL; }

// For save states (platform/savestate): only the backed top of ARAM.
extern "C" u8* port_ar_memory(u32* size, u32** alloc)
{
	static u32 none;
	*size  = 0;
	*alloc = &none;
	return g_aram;
}
