//
// bare_input.cpp
//
// by @mmoroca & arena.ai 2026  -  https://arena.ai
//
// BM-DOS - keyboard and mouse backend.
//
// Circle hands USB HID reports to us: a modifier byte plus up to six "usage
// IDs" for the keyboard, and a button mask plus relative X/Y for the mouse.
// libretro (and therefore DOSBox-core) wants RETROK_* key codes and a mouse
// position delta, so this file is mostly two lookup tables.
//
// Keys do NOT travel through input_cb at all.  DOSBox-core asks for a keyboard
// event callback instead (RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK, from
// libretro_input_init()), and pushes every press and release through it as an
// *event* - keycode, down, character, modifiers.  A frontend that keeps a key
// state array and answers input_cb(RETRO_DEVICE_KEYBOARD, k) is answering a
// question nothing asks: the answer looks perfect and no key ever reaches the
// emulated machine.  That is exactly what this port did until now, and it is
// why Caps Lock did not light up either - DOSBox only mirrors the LEDs to the
// host when it is told about a key.
//
// So the HID handler in this file calls back into bare_libretro.cpp
// (BareDeliverKey), which does two things: it keeps the state array - still
// useful, and it makes the state visible from outside - and it invokes the
// core's keyboard callback if the core installed one.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
#include "bare_env.h"
#include "key_report.h"

#include "libretro.h"

#include <circle/devicenameservice.h>
#include <circle/input/mouse.h>
#include <circle/logger.h>
#include <circle/sched/scheduler.h>
#include <circle/usb/usbgamepad.h>
#include <circle/usb/usbkeyboard.h>
#include <circle/util.h>

// snprintf, for the device names "mouse1".."mouse4".  <cstdio> is not pulled in
// by any of the Circle headers above, and circle-stdlib's newlib does provide
// it - the same one kernel.cpp uses for its own banner.
#include <cstdio>

static const char FromInput[] = "input";

