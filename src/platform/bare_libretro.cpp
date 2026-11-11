//
// bare_libretro.cpp
//
// by @mmoroca & arena.ai 2026  -  https://arena.ai
//
// BM-DOS - the libretro "frontend" that DOSBox-core boots into.
//
// DOSBox-core was written as a libretro core, so it expects a host that:
//   * registers a set of callbacks (video refresh, audio batch, input state,
//     environment, log),
//   * calls retro_init(), retro_load_game(), then retro_run() once per frame,
//   * and takes the frames back through the video/audio callbacks.
//
// In this port the host is a Circle kernel.  This file is the whole of that
// host: about 500 lines, no libretro library, no RetroArch.  Everything the
// core asks for is answered directly by Circle drivers through the small
// Bare* interfaces in bare_env.h.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
#include "bare_coreopts.h"
#include "bare_env.h"

#include "libretro.h"
#include "libretro_dosbox.h"


// Provided by bare_thread.cpp: the single threaded replacement for the shim's
// std::thread based context switch.  Keep these three declarations in sync with
// that file - it is the only place they exist.
typedef void TBareFrameNotify (void *pParam, boolean bFrameComplete);
extern "C" void BareThreadSetFrameNotify (TBareFrameNotify *pNotify, void *pParam);
extern "C" boolean BareThreadAborted (void);

#include <circle/logger.h>
#include <circle/startup.h>
#include <circle/timer.h>
#include <circle/util.h>

#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>
#include <strings.h>
#include <cstring>

// The ordering rule for retro_set_controller_port_device().  Header only, no
// dependencies, and check-portorder runs its state machine on the host.
#include "bare_portorder.h"

// The core exports these.  Declared here rather than by including
// libretro.h's C++ wrappers so the linkage is explicit.
extern "C" {
void retro_init (void);
void retro_deinit (void);
unsigned retro_api_version (void);
void retro_set_environment (retro_environment_t);
void retro_set_video_refresh (retro_video_refresh_t);
void retro_set_audio_sample (retro_audio_sample_t);
void retro_set_audio_sample_batch (retro_audio_sample_batch_t);
void retro_set_input_poll (retro_input_poll_t);
void retro_set_input_state (retro_input_state_t);
void retro_set_controller_port_device (unsigned port, unsigned device);
bool retro_load_game (const retro_game_info *game);
void retro_unload_game (void);
void retro_run (void);
void retro_reset (void);
void *retro_get_memory_data (unsigned id);
size_t retro_get_memory_size (unsigned id);
}

static const char FromBare[] = "dosbox";

// ---------------------------------------------------------------------------
// Frontend state
// ---------------------------------------------------------------------------

namespace {

constexpr unsigned MaxMessageLength = 78;

TBareFrameHook *s_pFrameHook = nullptr;
void *s_pFrameHookParam = nullptr;

boolean s_bWantsExit = FALSE;

// Ctrl+F9 asks for the shutdown, Alt+Pause stops the machine where it is.  Both
// are read by the frame loop in kernel.cpp.
boolean s_bPaused = FALSE;

// Whether the core is allowed to be told about the game port yet, what it has
// been told, and when.  See bare_portorder.h - the rule, the core code behind
// it, and the boot it cost.
bare_portorder::TState s_PortOrder = bare_portorder::NewState ();

char s_Message[MaxMessageLength + 1];
u64 s_MessageDeadline = 0;		// in microseconds

// Mouse deltas accumulated since the last frame.
s16 s_MouseDX = 0;
s16 s_MouseDY = 0;
u32 s_MouseButtons = 0;

// Keyboard state, indexed by RETROK_* up to 512 (libretro's key space).
// DOSBox-core's input shim reads this through the input callback.
bool s_KeyState[512];

} // namespace

// ---------------------------------------------------------------------------
// log
// ---------------------------------------------------------------------------

// libretro log levels map onto Circle's logger severity.
//
// Which header declares this typedef matters, and it took three attempts to get
// this line right.  There is exactly ONE libretro.h in this build: the one
// shipped inside dosbox-core (libretro/deps/common/include/libretro.h).  Our
// own hand-written subset in src/libretro-common is no longer on the include
// path, because with two headers the answer to "does the parameter type match"
// depends on -I order - and it silently did.
//
// Upstream's header says:
//
//     typedef void (RETRO_CALLCONV *retro_log_printf_t)(enum retro_log_level level, ...)
//
// so the callback takes `enum retro_log_level`, NOT `int`.  (The published
// libretro.h used to say `int level`; that is where the wrong comment that used
// to be here came from.)  Inside, switch(level) with the RETRO_LOG_* constants
// is unchanged: they become plain int values here, which is what Circle's
// logger wants.
static void BareLogCallback (retro_log_level level, const char *fmt, ...)
{
	char buffer[256];

	va_list args;
	va_start (args, fmt);
	vsnprintf (buffer, sizeof buffer, fmt, args);
	va_end (args);

	int circleLevel;
	switch (level)
	{
	case RETRO_LOG_DEBUG:	circleLevel = 1; break;
	case RETRO_LOG_INFO:	circleLevel = 2; break;
	case RETRO_LOG_WARN:	circleLevel = 3; break;
	case RETRO_LOG_ERROR:
	default:		circleLevel = 4; break;
	}

	BareLog (circleLevel, buffer);
}

// ---------------------------------------------------------------------------
// video
// ---------------------------------------------------------------------------

static void BareVideoRefresh (const void *data, unsigned width, unsigned height,
			      size_t pitch)
{
	// A null data pointer means "duplicate the previous frame" in libretro.
	// Our video backend keeps the frame it already has, so this is a no-op.
	if (data == nullptr)
	{
		return;
	}

	BareVideoPresent (static_cast<const u32 *> (data), width, height,
			  static_cast<unsigned> (pitch));
}

// ---------------------------------------------------------------------------
// audio
// ---------------------------------------------------------------------------

static size_t BareAudioBatch (const int16_t *data, size_t frames)
{
	// Never blocks (bare_audio.cpp drops what does not fit).  Pacing to real
	// time is the frame loop's job now: it lives in kernel.cpp, which is also
	// the side that can tell whether DOSBox is still running.
	BareAudioQueue (data, static_cast<unsigned> (frames));

	return frames;
}

// ---------------------------------------------------------------------------
// input
// ---------------------------------------------------------------------------

static void BareInputPollCallback (void)
{
	// The core polls us once per frame, before reading input state.  Circle's
	// USB stack and cooperative scheduler need to be serviced from TASK_LEVEL;
	// doing it here means it happens exactly once per frame, from the frame
	// thread, which is where we want it.
	BareInputPoll ();
}

