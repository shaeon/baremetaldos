//
// kernel.cpp
//
// BM-DOS - the Circle application around the DOSBox core.
//
// by @mmoroca & arena.ai 2026  -  https://arena.ai
//
// Boot order matters and is worth spelling out, because three of these steps
// are not obvious:
//
//  1. Circle brings up interrupts, screen, serial, logger, EMMC/FatFs, USB and
//     the newlib console (all of that is CStdlibAppStdio::Initialize()).
//     FatFs must be mounted before the core starts: DOSBox opens files through
//     stdio and expects "/" to be the card.
//  2. We allocate our own 32bpp frame buffer and never look at Circle's text
//     screen again.  Circle's CScreenDevice stays alive underneath (its buffer
//     is a different allocation) but nothing writes to it after this point.
//  3. Audio is started at the rate DOSBox will mix at.  The rate has to be
//     fixed *before* the core runs, because the mixer is initialised from
//     dosbox.conf inside retro_load_game().
//  4. Input handlers are registered before the core runs, so the very first
//     frame already sees keystrokes.
//  5. Only then does the core boot DOS.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
#include "kernel.h"

#include <cstdarg>
#include <cstdio>
#include "platform/bare_env.h"
#include "platform/bare_storage.h"

#include <circle/logger.h>
#include <circle/timer.h>
#include <circle/util.h>
#include <circle/startup.h>


// /boot.conf: the port's own settings, parsed in its own header so that the
// host can test it (make check-bootconf).  Included before anything reads
// BOOT_CONFIG or the display defaults, which is right below.
#include "platform/boot_config.h"

// The mixer rate DOSBox is configured for.  Keep this in sync with
// [mixer] rate= in sd/dosbox.conf; a mismatch is audible as pitch shift.
static constexpr unsigned MixerRate = 44100;

// Constructor: the three members that have a value before Initialize() runs.
//
//   mScheduler     is a CScheduler member, and *constructing* it is the whole
//                  point (see kernel.h): it sets the static instance pointer
//                  that libc++'s std::thread and bare_input.cpp both need, so it
//                  has to happen before anything else is constructed.  Being a
//                  member gives exactly that, with no call to make.
//   m_nStatusLine  which line of the status overlay the next Report() goes to.
//   m_bDisplayOwned FALSE until BareVideoInit() has succeeded, so that Report()
//                  knows whether it has a screen to write to.
//
// The base class ("dosbox") is what names the kernel in the Circle log.
CKernel::CKernel (void)
:	CStdlibAppStdio ("dosbox"),
	m_nBootLogLevel (2),
	m_bReportForced (FALSE),
	m_nStatusLine (0),
	m_bDisplayOwned (FALSE)
{
}

CKernel::~CKernel (void)
{
}

void CKernel::Cleanup (void)
{
	// ---------------------------------------------------------------------
	// Flush the card before anything else
	// ---------------------------------------------------------------------
	//
	// This is the answer to "para apagar el sistema basta con desconectar la
	// corriente? es seguro?", and it runs on every path out of Run(): the core
	// asking to exit, the Ctrl+F9 key, and the STALL watchdog giving up.
	//
	// No, cutting the power is not safe, and the reason is worth one paragraph
	// because it is not obvious on a machine that has no visible disk activity:
	//
	//   * FAT keeps a second copy of its allocation table and updates it out of
	//     order with respect to the data, so a power cut in the wrong
	//     millisecond leaves a file that exists with garbage in it, or space
	//     that is allocated to nobody;
	//   * the SD card is worse than the filesystem: it acknowledges a write into
	//     its own cache and programs the flash later, so "the write returned" is
	//     not "the write is on the card".  Pulling power during that window can
	//     lose the sector and, on a cheap card, the mapping table with it;
	//   * there is no fsck.  A desktop repairs the card on the next boot; here
	//     the next boot is DOS and it will just see a damaged save file.
	//
	// What is actually exposed per session is small, because this port does not
	// hold the card open: dosbox.conf is read once, a save is written when the
	// game writes it, and the screenshot path is explicit.  So the risk is "the
	// last save is corrupt", not "the card is dead" - but a corrupt save is
	// still the thing a person would mind losing.
	//
	// The flush is attempted unconditionally, and its *result* is logged rather
	// than acted on: a failing flush must not stop the shutdown, it has to be
	// visible.  See bare_storage.h - the first attempt at this called sync(),
	// which does not exist here, and the board build found it at the link step
	// after the whole cross build had run.
	//
	// main.cpp calls this once Run() has returned.  The stages that hold
	// hardware are torn down at the *end of Run()*, in this same order, and
	// deliberately not here:
	//
	//   * they have to run before Run() returns, because a halt at any point
	//     inside Run() is a legitimate way for this port to end;
	//   * BareVideoDeinit() is not called from either place: it deletes the
	//     frame buffer, and that blanks the display.  The status lines on it
	//     are the only account of what happened, and halt() leaves them where
	//     they are.
	//
	// So there is nothing left for this function to undo, and it exists
	// because the base class declares it virtual - an override that is
	// declared and never defined is an undefined reference at link time, which
	// is exactly how .46 failed to build.
	//
	// The flush itself, last, so that everything else has stopped writing.
	//
	// Not sync(): POSIX sync() does not exist in circle-stdlib and the .59 build
	// died at the link step on exactly that.  What does exist, and what was
	// checked in the sources rather than assumed, is FatFs's f_sync() on a file
	// object - which is what issues CTRL_SYNC, which in Circle's diskio.cpp
	// becomes IOCtl(DEVICE_IOCTL_SYNC) on the SD card driver.  The sequence and
	// its failure modes live in bare_storage.h, where check-storage can run them
	// on the host; this is only the call and the log line.
	{
		const bare_storage::TReport Report = bare_storage::Flush ();
		const bare_storage::TResult Result = Report.Result;

		const boolean bFlushed = Result == bare_storage::TResult::Flushed
					 || Result == bare_storage::TResult::NothingToDo;

		// Two lines when it failed, one when it worked.  The first is the
		// verdict; the second is the card's own reason, and it exists because
		// .65f printed "FLUSH FAILED ... (see the log)" and then put nothing
		// in the log: the FRESULT had been thrown away.  A message that sends
		// the reader to a place with no answer costs a round trip through a
		// screenshot, which is exactly what happened.
		CLogger::Get ()->Write (GetKernelName (),
				bFlushed ? LogNotice : LogWarning,
				"card: %s", bare_storage::Describe (Result));

		if (!bFlushed)
		{
			CLogger::Get ()->Write (GetKernelName (), LogWarning,
					"card: %s (%s)",
					bare_storage::Explain (Report),
					bare_storage::ResultNameOf (Report));
		}

		// On the panel too, and this is the *end* of the Ctrl+F9 message: the
		// frame loop wrote "flushing the card, then halt" when the key was
		// pressed, and the answer to that is here.  Without it the key press
		// has a message and no outcome, which is barely better than the silent
		// silent shutdown.  Same wording as the log line, so that a
		// photo of the screen and a serial log can be compared line for line.
		//
		// .65e: the line is not allowed to say "power can be cut" and then have
		// something else happen.  The audio device and the USB host are handed
		// back after this function returns, and a card that is only unmounted
		// afterwards is a card that can still be written to while somebody is
		// reaching for the plug.  So the *good* news comes last, after the two
		// teardowns below - and if the flush failed, the bad news replaces it
		// rather than sharing the screen with it.  A single line that can only
		// hold one sentence forces the choice, which is the point.
		BareVideoFooterSetVisible (TRUE);
		//
		// .65g: the failure line names the reason instead of pointing at a log
		// that has to be read on another machine.  It is still one sentence,
		// for the reason above, but the sentence is now the whole answer.
		BareVideoFooter (0, bFlushed
			? "DONE - the card is flushed, it is safe to cut the power"
			: bare_storage::FooterFailure (Report));
	}

	CStdlibAppStdio::Cleanup ();
}

// Defined by the Makefile (APP_DEFINES).  The fallback keeps a stale object
// file from breaking a build that was configured without it.
#ifndef DOSBOX_SNAPSHOT
#define DOSBOX_SNAPSHOT "unknown"
#endif

void CKernel::Report (const char *pFormat, ...)
{
	char Buffer[160];

	va_list Args;
	va_start (Args, pFormat);
	vsnprintf (Buffer, sizeof Buffer, pFormat, Args);
	va_end (Args);

	// Always the console: that is what ends up on the serial port for
	// anyone watching with a cable, and it is the log that survives a hang.
	CLogger::Get ()->Write (GetKernelName (), LogNotice, "%s", Buffer);

	// m_nBootLogLevel == 0 silences this port's progress, but never a failure:
	// a boot that stops because the display mode does not match has to say so on
	// the screen even when boot_log=off, or the card looks dead.
	if (!m_bDisplayOwned || (m_nBootLogLevel == 0 && !m_bReportForced))
	{
		return;
	}

	BareVideoStatusPrint (Buffer);
}