namespace {

CUSBKeyboardDevice *s_pKeyboard = nullptr;

// Raw modifier byte from the last HID report (see usbhid.h).
//
// Kept, and not just for reference: SetModifierKeys() is only called on the
// transition of a *key* report, and a keyboard that is asked to change its LED
// state re-sends its modifier byte with no key change - which is exactly when
// a half-pressed Shift has to be re-delivered.  Reading the last raw byte here
// is what lets the poll path notice that.  See BareInputPoll().
u8 s_ucModifiers = 0;

// The last report, so we can detect which usage IDs went down and up.
u8 s_LastKeys[6] = { 0, 0, 0, 0, 0, 0 };

// Mouse state accumulated since the last poll.
int s_nMouseDX = 0;
unsigned s_nMouseSpeed = 100;		// percent, boot.conf mouse_speed
int s_nMouseRestX = 0;			// the part of a step carried to the next poll
int s_nMouseRestY = 0;
int s_nMouseDY = 0;
u32 s_nMouseButtons = 0;

// USB HID usage ID (keyboard page 0x07) -> libretro RETROK_*.
// Only the keys a PC keyboard actually has are listed; anything else is
// ignored, which is what you want for a DOS machine.
struct TKeyMap
{
	u8 ucUsage;
	unsigned nRetroKey;
};

const TKeyMap s_KeyMap[] =
{
	// Letters, in USB HID order (a=0x04 ... z=0x1D).
	{ 0x04, RETROK_a }, { 0x05, RETROK_b }, { 0x06, RETROK_c }, { 0x07, RETROK_d },
	{ 0x08, RETROK_e }, { 0x09, RETROK_f }, { 0x0A, RETROK_g }, { 0x0B, RETROK_h },
	{ 0x0C, RETROK_i }, { 0x0D, RETROK_j }, { 0x0E, RETROK_k }, { 0x0F, RETROK_l },
	{ 0x10, RETROK_m }, { 0x11, RETROK_n }, { 0x12, RETROK_o }, { 0x13, RETROK_p },
	{ 0x14, RETROK_q }, { 0x15, RETROK_r }, { 0x16, RETROK_s }, { 0x17, RETROK_t },
	{ 0x18, RETROK_u }, { 0x19, RETROK_v }, { 0x1A, RETROK_w }, { 0x1B, RETROK_x },
	{ 0x1C, RETROK_y }, { 0x1D, RETROK_z },

	// Digits 1..9,0
	{ 0x1E, RETROK_1 }, { 0x1F, RETROK_2 }, { 0x20, RETROK_3 }, { 0x21, RETROK_4 },
	{ 0x22, RETROK_5 }, { 0x23, RETROK_6 }, { 0x24, RETROK_7 }, { 0x25, RETROK_8 },
	{ 0x26, RETROK_9 }, { 0x27, RETROK_0 },

	// Control and punctuation
	{ 0x28, RETROK_RETURN },	{ 0x29, RETROK_ESCAPE },
	{ 0x2A, RETROK_BACKSPACE },	{ 0x2B, RETROK_TAB },
	{ 0x2C, RETROK_SPACE },		{ 0x2D, RETROK_MINUS },
	{ 0x2E, RETROK_EQUALS },	{ 0x2F, RETROK_LEFTBRACKET },
	{ 0x30, RETROK_RIGHTBRACKET },	{ 0x31, RETROK_BACKSLASH },
	{ 0x33, RETROK_SEMICOLON },	{ 0x34, RETROK_QUOTE },
	{ 0x35, RETROK_BACKQUOTE },	{ 0x36, RETROK_COMMA },
	{ 0x37, RETROK_PERIOD },	{ 0x38, RETROK_SLASH },
	// The ISO key next to Enter ("non-US #"): 'ç' on a Spanish keyboard, '#'
	// on a UK one.  A PC keyboard sends the same scan code for it as for the
	// US backslash key (0x2B), and KEYB turns that into the right character.
	// It was RETROK_UNKNOWN: the key did nothing, in DOS or in the menu.
	{ 0x32, RETROK_BACKSLASH },
	{ 0x64, RETROK_OEM_102 },	// the extra key on ISO keyboards

	// Caps Lock.  Missing until now, and it is the one key the core maps to
	// DOS *and* mirrors to the host LEDs, so without it Caps Lock did nothing
	// at all and the LED never lit: the toggle arrives as a keyboard event,
	// never as a lock state.  (The other two locks, Num Lock and Scroll Lock,
	// are in the core's map and were already handled below.)
	{ 0x39, RETROK_CAPSLOCK },

	// Function keys
	{ 0x3A, RETROK_F1 },  { 0x3B, RETROK_F2 },  { 0x3C, RETROK_F3 },
	{ 0x3D, RETROK_F4 },  { 0x3E, RETROK_F5 },  { 0x3F, RETROK_F6 },
	{ 0x40, RETROK_F7 },  { 0x41, RETROK_F8 },  { 0x42, RETROK_F9 },
	{ 0x43, RETROK_F10 }, { 0x44, RETROK_F11 }, { 0x45, RETROK_F12 },

	// Navigation cluster
	{ 0x46, RETROK_SYSREQ },	// PrintScreen
	{ 0x47, RETROK_SCROLLOCK },	// ScrollLock
	{ 0x48, RETROK_PAUSE },
	{ 0x49, RETROK_INSERT }, { 0x4A, RETROK_HOME }, { 0x4B, RETROK_PAGEUP },
	{ 0x4C, RETROK_DELETE }, { 0x4D, RETROK_END },  { 0x4E, RETROK_PAGEDOWN },
	{ 0x4F, RETROK_RIGHT },  { 0x50, RETROK_LEFT },
	{ 0x51, RETROK_DOWN },   { 0x52, RETROK_UP },

	// Keypad
	{ 0x53, RETROK_NUMLOCK },
	{ 0x54, RETROK_KP_DIVIDE }, { 0x55, RETROK_KP_MULTIPLY },
	{ 0x56, RETROK_KP_MINUS },  { 0x57, RETROK_KP_PLUS },
	{ 0x58, RETROK_KP_ENTER },  { 0x62, RETROK_KP0 },
	{ 0x59, RETROK_KP1 }, { 0x5A, RETROK_KP2 }, { 0x5B, RETROK_KP3 },
	{ 0x5C, RETROK_KP4 }, { 0x5D, RETROK_KP5 }, { 0x5E, RETROK_KP6 },
	{ 0x5F, RETROK_KP7 }, { 0x60, RETROK_KP8 }, { 0x61, RETROK_KP9 },
	{ 0x63, RETROK_KP_PERIOD },
};

// Note on the three keys DOSBox-core *also* treats as frontend hotkeys:
// Caps Lock, Insert and F11/F12.  They are ordinary keys to DOS and they are
// delivered as such; the core's own hotkey layer runs in front of that and
// decides (see libretro_input.cpp: Ctrl+F11/Ctrl+F12 change the cycle count,
// Insert toggles the "pinhack" debug view, Ctrl+F4 swaps a disk).  Nothing on
// this side may swallow a key to implement a menu: the emulated machine gets
// every press.
unsigned UsageToRetro (u8 ucUsage)
{
	for (const auto &entry : s_KeyMap)
	{
		if (entry.ucUsage == ucUsage)
		{
			return entry.nRetroKey;
		}
	}

	return RETROK_UNKNOWN;
}

// Modifier bits, from Circle's usbhid.h.  Circle reports them in the same
// byte layout the HID spec uses.
constexpr u8 ModLeftCtrl   = 0x01;
constexpr u8 ModLeftShift  = 0x02;
constexpr u8 ModLeftAlt    = 0x04;
constexpr u8 ModLeftGui    = 0x08;
constexpr u8 ModRightCtrl  = 0x10;
constexpr u8 ModRightShift = 0x20;
constexpr u8 ModRightAlt   = 0x40;
constexpr u8 ModRightGui   = 0x80;

void SetModifierKeys (u8 ucModifiers)
{
	// Only on change.  Circle hands us a new HID report on every keyboard
	// event, and a modifier bit that stays down would otherwise be delivered
	// again and again, which DOSBox would see as auto-repeat.
	static u8 s_ucLastModifiers = 0;
	if (ucModifiers == s_ucLastModifiers)
	{
		return;
	}
	s_ucLastModifiers = ucModifiers;

	const struct
	{
		u8 ucBit;
		unsigned nRetroKey;
	} modifiers[] =
	{
		{ ModLeftCtrl,   RETROK_LCTRL },  { ModLeftShift,  RETROK_LSHIFT },
		{ ModLeftAlt,    RETROK_LALT },   { ModLeftGui,    RETROK_LSUPER },
		{ ModRightCtrl,  RETROK_RCTRL },  { ModRightShift, RETROK_RSHIFT },
		{ ModRightAlt,   RETROK_RALT },   { ModRightGui,   RETROK_RSUPER },
	};

	for (const auto &mod : modifiers)
	{
		BareDeliverKey (mod.nRetroKey, (ucModifiers & mod.ucBit) != 0);
	}

	// AltGr arrives as right-alt on PC keyboards; DOS only has one alt and
	// DOSBox maps it through the keyboard layout, so nothing else to do.
	//
	// Modifiers go through the same delivery path as any other key, because
	// DOSBox's keyboard callback treats them as ordinary key events.
}

// ---------------------------------------------------------------------------
// Caps Lock / Num Lock / Scroll Lock LEDs
// ---------------------------------------------------------------------------
//
// The LEDs are a *host* feature in this port: DOSBox has no way to ask for them
// (a libretro core cannot write to the keyboard, it can only send key events),
// so the toggles have to be tracked here, from the lock keys as they are
// delivered.
//
// Two rules make this work where a naive version does not:
//
//   * The LED write cannot happen in the USB handler.  SetLEDs() talks to the
//     USB stack and Circle's own header says it "must not be called in
//     interrupt context"; the handler runs in exactly that context.  So the
//     handler only counts, and BareInputPoll - once per frame, from the
//     emulator task - does the write.
//   * It is a count, not a flag.  Two Caps Lock presses within one frame are
//     two toggles, and a flag would lose one.  The LED *state* is a separate
//     variable, toggled per press.
constexpr u8 LEDNumLock    = 0x01;	// USB HID LED bits, as the keyboard
constexpr u8 LEDCapsLock   = 0x02;	// expects them in SetLEDs()' argument
constexpr u8 LEDScrollLock = 0x04;

unsigned s_nLEDUpdatesPending = 0;
u8 s_ucLEDState = 0;

void ToggleLED (u8 ucBit)
{
	s_ucLEDState ^= ucBit;
	if (s_nLEDUpdatesPending < 1000)
	{
		s_nLEDUpdatesPending++;		// bounded: a stuck keyboard must not
	}					// grow this without limit
}

void FlushLEDUpdates (void)
{
	if (s_nLEDUpdatesPending == 0 || s_pKeyboard == nullptr)
	{
		return;
	}

	s_nLEDUpdatesPending = 0;
	s_pKeyboard->SetLEDs (s_ucLEDState);
}

void KeyStatusHandler (unsigned char ucModifiers, const unsigned char RawKeys[6])
{
	// Only what CHANGED since the last report (key_report.h): a key that is
	// still held is not pressed again.  It used to be, in every report - so
	// typing "as" with the 'a' still down when the 's' arrived gave "aas", in
	// DOS and in the setup menu, and Caps Lock held while another key moved
	// flipped its LED once more.
	barekeys::Diff (s_LastKeys, RawKeys,
		[] (unsigned char ucUsage)
		{
			BareDeliverKey (UsageToRetro (ucUsage), FALSE);
		},
		[] (unsigned char ucUsage)
		{
			const unsigned nRetroKey = UsageToRetro (ucUsage);
			BareDeliverKey (nRetroKey, TRUE);

			// The LEDs follow the lock keys.  Circle's cooked mode does
			// this itself; raw mode (which is what gives DOS make and
			// break codes) leaves it to us.  Queued, not written here:
			// see s_nLEDUpdatesPending.
			if (nRetroKey == RETROK_CAPSLOCK)
			{
				ToggleLED (LEDCapsLock);
			}
			else if (nRetroKey == RETROK_NUMLOCK)
			{
				ToggleLED (LEDNumLock);
			}
			else if (nRetroKey == RETROK_SCROLLOCK)
			{
				ToggleLED (LEDScrollLock);
			}
		});

	// A rollover error report (too many keys) is not a new state: keep the
	// last real one, so that those keys are not "pressed" again after it.
	if (!(RawKeys[0] >= 1 && RawKeys[0] <= 3))
	{
		for (unsigned i = 0; i < 6; i++)
		{
			s_LastKeys[i] = RawKeys[i];
		}
	}

	s_ucModifiers = ucModifiers;
	SetModifierKeys (ucModifiers);
}

void MouseStatusHandler (unsigned nButtons, int nDisplacementX, int nDisplacementY,
			 int nWheelMove)
{
	// Circle's button bits: 1 = left, 2 = right, 4 = middle (same as DOS).
	s_nMouseButtons = nButtons;

	s_nMouseDX += nDisplacementX;
	s_nMouseDY += nDisplacementY;

	// Wheel: DOSBox's mouse emulation reads the wheel as buttons 4 and 5 in
	// the emulated mouse protocol only if int 33h's wheel API is enabled.
	// Keep it simple for the first milestone and ignore it; adding it is a
	// few lines in the input layer, not here.
	(void) nWheelMove;
}

// ---------------------------------------------------------------------------
// Gamepad
// ---------------------------------------------------------------------------
//
// Why this exists at all: DOSBox's emulated game port is off unless the
// frontend says a pad is connected, and then it reads it through libretro's
// joypad and analog devices.  Until .60 this file had no pad support and
// bare_libretro.cpp answered 0 to those two devices, so a gamepad that Circle
// detected perfectly (the log's "Product: ..." lines are Circle, not us) was
// invisible inside DOS.  Two separate faults, one symptom: "lo detecta Circle
// pero DOSBox no lo ve".
//
// Circle names gamepads "upad1", "upad2", ... and each driver class registers
// its own status handler.  The state arrives as a TGamePadState with axes in
// the pad's own units (0..255 for most, but the *pad's own* minimum/maximum
// are in the struct and must be used - a Switch Pro reports -32768..32767), a
// hat in 0..7 plus 8 for "released" (DS4: 0x08 is released, 0..7 clockwise
// from North), and a button bitmask of TGamePadButton values.
//
// The mapping is a **two-axis, two-button DOS joystick**, because that is what
// effectively every DOS game reads:
//
//   left stick / d-pad   -> axes 0 and 1 (X, Y)
//   B (bottom)           -> DOS button 1      (libretro RETRO_DEVICE_ID_JOYPAD_B)
//   A (right)            -> DOS button 2      (libretro RETRO_DEVICE_ID_JOYPAD_A)
//   right stick          -> axes 2 and 3, for the 4-axis games that exist
//   Y / X                -> buttons 3 and 4   (libretro Y and X)
//
// Which libretro bit goes where is not this file's decision: dosbox-core maps
// RETRO_DEVICE_ID_JOYPAD_B to "DOS Button 1" and _A to "Button 2" (see
// libretro_input.cpp, make2ButtonsDescArray).  So the pad's bottom face button
// ends up as button 1, which is what a player expects.
constexpr unsigned MaxPads = 2;		// DOSBox's game port has two

CUSBGamePadDevice *s_pPads[MaxPads] = {};
unsigned s_nPads = 0;

// The pad state, already in libretro's units.  Kept as one bitmask plus four
// axes rather than as a pointer to Circle's struct: the status handler runs
// from the USB task, and reading a struct that another task is rewriting is
// exactly the kind of shared state this port avoids everywhere else.
u32 s_nPadButtons = 0;		// RETRO_DEVICE_ID_JOYPAD_* bits
int s_nPadAxis[4] = {};		// -32768..32767, in order X, Y, X2, Y2
unsigned s_nPadLastInput = 0;	// for the one-time "this pad works" log

/// \brief Read one Circle gamepad axis, scaled to libretro units.
int PadAxisValue (const TGamePadState *pState, unsigned nIndex)
{
	if (pState == nullptr || static_cast<int> (nIndex) >= pState->naxes)
	{
		return 0;
	}

	const int nValue = pState->axes[nIndex].value;
	const int nMin = pState->axes[nIndex].minimum;
	const int nMax = pState->axes[nIndex].maximum;
	if (nMax <= nMin)
	{
		return 0;
	}

	// Map [min, max] onto libretro's [-32768, 32767], with the pad's own
	// centre - not mid-range - as zero.  A stick that reports 0 at rest and
	// -32768..32767 has its centre at (-32768+32767)/2 = 0, so this is a
	// no-op for those; a pad that reports 0..255 gets 127.5 as its centre and
	// this is what stops every axis reading "half pressed" at rest.
	const int nCentre = (nMin + nMax) / 2;
	const int nHalf = (nMax - nMin) / 2;
	if (nHalf == 0)
	{
		return 0;
	}

	long nScaled = static_cast<long> (nValue - nCentre) * 32767 / nHalf;
	if (nScaled > 32767)
	{
		nScaled = 32767;
	}
	else if (nScaled < -32768)
	{
		nScaled = -32768;
	}

	// Dead zone, because a stick at rest on a real pad never reports exactly
	// centre, and a DOS joystick has no dead zone of its own: without this the
	// emulated stick drifts and games that calibrate on start-up calibrate
	// against the drift.
	if (nScaled > -2000 && nScaled < 2000)
	{
		return 0;
	}

	return static_cast<int> (nScaled);
}

void GamePadStatusHandler (unsigned nDeviceIndex, const TGamePadState *pState)
{
	(void) nDeviceIndex;

	if (pState == nullptr)
	{
		return;
	}

	u32 nButtons = 0;

	// The two face buttons that are "DOS button 1 and 2".  Both the digital
	// button mask and the analog button axes are consulted: a pad with
	// analogue triggers reports them as axes and not as buttons, and a pad
	// with plain buttons reports them in the mask.  Either way the emulated
	// joystick sees a button.
	const u32 nDigital = pState->buttons;
	if (nDigital & GamePadButtonB)
	{
		nButtons |= 1u << RETRO_DEVICE_ID_JOYPAD_B;
	}
	if (nDigital & GamePadButtonA)
	{
		nButtons |= 1u << RETRO_DEVICE_ID_JOYPAD_A;
	}
	if (nDigital & GamePadButtonY)
	{
		nButtons |= 1u << RETRO_DEVICE_ID_JOYPAD_Y;
	}
	if (nDigital & GamePadButtonX)
	{
		nButtons |= 1u << RETRO_DEVICE_ID_JOYPAD_X;
	}
	if (nDigital & GamePadButtonLB)
	{
		nButtons |= 1u << RETRO_DEVICE_ID_JOYPAD_L;
	}
	if (nDigital & GamePadButtonRB)
	{
		nButtons |= 1u << RETRO_DEVICE_ID_JOYPAD_R;
	}
	if (nDigital & GamePadButtonStart)
	{
		nButtons |= 1u << RETRO_DEVICE_ID_JOYPAD_START;
	}
	if (nDigital & GamePadButtonSelect)
	{
		nButtons |= 1u << RETRO_DEVICE_ID_JOYPAD_SELECT;
	}

	// The d-pad, as the *same* axes as the left stick.  Many DOS games read
	// the joystick as a set of four switches and are much happier with a
	// digital full-deflection value than with an analogue ramp; and a pad
	// whose stick is drifting is still playable with the d-pad.
	if (nDigital & GamePadButtonUp)
	{
		nButtons |= 1u << RETRO_DEVICE_ID_JOYPAD_UP;
	}
	if (nDigital & GamePadButtonDown)
	{
		nButtons |= 1u << RETRO_DEVICE_ID_JOYPAD_DOWN;
	}
	if (nDigital & GamePadButtonLeft)
	{
		nButtons |= 1u << RETRO_DEVICE_ID_JOYPAD_LEFT;
	}
	if (nDigital & GamePadButtonRight)
	{
		nButtons |= 1u << RETRO_DEVICE_ID_JOYPAD_RIGHT;
	}

#if 0
	// Not enabled: the hat is kept as the digital buttons above, and Circle's
	// DS4 driver already folds the hat into m_State.buttons (see
	// usbgamepadps4.cpp, hat2buttons).  A pad that only reports hats - which
	// some do - would need this; it is written down rather than guessed at.
	if (pState->nhats > 0)
	{
	}
#endif

	s_nPadButtons = nButtons;

	// Axes: left stick first, then right.  Frame of reference is already
	// what libretro expects (X right, Y down), because that is the sign the
	// pads report.
	s_nPadAxis[0] = PadAxisValue (pState, GamePadAxisLeftX);
	s_nPadAxis[1] = PadAxisValue (pState, GamePadAxisLeftY);
	s_nPadAxis[2] = PadAxisValue (pState, GamePadAxisRightX);
	s_nPadAxis[3] = PadAxisValue (pState, GamePadAxisRightY);

	// One line, the first time anything at all is pressed or moved.  This is
	// the check that separates "the pad is attached" from "the pad is
	// attached and its reports are arriving", and it costs nothing after the
	// first event.
	if (s_nPadLastInput == 0 && (nButtons != 0 || s_nPadAxis[0] != 0
				     || s_nPadAxis[1] != 0))
	{
		s_nPadLastInput = 1;
		CLogger::Get ()->Write (FromInput, LogNotice,
					"gamepad: input received (buttons 0x%X, axis %d %d)",
					nButtons, s_nPadAxis[0], s_nPadAxis[1]);
	}
}

/// \brief Find every pad Circle has registered and attach to the new ones.
void ScanGamepads (void)
{
	for (unsigned nIndex = 1; nIndex <= MaxPads; nIndex++)
	{
		char Name[16];
		snprintf (Name, sizeof Name, "upad%u", nIndex);

		auto *pPad = static_cast<CUSBGamePadDevice *> (
			CDeviceNameService::Get ()->GetDevice (Name, FALSE));
		if (pPad == nullptr)
		{
			continue;
		}

		boolean bKnown = FALSE;
		for (unsigned i = 0; i < s_nPads; i++)
		{
			if (s_pPads[i] == pPad)
			{
				bKnown = TRUE;
			}
		}

		if (bKnown || s_nPads >= MaxPads)
		{
			continue;
		}

		s_pPads[s_nPads++] = pPad;
		pPad->RegisterStatusHandler (GamePadStatusHandler);

		// As with the mice: the name is the diagnosis.  "upad1" means Circle
		// classified it as a gamepad; nothing at all means the USB stack did
		// not, and then no amount of input mapping will help.
		const TGamePadState *pInit = pPad->GetInitialState ();
		CLogger::Get ()->Write (FromInput, LogNotice,
					"USB gamepad attached as %s (%d axes, %d hats, %d buttons)",
					Name,
					pInit != nullptr ? pInit->naxes : 0,
					pInit != nullptr ? pInit->nhats : 0,
					pInit != nullptr ? pInit->nbuttons : 0);
	}
}

} // namespace