static int16_t BareInputState (unsigned port, unsigned device, unsigned index,
			       unsigned id)
{
	if (port != 0)
	{
		return 0;
	}

	switch (device)
	{
	case RETRO_DEVICE_KEYBOARD:
		if (id < 512)
		{
			return s_KeyState[id] ? 1 : 0;
		}
		return 0;

	case RETRO_DEVICE_MOUSE:
		switch (id)
		{
		case RETRO_DEVICE_ID_MOUSE_X: {
			const s16 dx = s_MouseDX;
			s_MouseDX = 0;
			return dx;
		}
		case RETRO_DEVICE_ID_MOUSE_Y: {
			const s16 dy = s_MouseDY;
			s_MouseDY = 0;
			return dy;
		}
		case RETRO_DEVICE_ID_MOUSE_LEFT:
			return (s_MouseButtons & 1) ? 1 : 0;
		case RETRO_DEVICE_ID_MOUSE_RIGHT:
			return (s_MouseButtons & 2) ? 1 : 0;
		case RETRO_DEVICE_ID_MOUSE_MIDDLE:
			return (s_MouseButtons & 4) ? 1 : 0;
		default:
			return 0;
		}

	case RETRO_DEVICE_JOYPAD:
		// .60.  Until this round the only answer here was 0, under the comment
		// "no joypads in the first milestone" - and that is why a gamepad
		// Circle detects perfectly was invisible to DOSBox.  dosbox-core reads
		// the pad through this callback and through nothing else: with a
		// constant 0 it sees a controller that is connected and never pressed,
		// which is indistinguishable, from the emulator's side, from a pad
		// nobody is touching.
		if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
		{
			// The core asks for a whole bitmask when the frontend advertises
			// the fast path.  This port answers "no" to GET_INPUT_BITMASKS,
			// so the core builds the mask bit by bit and never reaches this
			// - but a zero here would be a wrong answer if it ever did, and
			// answering it properly costs one read of a variable that is
			// already maintained per poll.
			return static_cast<int16_t> (BareJoypadBits ());
		}
		return (BareJoypadBits () & (1u << id)) != 0 ? 1 : 0;

	case RETRO_DEVICE_ANALOG:
		// index = which stick (LEFT/RIGHT), id = X or Y.  16-bit signed, which
		// is what libretro specifies; the pad's own range is converted in
		// bare_input.cpp, because that is where the pads live.
		return static_cast<int16_t> (BareJoypadAxis (index, id));

	default:
		// Pointer, lightgun, rumble...: the documented "no" answer.
		return 0;
	}
}

// The other half of the input path: bare_input.cpp decodes USB HID reports and
// pushes key/button state in here; DOSBox reads it back through the input state
// callback above.  Declared in bare_env.h.
// The core's keyboard event callback.  DOSBox-core installs it during
// retro_load_game (libretro_input_init -> RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK)
// and it is the ONLY way a key reaches the emulated machine: see the comment at
// the top of bare_input.cpp.  Until this port stored it, no key could ever
// arrive, and the core answered every input_cb(RETRO_DEVICE_KEYBOARD) question
// that nobody asks.
static retro_keyboard_callback s_CoreKeyboardCallback {};

// The core's disk control interface (RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE).
static retro_disk_control_callback s_DiskControl {};
static boolean s_bDiskControl = FALSE;
static boolean s_bCoreKeyboardCallbackSet = FALSE;

// Keys delivered, and the last one.  Kept after the status line stopped showing
// them: this is how you answer "does the USB side see the key at all" from a
// debugger or from code, and it is three counters.  If the USB side is alive
// they change every time a key is touched, whether or not DOS reacts.
static unsigned s_nKeyEvents = 0;
static unsigned s_nLastKey = 0;
static boolean s_bLastKeyDown = FALSE;

extern "C" void BarePushKey (unsigned nRetroKey, boolean bDown)
{
	if (nRetroKey < 512)
	{
		s_KeyState[nRetroKey] = bDown ? true : false;
	}
}

/// \brief The one path from a USB key to the emulated machine.
///
/// Two things happen here, and the order matters:
///
///   1. the level array in this file is updated, so anything that *does* ask
///      input_cb keeps seeing the truth;
///   2. the core's keyboard callback is invoked, which is what makes DOSBox
///      queue a scancode, see the key, and (for Caps Lock) mirror the LEDs.
///
/// The character argument is 0: DOSBox-core ignores it (it maps keycodes to
/// scancodes through retro_dosbox_map) and passing a guess would be inventing
/// information.  The modifiers argument is the CURRENT modifier state, read
/// from the array just updated, which is what a real frontend reports.
// ---------------------------------------------------------------------------
// The emulated machine's own reset, which DOS asks for with Ctrl+Alt+Del
// ---------------------------------------------------------------------------
//
// DOS does not reboot by itself: Ctrl+Alt+Del is a keyboard *interrupt*, and
// the BIOS answers it by jumping to the reset vector.  DOSBox-core has no such
// path - the SDL frontend's Restart handler lives in sdlmain.cpp, which is not
// part of a libretro build, and nothing in the core watches for the sequence.
// So the port watches for it.
//
// It is checked while the keys go down, and the reboot is deferred to the
// keyboard callback that follows: the emulator task is parked at a frame
// boundary at that moment and it is safe to touch DOSBox's globals there.  The
// actual reboot reloads the program from the reset vector, which is what a
// power-on would do, and it keeps the mounted drives because the mount
// happened in autoexec - a *soft* reboot is what a PC's Ctrl+Alt+Del gives.
static boolean s_bRebootArmed = FALSE;

// A screenshot was asked for and the frame loop has not taken it yet.
static boolean s_bScreenshotPending = FALSE;

// Ctrl state, read from the level array.  Used by the hotkeys below; a small
// function because "either Ctrl" appears in three conditions and getting one of
// them wrong is the kind of bug that only shows up on the other keyboard.
static boolean IsCtrlDown (void)
{
	return (s_KeyState[RETROK_LCTRL] || s_KeyState[RETROK_RCTRL]) ? TRUE : FALSE;
}

static boolean IsAltDown (void)
{
	return (s_KeyState[RETROK_LALT] || s_KeyState[RETROK_RALT]) ? TRUE : FALSE;
}

extern "C" boolean BareTakeScreenshotRequest (void)
{
	if (!s_bScreenshotPending)
	{
		return FALSE;
	}

	s_bScreenshotPending = FALSE;

	return TRUE;
}

