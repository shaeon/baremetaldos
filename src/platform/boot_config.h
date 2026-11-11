//
// boot_config.h
//
// by @mmoroca & arena.ai 2026  -  https://arena.ai
//
//
// This file is original to BM-DOS unless stated otherwise, and is distributed
// under the GNU General Public License version 3.  The binary it is part of
// links Circle and DOSBox-core (both GPL), so GPL-3.0 is what it inherits.
// See LICENSE in the repository root.
//
// BM-DOS - the port's own settings file, /boot.conf on the card.
//
// This is a header with the whole implementation in it, and that deserves an
// explanation, because a header with code is normally a mistake.
//
// The problem it solves: the display mode has to be known before the frame
// buffer is created, which is inside CKernel::Initialize(); and the only file
// file meant to be edited after flashing a card is this one.  That puts the
// parser in kernel.cpp - the one file the host cannot compile (circle_stdlib_app.h
// and the entire kernel come with it).  The parser has no business being
// untestable, so it lives here: no Circle, no CStdlibApp, no iostream, only
// <cstdio>, <cstdarg> and <cstring>.  scripts/hostcheck/bootconf_test.cpp
// includes this header directly and feeds it the files people actually write.
//
// If you are about to #include this from a second translation unit: don't.  It
// has non-inline functions on purpose (static would mean a copy per unit and a
// -Wunused-function the moment one unit does not use it).  kernel.cpp is its
// only client.
//
// The format is deliberately dull: one "key = value" per line, '#' comments,
// anything unknown ignored but reported.  A config parser that can fail is a
// way to make a bare metal board unbootable from a text editor.
//
#ifndef BM_DOS_BOOT_CONFIG_H
#define BM_DOS_BOOT_CONFIG_H
//
// This file is original to BM-DOS unless stated otherwise, and is distributed
// under the GNU General Public License version 3.  The binary it is part of
// links Circle and DOSBox-core (both GPL), so GPL-3.0 is what it inherits.
// See LICENSE in the repository root.

#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cstring>
#include <cstddef>

// Where the file lives.  "SD:" because newlib's fopen() goes through Circle's
// FatFs glue, which needs the volume name to know which drive (bare_fs.cpp
// explains the two spellings).
//
// Overridable on purpose, and only for one caller: scripts/hostcheck/
// bootconf_test.cpp compiles with -DBOOT_CONFIG='"boot.conf"' so that the
// fixture files land in its working directory instead of a path that does not
// exist on the host.  The firmware build never defines it, so on the board this
// is always the line below.
#ifndef BOOT_CONFIG
#define BOOT_CONFIG		"SD:/boot.conf"
#endif

// Display mode we ask the firmware for, unless boot.conf says otherwise.
//
// Why this is a choice at all: every frame the emulator produces is converted
// from its own palette and copied into this buffer, so the cost of a frame is
// proportional to the number of pixels here, not to the size of the DOS screen.
// At 1920x1080 that copy is 2 million pixels per frame - 124 MB/s at 60 Hz - and
// on a Pi 4 with no JIT it is the single largest fixed cost of the port.  At
// 1280x720 it is 0.9 million pixels per frame.
//
// 1280x720 is the default because it is the mode a 1080p panel scales *exactly*
// - one output pixel becomes 1.5, and the firmware's scaler does that without
// the blur of a non-integer ratio - and because DOS graphics are 320x200 and
// 640x480, which want a multiple of 320 wide more than they want 1080 lines of
// vertical resolution.
//
// Change it in /boot.conf on the card (see sd/boot.conf), not here: recompiling
// to try another mode is a five-minute round trip, and this is a setting people
// should try and keep.
static constexpr unsigned DefaultDisplayWidth = 1280;
static constexpr unsigned DefaultDisplayHeight = 720;

// The largest mode this port will accept from the config file.  Not a display
// limit - it is a sanity limit on a file that a user can edit by hand, so that
// "display=99999x99999" cannot make the firmware fail in a confusing way.
static constexpr unsigned MaxDisplayWidth = 1920;
static constexpr unsigned MaxDisplayHeight = 1080;