// ---------------------------------------------------------------------------
// More than one keyboard, more than one mouse
// ---------------------------------------------------------------------------
//
// Written because of a report that reading the code could not answer: "el raton
// inalambrico con dongle USB no funciona, pero uno con cable si, y da igual
// conectarlo antes o despues de arrancar".
//
// Until .59 this file asked Circle for exactly two devices by fixed name:
//
//     CDeviceNameService::Get ()->GetDevice ("ukbd1", FALSE)
//     CDeviceNameService::Get ()->GetDevice ("mouse1", FALSE)
//
// and Circle numbers them: the first keyboard that finishes enumerating is
// ukbd1, the second ukbd2, and likewise for mice - independently of which one
// is plugged in first, which is a detail only the USB stack knows.  So with two
// pointing devices attached, one of them is mouse2 and this port never looked
// at it.  That is a real limitation and it matches the symptom exactly: a
// wireless receiver that enumerates *after* the wired mouse gets index 2 and is
// invisible to the emulator, while the wired one works.
//
// Whether that is the whole story for a given dongle is not something this
// comment can promise: many 2.4 GHz receivers also fail the class matching that
// Circle's USB factory does (they declare no boot protocol, so Circle falls back
// to reading the HID report descriptor and only accepts them as mice if it finds
// Usage Page Generic Desktop followed by Usage Mouse - see
// lib/usb/usbdevicefactory.cpp).  If the receiver fails that test it is driven as
// a gamepad and no amount of scanning here will find a mouse.  The difference
// between the two cases is visible in the log, which is why every device that is
// found is now named there: "mouse2" means index, "no mice at all" means the USB
// stack did not classify it as a mouse.
//
// Four is the ceiling.  Circle's device factory does not impose one, but a
// DOS machine with five pointing devices is not a thing that needs to work.
constexpr unsigned MaxPointers = 4;