static void CheckCtrlAltDel (unsigned nRetroKey, boolean bDown)
{
	if (!bDown)
	{
		if (nRetroKey == RETROK_DELETE)
		{
			s_bRebootArmed = FALSE;
		}
		return;
	}

	if (IsCtrlDown () && IsAltDown () && nRetroKey == RETROK_DELETE)
	{
		s_bRebootArmed = TRUE;
	}
}

extern "C" void BareDeliverKey (unsigned nRetroKey, boolean bDown)
{
	if (nRetroKey >= 512)
	{
		return;
	}

	// The setup menu (Win+F12) sees every key first.  While it is open the
	// keys are its own: none reaches DOS, so nothing typed in the menu turns
	// up in the game afterwards.  Win counts as held while its key is down,
	// including the press of Win itself.
	{
		const boolean bWin = (nRetroKey == RETROK_LSUPER || nRetroKey == RETROK_RSUPER)
				     ? bDown
				     : (s_KeyState[RETROK_LSUPER] || s_KeyState[RETROK_RSUPER]) ? TRUE : FALSE;
		if (BareMenuKey (nRetroKey, bDown, bWin))
		{
			return;
		}
	}

	BarePushKey (nRetroKey, bDown);

	s_nKeyEvents++;
	s_nLastKey = nRetroKey;
	s_bLastKeyDown = bDown;

	// Ctrl+Alt+Del's rebooting did not work, and the reason is worth writing
	// down: delivering the three-key sequence to the guest is correct, but by
	// the time it is delivered DOSBox has consumed the Ctrl and Alt presses
	// and the emulated BIOS only ever sees Delete going down and up on its
	// own.  What a real machine does with Ctrl+Alt+Del is a *keyboard
	// interrupt* the BIOS recognises as a whole, and this wrapper cannot
	// produce that.
	//
	// So the emulated machine keeps the plain three keys (DOS apps use them
	// for themselves), and the reboot a person actually wants - "start this
	// machine again from scratch" - lives on a key combination no DOS program
	// uses and no DOSBox hotkey claims.

	// PetSis on its own: show or hide the status line.  Started hidden, so the
	// first press brings it up.  This is a port feature, not a DOS one - the
	// key still reaches DOS as PrintScreen as well, because a diagnostic
	// switch is not worth stealing a key from the emulated machine.
	//
	// Note the order: Ctrl+Alt+PetSis (the reboot) is checked below, so that
	// combination does not also toggle the panel on its way past.
	if (nRetroKey == RETROK_SYSREQ && bDown && !IsCtrlDown () && !IsAltDown ())
	{
		const boolean bNewState = BareVideoFooterVisible () ? FALSE : TRUE;
		BareVideoFooterSetVisible (bNewState);
	}

	// Ctrl+F9: shut the machine down safely.
	//
	// This was Alt+F12 in the first version of this round, and that was wrong
	// twice over.  Wrong because the emulated DOSBox's own "intro special"
	// screen - which a user will read, because it is the first thing typed at
	// the prompt - says "ALT-F12 : Unlock speed (turbo button/fast forward)".
	// A key that the documentation on screen assigns to turbo must not shut the
	// machine down.  And wrong because Ctrl+F9 is *already* the right key: it is
	// what that same screen calls "Kill DOSBox", the meaning is exactly
	// "stop the emulator and exit", and in this build it does nothing at all
	// (libretro_input.cpp handles only Ctrl+F11, Ctrl+F12, Ctrl+F4 and Insert,
	// so every other entry of that list is dead).  So this makes one entry of
	// DOSBox's own list true instead of inventing a key next to it.
	//
	// Not the old key again: Alt+F12 is deliberately left alone, so that
	// pressing what the screen advertises as turbo does not stop the machine.
	//
	// Where the message goes, and why this is not obvious: this used to call
	// BareVideoStatusPrint(), which writes to the *boot*
	// overlay in the top-left corner - and that overlay switches itself off on
	// the first frame the core presents (BareVideoPresent).  During DOS it is
	// therefore a no-op, so the one key that stops the machine without asking
	// was the only one that gave no sign at all.  The status line is the right
	// place: it is drawn in the strip the emulator never touches (see
	// ComputeDestination), it is repainted every frame by DrawSlot(), and if
	// the panel has been hidden there is still the logger, which goes to tty1
	// and to the serial port.
	//
	// The text says what is about to happen and what must not be done, because
	// the next thing on screen is a black display with no cursor.
	if (nRetroKey == RETROK_F9 && bDown && IsCtrlDown ())
	{
		BareCoreRequestShutdown ();
		return;
	}

	// Alt+Pause: pause and resume the emulated machine.
	//
	// Another entry of that same screen made true: "ALT-PAUSE : Pause DOSBox",
	// which does nothing in a libretro build.  Pausing here means *not calling
	// retro_run*, which is the whole of it: the emulated machine stops where it
	// is, the picture stays, and the pacer has nothing to wait for.  The
	// governor sees an idle screen and walks down to 600 MHz on its own, which
	// is the behaviour a paused machine should have anyway.
	//
	// Resuming does not reset anything: the frame deadline is re-anchored on the
	// first frame after the pause (see the loop in kernel.cpp), so coming back
	// does not bill the machine for the time it spent stopped.
	if (nRetroKey == RETROK_PAUSE && bDown && IsAltDown ())
	{
		s_bPaused = !s_bPaused;
		CLogger::Get ()->Write (FromBare, LogNotice, "Alt+Pause: %s",
					s_bPaused ? "paused" : "resumed");
		return;
	}

	// The old Alt+F12 comment, kept for the reasoning it carries about why a
	// bare metal port needs a safe shutdown at all:
	// exists because the honest answer to "basta con desconectar la corriente?"
	// is no - see the long note in CKernel::Cleanup(), which is where the card
	// is actually flushed.  This only asks for it: the flush happens after the
	// emulator has stopped, in the frame loop's exit path, because flushing a
	// FAT volume from inside a keyboard callback (which runs inside the
	// emulator's own call chain) would stop the emulator dead for however long
	// the card takes.
	//
	// Why Alt+F12 and not Ctrl+F9, which is what desktop DOSBox uses: the core
	// owns F12 in combination with Ctrl (Ctrl+F12 changes the cycle count, and
	// that is a genuinely useful key here), and Ctrl+F9 does not exist in a
	// libretro build at all - there is no window to close.  Alt+F12 is free:
	// the core's mapper has no Alt+F12 handler, and DOS programs do not use it.
	// F12 on its own still goes through to DOS.
	// Ctrl+Alt+PetSis: reboot the BOARD.  "PetSis" is how the Spanish keyboard
	// calls the key marked ImpPt / PetSis (Print Screen); Circle reports it as
	// RETROK_SYSREQ from USB usage 0x46.  Alt+PrintScreen is a DOS-era
	// hotkey in some programs, which is exactly why Ctrl is in the combination
	// as well: Ctrl+Alt+PetSis is not used by DOS, by DOSBox's mapper
	// (MK_printscreen is free) or by anything else here.  This is the one hotkey DOSBox's own
	// mapper has that a bare metal port needs more than a desktop does: if
	// the emulator goes wrong, Ctrl+Alt+Del cannot save you, because the
	// emulated machine is not what is broken.  Circle reboots the SoC and the
	// kernel comes back from the SD card, which is the same thing the power
	// switch does but without getting up.
	if (nRetroKey == RETROK_SYSREQ && bDown && IsCtrlDown () && IsAltDown ())
	{
		BareCoreRequestReboot ();
		return;
	}

	// Ctrl+F5 is the port's own screenshot hotkey.  Deliberately *not* reported
	// to the core, and only on the press: the core does not implement
	// DOSBox's screenshot (C_SSHOT is off in a libretro build, so
	// MAPPER_AddHandler for it is not even compiled in), so this is the only
	// Ctrl+F5 that can exist here.  F5 alone goes through to DOS untouched -
	// plenty of games use it.
	//
	// Why Ctrl+F5 and not F12: some emulators grab F12 for a menu, and this
	// port needs a key that reaches it and nothing else uses.
	if (nRetroKey == RETROK_F5 && bDown && IsCtrlDown ()
	    && !s_bScreenshotPending)
	{
		// Wanting a screenshot is recorded here; *taking* it happens in the
		// frame loop, at a point where the emulator is not running.
		//
		// That indirection is not tidiness.  Writing a BMP to the SD card is
		// slow - tens of milliseconds, sometimes hundreds, because FatFs
		// writes a sector at a time and the card can stall - and this function
		// runs inside the emulator's own call chain (DOSBox -> keyboard
		// callback -> here).  Doing it here stops the emulator dead for that
		// long, and the sound device, which is fed from the same task, runs
		// out of data and drops everything that was queued.  That is exactly
		// the shape of the "drop keeps climbing" the panel was showing.
		s_bScreenshotPending = TRUE;
		return;
	}

	if (!s_bCoreKeyboardCallbackSet || s_CoreKeyboardCallback.callback == nullptr)
	{
		return;
	}

	// Ctrl+Alt+Del: the sequence is consumed here, so DOS never sees the
	// three keys as typing, and the reboot happens on the Delete press.
	CheckCtrlAltDel (nRetroKey, bDown);
	if (s_bRebootArmed)
	{
		s_bRebootArmed = FALSE;

		CLogger::Get ()->Write (FromBare, LogNotice, "Ctrl+Alt+Del: rebooting");
		BareVideoStatusPrint ("Ctrl+Alt+Del: rebooting");

		// NOT retro_reset().  DOSBox-core's retro_reset() does
		// Control->StartUp() again on the emulator thread - the same call
		// that builds the machine at power-on - and this port has no way to
		// know what half-initialised state that leaves behind, while the
		// screen keeps showing the old machine.  The honest alternative is
		// to finish the sequence for the guest: the three keys are down, so
		// the emulated BIOS gets the Delete it was waiting for and resets
		// itself, which is exactly what a PC does.
		s_CoreKeyboardCallback.callback (true, RETROK_DELETE, 0, 0);
		s_CoreKeyboardCallback.callback (false, RETROK_DELETE, 0, 0);

		return;
	}

	// (There used to be a RETROK_RSHIFT -> RETROK_LSHIFT rewrite here.  It was
	// wrong twice: retro_dosbox_map *does* contain RETROK_RSHIFT -> KBD_rightshift
	// - I claimed otherwise without checking, and the key that broke was the
	// one I had not added at all, Caps Lock - and rewriting it also made both
	// Shift keys arrive as the left one, which is worse than what it fixed.
	// The two shifts are distinct scancodes in the emulated keyboard and DOS
	// apps can tell them apart; they are now delivered untouched.)

	unsigned nModifiers = 0;
	if (s_KeyState[RETROK_LSHIFT] || s_KeyState[RETROK_RSHIFT])
	{
		nModifiers |= RETROKMOD_SHIFT;
	}
	if (s_KeyState[RETROK_LCTRL] || s_KeyState[RETROK_RCTRL])
	{
		nModifiers |= RETROKMOD_CTRL;
	}
	if (s_KeyState[RETROK_LALT] || s_KeyState[RETROK_RALT])
	{
		nModifiers |= RETROKMOD_ALT;
	}

	s_CoreKeyboardCallback.callback (bDown ? true : false, nRetroKey,
					 /*character*/ 0,
					 static_cast<uint16_t> (nModifiers));
}