// ---------------------------------------------------------------------------
// The parser
// ---------------------------------------------------------------------------
//
// Why this file exists at all, and why it is not dosbox.conf: dosbox.conf is
// parsed by DOSBox, so by the time DOSBox reads it the frame buffer already
// exists and the display mode has already been asked for.  A display mode has
// to be known *before* video initialisation, which is the second thing this
// kernel does.  So the port has its own four-line file, read with fopen() as
// soon as Circle has mounted the card.
//
// The format is deliberately dull: one "key = value" per line, '#' comments,
// anything unknown ignored and *reported*.  A config parser that can fail is a
// way to make a bare metal board unbootable from a text editor.
struct TBootConfig
{
	// bool, not Circle's boolean: this header has to compile on the host (see
	// the top of the file), and boolean/TRUE/FALSE only exist in Circle.  The
	// caller converts, which is whoever asked the question.

	unsigned nWidth;
	unsigned nHeight;
	bool bHaveDisplay;			// true = the file chose the mode
	unsigned nCPUMaxMHz;		// 0 = let the firmware decide
	char Mode[16];			// "auto", "performance", "powersave"

	// How much of the boot writes to the screen once the port owns it:
	//
	//   "auto"  everything, which is what a boot you have to debug needs
	//   "quiet" the version block and the panel, nothing from the core
	//   "off"   nothing at all: straight to the picture
	//
	// "quiet" is the useful one.  The core's own messages are what fills the
	// screen at boot (forty of them, "Variable dosbox::machine updated to ..."),
	// and their only purpose is to tell you that a config file was read.  Nobody
	// reads that twice.  "off" also hides the version block, so it is for a
	// card that is finished, not for one being worked on.
	char BootLog[8];		// "auto", "quiet", "off"

	// Which core puts the picture on the screen while the emulator runs:
	//
	//   2  core 2 scales into a hidden page, flips and waits for vsync;
	//      core 0 only hands the frame over.  The default.
	//   0  core 0 scales straight into the page on screen, as before -
	//      for comparing the two, or if core 2 misbehaves on a board.
	//
	// If core 2 is not available the port falls back to 0 by itself.
	unsigned nVideoCore;

	// With video_core = 2: wait for vertical sync after each flip.  On, the
	// picture never tears (a 70 Hz DOS mode on a 60 Hz display drops about
	// one frame in seven instead).  Off, core 2 flips as soon as a frame is
	// ready.  The emulator's speed does not depend on it either way.
	bool bVsync;

	// How a picture enlarged by a fraction is sampled vertically:
	//   sharp   area sampling - only the rows on a source boundary mix
	//           (the default; text keeps its edges)
	//   smooth  linear interpolation, the port's original look
	bool bSharpScale;

	// Sound output: HDMI (default) or the 3.5 mm jack (PWM).  The other one is
	// still tried if the chosen one does not start.
	bool bAudioJack;

	// Volumes in percent: the whole output (0-100), and the MIDI synthesizers
	// relative to DOSBox's own sound (25-200).  Both applied by the port, after
	// DOSBox's mixer.
	unsigned nVolume;
	unsigned nMidiVolume;

	// Mouse speed in percent (25-400), applied by the port to the USB mouse's
	// movement before DOSBox sees it.
	unsigned nMouseSpeed;

	// What the file said that could not be used.  ReadBootConfig is a plain
	// function on purpose (it runs before anything else exists and has no
	// business touching the kernel), so it cannot call CKernel::Report();
	// it collects the complaint and the caller prints it, through Report(),
	// so the line also reaches the frame buffer's status overlay.
	char Warning[96];
};

void Trim (char *p)
{
	// Leading and trailing spaces, in place.  Written out because strtok() on a
	// const string and strdup() on a bare metal board are both traps.
	char *pStart = p;
	while (*pStart == ' ' || *pStart == '\t')
	{
		pStart++;
	}

	if (pStart != p)
	{
		memmove (p, pStart, strlen (pStart) + 1);
	}

	size_t n = strlen (p);
	while (n != 0 && (p[n - 1] == ' ' || p[n - 1] == '\t' || p[n - 1] == '\r'
			  || p[n - 1] == '\n'))
	{
		p[--n] = '\0';
	}
}