CMouseDevice *s_pMice[MaxPointers] = {};
unsigned s_nMice = 0;

/// \brief Look for every mouse Circle has registered and attach to the new ones.
///
/// Called from BareInputInit() and again on every poll, because USB enumeration
/// finishes asynchronously: a receiver plugged in after boot (or one that simply
/// takes longer) appears here on a later frame.  Attaching twice to the same
/// device would deliver every movement twice, so each pointer is remembered.
void ScanMice (void)
{
	for (unsigned nIndex = 1; nIndex <= MaxPointers; nIndex++)
	{
		char Name[16];
		snprintf (Name, sizeof Name, "mouse%u", nIndex);

		auto *pMouse = static_cast<CMouseDevice *> (
			CDeviceNameService::Get ()->GetDevice (Name, FALSE));
		if (pMouse == nullptr)
		{
			continue;
		}

		boolean bKnown = FALSE;
		for (unsigned i = 0; i < s_nMice; i++)
		{
			if (s_pMice[i] == pMouse)
			{
				bKnown = TRUE;
			}
		}

		if (bKnown || s_nMice >= MaxPointers)
		{
			continue;
		}

		s_pMice[s_nMice++] = pMouse;
		pMouse->RegisterStatusHandler (MouseStatusHandler);

		// The name is the point of this line, not decoration: "mouse2"
		// answers "did my dongle enumerate at all, and as what".
		CLogger::Get ()->Write (FromInput, LogNotice,
					"USB mouse attached as %s (%u buttons%s, %u total)",
					Name, pMouse->GetButtonCount (),
					pMouse->HasWheel () ? ", wheel" : "", s_nMice);
	}
}