const char *CKernel::PickContent (void) const
{
	// If there is a dosbox.conf, hand *that* to the core: dosbox-core then
	// boots into its own shell with our [autoexec] section doing the mounting
	// (that is what the shipped sd/dosbox.conf does).  If there is not, fall
	// back to mounting the games directory directly, which also works and is
	// the fastest way to try a new game.
	// This probe is only about "is there a config to hand to the core", so the
	// Circle volume name is fine here: newlib's fopen resolves it.
	FILE *pFile = fopen (DOSBOX_CONFIG, "r");
	if (pFile != nullptr)
	{
		fclose (pFile);

		// Handed to the core, so it has to be the spelling the core can
		// canonicalise.  See the comment in kernel.h.
		return DOSBOX_CONFIG_ABS;
	}

	// No config: let the core mount the games directory as C:.
	return DOSBOX_GAME_DIR_ABS;
}

boolean CKernel::Initialize (void)
{
	if (!CStdlibAppStdio::Initialize ())
	{
		return FALSE;
	}

	// --- the clock, before anything measures time -------------------------
	// Everything below is timed: the governor's transition delays, the frame
	// pacer, the watchdog, the audio staleness counter.  Until .57 the clock
	// they all used was the raw physical counter treated as microseconds,
	// which is 54 times too fine on a Pi 4 and is why the emulator ran flat
	// out (see bare_time.cpp).  It is measured here, once, against Circle's
	// own 1 MHz clock, so that no other file has to know about CNTFRQ.
	BareTimeCalibrate ();

	// NO banner here.  It used to print the version block twice: once through
	// Banner() on Circle's console (printf + logger, both of which end up on that
	// screen) and then again through Report() as soon as the frame buffer
	// existed.  Two different screens, the same three lines, and the photo of the
	// boot shows exactly that - "BM-DOS 2026-11-09.73 - DOSBox on baremetal
	// Raspberry Pi" printed twice, once by each.  The version block is printed
	// once, below, after BareVideoInit, where it lands on the screen that stays.

	// --- the port's own settings ------------------------------------------
	// Read before the frame buffer exists, because the display mode is one of
	// them, and before the logger loses the screen, so that a bad file is
	// reported where it can still be read.
	TBootConfig Config;
	ReadBootConfig (&Config);
	if (Config.Warning[0] != '\0')
	{
		Report ("%s", Config.Warning);
	}

	// --- how much of this boot goes on the screen --------------------------
	//
	// Read from boot.conf (boot_log = auto|quiet|off) and applied ONCE, here,
	// because it decides how much of what follows is drawn.  2 = all, 1 = the
	// port's own lines and nothing from the core, 0 = nothing at all.
	//
	// The core's log is routed to BareVideoStatusPrint() by bare_time.cpp, which
	// is why this needs a level and not a switch: the port's own progress lines
	// are the ones you need when something fails, and the core's config dump
	// ("Variable dosbox::machine updated to ...") is the one that fills the
	// screen with forty lines nobody reads twice.
	m_nBootLogLevel = 2;
	if (strcmp (Config.BootLog, "quiet") == 0)
	{
		m_nBootLogLevel = 1;
	}
	else if (strcmp (Config.BootLog, "off") == 0)
	{
		m_nBootLogLevel = 0;
	}

	// And told to the core's log path, which is the side that paints.
	BareLogSetDisplayLevel (m_nBootLogLevel);

	// --- cmdline.txt has the last word on the display mode ------------------
	//
	// Two frame buffers exist before this function reaches BareVideoInit:
	// Circle's console (CScreenDevice, created by CStdlibAppStdio::Initialize()
	// a few lines above, sized from cmdline.txt's width= / height= or from the
	// maximum the firmware reports when those are absent) and the one this port
	// is about to create.  The console's buffer is built first and is not ours
	// to resize: asking the firmware for a *different* geometry now is a mode
	// change at run time, Circle's own doc/cmdline.txt warns that the firmware
	// may refuse it, and when it half-succeeds the display ends up scanning
	// memory the console wrote text into, in the console's own palette.  That
	// is the garbage of docs/PORTING.md row 86.
	//
	// So the console wins, always, and the port follows it.  boot.conf's
	// "display =" is what scripts/make-sd.sh and scripts/update-sd.sh write into
	// cmdline.txt when the card is built; once the card exists, cmdline.txt is
	// the file the firmware and Circle actually read, and a boot.conf edited
	// afterwards cannot move the mode on its own.  Saying so on the status
	// overlay - and carrying on - is the honest behaviour: the board boots, at
	// the mode the card was built for, and the line tells you which file to
	// edit.  Refusing to boot over it, as this check used to, left the machine
	// dead because of a cosmetic disagreement between two text files.
	//
	// mScreen is the console CStdlibAppScreen already created, and it is
	// protected, which is exactly what a derived class is for.  GetFrameBuffer()
	// hands back the CBcmFrameBuffer that CScreenDevice::Initialize() built from
	// cmdline.txt's width= / height=.
	{
		CBcmFrameBuffer *pConsoleFB = mScreen.GetFrameBuffer ();

		if (pConsoleFB != nullptr && pConsoleFB->GetWidth () != 0)
		{
			// What GetWidth()/GetHeight() return is the *physical* size the
			// console asked the firmware for - CBcmFrameBuffer::GetWidth()
			// returns m_nWidth, the number in SetPhysWidthHeight, not a
			// read-back (lib/bcmframebuffer.cpp).  When cmdline.txt had no
			// width=/height= at all, the console asked for 0x0 and the
			// constructor filled m_nWidth from PROPTAG_GET_DISPLAY_DIMENSIONS,
			// which is the display's size with the overscan border already
			// subtracted: that is where 1824x984 came from, the firmware's
			// answer rather than a mode anybody chose.
			//
			// So this compares two *requests*, and that is all it can honestly
			// do.  The virtual height is not compared: the console asks for 0
			// (single page) and this port asks for twice the physical height
			// (two pages), and those two requests have never been seen to
			// disturb each other - it is the physical geometry that has to
			// match.
			const unsigned nConsoleWidth  = pConsoleFB->GetWidth ();
			const unsigned nConsoleHeight = pConsoleFB->GetHeight ();

			if (   nConsoleWidth  != Config.nWidth
			    || nConsoleHeight != Config.nHeight)
			{
				// Forced past boot_log=off: a line that explains why the
				// picture is not the size you asked for has to be readable.
				m_bReportForced = TRUE;

				// "boot.conf says" only if the file actually said it: the
				// value is the built-in default otherwise, and blaming a
				// file for a number that is not in it is how a support
				// thread starts.
				Report ("display: %s has %ux%u, cmdline.txt gives %ux%u",
					Config.bHaveDisplay ? "boot.conf" : "the default",
					Config.nWidth, Config.nHeight,
					nConsoleWidth, nConsoleHeight);
				Report ("display: using cmdline.txt; to change it, edit width=%u height=%u there",
					nConsoleWidth, nConsoleHeight);

				// The other way the number can come out short, and it is the
				// one that produced 1824x984 on a 1080p TV: the firmware
				// subtracts an overscan border from the display mode before it
				// answers, and in CEA (TV) modes that border is 48 px per edge
				// by default.  So a mode that "does not exist" may simply be
				// the requested one minus 96 in each direction.  Said as a
				// possibility, not as a diagnosis, because only config.txt
				// knows.
				if (   nConsoleWidth  < Config.nWidth
				    || nConsoleHeight < Config.nHeight)
				{
					Report ("if it is smaller than you asked, overscan: disable_overscan=1");
				}

				CLogger::Get ()->Write (GetKernelName (), LogWarning,
							"Display from cmdline.txt %ux%u (config asked %ux%u)",
							nConsoleWidth, nConsoleHeight,
							Config.nWidth, Config.nHeight);

				// Only this explanation is forced; the rest of the boot
				// obeys boot_log again.
				m_bReportForced = FALSE;
			}

			// The port follows the console, whatever the file said.
			Config.nWidth  = nConsoleWidth;
			Config.nHeight = nConsoleHeight;
			Config.bHaveDisplay = TRUE;

			Report ("display: %ux%u from cmdline.txt",
				Config.nWidth, Config.nHeight);
		}
		// No console object found: nothing can be compared, and the mode from
		// boot.conf (or the default) is the best guess left.
	}

	// --- video ------------------------------------------------------------
	// The screen logger works up to this point.  BareVideoInit switches the
	// display over to our own frame buffer, which is why the status overlay
	// starts immediately afterwards: without it the next failure would be a
	// silent black screen.
	if (!BareVideoInit (Config.nWidth, Config.nHeight))
	{
		CLogger::Get ()->Write (GetKernelName (), LogError,
					"Cannot initialise the display");
		return FALSE;
	}

	m_bDisplayOwned = TRUE;
	m_nStatusLine = 0;
	BareVideoStatusReset ();
	Report ("BM-DOS %s - DOSBox on baremetal Raspberry Pi", DOSBOX_SNAPSHOT);
	Report ("by @mmoroca & arena.ai 2026");
	Report ("Display %ux%u", Config.nWidth, Config.nHeight);

	// --- scheduler ---------------------------------------------------------
	// The member is constructed at that point (before Initialize() runs at
	// all), and that is the entire job: CScheduler's constructor sets the
	// static instance pointer, there is no Initialize() to call.  Reported here
	// because "is there a scheduler" is the difference between std::thread
	// working and the emulator dying with no message - see kernel.h.
	Report ("scheduler: %s", CScheduler::IsActive () != 0 ? "present" : "MISSING");

	// Clock first, then everything else: without this the SoC never leaves its
	// low rate (see bare_sysinfo.cpp), and every measurement afterwards - the
	// frame rate, the temperature - would be taken at the wrong speed.
	// A ceiling from boot.conf goes in before the governor picks its levels, so
	// that "cpu_max_mhz=1500" means what it says.
	BareSysInfoSetCPUMax (Config.nCPUMaxMHz);
	BareSysInfoSetPerformance ();

	// --- audio ------------------------------------------------------------
	Report ("audio: initialising (%u Hz)", MixerRate);
	if (!BareAudioInit (MixerRate, &mInterrupt))
	{
		Report ("FAIL: cannot initialise audio");
		return FALSE;
	}

	Report ("audio: ok");

	// --- secondary cores --------------------------------------------------
	// Started once, parked in WFE until something gives them a job
	// (bare_cores.cpp).  Not having them is not a failed boot: MIDI and the
	// core 2 presenter are then off and everything runs on core 0 as before.
	const boolean bCores = BareCoresInit ();
	Report ("cores: %s", bCores ? "secondary cores parked" : "core 0 only");

	// --- video presenter on core 2 (boot.conf video_core / vsync) ----------
	// Only armed here: it starts by itself once the emulator's picture owns
	// the screen, and falls back to core 0 if core 2 cannot take it.
	const boolean bCore2Video = Config.nVideoCore == 2 && bCores;
	BareVideoPresenterConfigure (bCore2Video, Config.bVsync ? TRUE : FALSE);
	Report ("video: presented by core %u%s", bCore2Video ? 2u : 0u,
		bCore2Video ? (Config.bVsync ? ", vsync on" : ", vsync off") : "");

	// --- MIDI synthesizers on core 1 ---------------------------------------
	// Before the core: MIDI_Init() runs inside DOSBox's start-up and needs core
	// 1 already waiting.  A build without the MIDI libraries, or a core that
	// does not start, only means no MT-32 / General MIDI - not a failed boot.
	Report ("midi: core 1 %s", BareMidiInit (MixerRate) ? "ready" : "not started (no MIDI)");

	// --- input ------------------------------------------------------------
	Report ("input: initialising");
	if (!BareInputInit ())
	{
		Report ("FAIL: cannot initialise input");
		return FALSE;
	}

	Report ("input: ok");

	// --- report what we found --------------------------------------------
	const char *pContent = PickContent ();
	Report ("content: %s", pContent);

	// --- the core ---------------------------------------------------------
	Report ("core: init");
	if (!BareCoreInit ())
	{
		Report ("FAIL: DOSBox core did not initialise");
		return FALSE;
	}

	Report ("core: init ok");

	// Every frame boundary inside DOSBox comes back to us here.
	BareCoreSetFrameHook (FrameHook, this);

	// What the core is about to do, asked of the filesystem directly.  fopen()
	// is newlib -> FatFs; std::filesystem::canonical() is libc++ on top of it.
	// retro_load_game() uses the second one first, and fails silently when it
	// throws, so both answers are worth having on screen.
	Report ("%s", BareProbeFile (pContent));
	// The first two are for opendir(), which needs the volume name the same
	// way fopen() does; the last two are the exact strings the core will be
	// given, so they have to work through std::filesystem as well.
	Report ("%s", BareProbeDir ("SD:/dos"));
	Report ("%s", BareProbeDir ("SD:/dosbox"));
	Report ("%s", BareProbeDir ("/dosbox"));
	Report ("%s", BareProbeDir ("/dos"));

	Report ("core: load %s", pContent);
	if (!BareCoreLoadGame (pContent))
	{
		Report ("FAIL: cannot load %s", pContent);
		return FALSE;
	}

	Report ("core: load ok");

	// ---------------------------------------------------------------------
	// .65e: the rate at the DOS prompt, and the 1800 MHz nobody asked for
	// ---------------------------------------------------------------------
	//
	// The observed behaviour: the board starts at 600 MHz, jumps to 1500 almost
	// at once and then to 1800 - on the DOS text screen, which needs nothing.
	//
	// It was not the text screen being expensive; it was the pacer running at
	// its sanity ceiling.  dosbox-core announces its rate through
	// SET_SYSTEM_AV_INFO, and only *when the rate changes* - and at boot it
	// never changes, because there is no mode change between "no mode" and
	// "text".  So BareVideoRefreshMilliHz() stayed 0, kernel.cpp fell back to
	// 1000000/nMaxHz (240 Hz, the ceiling that exists to stop a bogus fps from
	// dividing by zero), and:
	//
	//   * the governor asked for a 4.2 ms frame budget on a screen that needs
	//     14.3, so every frame was an overrun and it escalated to 1800;
	//   * the panel could not print a denominator either, which is why the
	//     panel over the prompt says "pace 30 Hz" and nothing else.
	//
	// libretro has the function for exactly this: retro_get_system_av_info()
	// can be called at any time and fills in what the core knows right now.
	// Asking once, after the load (the mode exists by then) and before the
	// frame loop starts, turns "not announced" into the real number without
	// waiting for a mode change that a machine sitting at a prompt will never
	// produce.
	{
		const unsigned nMilliHz = BareCorePollRefreshMilliHz ();

		if (nMilliHz != 0)
		{
			Report ("core: mode refreshes at %u.%03u Hz",
				nMilliHz / 1000, nMilliHz % 1000);
		}
		else
		{
			// Not fatal - the frame loop keeps its ceiling and the panel keeps
			// saying "pace N Hz" - but it has to be findable, because it is the
			// difference between a governor that works and one that pins the
			// board at 1800 MHz on a DOS prompt.
			Report ("WARN: the core did not report a rate; pacing at the ceiling");
		}
	}

	Report ("first frame...");

	// Only reached when the load worked; on the failing path the probe lines
	// above are the ones that matter.

	// ---------------------------------------------------------------------
	// The shortcut card, on screen, once
	// ---------------------------------------------------------------------
	//
	// Until .59 the only thing this port ever said about its own keys was
	// "Ctrl+F1 is not available yet", which is true and useless: it names the
	// one thing that does not work while hiding everything that does.  The
	// question worth answering is which of DOSBox's advertised keys exist here - because a DOSBox core on a desktop
	// proposes Ctrl+F1 for its mapper, Ctrl+F9 for shutdown and Ctrl+F12 for
	// cycles, and two of those three do not exist here:
	//
	//   Ctrl+F1  opens DOSBox's mapper, a GUI.  A libretro build has no mapper
	//            GUI at all (it is compiled out with the rest of the SDL frontend
	//            and the mapper lives in the core's own menu), so pressing it
	//            does nothing.  Settings come from dosbox/dosbox.conf.
	//   Ctrl+F9  closes the window.  There is no window; the machine is the
	//            board.  It is listed because DOSBox lists it, and pressing it
	//            does nothing.
	//   Ctrl+F5  screenshot, and it IS ours: F5 goes through to DOS untouched
	//            and only the Ctrl combination is taken (bare_libretro.cpp).
	//   Ctrl+F11 / Ctrl+F12  cycles, handled by the core, and the steps are set
	//            to 2000 in dosbox.conf because DOSBox's defaults (10 and 20)
	//            would need four hundred presses to cross the table.
	//   Insert   DOSBox's "pinhack" view.  Core option, off by default.
	//
	// And the four keys that are this port's own, not DOSBox's:
	//
	//   PetSis            show / hide the status panel
	//   Ctrl+Alt+PetSis   reboot the BOARD (not the emulated machine)
	//   Ctrl+F9           shut down safely (see the note below)
	//
	// Ctrl+F9 was Alt+F12 in a draft, and the draft was wrong: DOSBox's own
	// help calls Alt+F12 turbo, so the key would have meant two things.  Three
	// comment blocks went on saying Alt+F12 for a round after the key had
	// changed, which is worse than a bug - the code was right and the only
	// written record of it was wrong.
	//
	// Ctrl+F9 exists because cutting the power to stop the system is not safe,
	// and the reason is worth stating once, here, where it will be read.
	//
	// FAT (the filesystem on the card) keeps a region it updates out of order
	// with the data, and the SD card itself lies about when a write has landed -
	// it acknowledges into a cache and flushes later.  Pulling power can
	// therefore leave dosbox.conf, the mounts, or a game's save file half
	// written.  Nothing on this port holds the card open between writes, so the
	// exposure is one write, not a whole session... but one write is enough to
	// lose a save, and there is no fsck on a bare metal Pi to fix it afterwards.
	// Ctrl+F9 stops the emulator, hands the audio device back, flushes the card and
	// unmounts the volumes, and then halts the SoC with a message on screen.  At
	// that point cutting the power is safe.
	// Report() and not Banner(), and that is the whole point: this runs after
	// BareVideoInit, so the status overlay owns the screen and Report() writes
	// there (plus the log).  Banner() writes to Circle's console - a different
	// frame buffer that nothing is scanning any more - so these nine lines were
	// being drawn into memory nobody could see, at coordinates that do not match
	// the overlay's, which is the tangle at the bottom of the boot screen.
	Report ("DOS booting.");
	Report ("  PetSis           show/hide the status panel");
	Report ("  Ctrl+Alt+PetSis  reboot the board");
	Report ("  Ctrl+F9          shut down safely, then you may cut power");
	Report ("  Alt+Pause        pause / resume the emulated machine");
	Report ("  Ctrl+F5          screenshot");
	Report ("  Ctrl+F11 / F12   emulated cycles (2000 per press)");
	Report ("  Alt+F12 is NOT implemented: dosbox's own help calls it turbo.");
	Report ("  Settings live in dosbox/dosbox.conf on the card.");

	return TRUE;
}