void BareInputLastKey (unsigned *pKey, boolean *pDown)
{
	if (pKey != nullptr)
	{
		*pKey = s_nLastKey;
	}

	if (pDown != nullptr)
	{
		*pDown = s_bLastKeyDown;
	}
}

boolean BareInputCoreCallbackInstalled (void)
{
	return s_bCoreKeyboardCallbackSet
		&& s_CoreKeyboardCallback.callback != nullptr ? TRUE : FALSE;
}

/// \brief Keys delivered since the last call, and reset the counter.
///
/// Nothing calls this from the frame loop any more - the status line dropped the
/// key counters once the keyboard was proven - but it stays as the cheap way to
/// check the USB path from code.
unsigned BareInputKeyEvents (void)
{
	const unsigned n = s_nKeyEvents;
	s_nKeyEvents = 0;

	return n;
}

extern "C" void BarePushMouse (int nDX, int nDY, u32 nButtons)
{
	s_MouseDX = static_cast<s16> (nDX);
	s_MouseDY = static_cast<s16> (nDY);
	s_MouseButtons = nButtons;
}

// ---------------------------------------------------------------------------
// environment: the core asks the host a hundred questions at start-up.
// We answer the ones that matter and say "no" to everything else, which is
// the documented libretro behaviour for an unsupported feature.
// ---------------------------------------------------------------------------