boolean BareInputInit (void)
{
	// The devices appear asynchronously when USB enumeration finishes, so we
	// do not fail if they are not there yet: BareInputPoll retries.
	s_pKeyboard = static_cast<CUSBKeyboardDevice *> (
		CDeviceNameService::Get ()->GetDevice ("ukbd1", FALSE));
	if (s_pKeyboard != nullptr)
	{
		s_pKeyboard->RegisterKeyStatusHandlerRaw (KeyStatusHandler);
		CLogger::Get ()->Write (FromInput, LogNotice, "USB keyboard attached");
	}

	ScanMice ();
	ScanGamepads ();

	// Nothing found is not an error, but it must not be silent either: this is
	// the line that tells a "the radio mouse does nothing" report apart from
	// "it was classified as a gamepad by the USB stack".
	if (s_nMice == 0)
	{
		CLogger::Get ()->Write (FromInput, LogWarning,
					"no USB mouse found (mouse1..mouse%u): check the log above "
					"for how the device was enumerated",
					MaxPointers);
	}

	return TRUE;
}

void BareInputDeinit (void)
{
	if (s_pKeyboard != nullptr)
	{
		s_pKeyboard->UnregisterKeyStatusHandlerRaw ();
		s_pKeyboard = nullptr;
	}

	// No UnregisterStatusHandler on a CMouseDevice: Circle's CMouseDevice has
	// no such call, and the handler lives in this file, which is not going
	// away.  The pointers are dropped so that a later init does not think it is
	// already attached.
	for (unsigned i = 0; i < MaxPointers; i++)
	{
		s_pMice[i] = nullptr;
	}
	s_nMice = 0;

	// Same reasoning as the mice: CUSBGamePadDevice has no unregister call
	// that is safe to use from here, and the handler lives in this file.
	for (unsigned i = 0; i < MaxPads; i++)
	{
		s_pPads[i] = nullptr;
	}
	s_nPads = 0;
}