void CKernel::FrameHook (void *pParam)
{
	auto *pThis = static_cast<CKernel *> (pParam);

	(void) pThis;

	// The core hands control back here once per emulated frame.  Everything
	// that has to happen between frames belongs here rather than in a
	// separate task, so that this port has no scheduling assumptions to get
	// wrong later:
	//
	//   * USB is pumped by bare_input.cpp through the core's input poll
	//     callback, which runs earlier in the same frame.
	//   * Circle's cooperative scheduler is yielded there too.
	//   * Presentation happened synchronously in the video callback.
	//
	// So there is genuinely nothing to do at this instant.  It stays as the
	// hook (a) because a later milestone will put the frame pacing here and
	// (b) so the call site exists and is exercised from day one.
}

namespace {

// What the panel's "pace" field shows, measured once a second by the frame
// loop and read by BuildPanel().  Four numbers, because "va rápido" and "va
// lento" are different faults and one number cannot tell them apart:
//
//   s_PacePresented  frames per second that actually reached the screen
//   s_PaceAsked      frames per second the *loop* asked the core for
//   s_PaceDrives     frame boundaries the emulator produced on its own
//   s_PaceWant       the rate the core announced for the mode (uncapped)
//   s_PaceBudgetHz   the rate the pacer is actually using (capped at 60)
//
// The interesting reading is PacePresented against PaceWant.  If the mode is
// 70 Hz and the display is getting 50, the machine is running 40 % slow even
// though every frame it does produce is on time - and that is invisible in
// every other number the port has.
static unsigned s_PacePresented = 0;
static unsigned s_PaceAsked = 0;
static unsigned s_PaceDrives = 0;
// Boundaries per second: how often the EMULATED MACHINE advanced, as opposed to
// how many pictures reached the screen.  The two are the same number on a
// healthy machine, and the difference between them is the whole question
// "va acelerado" asks.  See the note on s_nBoundaries in bare_thread.cpp.
static unsigned s_PaceBounds = 0;
static unsigned s_PaceWant = 0;
static unsigned s_PaceBudgetHz = 0;
// FALSE until the core has announced a rate.  The panel then prints "pace 31 Hz"
// instead of "pace 31/240": 240 was the internal sanity ceiling and nothing the
// core ever asked for, and showing it as a denominator made a healthy boot look
// like a machine failing to reach 240 frames a second.
static unsigned s_PaceRateKnown = 0;


// Percent of the wall-clock second actually spent waiting for the next frame.
//
// This is the number that answers "va acelerado" directly, and the port did not
// have it.  In a machine that is being paced properly, the frame loop *waits*
// for most of every second and retro_run() costs the rest: 40-90 %.  A loop that
// is not pacing - because its slot has already passed when it looks - waits 0 %
// and calls the emulator as fast as it can, and `pace` does not show it: the
// presented count is produced inside the core, so it can read a plausible 38/70
// while the machine runs at twice its own speed.  This field exists for exactly
// that case: a machine running fast under a healthy-looking pace line.
static unsigned s_PaceWaited = 0;

// Frames the core presented as "the same picture again" in the same second.
// This is the honest answer to "does it have frame skipping": the core's own
// [render] frameskip is 0 and stays 0, but dosbox-core still hands us a null
// buffer when it has nothing new, and the panel never said so.  A value near
// the presented count means the emulator is only producing half the pictures
// its own mode refreshes at - which looks exactly like judder, not like speed.
static unsigned s_PaceDuped = 0;

// Samples of audio discarded in the same second.  A *rate*, not a total, and
// that is the .59 answer to "es normal que salgan drops constantes al iniciar
// sin hacer nada": in a queue that runs near full, the tail of almost every
// batch does not fit, so the total grows for ever and says nothing at all.  The
// rate says everything: a few dozen samples a second out of 44100 is 0.05 % and
// inaudible, while thousands a second is a real problem.
static unsigned s_PaceDropped = 0;

// How long one retro_run() took, averaged over the same second, in tenths of a
// millisecond.  This is the number that explains a low "pace R/W": the panel
// says the emulator is producing R frames a second when its mode asks for W, and
// this says *why* - if a frame costs 33 ms of host time, 30 frames a second is
// exactly what the interpreter can do and no amount of clock or cycles will
// change it.  Written down because two rounds went into the clock before anyone
// measured the thing the clock is supposed to make short enough.
static unsigned s_PaceCallMicros = 0;
static unsigned s_PaceMicrosPublished = 0;	// 0 until the first second is measured

/// \brief The one status line, bottom-left, drawn on top of the emulator.
///
/// Everything that can be measured cheaply and read at a glance, in one line,
/// because the panel must not compete with the game for attention or screen
/// space.  In order: how fast the machine is really running, on how many cores
/// and at what temperature, whether the firmware is slowing it down, and
/// whether the sound device is consuming what it is given.
///
/// What is deliberately NOT here any more: the key counters.  They did their
/// job - they proved the USB path worked - and once typing works they are just
/// four numbers nobody reads.
///
/// What is also not here: a figure per ARM core.  It was tried
/// (PROPTAG_GET_CLOCK_RATE_MEASURED with CLOCK_ID_ARM, which is documented to
/// return one word per core) and the firmware answered nothing, which is why
/// the panel read "core?" on the board.  There is also nothing to show: a Pi 4
/// has one clock tree for all four cores.  So the panel says how many cores
/// there are and at what rate the domain runs.
///
/// The "free" audio number is the honest test of the sound path: it is the
/// room left in the device queue, asked of the device itself.  Stuck at 0 with
/// drops climbing means the device is not taking data; oscillating near the
/// queue size means it is.
///
/// \return A pointer to a static buffer.  Not reentrant, and not meant to be:
///\t   it is called once per second from the frame loop.
const char *BuildPanel (void)
{
	static char Panel[192];

	unsigned nArmMHz = 0;
	unsigned nVpuMHz = 0;
	unsigned nCelsius = 0;
	BareSysInfoGet (&nArmMHz, &nVpuMHz, &nCelsius);
	(void) nVpuMHz;	// read for completeness; the panel shows the ARM domain

	// The ARM domain, not four cores.  There is one clock tree on a Pi 4 and
	// the firmware reports one rate for it, so four numbers would be the same
	// number written four times - and the one attempt to read them separately
	// (PROPTAG_GET_CLOCK_RATE_MEASURED) came back empty on real hardware, which
	// is why the panel once said "core?".
	//
	// .60 puts back the board model and the core count.  .59 had dropped them to
	// buy characters for "fr" and "dup", and that was the wrong trade: without
	// the model, "1800 MHz" on a Pi 400 looks like a governor stuck at the level
	// of a different board, and the frequency cannot be read at all.  What went
	// instead is "MHz" after nothing and the "arm" prefix (see the width note
	// below), and "L1/3" stays because it is the governor's own account.

	// The firmware's throttling bits (PROPTAG_GET_THROTTLED returns a mask, not
	// a level).  The four "now" bits come first, because they are the ones that
	// explain a slow frame; under-voltage is last but is the most common cause
	// on a Pi 4 and the one that silently cancels the clock the governor asked
	// for.
	const unsigned nFirmware = BareSysInfoFirmwareMode ();
	const char *pThrottle = "";
	if (nFirmware & 0x08)
	{
		pThrottle = " SOFT-TEMP-LIMIT";
	}
	else if ((nFirmware & 0x04) != 0 || (nFirmware & 0x02) != 0)
	{
		pThrottle = " THROTTLED";
	}
	else if ((nFirmware & 0x01) != 0)
	{
		pThrottle = " UNDER-VOLTAGE";
	}

	unsigned nFree = 0;
	unsigned nSize = 0;
	u64 nQueued = 0;
	u64 nDropped = 0;
	u64 nSinceWrite = 0;
	const unsigned bHasAudio = BareAudioQueueFrames (&nFree, &nSize);
	BareAudioStats (&nQueued, &nDropped, &nSinceWrite);

	// The rate the emulator is *really* running at, against the rate it was
	// asked for.  This is the number that answers "is it going too fast", and
	// it is here because guessing at that question from a description is what
	// cost a round.  Frames are counted by the video layer and
	// divided by wall-clock microseconds, so a value of 100 % means the
	// emulated machine is running at exactly its own nominal speed; 117 % is
	// the 70 Hz-into-60 Hz mistake; 300 % means nothing is pacing at all.
	char Pace[40] = "";
	{
		const unsigned nWant = s_PaceWant != 0 ? s_PaceWant : s_PaceBudgetHz;

		if (s_PaceRateKnown && nWant != 0 && s_PaceMicrosPublished != 0)
		{
			// Deliberately "presented/wanted", not "presented/budget": a
			// 70 Hz mode that is only managing 50 frames shows 50/70, which
			// is the truth, and not 50/60, which would read as "fine".
			// "pace 70/70 dup 0" is a machine running at its own rate.
			// "pace 51/70 dup 34" is one that is only *drawing* 51
			// pictures a second and telling us to repeat 34 more -
			// which is judder, and reads on screen as a frame skip.
			// Two different faults, two
			// different fields; before .59 the panel could not tell
			// them apart, which is why the round before this one
			// chased the clock and the cycles.
			//
			// The percentage is the honest side of "pace": frames per second
			// can look plausible while the loop is not waiting at all, because
			// the pictures are produced inside the core.  0 % waiting is the
			// signature of a machine running flat out (the .65f bug), and
			// anything under ~25 % on a mode the host can handle is worth a
			// second look.
			// `bd` is the number this line has been missing for six rounds:
			// how often the emulated machine advanced, next to how many
			// pictures it produced.  On a machine that is metered by this
			// loop the two agree to within a frame or two; when bd runs well
			// above `pace`, the emulator is advancing faster than it draws
			// and no wait in this loop can fix it.  The `+` still appears
			// when the emulator reached a boundary the loop did not ask for,
			// but it now means that and only that (see bare_thread.cpp).
			snprintf (Pace, sizeof Pace, "  pace %u/%u w%u%% bd%u dup %u%s",
				  s_PacePresented, nWant,
				  s_PaceWaited,
				  s_PaceBounds,
				  s_PaceDuped,
				  s_PaceDrives != 0 ? " +" : "");
		}
		else if (s_PacePresented != 0)
		{
			// No denominator yet: the core has not said what the mode
			// refreshes at, so there is nothing to compare against and any
			// number put there would be the code talking about itself.
			snprintf (Pace, sizeof Pace, "  pace %u Hz", s_PacePresented);
		}
	}

	// A paused machine says so in the governor field, which is the one that has
	// room and the one nobody is reading while the machine is stopped.  Not a
	// new field: the line has to keep fitting (see the width note below).
	const char *pGovernor = BareCorePaused () ? "PAUSED"
						 : BareSysInfoGovernorState ();

	// Where "4 cores" was: with the core 2 presenter running, what it did in
	// the last second instead - frames flipped, frames replaced before it
	// could take them, the worst scaling time, and "vs" when vsync is in
	// force ("1p" when the firmware gave one page only).  The number of
	// cores never changes, and the panel is one line.
	char Cores[40];
	{
		static u64 s_nLastShown = 0;
		static u64 s_nLastOverwritten = 0;

		TBareVideoPresenterStats Video;
		BareVideoPresenterStats (&Video);
		if (Video.bRunning)
		{
			const unsigned nShown = static_cast<unsigned> (Video.nShown - s_nLastShown);
			const unsigned nOver = static_cast<unsigned> (Video.nOverwritten - s_nLastOverwritten);
			s_nLastShown = Video.nShown;
			s_nLastOverwritten = Video.nOverwritten;

			snprintf (Cores, sizeof Cores, "v2 %u/s ov%u %u.%ums%s",
				  nShown, nOver,
				  Video.nPeakMicros / 1000, (Video.nPeakMicros % 1000) / 100,
				  !Video.bTwoPages ? " 1p" : Video.bVsync ? " vs" : "");
		}
		else
		{
			s_nLastShown = 0;
			s_nLastOverwritten = 0;
			snprintf (Cores, sizeof Cores, "%u cores", BareSysInfoCores ());
		}
	}

	// MIDI, once there is something to say: the synthesizer ("gm" for
	// FluidSynth, "mt32") or "midi off", and the bytes per second the
	// MPU-401 received.  "midi off 900/s" is a game sending MIDI with no
	// synthesizer open; "gm 0/s" a synthesizer waiting for a game that
	// sends nothing.
	// Audio gaps in the last second: the device played silence because the
	// emulator was behind.  Each is a click; with the DC blocker in place
	// they no longer turn a held level into a buzz.
	static u64 s_nLastGaps = 0;
	const u64 nGaps = BareAudioGaps ();
	const unsigned nGapRate = static_cast<unsigned> (nGaps - s_nLastGaps);
	s_nLastGaps = nGaps;

	char Midi[40] = "";
	{
		static u64 s_nLastBytes = 0;
		const u64 nBytes = BareMidiBytesIn ();
		const unsigned nRate = static_cast<unsigned> (nBytes - s_nLastBytes);
		s_nLastBytes = nBytes;

		// With a synthesizer: also the frames core 1 rendered in the last
		// second (thousands; ~44 when it keeps up) and the loudest MIDI
		// sample mixed into the output (0..32767).  "r0" is core 1 not
		// rendering; "r44 p0" a synthesizer producing silence.
		static u64 s_nLastRendered = 0;
		u64 nRendered = 0;
		unsigned nPeak = 0;
		BareMidiLevels (&nRendered, &nPeak);
		const unsigned nRenderK = static_cast<unsigned> ((nRendered - s_nLastRendered) / 1000);
		s_nLastRendered = nRendered;

		// And "u" when the mixer found core 1's ring short in the last second
		// (each one is a gap in the music, heard as a click).
		static u64 s_nLastUnder = 0;
		u64 nUnder = 0;
		BareMidiStats (&nUnder, nullptr);
		const unsigned nUnderRate = static_cast<unsigned> (nUnder - s_nLastUnder);
		s_nLastUnder = nUnder;

		if (BareMidiActive ())
		{
			const char *pName = BareMidiName ();
			char Under[16] = "";
			if (nUnderRate != 0)
			{
				snprintf (Under, sizeof Under, " u%u", nUnderRate);
			}
			snprintf (Midi, sizeof Midi, "  %s %u/s r%u p%u%s",
				  strcmp (pName, "fluidsynth") == 0 ? "gm" : pName,
				  nRate, nRenderK, nPeak, Under);
		}
		else if (nBytes != 0)
		{
			snprintf (Midi, sizeof Midi, "  midi off %u/s", nRate);
		}
	}

	if (bHasAudio)
	{
		// ASCII only, and that is not cosmetic: Circle's built-in font has no
		// glyph for most of Latin-1, which is why an earlier line with a
		// middle dot arrived on the board as "Pi 4 ? arm".  Anything outside
		// 0x20-0x7E here is a '?' waiting to happen.
		// Audio counts are in hardware frames (a "frame" of the device
		// is 384 samples for HDMI) and "drop" is in samples.  Naming the
		// units exists because a bare count of drops looks alarming with no
		// unit next to it: it is a rate, and saying so is the whole fix.
		// Budget check, because this line has grown every round and nothing
		// was measuring it.  Worst case, every field at its maximum, is now
		//
		//   "Pi 400  4 cores  1800 MHz  85C THROTTLED  L2/2 busy over65535"
		//   "  pace 240/240 w100% bd240 dup 240 +  fr 1234us  HDMI 5292/5292  drop 44100"
		//
		// = 133 characters = 1064 px at 8 px per character (Circle's font), so it
		// DOES NOT fit a 1024-wide mode any more: the right-hand fields - which
		// are last - are clipped below 1088.  That is stated rather than
		// discovered, and it is the price of `bd` (.65g): the field answers the
		// question the line existed for, and the alternative is two lines, which
		// means giving up a second row of the picture.  The typical line, with
		// the panel at rest, is ~118 columns and fits 1024 - it is the worst
		// case that is clipped, and the last field is `drop`.
		//
		// .65f: two changes, both prompted by what the panel could not show.
		// `fr` now prints
		// MICROSECONDS per retro_run() call instead of milliseconds: one
		// millisecond of resolution could not tell 3 ms from 3.9, and 250 us
		// from 1.4 ms looked identical - while "is the core producing one frame
		// per call or six" is exactly the question those digits answer.  And
		// `pace` gained `wNN%`, the fraction of the second this loop spent
		// waiting for its own next frame; see s_PaceWaited.
		//
		// Width: the worst case used to be 118 columns; `fr 1234us` keeps the
		// same length as `fr 33ms`, and ` w100%` adds five.  Both fit the 944 px
		// the font gives in a 1024-wide mode, and the measurement is in
		// README.md, "The status panel".
		//
		// Every word here has been paid for.  "4 cores" and the "arm" went in
		// .59 (the core count is in the banner, the clock is the only number the
		// line has); "temp" went when "fr" arrived; the "/s" on drop went because
		// the field is a rate by construction and the column is not wide enough
		// for a unit that is already implied.  The old line was 133 characters
		// and did not fit 1024 - the kind of thing that only shows up on the
		// board, hence the arithmetic written down.
		snprintf (Panel, sizeof Panel,
			  "%s  %s  %u MHz  %uC%s  %s%s  fr %uus  %s %u/%u  drop %u gap %u%s",
			  BareSysInfoBoardName (), Cores,
			  nArmMHz, nCelsius, pThrottle, pGovernor, Pace,
			  s_PaceCallMicros,
			  BareAudioDeviceName (), nFree, nSize, s_PaceDropped, nGapRate, Midi);
	}
	else
	{
		snprintf (Panel, sizeof Panel,
			  "%s  %s  %u MHz  %uC%s  no audio  %s",
			  BareSysInfoBoardName (), Cores,
			  nArmMHz, nCelsius, pThrottle, Pace);
	}

	return Panel;
}

} // namespace