static bool BareEnvironment (unsigned cmd, void *data)
{
	switch (cmd)
	{
	case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
	case RETRO_ENVIRONMENT_SET_GEOMETRY:
	{
		// The core telling us the rate it is running at (or the geometry it
		// just switched to).  Answering "no" here - which is what this port
		// used to do - is not harmless: with RUN_SYNC the core then returns
		// from retro_run() once per *emulated* frame at that frame's rate, and
		// the frame loop had no way to know whether that was 70 Hz or 50 Hz.
		// It paced everything at 60, so a 70 Hz game ran at 70/60 = 1.17x
		// speed on a 60 Hz screen... except slower, because the loop then
		// waited.  Both symptoms - running too fast, and cycles not fixing it -
		// come from that one mismatch: too fast
		// because the emulated clock advances 70 times per 60 presented
		// frames; not fixable with cycles because cycles change how much
		// *work* fits in an emulated frame, not how many frames are run per
		// second.
		//
		// Only the timing is taken here.  The geometry is the core's business
		// and it arrives with every frame anyway; the aspect ratio is the one
		// number this port cannot derive (only the core knows what the VGA is
		// doing), so it is passed on to the video layer.
		const retro_system_av_info *pInfo =
			static_cast<const retro_system_av_info *> (data);
		if (pInfo == nullptr)
		{
			return false;
		}

		BareVideoSetRefreshHz (pInfo->timing.fps);

		// aspect_ratio is a double; turn it into a small fraction.  The core
		// reports 4/3 for every mode that matters, and 1.0 for the ones that
		// do not, so six candidates is plenty and keeps the arithmetic in the
		// video layer in integers.
		static const struct { unsigned nNum; unsigned nDen; } Aspects[] = {
			{ 4, 3 }, { 16, 9 }, { 16, 10 }, { 5, 4 }, { 1, 1 },
		};
		double fWant = pInfo->geometry.aspect_ratio;
		if (fWant > 0.5 && fWant < 3.0)
		{
			unsigned nBest = 0;
			double fBestDiff = 1.0;
			for (unsigned n = 0; n < sizeof Aspects / sizeof Aspects[0]; n++)
			{
				const double fThis = static_cast<double> (Aspects[n].nNum)
						     / static_cast<double> (Aspects[n].nDen);
				const double fDiff = fThis > fWant ? fThis - fWant : fWant - fThis;
				if (fDiff < fBestDiff)
				{
					fBestDiff = fDiff;
					nBest = n;
				}
			}

			if (fBestDiff < 0.02)
			{
				BareVideoSetSourceAspect (Aspects[nBest].nNum,
							  Aspects[nBest].nDen);
			}
		}

		return true;
	}

	case RETRO_ENVIRONMENT_GET_TARGET_REFRESH_RATE:
	{
		// How fast the display refreshes, in Hz.  The core uses it to decide
		// whether it can run a 70 Hz game at its own rate; this port's
		// display runs at whatever the firmware chose, and the frame loop
		// paces against the *core's* rate, so what is reported here is the
		// nominal 60 and the honest answer to "which one wins" is in
		// kernel.cpp: the slower of the two.
		if (data == nullptr)
		{
			return false;
		}
		*static_cast<float *> (data) = 60.0f;
		return true;
	}

	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
		// The core asks for XRGB8888.  That is what Circle's frame buffer
		// works in too, so accept every format we can convert quickly.
		return *static_cast<retro_pixel_format *> (data) == RETRO_PIXEL_FORMAT_XRGB8888;

	case RETRO_ENVIRONMENT_GET_CAN_DUPE:
		*static_cast<bool *> (data) = true;
		return true;

	// Paths without the "SD:" volume name on purpose: the core puts these
	// through std::filesystem (it joins them, canonicalises them and iterates
	// them), and std::filesystem has no notion of a volume name.  See the
	// comment on DOSBOX_CONFIG_ABS in kernel.h.
	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
		*static_cast<const char **> (data) = "/dosbox/system";
		return true;

	case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
		*static_cast<const char **> (data) = "/dosbox/saves";
		return true;

	case RETRO_ENVIRONMENT_GET_CONTENT_DIRECTORY:
		*static_cast<const char **> (data) = "/dosbox";
		return true;

	case RETRO_ENVIRONMENT_GET_PERF_INTERFACE:
		// The core uses this for retro_ticks().  Giving it a real interface
		// means its log timestamps and its internal frame timing work; it
		// degrades gracefully if we say no, but there is no reason to.
		{
			// retro_time_t is int64_t and BareMicros() returns u64 (Circle's
			// 64-bit clock counter), so the member gets a small adapter rather
			// than an implicit conversion the compiler will not make.
			static const retro_perf_callback perf = {
				[]() -> retro_time_t {
					return static_cast<retro_time_t> (BareMicros ());
				},			// get_time_usec
				nullptr,		// get_cpu_features
				nullptr,		// get_perf_counter
				nullptr,		// perf_register
				nullptr,		// perf_start
				nullptr,		// perf_stop
				nullptr,		// perf_log
			};
			*static_cast<retro_perf_callback **> (data) =
				const_cast<retro_perf_callback *> (&perf);
		}
		return true;

	case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
		return false;		// we do not need the bitmask fast path

	case RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK:
		{
			const auto *pCallback = static_cast<const retro_keyboard_callback *> (data);
			if (pCallback != nullptr && pCallback->callback != nullptr)
			{
				s_CoreKeyboardCallback = *pCallback;
				s_bCoreKeyboardCallbackSet = TRUE;

				CLogger::Get ()->Write (FromBare, LogNotice,
					"core: keyboard callback installed");
				return true;
			}
		}
		return false;

	case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
		// A braced list cannot be assigned to this struct; set the member.
		static_cast<retro_log_callback *> (data)->log = BareLogCallback;
		return true;

	case RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION:
		// Version 0 = the simple retro_message {text, duration_frames}.
		*static_cast<unsigned *> (data) = 0;
		return true;

	case RETRO_ENVIRONMENT_SET_MESSAGE:
		{
			const auto *msg = static_cast<const retro_message *> (data);
			if (msg && msg->msg)
			{
				strncpy (s_Message, msg->msg, MaxMessageLength);
				s_Message[MaxMessageLength] = '\0';
				// duration is in frames; assume 60 for the deadline
				s_MessageDeadline = BareMicros ()
						  + (u64) msg->frames * 1000000ULL / 60ULL;
				BareVideoShowMessage (s_Message);
			}
		}
		return true;

	case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
	case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK:
	case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
		return true;		// accepted, nothing to do

	case RETRO_ENVIRONMENT_SHUTDOWN:
		s_bWantsExit = TRUE;
		return true;

	case RETRO_ENVIRONMENT_GET_FASTFORWARDING:
		*static_cast<bool *> (data) = false;
		return true;

	// Note for the log level: dosbox-core's own default for the "log_level"
	// option is "debug" (libretro_core_options.cpp), and retro::internal's
	// log_level starts at RETRO_LOG_DEBUG too (log.cpp).  Since this frontend
	// answers GET_VARIABLE with false, update_log_verbosity() is never able to
	// lower it, so the core runs at debug already.  That is why the log
	// callback in bare_time.cpp sees every message and can put all of them on
	// screen.  Answering the option properly would LOWER the verbosity.
	case RETRO_ENVIRONMENT_SET_VARIABLES:
	case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
		// There is no option UI and nothing ever changes an option at run
		// time, so "not updated" is the whole truth here.
		return false;

	case RETRO_ENVIRONMENT_GET_VARIABLE:
	{
		// This used to return false for every option, which hands each one to
		// the default baked into dosbox-core's own table.  For the CPU cycles
		// that default is 386DX class and it *overrides* sd/dosbox.conf - the
		// core re-applies its own value to the variable during startup, so the
		// .conf line was decoration.  bare_coreopts.h has the full account;
		// the short version is that the emulated machine ran at 10000 cycles
		// no matter what the card said, instead of the 486DX2-66 it was meant to
		// emulate.
		//
		// Only the five options in that table are answered.  Everything else
		// still falls through to the core's default, which is what this port
		// wants for the other forty.
		auto *pVar = static_cast<retro_variable *> (data);
		const char *pValue = bare_coreopts::Lookup (pVar != nullptr ? pVar->key : nullptr);
		if (pValue == nullptr)
		{
			return false;
		}

		pVar->value = pValue;
		return true;
	}

	// Disk control: how the setup menu changes the floppy in A: and the CD in
	// D: while DOS runs.  DOSBox-core offers the extended interface first and
	// the plain one if that is refused; the plain one has everything the menu
	// uses (eject, add, replace, select), so that is the one taken - the
	// extended asks the frontend for version 1 support it does not need.
	case RETRO_ENVIRONMENT_GET_DISK_CONTROL_INTERFACE_VERSION:
	case RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE:
		return false;

	case RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE:
		if (data != nullptr)
		{
			s_DiskControl = *static_cast<const retro_disk_control_callback *> (data);
			s_bDiskControl = TRUE;
			return true;
		}
		return false;

	default:
		return false;
	}
}