void BareInputPoll (void)
{
	const u64 nProfileStart = BareProfileClock ();

	// Late attach: USB enumeration runs in the background, so a keyboard
	// plugged in after boot shows up here on the next frame.
	if (s_pKeyboard == nullptr)
	{
		s_pKeyboard = static_cast<CUSBKeyboardDevice *> (
			CDeviceNameService::Get ()->GetDevice ("ukbd1", FALSE));
		if (s_pKeyboard != nullptr)
		{
			s_pKeyboard->RegisterKeyStatusHandlerRaw (KeyStatusHandler);
			CLogger::Get ()->Write (FromInput, LogNotice,
						"USB keyboard attached (late)");
		}
	}

	// Late attach, and late *second* attach: USB enumeration finishes in the
	// background, so a receiver that shows up after the wired mouse is picked
	// up here on a later frame instead of never.
	ScanMice ();
	ScanGamepads ();

	// Re-deliver the modifier state if the keyboard re-sent its reports since
	// the last poll with the same bits but a different key set.  This is the
	// one case SetModifierKeys() cannot catch on its own: it runs inside the
	// USB handler, and it only fires when the modifier *bits* change.
	//
	// Concretely, holding Shift while the keyboard answers an LED write is
	// enough for a report to arrive with the modifier byte unchanged and no
	// key in the array - DOSBox would then keep Shift down after it is let
	// go, or (worse) deliver Right-Shift as Left-Shift.  Asking the bit byte
	// every poll costs a comparison and closes that hole.
	{
		static u8 s_ucLastPolled = 0;
		if (s_ucModifiers != s_ucLastPolled)
		{
			s_ucLastPolled = s_ucModifiers;
			SetModifierKeys (s_ucModifiers);
		}
	}

	// LED writes happen here, not in the USB handler: see the comment on
	// s_nLEDUpdatesPending.
	FlushLEDUpdates ();

	// Hand the accumulated mouse state to the core.  The core asks for it
	// through input_cb() right after this returns, and clears our deltas as
	// it reads them.
	// Mouse speed (boot.conf mouse_speed, the menu): scale the movement, and
	// keep what does not make a whole step for the next poll, so that a slow
	// setting still moves on small motions instead of rounding them away.
	if (s_nMouseSpeed != 100)
	{
		const int nX = s_nMouseDX * static_cast<int> (s_nMouseSpeed) + s_nMouseRestX;
		const int nY = s_nMouseDY * static_cast<int> (s_nMouseSpeed) + s_nMouseRestY;
		s_nMouseDX = nX / 100;
		s_nMouseDY = nY / 100;
		s_nMouseRestX = nX % 100;
		s_nMouseRestY = nY % 100;
	}

	BarePushMouse (s_nMouseDX, s_nMouseDY, s_nMouseButtons);
	s_nMouseDX = 0;
	s_nMouseDY = 0;

	// Give Circle's cooperative scheduler a chance to run the USB task.  This
	// is what keeps the USB stack alive while the emulator is running: the
	// scheduler is non-preemptive, so anything that loops for a long time has
	// to yield explicitly.  Doing it once per frame is enough and costs
	// nothing measurable.
	// Only the input work counts as "Teclado y ratón": the Yield below runs
	// the emulator (and the network's tasks), which is not input.
	BareProfileAdd (BareProfileInput, BareProfileClock () - nProfileStart);

	CScheduler::Get ()->Yield ();
}

