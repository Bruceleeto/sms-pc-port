// PAD on the Dreamcast: the first maple controller as GameCube port 1.
//
//   DC                GameCube
//   A B X Y Start     A B X Y Start
//   L / R triggers    L / R (analog; the digital click past 250)
//   analog stick      control stick
//   D-pad             C-stick (camera)
//   second stick      C-stick, on dual-analog pads
//   C or Z button     Z, on pads that have one; else D-pad down with L held
//
// Replaces platform/pad/pad.cpp; PADClamp is the decomp's, as on PC.
#include "port_compat.h"
#include <dolphin/pad.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>
#include <string.h>

extern "C" BOOL PADInit() { return TRUE; }
extern "C" int PADReset(unsigned long) { return TRUE; }
extern "C" BOOL PADRecalibrate(u32) { return TRUE; }
extern "C" BOOL PADSync(void) { return TRUE; }
extern "C" void PADSetSpec(u32) {}
extern "C" void PADSetAnalogMode(u32) {}
extern "C" void PADSetSamplingRate(unsigned long) {}
extern "C" void PADControlMotor(s32, u32) {}
extern "C" void PADControlAllMotors(const u32*) {}
// The PC's autopress (scripted input) hook; nothing to press here.
extern "C" void port_pad_autopress_field(u32) {}

// A DC axis (-128..127) as a GameCube stick reading (about +-100 at the rim).
static s8 stick(int v)
{
	int x = v * 100 / 128;
	return (s8)(x > 127 ? 127 : x < -128 ? -128 : x);
}

extern "C" u32 PADRead(PADStatus* status)
{
	for (int i = 0; i < 4; i++) {
		memset(&status[i], 0, sizeof status[i]);
		status[i].err = PAD_ERR_NO_CONTROLLER;
	}
	maple_device_t* dev = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
	cont_state_t* st    = dev ? (cont_state_t*)maple_dev_status(dev) : NULL;
	if (!st)
		return 0;

	PADStatus& s = status[0];
	s.err        = PAD_ERR_NONE;
	u32 in       = st->buttons;
	u16 b        = 0;
	if (in & CONT_A) b |= PAD_BUTTON_A;
	if (in & CONT_B) b |= PAD_BUTTON_B;
	if (in & CONT_X) b |= PAD_BUTTON_X;
	if (in & CONT_Y) b |= PAD_BUTTON_Y;
	if (in & CONT_START) b |= PAD_BUTTON_START;
	bool zCombo = (in & CONT_DPAD_DOWN) && st->ltrig > 128;
	if ((in & (CONT_C | CONT_Z)) || zCombo)
		b |= PAD_TRIGGER_Z;

	int tl = zCombo ? 0 : st->ltrig, tr = st->rtrig;
	if (tl >= 250) b |= PAD_TRIGGER_L;
	if (tr >= 250) b |= PAD_TRIGGER_R;
	s.triggerLeft  = (u8)tl;
	s.triggerRight = (u8)tr;
	s.analogA      = (b & PAD_BUTTON_A) ? 255 : 0;
	s.analogB      = (b & PAD_BUTTON_B) ? 255 : 0;
	s.button       = b;

	s.stickX = stick(st->joyx);
	s.stickY = stick(-st->joyy);
	int cx = 0, cy = 0;
	if (!zCombo) {
		cx = ((in & CONT_DPAD_RIGHT) ? 100 : 0) - ((in & CONT_DPAD_LEFT) ? 100 : 0);
		cy = ((in & CONT_DPAD_UP) ? 100 : 0) - ((in & CONT_DPAD_DOWN) ? 100 : 0);
	}
	s.substickX = cx ? (s8)cx : stick(st->joy2x);
	s.substickY = cy ? (s8)cy : stick(-st->joy2y);
	return PAD_CHAN0_BIT;
}

// PC-only options of pad.cpp: no free camera or mouse look, no save state key.
extern "C" {
int port_free_camera = 0;
f32 port_camera_speed_x = 1.0f;
f32 port_camera_speed_y = 1.0f;
int port_camera_take_mouse(f32* dx, f32* dy)
{
	*dx = *dy = 0.0f;
	return 0;
}
int port_pad_savestate_labels(int, char labels[2][16])
{
	labels[0][0] = labels[1][0] = 0;
	return 0;
}
}