// ---------------------------------------------------------------------------
// public interface
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// The frontend, in the order DOSBox calls it
// ---------------------------------------------------------------------------
//
// A libretro frontend is small: set up callbacks, load the content, run frames
// until the core says stop.  Each of the three steps below is a function of
// this file, and each one has a failure mode that is worth knowing before
// reading it:
//
//   BareCoreInit      the core builds its option table and asks ~40 questions
//                     through environ_cb.  Answering "yes" to something the
//                     port does not implement is the classic way to break a
//                     core silently, so the default is "no" (see
//                     BareEnvironment).
//
//   BareCoreLoadGame  retro_load_game() is where DOSBox starts its own task
//                     (std::thread, which circle-stdlib turns into a Circle
//                     task) and mounts the machine.  From the moment it
//                     returns, there are two tasks and control is handed over
//                     with switchThread() - see bare_thread.cpp, which is the
//                     file that makes the hand-over work on a cooperative
//                     scheduler.
//
//   BareCoreRunFrame  one call per emulated frame.  It returns; the frame
//                     itself was already presented from inside the core, by
//                     the video and audio callbacks.
//
boolean BareCoreInit (void)
{
	retro_set_environment (BareEnvironment);
	retro_set_video_refresh (BareVideoRefresh);
	retro_set_audio_sample_batch (BareAudioBatch);
	retro_set_audio_sample (nullptr);
	retro_set_input_poll (BareInputPollCallback);
	retro_set_input_state (BareInputState);

	if (retro_api_version () != RETRO_API_VERSION)
	{
		CLogger::Get ()->Write (FromBare, LogError,
					"libretro API version mismatch (%u != %u)",
					retro_api_version (), RETRO_API_VERSION);
		return FALSE;
	}

	// retro_init() is where the core builds its option table and negotiates the
	// environment, so nothing has to be primed here beyond the callbacks above.
	retro_init ();

	return TRUE;
}

boolean BareCoreLoadGame (const char *pImagePath)
{
	retro_game_info game {};
	game.path = pImagePath;
	game.data = nullptr;
	game.size = 0;
	game.meta = nullptr;

	// dosbox-core with need_fullpath=true ignores data/size and uses path.
	// A ".conf" path is interpreted by the core as "load this configuration
	// and boot into the shell"; anything else is mounted as the game.
	// Written before the call, so that a hang inside it is visibly a hang
	// inside it: the next line on screen is either ours (hand-back), ours
	// (load ok / FAIL), or DOSBox's own error through the log callback.
	// NOT before the load.  .60 did that and the board stopped dead on the
	// next line; bare_portorder.h has the whole account.  The short version,
	// in the core's own code:
	//
	//   retro_set_controller_port_device()
	//     -> libretro_input_init()
	//       -> update_dosbox_variable("joystick", "joysticktype", ...)
	//         -> control->GetSection(...)        <-- `control` is null here
	//
	// `control` is assigned by the emulation task, and that task does not
	// exist until retro_load_game() has started it.  So the announcement goes
	// immediately *after* the load, which is still early enough to matter:
	// DOSBox reads the layout from libretro_input_init(), which runs inside
	// retro_load_game() and again on every video mode change - and there is
	// one of those inside the load.
	//
	// Always a gamepad from the core's point of view: this port presents a
	// modern pad with a d-pad and analogue sticks, which is RETRO_DEVICE_JOYPAD.
	// RETRO_DEVICE_JOYSTICK would only give two axes and two buttons.
	static_assert (RETRO_DEVICE_NONE == bare_portorder::DeviceNone,
		       "libretro.h changed RETRO_DEVICE_NONE");
	static_assert (RETRO_DEVICE_JOYPAD == bare_portorder::DeviceGamepad,
		       "libretro.h changed RETRO_DEVICE_JOYPAD");

	BareVideoStatusPrint ("core: entering retro_load_game");

	if (!retro_load_game (&game))
	{
		CLogger::Get ()->Write (FromBare, LogError,
					"retro_load_game(\"%s\") failed", pImagePath);
		return FALSE;
	}

	// First announcement, and the only unconditional one.  "No pad" has to be
	// said out loud as well: the core's default is "Keyboard + Mouse", so
	// silence leaves the emulated game port switched off with nothing on
	// screen to explain it.
	{
		bare_portorder::NoteLoaded (s_PortOrder);

		const boolean bPad = BareInputHasGamepad () != FALSE;
		bare_portorder::SetPadPresent (s_PortOrder, 0, bPad != FALSE);

		retro_set_controller_port_device (
			0, bare_portorder::DeviceFor (bPad != FALSE));
		bare_portorder::NoteAnnounced (s_PortOrder, 0, BareMicros ());

		BareVideoStatusPrint (bPad ? "core: port 0 = gamepad"
					  : "core: port 0 = none (no pad found)");
	}

	return TRUE;
}