// One complaint per file is enough: the first one is the one to fix, and the
// buffer is 96 bytes because Report's own line buffer is 160 and the overlay
// truncates anyway.  Kept dumb on purpose.
void BootConfigWarn (TBootConfig *pConfig, const char *pFormat, ...)
{
	if (pConfig->Warning[0] != '\0')
	{
		return;			// keep the first
	}

	va_list Args;
	va_start (Args, pFormat);
	vsnprintf (pConfig->Warning, sizeof pConfig->Warning, pFormat, Args);
	va_end (Args);
}

void ReadBootConfig (TBootConfig *pConfig)
{
	pConfig->nWidth = DefaultDisplayWidth;
	pConfig->nHeight = DefaultDisplayHeight;
	pConfig->bHaveDisplay = false;
	pConfig->nCPUMaxMHz = 0;
	strcpy (pConfig->Mode, "auto");
	strcpy (pConfig->BootLog, "auto");
	pConfig->nVideoCore = 2;
	pConfig->bVsync = true;
	pConfig->bSharpScale = true;
	pConfig->bAudioJack = false;
	pConfig->nVolume = 100;
	pConfig->nMidiVolume = 100;
	pConfig->nMouseSpeed = 100;
	pConfig->Warning[0] = '\0';

	FILE *pFile = fopen (BOOT_CONFIG, "r");
	if (pFile == nullptr)
	{
		return;
	}

	char Line[128];
	while (fgets (Line, sizeof Line, pFile) != nullptr)
	{
		char *pHash = strchr (Line, '#');
		if (pHash != nullptr)
		{
			*pHash = '\0';
		}

		Trim (Line);
		if (*Line == '\0')
		{
			continue;
		}

		char *pEquals = strchr (Line, '=');
		if (pEquals == nullptr)
		{
			continue;
		}

		*pEquals = '\0';
		char *pKey = Line;
		char *pValue = pEquals + 1;
		Trim (pKey);
		Trim (pValue);

		if (strcmp (pKey, "display") == 0)
		{
			unsigned nW = 0;
			unsigned nH = 0;
			// "WIDTHxHEIGHT", the way xrandr and every monitor OSD writes it.
			// A capital X is accepted because it is the obvious way to get it
			// wrong, and sscanf's literal match is case sensitive.  The buffer
			// is ours (it is the line read from the file), so in place is fine.
			for (char *p = pValue; *p != '\0'; p++)
			{
				if (*p == 'X')
				{
					*p = 'x';
				}
			}

			if (sscanf (pValue, "%ux%u", &nW, &nH) != 2)
			{
				BootConfigWarn (pConfig, "boot.conf: cannot read \"%s\" as WxH", pValue);
			}
			else if (nW < 320 || nH < 200
				 || nW > MaxDisplayWidth || nH > MaxDisplayHeight)
			{
				// Say so instead of ignoring it: a typo in the one file the
				// user edits has to be findable from the log, and the panel
				// cannot show a line before there is a frame buffer.
				BootConfigWarn (pConfig, "boot.conf: %ux%u out of range (320x200..%ux%u)",
					nW, nH, MaxDisplayWidth, MaxDisplayHeight);
			}
			else
			{
				pConfig->nWidth = nW;
				pConfig->nHeight = nH;
				pConfig->bHaveDisplay = true;
			}
		}
		else if (strcmp (pKey, "cpu_max_mhz") == 0)
		{
			unsigned n = 0;
			if (sscanf (pValue, "%u", &n) != 1)
			{
				BootConfigWarn (pConfig, "boot.conf: cannot read \"%s\" as a frequency", pValue);
			}
			else if (n != 0 && (n < 600 || n > 2400))
			{
				BootConfigWarn (pConfig, "boot.conf: %u MHz out of range (0 or 600..2400),"
					" using the firmware's ceiling", n);
			}
			else
			{
				// 0 means "no ceiling": the governor builds its levels from
				// whatever the firmware says the maximum is.
				pConfig->nCPUMaxMHz = n;
			}
		}
		else if (strcmp (pKey, "boot_log") == 0)
		{
			if (   strcmp (pValue, "auto") == 0
			    || strcmp (pValue, "quiet") == 0
			    || strcmp (pValue, "off") == 0)
			{
				strcpy (pConfig->BootLog, pValue);
			}
			else
			{
				BootConfigWarn (pConfig, "boot.conf: unknown boot_log \"%s\""
					" (auto|quiet|off)", pValue);
			}
		}
		else if (strcmp (pKey, "video_core") == 0)
		{
			if (strcmp (pValue, "0") == 0 || strcmp (pValue, "2") == 0)
			{
				pConfig->nVideoCore = static_cast<unsigned> (pValue[0] - '0');
			}
			else
			{
				BootConfigWarn (pConfig, "boot.conf: unknown video_core \"%s\""
					" (0|2)", pValue);
			}
		}
		else if (strcmp (pKey, "vsync") == 0)
		{
			if (strcmp (pValue, "on") == 0 || strcmp (pValue, "off") == 0)
			{
				pConfig->bVsync = strcmp (pValue, "on") == 0;
			}
			else
			{
				BootConfigWarn (pConfig, "boot.conf: unknown vsync \"%s\""
					" (on|off)", pValue);
			}
		}
		else if (strcmp (pKey, "audio_out") == 0)
		{
			if (strcmp (pValue, "hdmi") == 0 || strcmp (pValue, "jack") == 0)
			{
				pConfig->bAudioJack = strcmp (pValue, "jack") == 0;
			}
			else
			{
				BootConfigWarn (pConfig, "boot.conf: unknown audio_out \"%s\""
					" (hdmi|jack)", pValue);
			}
		}
		else if (strcmp (pKey, "mouse_speed") == 0)
		{
			char *pEnd = nullptr;
			const unsigned long n = strtoul (pValue, &pEnd, 10);
			if (pEnd == pValue || *pEnd != '\0' || n < 25 || n > 400)
			{
				BootConfigWarn (pConfig, "boot.conf: mouse_speed \"%s\" is not 25-400", pValue);
			}
			else
			{
				pConfig->nMouseSpeed = static_cast<unsigned> (n);
			}
		}
		else if (strcmp (pKey, "volume") == 0 || strcmp (pKey, "midi_volume") == 0)
		{
			const bool bMidi = strcmp (pKey, "midi_volume") == 0;
			char *pEnd = nullptr;
			const unsigned long n = strtoul (pValue, &pEnd, 10);
			const unsigned long nMin = bMidi ? 25 : 0;
			const unsigned long nMax = bMidi ? 200 : 100;
			if (pEnd == pValue || *pEnd != '\0' || n < nMin || n > nMax)
			{
				BootConfigWarn (pConfig, "boot.conf: %s \"%s\" is not %lu-%lu",
						pKey, pValue, nMin, nMax);
			}
			else if (bMidi)
			{
				pConfig->nMidiVolume = static_cast<unsigned> (n);
			}
			else
			{
				pConfig->nVolume = static_cast<unsigned> (n);
			}
		}
		else if (strcmp (pKey, "scaler") == 0)
		{
			if (strcmp (pValue, "sharp") == 0 || strcmp (pValue, "smooth") == 0)
			{
				pConfig->bSharpScale = strcmp (pValue, "sharp") == 0;
			}
			else
			{
				BootConfigWarn (pConfig, "boot.conf: unknown scaler \"%s\""
					" (sharp|smooth)", pValue);
			}
		}
		else if (strcmp (pKey, "mode") == 0)
		{
			if (   strcmp (pValue, "auto") == 0
			    || strcmp (pValue, "performance") == 0
			    || strcmp (pValue, "powersave") == 0)
			{
				strcpy (pConfig->Mode, pValue);
			}
			else
			{
				BootConfigWarn (pConfig, "boot.conf: unknown mode \"%s\""
					" (auto|performance|powersave)", pValue);
			}
		}
		else
		{
			// Not an error, and not silent either: a misspelt key would
			// otherwise look exactly like a setting that does nothing.
			BootConfigWarn (pConfig, "boot.conf: unknown key \"%s\" ignored", pKey);
		}
	}

	fclose (pFile);

	// "mode" is parsed so that the file does not have to change shape when it
	// starts doing something, but honouring it means overriding the governor,
	// and that is deliberately not in this round.
	if (strcmp (pConfig->Mode, "auto") != 0)
	{
		BootConfigWarn (pConfig, "boot.conf: mode=%s parsed but not applied yet", pConfig->Mode);
	}
}


#endif
