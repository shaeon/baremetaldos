//
// midi_stub.cpp
//
// by @mmoroca & arena.ai 2026  -  https://arena.ai
//
// BM-DOS - the MIDI side of DOSBox.  Originally wired to nothing; now to
// the synthesizers on core 1 when the build has them (see the end of the file).
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
// Why this file exists
// --------------------
// The first link of this port failed with
//
//     undefined reference to MIDI_Init(Section*)
//     undefined reference to MIDI_RawOutByte(unsigned char)
//     undefined reference to MIDI_Available()
//     undefined reference to midi
//
// and it was tempting to answer "we did not build midi.cpp, therefore MIDI is
// off".  That is not how DOSBox is structured: src/dosbox.cpp calls MIDI_Init()
// from DOSBOX_Init() unconditionally, and MPU401 and SBLASTER call
// MIDI_RawOutByte() and MIDI_Available() from their constructors and command
// handlers.  A missing definition is a link error, not a disabled feature.
//
// src/gui/midi.cpp is the real implementation, and it is 300 lines of device
// negotiation: OSS on Linux, CoreMIDI on macOS, ALSA, MT-32 through libmt32emu,
// and the libretro MIDI interface.  On a bare metal board there is no device to
// negotiate with, so this file provides the same four entry points with the
// behaviour DOSBox would reach in its "none" handler:
//
//   MIDI_Init()        claims the [midi] section, logs that MIDI is off, and
//                      registers nothing.  DOS games that probe for an MPU-401
//                      get "not available", which is the truth.
//   MIDI_Available()   false, always.
//   MIDI_RawOutByte()  drops the byte.  It still has to exist: MPU401 calls it
//                      from its track-update path even when no device opened.
//   midi               the global state struct declared in midi.h.  Zero
//                      initialised, which is exactly what "no handler" means:
//                      midi.handler == nullptr and midi.available == false.
//
// What this DOES affect: General MIDI / MT-32 soundtracks play as silence.  OPL
// (AdLib, Sound Blaster FM) is a completely separate path - adlib.cpp, dbopl.cpp
// and nukedopl.c - and is not touched by any of this.
//
// That was the first version of this file, and it still is the behaviour when
// [midi] asks for no device or the build has no MIDI libraries.  The software
// MT-32 / General MIDI renderer this paragraph used to anticipate is now in
// bare_midi.cpp; the entry points below hand it the bytes.  midi.cpp is still
// not built: its device negotiation has nothing to negotiate with here.
//
#include "dosbox.h"
#include "midi.h"
#include "setup.h"

#include "bare_env.h"

#include <circle/logger.h>

#include <cstdio>
#include <string>

DB_Midi midi;

// ---------------------------------------------------------------------------
// MIDI to core 1 (bare_midi.cpp)
// ---------------------------------------------------------------------------
//
// The three entry points DOSBox calls are now wired to a synthesizer when
// [midi] asks for one and the build has the libraries:
//
//   mididevice = mt32         Munt, ROMs from /dosbox/system/
//                             (midiconfig = cm32l to prefer the CM-32L pair)
//   mididevice = fluidsynth   FluidSynth, midiconfig = path of the .sf2
//                             (default /dosbox/system/default.sf2)
//   anything else             as before: no MIDI device
//
// The byte stream is NOT parsed here.  DOSBox's own midi.cpp parses it on the
// emulator's thread and calls the device per message; this port moves the
// whole device to core 1, so the parser (midi_queue.h) goes with it and core 0
// only appends bytes to a ring.  That keeps the cost on core 0 at one store per
// byte, whatever the device is.
//
// What is NOT reproduced from midi.cpp: the SysEx delays for an MT-32 rev. 0
// ("delaysysex").  They exist for real hardware with a slow MIDI input; Munt
// takes SysEx at any speed.

void MIDI_RawOutByte (Bit8u data)
{
	BareMidiOutByte (data);
}

bool MIDI_Available (void)
{
	return BareMidiActive () != FALSE;
}

void MIDI_Init (Section *pSection)
{
	auto *pProps = static_cast<Section_prop *> (pSection);
	const char *pDevice = pProps->Get_string ("mididevice");
	const char *pConfig = pProps->Get_string ("midiconfig");

	midi.available = BareMidiOpen (pDevice, pConfig) != FALSE;

	// On the boot screen, and only when something changed.  dosbox-core runs
	// this init again every time one of its core options touches the [midi]
	// section (a dozen times while it starts), and the sequence of values is
	// exactly what has to be seen if MIDI does not work: a line saying
	// mididevice=fluidsynth followed by one saying mididevice=none would mean
	// the core rewrote dosbox.conf's value after reading it.
	static std::string s_LastLine;
	char Line[160];
	snprintf (Line, sizeof Line, "midi: mididevice=%s midiconfig=%s -> %s",
		  pDevice != nullptr ? pDevice : "?",
		  pConfig != nullptr && *pConfig != '\0' ? pConfig : "(none)",
		  midi.available ? "ON" : "off");
	if (s_LastLine != Line)
	{
		s_LastLine = Line;
		CLogger::Get ()->Write ("midi", LogNotice, "%s", Line);
		BareVideoStatusPrint (Line);
		BareVideoStatusPrint (BareMidiStatus ());
	}
}