void BareCoreRunFrame (void)
{
	// A pad plugged in *after* the machine started has to be announced, or
	// DOSBox keeps the emulated game port switched off - `connected[0]` inside
	// the core only ever changes when the frontend says so.  Same shape as the
	// mouse problem: USB enumeration finishes whenever it finishes, and the
	// load above took the world as it was.
	//
	// Through the same state machine as the first announcement, because the
	// two rules are the same rules: never before retro_load_game() has
	// returned (bare_portorder.h), and never more than once per state change
	// plus a metering window - retro_set_controller_port_device() destroys and
	// rebuilds the core's whole input map, which is not something to do
	// because one USB report arrived late.
	{
		const boolean bPadPresent = BareInputHasGamepad () != FALSE;
		bare_portorder::SetPadPresent (s_PortOrder, 0, bPadPresent != FALSE);

		if (bare_portorder::Due (s_PortOrder, 0, BareMicros ())
		    == bare_portorder::TAction::Announce)
		{
			retro_set_controller_port_device (
				0, bare_portorder::DeviceFor (bPadPresent != FALSE));
			bare_portorder::NoteAnnounced (s_PortOrder, 0, BareMicros ());

			BareVideoStatusPrint (bPadPresent
					      ? "core: gamepad attached, port 0 = gamepad"
					      : "core: gamepad removed, port 0 = none");
		}
	}

	retro_run ();

	if (s_MessageDeadline != 0 && BareMicros () > s_MessageDeadline)
	{
		s_Message[0] = '\0';
		s_MessageDeadline = 0;

		// Erase the line, do not just forget it.  The message is painted on
		// top of the emulator's picture and nothing repaints that corner, so
		// "forgetting" it left it on screen for ever.
		BareVideoLine (1, nullptr);
	}

	if (s_pFrameHook != nullptr)
	{
		s_pFrameHook (s_pFrameHookParam);
	}
}

boolean BareCorePaused (void)
{
	return s_bPaused;
}

boolean BareCoreWantsExit (void)
{
	return s_bWantsExit || BareThreadAborted () ? TRUE : FALSE;
}

// Adapter between the two shapes of the same idea: bare_thread.cpp says "the
// frame boundary was reached, was it a complete frame or a mode change", and
// the frame hook we expose only cares that *something* happened.
static void FrameNotifyAdapter (void * /*pParam*/, boolean bFrameComplete)
{
	// One line the first time DOSBox hands control back from inside the load.
	// Everything up to "core: load" is our own code; retro_load_game() runs the
	// emulator as far as its first video mode and returns.  If this line never
	// appears, the hang is inside DOSBox's initialisation and not in our frame
	// loop, and that is a different investigation entirely.
	static boolean bSeenOnce = FALSE;
	if (!bSeenOnce)
	{
		bSeenOnce = TRUE;
		BareVideoStatusPrint (bFrameComplete ? "core: first hand-back (frame)"
						     : "core: first hand-back (mode change)");
	}

	if (s_pFrameHook != nullptr)
	{
		s_pFrameHook (s_pFrameHookParam);
	}
}

unsigned BareCorePollRefreshMilliHz (void)
{
	retro_system_av_info Info = {};

	// A zeroed struct on purpose: if the core fills in nothing, timing.fps stays
	// 0.0 and the caller is told "no answer" rather than being handed a number
	// from a struct that was never written.
	retro_get_system_av_info (&Info);

	if (Info.timing.fps > 0.0)
	{
		// Through the announcement path, so the pin and the thread budget see it
		// exactly as they would see a real SET_SYSTEM_AV_INFO.
		BareVideoSetRefreshHz (Info.timing.fps);
	}

	return BareVideoRefreshMilliHz ();
}

void BareCoreSetFrameHook (TBareFrameHook *pHook, void *pParam)
{
	s_pFrameHook = pHook;
	s_pFrameHookParam = pParam;

	BareThreadSetFrameNotify (FrameNotifyAdapter, nullptr);
}

// The shutdown path, and the one call in the port that can hang.
//
// retro_deinit() joins the core's own emulator task.  That join used to wait for
// ever on this port - see the note on s_bExiting in bare_thread.cpp - so the
// board hung with "flushing the card, then halt" on screen.  BareThreadBeginExit()
// is what makes the join finish; the timing below is what makes it *visible*,
// because "it took 40 ms" and "it took 4 s" are two different bugs and a silent
// shutdown cannot tell them apart.
void BareCoreDeinit (void)
{
	// Say "we are leaving" BEFORE anything joins.  This is the first half of the
	// fix for the hang: without it, the emulator task's frame boundaries keep
	// handing the CPU back and letting DOSBox run, so retro_deinit()'s join waits
	// for a task that has no reason to stop.  The other half is in
	// bare_thread.cpp: the exiting branch *throws* out of DOSBox instead of
	// returning into it, because a cooperative-scheduler join needs the task to
	// end, not to keep yielding.
	BareThreadBeginExit ();

	CLogger::Get ()->Write (FromBare, LogNotice,
				"core: unloading (the board halts when this returns)");

	// On the panel as well: this is the step that used to never finish, so it is
	// the one that has to be visible while it happens.  Before the call, because
	// nothing inside it can draw.
	BareVideoFooter (0, "stopping the emulator (waiting for its task to leave)");

	const u64 nStart = BareMicros ();

	// retro_unload_game() is empty in the pinned revision (an empty body in
	// libretro.cpp) and is kept for the sake of the API contract.  retro_deinit()
	// is the one that joins, so it gets its own status line - if a board ever
	// hangs in this function again, the last line on screen says which of the
	// two it was in.
	retro_unload_game ();

	BareVideoFooter (0, "stopping the emulator: unloading the core");
	retro_deinit ();

	const u64 nEnd = BareMicros ();

	// How long the unload took, and the threshold is not arbitrary: a join that
	// finished normally is a handful of milliseconds (one frame boundary and the
	// task leaving).  Anything past half a second means the core did not leave
	// cleanly, and the log has to say that in the one place where a person will
	// look after a shutdown that felt wrong.
	const unsigned nMs = static_cast<unsigned> ((nEnd - nStart) / 1000);
	CLogger::Get ()->Write (FromBare,
				nMs > 500 ? LogWarning : LogNotice,
				"core: unloaded in %u ms", nMs);

	BareVideoFooter (0, "emulator stopped: flushing the card");
}