CStdlibApp::TShutdownMode CKernel::Run (void)
{
	// From here on DOSBox owns the frame loop.  retro_run() runs one emulated
	// frame and returns; the video and audio callbacks present its output.
	// We call it in a loop and stop when the core asks to (E_Exit, or "exit"
	// typed at the DOS prompt).
	u64 nFrames = 0;

	// Watchdog state.  The emulator stopping silently is the failure mode this
	// port keeps running into, so the frame loop keeps its own account of what
	// it can *measure*, not of what it has drawn: frames presented, audio
	// queued and dropped, and how long retro_run itself took.  Two seconds is
	// far above any legitimate gap (one frame is 16 ms) and it reports at most
	// once a second, so a stall cannot flood the display.
	const u64 nStallMicros = 2000000;
	u64 nLastReportMicros = 0;
	u64 nMaxCallMicros = 0;
	u64 nLastPanelMicros = 0;

	// Real time.  This loop is what paces the emulator: the audio path no
	// longer blocks (it drops instead), so an unpaced loop would run DOS as
	// fast as the interpreter can, which looks like a very fast clock and a
	// twitchy keyboard.
	//
	// How fast, exactly: with RUN_SYNC - the libretro "external timing" mode,
	// which is the core's default and the recommended one - the core returns
	// from retro_run() once per *emulated* frame.  A text mode refreshes at
	// 70 Hz and a 320x200 game at 70 Hz too, so the loop is asked for 70
	// frames per second, not 60.  Pacing that at 60 runs the machine 17 %
	// fast, and no amount of "cycles" fixes it: cycles say how much work fits
	// into an emulated frame, not how many frames happen per second.  That is
	// exactly the report - "when the resolution goes down it all seems to go
	// faster and the clock cycles do not fix it".
	//
	// So the target is the rate the core announced (BareVideoRefreshMilliHz,
	// fed from SET_SYSTEM_AV_INFO).
	//
	// Measured from the start of the *first* call so that overtime carries
	// over; re-anchored if the loop falls more than 200 ms behind.
	//
	// ---------------------------------------------------------------------
	// The .57 correction: when this loop cannot sleep, it says so
	// ---------------------------------------------------------------------
	// Until .56 every constant in this block was written in microseconds and
	// compared against a counter that was not one (see bare_time.cpp: Circle's
	// GetClockTicks64() is the 54 MHz physical counter).  The consequence was
	// that "how far behind am I" was always huge, the wait was always skipped,
	// and the emulator ran as fast as the interpreter could be driven - which
	// is exactly the report: *everything* fast, video and audio, and no number
	// of cycles fixes it, because cycles were never the limit.
	//
	// The clock is fixed, but the lesson is worth keeping in the code: a pacer
	// that silently does not pace looks identical to an emulator that is
	// "too fast".  So the loop now counts how many frames it *could not* slow
	// down, and reports that once a second while it is happening.  Frame
	// budget overruns are normal and expected on a Pi 4 without a JIT; what is
	// not normal is never sleeping at all.  Those are different sentences and
	// the panel should not leave which one is happening to be guessed.
	//
	// ---------------------------------------------------------------------
	// .58: the 60 Hz cap was itself a speed error, and it is gone
	// ---------------------------------------------------------------------
	//
	// Until .57 this line said "capped at 60", on the reasoning that a Pi 4
	// without a JIT cannot hold 70 Hz in a game, so pretending to would only
	// make the audio skip.  That reasoning is wrong in one specific way, and
	// the error is easy to get the sign of wrong, so it is written out here:
	//
	//   In RUN_SYNC (the core's default, core_timing = "external") the core
	//   returns from retro_run() once per *emulated* frame, and one emulated
	//   frame is 1/70 s of emulated time in a 70 Hz mode.  The number of
	//   retro_run() calls per real second is therefore the emulated clock:
	//
	//       calling it 60 times a second -> 60/70 = 86 % speed  (slow)
	//       calling it 70 times a second -> 70/70 = 100 % speed
	//
	//   Pacing a 70 Hz mode at 60 does not protect the picture; it makes the
	//   emulated machine run 14 % slow, and it does so *silently*, because
	//   nothing is over budget and nothing complains.  The cap cannot help
	//   when the host is too slow either: if a frame takes 30 ms, the loop is
	//   simply late, does not sleep, and the machine runs at whatever the
	//   interpreter can drive - which is 43 % speed, and that is the honest
	//   limit, not something a cap improves.
	//
	// So the target is the announced rate, full stop.  What is left is a
	// *sanity* ceiling, for the case where the core announces something
	// absurd (a division by a bogus fps would otherwise produce a 0 us budget
	// and a divide by zero later): 240 Hz is above every mode DOSBox can
	// produce (VGA text tops out at 70) and below anything that would make
	// the arithmetic misbehave.
	const unsigned nMaxHz = 240;
	u64 nFrameMicros = 1000000 / nMaxHz;
	unsigned nPacingMilliHz = 0;		// what the current nFrameMicros means

	// Whether the core has announced a rate yet.  It has not, until the first
	// SET_SYSTEM_AV_INFO or the first retrofit of it, and until then the
	// numbers above are placeholders - which is exactly why the panel must not
	// print them.  It did: on the board, the first second of a boot read
	// "pace 31/240", where 240 is this ceiling and not a rate anybody asked
	// for.  A status field that shows an invented number once is a status field
	// whose numbers have to be doubted every time.
	boolean bRateKnown = FALSE;

	// The rate every frame is paced to, as the core announced it.  Kept apart
	// from nFrameMicros (microseconds per frame) because the panel wants the
	// rate in Hz, and because the 1/rate rounding is lossy: a 59.923 Hz mode
	// is 16688 us per frame, and dividing that back gives 59. The panel shows
	// the announced number, not the arithmetic that came out of it.
	unsigned nCoreHz = 0;

	// The last full second the pacer was watched over, for the panel.
	u64 nPacePresentedStart = 0;		// BareVideoFramesPresented() at the start
	u64 nPaceDupedStart = 0;		// BareVideoDuplicatePresents() at the start
	u64 nPaceDroppedStart = 0;		// BareAudioDroppedSamples() at the start
	u64 nPaceCallMicros = 0;		// sum of retro_run() durations this window
	u64 nWaitMicros = 0;			// sum of the time this loop spent waiting
	u64 nPaceMicrosStart = 0;

	// Published once per second and read by BuildPanel() (which lives above
	// this function, hence s_Pace* at file scope - see its declaration).
	unsigned nPaceFrames = 0;		// frames the LOOP asked for
	unsigned nPaceDrives = 0;		// frames the emulator drove by itself
	unsigned nPaceBounds = 0;		// frame boundaries the EMULATED MACHINE reached
	unsigned nPaceBoundsStart = 0;
	u64 nLastChangedForGovernor = BareVideoFramesChanged ();		// ...at the start of this one-second window

	u64 nRefStart = BareMicros ();
	u64 nFrameIndex = 0;

	// Pacer health, reported once a second while it is bad and once when it
	// recovers.  nLateFrames counts frames whose slot had already passed;
	// nZeroWaits counts frames where the wait was not even a microsecond, and
	// a whole second of those is the signature of this bug and of nothing else.
	u64 nLateFrames = 0;
	u64 nZeroWaitFrames = 0;
	u64 nPacerWindowFrames = 0;
	constexpr u64 PacerWindowMicros = 1000000;
	u64 nPacerWindowStart = nRefStart;
	boolean bPacerComplained = FALSE;

	while (!BareCoreWantsExit ())
	{
		// Alt+Pause: do not run the emulator at all.  Everything else in
		// this loop measures how long a frame took and how it compares to
		// its budget; none of that means anything while the machine is
		// stopped, so the whole block is skipped rather than fed zeros -
		// zeros would look like a perfect governor condition and walk the
		// clock down, which is right, but they would also look like a
		// stall to the watchdog, which is not.
		if (BareCorePaused ())
		{
			CTimer::SimpleMsDelay (20);

			// Re-anchor the pacer on resume: the machine did not run
			// during the pause, so the frame index must not bill it for
			// the wall-clock time that passed.
			nRefStart = BareMicros ();
			nFrameIndex = 0;

			continue;
		}

		// --- the slot for this call, paid *before* the call -------------------
		// See the long note further down: the wait used to sit after
		// retro_run(), which is only a pacer while retro_run() returns once per
		// emulated frame.  Paying it up front makes "one call per frame" true
		// even when the core presents several frames inside one call.
		nFrameIndex++;
		{
			const u64 nDue = nRefStart + nFrameIndex * nFrameMicros;
			const u64 nSlotNow = BareMicros ();
			if (nDue > nSlotNow)
			{
				const u64 nWait = nDue - nSlotNow;
				if (nWait < 1000)
				{
					nZeroWaitFrames++;
				}
				CTimer::SimpleusDelay (static_cast<unsigned> (nWait));
				nWaitMicros += nWait;
			}
			else
			{
				nLateFrames++;
				nZeroWaitFrames++;

				// Far behind: the frame budget just changed, or the core is
				// simply not keeping up.  Re-anchor so the lag does not
				// accumulate for ever.
				if (nSlotNow - nDue > 200000)
				{
					nRefStart = nSlotNow;
					nFrameIndex = 0;
				}
			}
		}

		const u64 nCallStart = BareMicros ();
		BareCoreRunFrame ();
		const u64 nCallMicros = BareMicros () - nCallStart;

		// One "now" for everything below that compares against the window.  Read
		// here, after the call, so the numbers the panel shows describe the frame
		// that has just happened rather than the one being waited for.
		const u64 nNow = BareMicros ();
		if (nCallMicros > nMaxCallMicros)
		{
			nMaxCallMicros = nCallMicros;
		}

		nFrames++;

		// Ten frames is enough to prove the core is producing video: if the
		// screen shows this line, the frame loop has run and the black screen
		// is upstream of it (mode change, blank frame, wrong page).
		if (nFrames == 10)
		{
			Report ("core: 10 frames done");
		}

		// Pacing.  Sleep only the remaining part of the frame; if the frame
		// took longer than that (the interpreter is slow at 640x400), there is
		// nothing to wait for and the index re-anchors so the lag does not
		// accumulate for ever.
		// Has the core told us a different rate?  Cheap enough to ask every
		// frame (one load), and it is the only way to notice a game switching
		// from a 70 Hz text screen to a 59.9 Hz VGA mode without a callback
		// into this file.
		{
			const unsigned nMilliHz = BareVideoRefreshMilliHz ();
			if (nMilliHz != nPacingMilliHz)
			{
				nPacingMilliHz = nMilliHz;

				if (nMilliHz != 0)
				{
					// The core's rate.  Rounded up to the nearest microsecond
					// rather than truncated, so 59.923 Hz does not become
					// 59.99, and clamped only by the sanity ceiling above.
					unsigned nHz = (nMilliHz + 500) / 1000;
					if (nHz == 0)
					{
						nHz = 1;
					}
					if (nHz > nMaxHz)
					{
						nHz = nMaxHz;
					}
					nFrameMicros = (1000000 + nHz / 2) / nHz;
				}
				else
				{
					nFrameMicros = 1000000 / nMaxHz;
				}

				// The announced rate, as announced.  The panel shows it as
				// the denominator of "pace R/W": a mode that asks for 70 and
				// gets 70 reads "pace 70/70", and one that asks for 70 and
				// gets 50 reads "pace 50/70", which is the sentence the
				// user needs.  Before .58 the W was the *budget* (60), so a
				// slow 70 Hz mode read "50/60" and looked healthy.
				nCoreHz = nMilliHz != 0 ? (nMilliHz + 500) / 1000 : 0;
				bRateKnown = nCoreHz != 0;

				// Re-anchor: the frame budget just changed, and keeping the
				// old index would credit or bill the past frames at the new
				// rate.
				nRefStart = BareMicros ();
				nFrameIndex = 0;

				Report ("pacing: %u Hz from the core, running at %u us/frame",
					(nMilliHz + 500) / 1000,
					static_cast<unsigned> (nFrameMicros));
			}
		}

		// ------------------------------------------------------------------
		// .65f: and why the slot is paid up front
		// ------------------------------------------------------------------
		//
		// The order above is the whole fix, so it is stated once: the wait is
		// at the top of the iteration, before retro_run().
		//
		// It used to be here, after the call, and that is only a pace-maker
		// while retro_run() *returns* once per emulated frame.  In the other
		// case - the common one on this port, a game whose blitter draws faster
		// than its own video mode - the core keeps emulating and never comes
		// back, and the video callback inside it presents frame after frame
		// while this loop sits asleep in the middle of one iteration.  The only
		// thing still metering anything was bare_thread.cpp's drive wait, which
		// is capped at one budget and only runs on emulator-driven boundaries:
		// so the machine ran at its own pace *plus* whatever it could steal.
		// That is "la acción y el sonido van acelerados" with a perfectly
		// plausible `pace` reading on screen.
		//
		// Paying the slot before the call makes "one call per frame" true for
		// every call, whether the previous one presented one picture, twelve, or
		// none.  The bookkeeping below - and the governor, the panel and the
		// watchdog - now runs *inside* the slot it just waited for, so none of
		// it is charged to the emulator's budget either.  Where it used to be is
		// exactly how the drift existed.

		// What the panel shows as "pace": frames the loop asked for, frames
		// the emulator drove on its own, and frames actually presented, all
		// over the same wall-clock second.  Measured here because this is the
		// only place that sees all three.
		nPaceFrames++;
		nPaceDrives += BareThreadTrueDrivesSinceReport ();
		nPaceBounds += BareThreadBoundariesSinceReport ();
		nPaceCallMicros += static_cast<unsigned> (nCallMicros);

		// Pacer health, once a second.  Two different sentences on purpose:
		// "over budget" is the interpreter being slow, "no wait at all" is
		// the pacer not working, and conflating them is how .56 spent a round
		// looking at the wrong thing.
		nPacerWindowFrames++;
		if (nNow - nPacerWindowStart >= PacerWindowMicros)
		{
			const u64 nPerFrame = (nNow - nPacerWindowStart) / nPacerWindowFrames;

			if (nZeroWaitFrames * 2 >= nPacerWindowFrames)
			{
				Report ("pace: %llu/%llu frames never waited, %llu us/frame"
					" (budget %u) - the pacer is not working",
					(unsigned long long) nZeroWaitFrames,
					(unsigned long long) nPacerWindowFrames,
					(unsigned long long) nPerFrame,
					static_cast<unsigned> (nFrameMicros));
				bPacerComplained = TRUE;
			}
			else if (bPacerComplained)
			{
				Report ("pace: back to %llu us/frame, %llu late of %llu",
					(unsigned long long) nPerFrame,
					(unsigned long long) nLateFrames,
					(unsigned long long) nPacerWindowFrames);
				bPacerComplained = FALSE;
			}

			// The rates, for the panel.  Presented counts what actually
			// reached the screen; asked counts what the loop requested;
			// drives is what the emulator did without asking.  The three
			// together are the only honest answer to "is it running at its
			// own speed".
			const u64 nPresented = BareVideoFramesPresented ();
			const u64 nDuped = BareVideoDuplicatePresents ();
			if (nPaceMicrosStart != 0 && nNow > nPaceMicrosStart)
			{
				s_PacePresented = static_cast<unsigned> (
					(nPresented - nPacePresentedStart) * 1000000
					/ (nNow - nPaceMicrosStart));
				s_PaceDuped = static_cast<unsigned> (
					(nDuped - nPaceDupedStart) * 1000000
					/ (nNow - nPaceMicrosStart));

				// Drop rate: this second's samples, extrapolated to a
				// second when the window is not exactly one.
				const u64 nDropped = BareAudioDroppedSamples ();
				s_PaceDropped = static_cast<unsigned> (
					(nDropped - nPaceDroppedStart) * 1000000
					/ (nNow - nPaceMicrosStart));
				nPaceDroppedStart = nDropped;
			}
			s_PaceAsked = nPaceFrames;
			s_PaceDrives = nPaceDrives;
			if (nNow > nPaceMicrosStart)
			{
				s_PaceBounds = static_cast<unsigned> (
					(nPaceBounds - nPaceBoundsStart) * 1000000
					/ (nNow - nPaceMicrosStart));
			}
			s_PaceWant = nCoreHz;
			s_PaceRateKnown = bRateKnown ? 1 : 0;

			// What fraction of the second the loop spent *waiting* for its own
			// next frame.  The other side of the coin - how long the emulator
			// took - is `fr` below, and between the two of them a person can
			// tell "the machine is metered" from "the machine is running flat
			// out and something else is producing the pictures".
			if (nNow > nPaceMicrosStart)
			{
				s_PaceWaited = static_cast<unsigned> (
					nWaitMicros * 100 / (nNow - nPaceMicrosStart));
			}

			s_PaceBudgetHz = static_cast<unsigned> (1000000 / nFrameMicros);
			s_PaceMicrosPublished = 1;
			s_PaceCallMicros = nPaceFrames != 0
					 ? static_cast<unsigned> (nPaceCallMicros / nPaceFrames)
					 : 0;

			nPacePresentedStart = nPresented;
			nPaceDupedStart = nDuped;
			nPaceMicrosStart = nNow;
			nWaitMicros = 0;
			nPaceFrames = 0;
			nPaceDrives = 0;
			nPaceBoundsStart = nPaceBounds;
			nPaceCallMicros = 0;

			nPacerWindowStart = nNow;
			nPacerWindowFrames = 0;
			nZeroWaitFrames = 0;
			nLateFrames = 0;
		}

		// The watchdog is evaluated BEFORE the sleep of this frame, so a long
		// wait cannot itself look like a stall: what it measures is the gap
		// since the last frame the core presented, against two seconds.
		//
		// Screenshots are taken here, not in the keyboard callback where the
		// request arrives.  Writing a BMP to the card blocks for tens of
		// milliseconds - sometimes much longer - and the keyboard callback
		// runs inside the emulator's own call chain, so doing it there stops
		// the emulator and starves the sound device.  Here, the emulator is
		// between frames and has already returned.
		// The clock governor.  It wants two numbers and they are different
		// questions:
		//
		//   nCallMicros    how long the emulated frame took
		//   nFrameMicros   how long it was *allowed* to take
		//
		// Until .57 only the first was passed and the governor compared it
		// against a hard-coded 16.6 ms.  That is right for a 60 Hz mode and
		// wrong for everything else: a DOS text screen is 70 Hz (14.3 ms), so
		// a perfectly paced 70 Hz machine looked like a permanent overrun and
		// the governor climbed to the top level and stayed there.  Passing the
		// budget makes "is this frame late" a question about the mode that is
		// actually running.
		// .65e: and only with a rate the core has actually announced.  Until
		// then nFrameMicros is the sanity ceiling's 1/240 s, which is not a
		// budget anything can be measured against - see the note in
		// BareSysInfoGovernorTick().  Passing 0 says "no measurement" instead of
		// handing the governor a number the port invented.
		// The third argument is the one .65g added, and it is what makes a
		// prompt look like a prompt: whether this frame's picture differs
		// from the last one.  The frame count alone could not say it - a text
		// screen presents seventy identical frames a second - so the board
		// reported itself busy, at 1800 MHz, doing nothing.
		const u64 nChangedNow = BareVideoFramesChanged ();
		const boolean bFrameChanged = nChangedNow != nLastChangedForGovernor;
		nLastChangedForGovernor = nChangedNow;

		BareSysInfoGovernorTick (static_cast<unsigned> (nCallMicros),
					 bRateKnown
						? static_cast<unsigned> (nFrameMicros)
						: 0,
					 bFrameChanged);

		// Does the emulator have something on screen?  A DOS prompt presents no
		// frames, so there is nothing to be slow at and the board belongs at
		// 600 MHz.  "Recently presented" is the cheap test: the counter only
		// moves from the video callback, and one second of silence is the line.
		//
		// The two seconds are not the same thing: the panel is redrawn once a
		// second, the text mode refreshes at 70 Hz but the core only hands back
		// a *frame* when it has one, and a prompt that just scrolled hands back
		// a few and then stops.
		{
			static u64 nLastPresentedFrames = 0;
			static u64 nLastPresentedMicros = 0;
			const u64 nPresented = BareVideoFramesPresented ();
			if (nPresented != nLastPresentedFrames)
			{
					nLastPresentedFrames = nPresented;
				nLastPresentedMicros = BareMicros ();
			}

			BareSysInfoSetCLIActivity (BareMicros () - nLastPresentedMicros > 1000000);
		}

		if (BareTakeScreenshotRequest ())
		{
			// BareVideoScreenshot() writes the line itself, to the same
			// logger and through the same path, so a failure is visible on
			// screen and not only here.
			(void) BareVideoScreenshot ();
		}

		// The status line, once a second, in the unused strip at the bottom of
		// the display.  Hidden by default; PetSis toggles it (bare_libretro.cpp)
		// and BareVideoFooter() itself returns early while it is hidden, so
		// this costs a clock read and nothing else.
		//
		// It does NOT take the screen away from the emulator - that is the
		// watchdog's job, and only when the emulator has actually stopped.
		// The panel: only the *text* is decided here, once a second.
		//
		// The drawing is not this file's job and this is not where it happens.
		// BareVideoPresent() repaints every line slot on every frame, from the
		// text stored in the slot - which is the only way a line that sits on
		// top of the picture survives a game that rewrites its frame buffer
		// every frame.  Doing it here as well would be a second write of the
		// same pixels per frame, for nothing.
		//
		// So the split is: this loop decides what the panel *says* (and asks
		// the firmware for the temperature at most once a second, because that
		// call is not free), and bare_video.cpp decides when it is *drawn*.
		if (BareVideoFramesPresented () != 0)
		{
			// Turning the panel on with PetSis has to show it *now*, not at
			// the next one-second boundary: the slot has no text stored yet,
			// so the per-frame repaint would have nothing to paint until this
			// loop filled it in.
			static boolean bPanelWasVisible = FALSE;
			const boolean bPanelVisible = BareVideoFooterVisible () != FALSE;
			if (bPanelVisible != bPanelWasVisible)
			{
				bPanelWasVisible = bPanelVisible;
				if (bPanelVisible)
				{
					nLastPanelMicros = BareMicros ();
					BareSysInfoRefresh ();
					BareVideoFooter (0, BuildPanel ());
				}
			}

			if (bPanelVisible && BareMicros () - nLastPanelMicros > 1000000)
			{
				nLastPanelMicros = BareMicros ();
				BareSysInfoRefresh ();

				// Cheap when the text has not changed; the slot only stores
				// it.  The next present() paints it.
				BareVideoFooter (0, BuildPanel ());
			}
		}

		const u64 nSinceCore = BareThreadMicrosSinceHandBack ();
		if (nSinceCore > nStallMicros && BareMicros () - nLastReportMicros > 1000000)
		{
			nLastReportMicros = BareMicros ();

			u64 nQueued = 0;
			u64 nDropped = 0;
			u64 nSinceWrite = 0;
			BareAudioStats (&nQueued, &nDropped, &nSinceWrite);

			const unsigned nHandBacks = BareThreadHandBacksSinceReport ();
			const unsigned nYields = BareThreadEmulatorYields ();
			const unsigned nDrives = BareThreadTrueDrivesSinceReport ();
			const unsigned nBounds = BareThreadBoundariesSinceReport ();

			// The emulator has the screen but is not painting it.  Take the
			// display back: a frozen picture explains nothing.
			BareVideoStatusEnable ();
			Report ("STALL: core silent %llu ms, %u hand-backs",
				nSinceCore / 1000, nHandBacks);
			Report ("STALL: %llu calls, %llu frames, %llu dup, %u yields",
				nFrames, BareVideoFramesPresented (),
				BareVideoDuplicatePresents (), nYields);
			Report ("STALL: %u boundaries/s of which %u self-driven", nBounds, nDrives);
			Report ("STALL: max %llu ms, driving %s, %u self-driven, audio %llu/%llu q/d",
				nMaxCallMicros / 1000,
				BareThreadEmulatorDriving () ? "yes" : "no",
				nDrives,
				nQueued, nDropped);
		}
	}

	// The panel is about to outlive its own data: from here on nothing in this
	// file updates it, so the last text has to be *true for ever*.  Saying
	// "flushing the card" and then going quiet is what left a user watching a
	// frozen screen and guessing; the wording below says what is happening now
	// and, crucially, which of the two possible endings each line is.
	// Core 0 takes the screen back from the core 2 presenter before the
	// shutdown lines: from here on the emulator produces no frames, and the
	// teardown below must not race a core that is flipping pages and owns
	// the mailbox.  A no-op when the presenter is not running.
	BareVideoPresenterStop ();

	BareVideoFooterSetVisible (TRUE);
	BareVideoFooter (0, "shutting down: stopping the emulator, do NOT cut power yet");

	BareAudioDrain ();

	BareCoreDeinit ();

	BareInputDeinit ();
	BareAudioDeinit ();

	// Nothing else is written to the panel from here: Cleanup() runs after this
	// function returns and it is the one that flushes the card, so *it* owns the
	// last sentence.  Saying "done" here was the .65e mistake in draft form - it
	// would have been true about the teardown and false about the card.
	BareVideoFooter (0, "emulator stopped: flushing the card");

	// Not BareVideoDeinit(): see CKernel::Cleanup().

	return ShutdownHalt;
}