// ---------------------------------------------------------------------------
// What bare_libretro.cpp asks for, once per retro_run()
// ---------------------------------------------------------------------------

boolean BareInputHasGamepad (void)
{
	return s_nPads != 0;
}

unsigned BareInputGamepads (void)
{
	return s_nPads;
}

extern "C" u32 BareJoypadBits (void)
{
	u32 nBits = s_nPadButtons;

	// The d-pad does double duty: the digital directions *and* full deflection
	// on the left stick.  Done here rather than in the status handler so that
	// it is one place and so that a pad which only sends the hat still drives
	// the axes.  Full deflection is 32767, which is what a DOS joystick reads
	// as "all the way over".
	if (s_nPadAxis[0] == 0 && s_nPadAxis[1] == 0)
	{
		if (nBits & (1u << RETRO_DEVICE_ID_JOYPAD_LEFT))
		{
			s_nPadAxis[0] = -32768;
		}
		else if (nBits & (1u << RETRO_DEVICE_ID_JOYPAD_RIGHT))
		{
			s_nPadAxis[0] = 32767;
		}

		if (nBits & (1u << RETRO_DEVICE_ID_JOYPAD_UP))
		{
			s_nPadAxis[1] = -32768;
		}
		else if (nBits & (1u << RETRO_DEVICE_ID_JOYPAD_DOWN))
		{
			s_nPadAxis[1] = 32767;
		}
	}

	return nBits;
}