// ---------------------------------------------------------------------------
// Shared with the setup menu (bare_menu.cpp)
// ---------------------------------------------------------------------------

void BareCoreReleaseAllKeys (void)
{
	// Every key DOS believes is down is released, in the state the core polls
	// and through its keyboard callback.  Opening the menu with Win+F12 leaves
	// Win down as far as DOS knows, and a key released while the menu owns the
	// keyboard would otherwise stay down in the game for ever.
	for (unsigned nKey = 0; nKey < 512; nKey++)
	{
		if (!s_KeyState[nKey])
		{
			continue;
		}

		s_KeyState[nKey] = false;
		if (s_bCoreKeyboardCallbackSet && s_CoreKeyboardCallback.callback != nullptr)
		{
			s_CoreKeyboardCallback.callback (false, nKey, 0, 0);
		}
	}
}

// Ctrl+F9 and the menu's "Apagado seguro" take the same road.
void BareCoreRequestShutdown (void)
{
	CLogger::Get ()->Write (FromBare, LogWarning,
				"Ctrl+F9: shutting down, do not cut power yet");

	// Forced visible: this message is the whole point of the key press,
	// and honouring "panel hidden" here would reproduce exactly the
	// silent shutdown it exists to prevent.
	//
	// .65e: and the *shape* of this matters as much as the visibility.
	// Pressing the key with the panel hidden used to switch the panel on
	// and freeze both its text and the picture: writing the
	// line and only then switching the panel on draws the text into a
	// strip that ComputeDestination() has not reserved yet, so the next
	// frame of the emulator paints over it and the slot remembers a
	// rectangle it no longer owns.  Nothing repaints it, so the two of
	// them - the frozen text and the frozen picture - stay on screen for
	// ever.  Turning the panel on FIRST reserves the strip and draws an
	// empty slot, and the message then goes into a rectangle the emulator
	// will never touch.
	BareVideoFooterSetVisible (TRUE);
	BareVideoFooter (0, "Ctrl+F9: flushing the card, then halt - do NOT cut power yet");

	// The banner card too, so that the reason the machine is about to stop
	// is on the screen as well as in the log.
	BareVideoStatusPrint ("shutting down: the card is being flushed now");

	s_bWantsExit = TRUE;
}

// Ctrl+Alt+PetSis and the menu's "Reiniciar la placa".
void BareCoreRequestReboot (void)
{
	CLogger::Get ()->Write (FromBare, LogWarning,
				"Ctrl+Alt+PetSis: rebooting the board");
	BareVideoStatusPrint ("Ctrl+Alt+PetSis: rebooting");

	// One frame's worth of time for the message to reach the display,
	// then Circle's own reboot().  Not returnToEL2h(), not halt(): the
	// point is to come back up, not to stop.
	CTimer::SimpleMsDelay (200);
	reboot ();
}

// ---------------------------------------------------------------------------
// Disk images in A: / D: (the setup menu's Unidades page)
// ---------------------------------------------------------------------------
//
// Through the core's disk control: the core mounts a .img in A: and an .iso or
// .cue in D: (libretro/src/disk_control.cpp).  Called from the frame loop
// while the menu has the machine paused, between two retro_run()s - the place
// a libretro frontend is expected to call it from.

// The core keeps ONE list of images and ONE tray for both drives: closing the
// tray mounts the selected image in A: or D: by its extension, opening it
// unmounts the selected image from its drive.  So the frontend keeps what it
// put in each drive (index into the core's list), and only ever opens the tray
// over the image of the drive being changed - inserting a CD must not take
// the floppy out.
static std::vector<std::string> s_DiskPaths;	// parallel to the core's list
static int s_nDiskInDrive[2] = { -1, -1 };	// [0] A:, [1] D: - index or -1

static unsigned DiskDrive (const char *pPath)
{
	const size_t n = strlen (pPath);
	return n > 4 && strcasecmp (pPath + n - 4, ".img") == 0 ? 0 : 1;
}

// Unmount what this frontend put in a drive, if anything.
static void DiskOut (unsigned nDrive)
{
	const int nIndex = s_nDiskInDrive[nDrive];
	if (nIndex < 0)
	{
		return;
	}
	if (s_DiskControl.get_image_index () != static_cast<unsigned> (nIndex))
	{
		s_DiskControl.set_image_index (static_cast<unsigned> (nIndex));
	}
	s_DiskControl.set_eject_state (true);	// unmounts the selected image
	s_nDiskInDrive[nDrive] = -1;
}

boolean BareDiskInsert (const char *pPath)
{
	if (!s_bDiskControl || pPath == nullptr || s_DiskControl.add_image_index == nullptr)
	{
		return FALSE;
	}

	const unsigned nDrive = DiskDrive (pPath);
	DiskOut (nDrive);

	if (!s_DiskControl.add_image_index ())
	{
		return FALSE;
	}
	const unsigned nIndex = s_DiskControl.get_num_images () - 1;

	retro_game_info Info {};
	Info.path = pPath;
	if (!s_DiskControl.replace_image_index (nIndex, &Info)
	    || !s_DiskControl.set_image_index (nIndex))
	{
		return FALSE;
	}
	s_DiskPaths.resize (nIndex + 1);
	s_DiskPaths[nIndex] = pPath;

	// Closing the tray mounts it.  The core sets the state and mounts the
	// selected image whatever the state was, so this works with the other
	// drive's image still in place.
	const bool bOk = s_DiskControl.set_eject_state (false);
	if (bOk)
	{
		s_nDiskInDrive[nDrive] = static_cast<int> (nIndex);
	}
	CLogger::Get ()->Write (FromBare, bOk ? LogNotice : LogWarning,
				"%s: %s %s", nDrive == 0 ? "A:" : "D:", pPath,
				bOk ? "inserted" : "could NOT be inserted");
	return bOk ? TRUE : FALSE;
}

boolean BareDiskEject (char cDrive)
{
	if (!s_bDiskControl)
	{
		return FALSE;
	}
	const unsigned nDrive = (cDrive == 'A' || cDrive == 'a') ? 0 : 1;
	if (s_nDiskInDrive[nDrive] < 0)
	{
		return FALSE;
	}
	DiskOut (nDrive);
	return TRUE;
}

boolean BareDiskAvailable (void)
{
	return s_bDiskControl;
}