extern "C" s16 BareJoypadAxis (unsigned nIndex, unsigned nId)
{
	// Called with the libretro index/id pair.  Only the four axes of the two
	// sticks mean anything here; everything else (triggers) is reachable as a
	// digital button and is not an axis of a DOS joystick.
	unsigned nSlot;
	switch (nIndex)
	{
	case RETRO_DEVICE_INDEX_ANALOG_LEFT:
		nSlot = nId == RETRO_DEVICE_ID_ANALOG_X ? 0 : 1;
		break;
	case RETRO_DEVICE_INDEX_ANALOG_RIGHT:
		nSlot = nId == RETRO_DEVICE_ID_ANALOG_X ? 2 : 3;
		break;
	default:
		return 0;
	}

	// Range-checked, because the sign of a bad index here would be a read out
	// of a static array in a file that has no way to recover.
	if (nSlot >= sizeof s_nPadAxis / sizeof s_nPadAxis[0])
	{
		return 0;
	}

	// s16 and not int16_t: see the note in bare_env.h - this file cannot
	// assume <cstdint>, and Circle's s16 is the same type libretro specifies.
	return static_cast<s16> (s_nPadAxis[nSlot]);
}

void BareInputSetMouseSpeed (unsigned nPercent)
{
	s_nMouseSpeed = nPercent < 25 ? 25 : nPercent > 400 ? 400 : nPercent;
	s_nMouseRestX = s_nMouseRestY = 0;
}

boolean BareInputCapsLock (void)
{
	// The LED state is the lock state: both are toggled by the same key press.
	return (s_ucLEDState & LEDCapsLock) != 0 ? TRUE : FALSE;
}
