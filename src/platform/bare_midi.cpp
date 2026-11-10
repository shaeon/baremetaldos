//
// bare_midi.cpp
//
// BM-DOS - MT-32 (Munt) and General MIDI (FluidSynth) on core 1.
//
// The shape, and why it is this one:
//
//   core 0 (emulator)                       core 1 (this file)
//   -----------------                       ------------------
//   MPU-401 -> MIDI_RawOutByte()
//            -> BareMidiOutByte() --bytes--> parse -> synth.playMsg/SysEx
//                                                     synth.Render()
//   BareAudioQueue() <---------- samples ----------- s_Out
//     mixes them into DOSBox's batch
//
// Nothing of DOSBox runs on core 1.  DOSBox-core is not reentrant and its
// mixer touches globals without locks ("Lessons learned: only one core is
// used here"), so the mixer stays where it is.  What moves is the synthesizer,
// which is the expensive part (an MT-32 or a SoundFont costs more than the
// rest of the audio together) and which has no state shared with DOSBox: it
// sees a byte stream in and a sample stream out, two single-producer /
// single-consumer rings (midi_queue.h).
//
// Everything that allocates or touches the SD card - loading the ROMs or the
// SoundFont, creating the synthesizer - happens on core 0, in MIDI_Init(),
// before the synthesizer is published to core 1.  FatFs and the heap are not
// shared with a second core by this port, and they never have to be.
//
// The idea of running Munt and FluidSynth on bare metal Circle, the pinned
// library revisions and the FluidSynth patch come from mt32-pi by Dale Whinham
// (https://github.com/dwhinham/mt32-pi, GPL-3.0).  The FluidSynth hooks below
// follow its soundfontsynth.cpp, through stdio instead of FatFs directly.
//
// Shaeon (Carlos Palmero) 2026, for BM-DOS by @mmoroca.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
#include "bare_env.h"
#include "midi_queue.h"

#include <circle/logger.h>
#include <circle/timer.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if BARE_MIDI
#define MT32EMU_API_TYPE 0
#include <mt32emu/mt32emu.h>
#include <fluidsynth.h>
#endif

static const char FromMidi[] = "midi";

#include <cstdarg>

// The last thing the MIDI side had to say, for the screen.  CLogger writes to
// Circle's console, which is NOT the frame buffer this port shows, so a MIDI
// failure used to be invisible: a missing SoundFont and a game sending
// nothing looked the same - silence.  Every message goes to the log AND here,
// and midi_stub.cpp puts this line on the boot screen.  Core 0 only.
static char s_MidiStatus[112] = "midi: no device opened";

static void MidiNote (TLogSeverity Severity, const char *pFormat, ...)
{
	va_list Args;
	va_start (Args, pFormat);
	vsnprintf (s_MidiStatus, sizeof s_MidiStatus, pFormat, Args);
	va_end (Args);

	CLogger::Get ()->Write (FromMidi, Severity, "%s", s_MidiStatus);
}

namespace {

// --- sizes ------------------------------------------------------------------
//
// Latency is the fill target of the sample ring: core 1 keeps about this many
// frames rendered ahead of the mixer, and a MIDI event lands in the next block
// it renders.  1024 frames at 44.1 kHz is 23 ms on top of the device queue
// that DOSBox's own audio already goes through - inaudible against the game,
// and enough slack for core 1 to ride out a slow block.
constexpr unsigned BlockFrames  = 128;		// rendered per step on core 1
constexpr unsigned TargetFrames = 1024;		// kept ready for the mixer

baremidi::TSPSCRing<u8, 65536> s_In;		// core 0 -> core 1, MIDI bytes
baremidi::TSPSCRing<s16, 8192> s_Out;		// core 1 -> core 0, stereo s16

// --- the synthesizer interface ------------------------------------------------

class CBareSynth
{
public:
	virtual ~CBareSynth (void) = default;
	virtual void ShortMsg (u32 nMsg) = 0;
	virtual void SysEx (const u8 *pData, std::size_t nSize) = 0;
	virtual void Render (s16 *pFrames, unsigned nFrames) = 0;	// stereo
	virtual const char *Name (void) const = 0;
};

// Published to core 1 with release, read with acquire.  nullptr = no synth.
std::atomic<CBareSynth *> s_pSynth {nullptr};

// The synthesizer core 1 is using right now (a hazard pointer).  Core 1 sets
// it, then checks s_pSynth again; core 0 clears s_pSynth, then waits until
// this no longer names the old synthesizer before deleting it.  Both sides use
// sequentially consistent operations, which is what makes "set, then check"
// on one core and "clear, then wait" on the other impossible to interleave
// into a use after free.  A plain "core 1 is idle" flag is not enough: core 0
// can read the flag before core 1 has had the chance to lower it.
std::atomic<CBareSynth *> s_pInUse {nullptr};

std::atomic<bool> s_bCore1Running {false};

// Bytes the parser sees, kept on core 1 only.
baremidi::TMidiParser s_Parser;

// Counters for the panel and the log.  Written by one side each.
std::atomic<u64> s_nBytesDropped {0};		// core 0: MIDI byte ring full
std::atomic<u64> s_nUnderruns {0};		// core 0: mixer found too few samples
std::atomic<u64> s_nRendered {0};		// core 1: frames rendered so far
std::atomic<unsigned> s_nPeak {0};		// core 0: loudest MIDI sample mixed

unsigned s_nSampleRate = 44100;

// --- core 1 -------------------------------------------------------------------

struct TSynthSink
{
	CBareSynth *pSynth;
	void ShortMsg (u32 nMsg) { pSynth->ShortMsg (nMsg); }
	void SysEx (const u8 *p, std::size_t n) { pSynth->SysEx (p, n); }
};

void Core1Loop (void)
{
	s_bCore1Running.store (true, std::memory_order_release);

	s16 Block[BlockFrames * 2];
	u8 Bytes[256];

	for (;;)
	{
		CBareSynth *pSynth = s_pSynth.load ();
		s_pInUse.store (pSynth);
		if (pSynth == nullptr || s_pSynth.load () != pSynth)
		{
			s_pInUse.store (nullptr);
			// No synthesizer: sleep in WFE until BareMidiOpen() publishes one
			// (it calls BareCoresWake).  A busy delay here would keep core 1
			// at full clock for the whole session of a user without MIDI.
			if (pSynth == nullptr)
			{
				BareCoresWait ();
			}
			continue;
		}

		// Events first, so that a note-on that arrived during the last block
		// is heard in the next one.
		TSynthSink Sink {pSynth};
		std::size_t nBytes;
		while ((nBytes = s_In.Read (Bytes, sizeof Bytes)) != 0)
		{
			for (std::size_t i = 0; i < nBytes; i++)
			{
				s_Parser.Feed (Bytes[i], Sink);
			}
		}

		if (s_Out.Count () / 2 < TargetFrames)
		{
			pSynth->Render (Block, BlockFrames);
			s_Out.Write (Block, BlockFrames * 2);	// cannot overflow: Count < Target < capacity
			s_nRendered.fetch_add (BlockFrames, std::memory_order_relaxed);
		}
		else
		{
			// Ahead of the mixer.  A short sleep, not a spin: core 1 has its
			// own share of the SoC's heat budget, and the governor on core 0
			// reads the same thermometer.
			CTimer::SimpleusDelay (500);
		}

		s_pInUse.store (nullptr);
	}
}


// Wait until core 1 no longer uses pSynth.  Not bounded on purpose: deleting
// a synthesizer that core 1 is rendering is a crash, and core 1 drops the
// pointer within one block (128 frames, under 3 ms) unless it has hung - in
// which case there is no sound to save anyway.
void WaitCore1Released (CBareSynth *pSynth)
{
	while (s_pInUse.load () == pSynth)
	{
		CTimer::SimpleusDelay (100);
	}
}

#if BARE_MIDI

// --- files ---------------------------------------------------------------------

bool ReadFile (const std::string &Path, std::vector<u8> &Data)
{
	FILE *pFile = std::fopen (Path.c_str (), "rb");
	if (pFile == nullptr)
	{
		return false;
	}

	std::fseek (pFile, 0, SEEK_END);
	const long nSize = std::ftell (pFile);
	std::fseek (pFile, 0, SEEK_SET);

	bool bOk = nSize > 0;
	if (bOk)
	{
		Data.resize (static_cast<std::size_t> (nSize));
		bOk = std::fread (Data.data (), 1, Data.size (), pFile) == Data.size ();
	}

	std::fclose (pFile);
	return bOk;
}

// --- Munt -----------------------------------------------------------------------
//
// ROMs in /dosbox/system/, with the file names DOSBox's own MT-32 support
// uses: MT32_CONTROL.ROM + MT32_PCM.ROM, or CM32L_CONTROL.ROM + CM32L_PCM.ROM.
// The order is MT-32 first unless midiconfig says "cm32l".  Munt identifies
// the images by their SHA1, so a wrong file is refused here, at boot, with a
// log line - not as silence in the middle of a game.

class CBareMT32 : public CBareSynth
{
public:
	~CBareMT32 (void) override
	{
		delete m_pConverter;
		m_Synth.close ();
		if (m_pControl != nullptr) MT32Emu::ROMImage::freeROMImage (m_pControl);
		if (m_pPCM != nullptr)     MT32Emu::ROMImage::freeROMImage (m_pPCM);
		delete m_pControlFile;
		delete m_pPCMFile;
	}

	bool Open (const char *pConfig)
	{
		const bool bCM32LFirst = pConfig != nullptr && std::strstr (pConfig, "cm32l") != nullptr;
		const char *Models[2][2] = {
			{"MT32_CONTROL.ROM",  "MT32_PCM.ROM"},
			{"CM32L_CONTROL.ROM", "CM32L_PCM.ROM"},
		};

		for (unsigned i = 0; i < 2; i++)
		{
			const unsigned nModel = bCM32LFirst ? 1 - i : i;
			if (LoadPair (Models[nModel][0], Models[nModel][1]))
			{
				break;
			}
		}

		if (m_pControl == nullptr || m_pPCM == nullptr)
		{
			MidiNote (LogWarning, "mt32: no MT32_CONTROL.ROM + MT32_PCM.ROM (or CM32L_*) usable in %s", SystemDir);
			return false;
		}

		if (!m_Synth.open (*m_pControl, *m_pPCM, MT32Emu::DEFAULT_MAX_PARTIALS,
				   MT32Emu::AnalogOutputMode_COARSE))
		{
			MidiNote (LogWarning, "mt32: synth did not open");
			return false;
		}

		// Munt runs at 32 kHz internally; its own converter brings it to the
		// mixer rate.  FASTEST on a Pi 3, where core 1 is a 1.2 GHz A53.
#if RASPPI <= 3
		const auto Quality = MT32Emu::SamplerateConversionQuality_FASTEST;
#else
		const auto Quality = MT32Emu::SamplerateConversionQuality_GOOD;
#endif
		m_pConverter = new MT32Emu::SampleRateConverter (m_Synth, s_nSampleRate, Quality);

		MidiNote (LogNotice, "mt32: %s ready",
					m_pControl->getROMInfo ()->shortName);
		return true;
	}

	void ShortMsg (u32 nMsg) override  { m_Synth.playMsg (nMsg); }
	void SysEx (const u8 *p, std::size_t n) override { m_Synth.playSysex (p, static_cast<MT32Emu::Bit32u> (n)); }
	void Render (s16 *pFrames, unsigned nFrames) override { m_pConverter->getOutputSamples (pFrames, nFrames); }
	const char *Name (void) const override { return "mt32"; }

	static constexpr const char *SystemDir = "/dosbox/system/";

private:
	bool LoadPair (const char *pControlName, const char *pPCMName)
	{
		std::vector<u8> Control, PCM;
		if (!ReadFile (std::string (SystemDir) + pControlName, Control)
		    || !ReadFile (std::string (SystemDir) + pPCMName, PCM))
		{
			return false;
		}

		// ArrayFile keeps a pointer, so the data must outlive the synth.
		m_ControlData = std::move (Control);
		m_PCMData = std::move (PCM);
		m_pControlFile = new MT32Emu::ArrayFile (m_ControlData.data (), m_ControlData.size ());
		m_pPCMFile = new MT32Emu::ArrayFile (m_PCMData.data (), m_PCMData.size ());
		m_pControl = MT32Emu::ROMImage::makeROMImage (m_pControlFile);
		m_pPCM = MT32Emu::ROMImage::makeROMImage (m_pPCMFile);

		const bool bOk = m_pControl != nullptr && m_pPCM != nullptr
			&& m_pControl->getROMInfo () != nullptr && m_pPCM->getROMInfo () != nullptr
			&& m_pControl->getROMInfo ()->type == MT32Emu::ROMInfo::Control
			&& m_pPCM->getROMInfo ()->type == MT32Emu::ROMInfo::PCM;
		if (!bOk)
		{
			MidiNote (LogWarning, "mt32: %s / %s are not a known ROM pair", pControlName, pPCMName);
			if (m_pControl != nullptr) MT32Emu::ROMImage::freeROMImage (m_pControl);
			if (m_pPCM != nullptr)     MT32Emu::ROMImage::freeROMImage (m_pPCM);
			m_pControl = m_pPCM = nullptr;
			delete m_pControlFile; delete m_pPCMFile;
			m_pControlFile = m_pPCMFile = nullptr;
		}

		return bOk;
	}

	MT32Emu::Synth m_Synth;
	MT32Emu::SampleRateConverter *m_pConverter = nullptr;
	std::vector<u8> m_ControlData, m_PCMData;
	MT32Emu::ArrayFile *m_pControlFile = nullptr, *m_pPCMFile = nullptr;
	const MT32Emu::ROMImage *m_pControl = nullptr, *m_pPCM = nullptr;
};

// --- FluidSynth ------------------------------------------------------------------
//
// midiconfig names the SoundFont (a path on the card); without it,
// /dosbox/system/default.sf2.  The whole SoundFont is loaded here, on core 0:
// FluidSynth reads sample data at load time unless dynamic loading is asked
// for, and it is not.

class CBareSoundFont : public CBareSynth
{
public:
	~CBareSoundFont (void) override
	{
		if (m_pSynth != nullptr)    delete_fluid_synth (m_pSynth);
		if (m_pSettings != nullptr) delete_fluid_settings (m_pSettings);
	}

	bool Open (const char *pConfig)
	{
		std::string Path = (pConfig != nullptr && *pConfig != '\0')
			? std::string (pConfig) : std::string ("/dosbox/system/default.sf2");

		m_pSettings = new_fluid_settings ();
		if (m_pSettings == nullptr)
		{
			return false;
		}

		// Device ID 0x10, the Sound Canvas default, so that GS SysEx from
		// games is recognised (mt32-pi does the same).
		fluid_settings_setint (m_pSettings, "synth.device-id", 0x10);
		fluid_settings_setnum (m_pSettings, "synth.sample-rate", static_cast<double> (s_nSampleRate));
		fluid_settings_setint (m_pSettings, "synth.threadsafe-api", 0);

		// FluidSynth's default gain (0.2) leaves a full chord at about 12 %
		// of full scale (measured on the PC with GeneralUser GS), well under
		// the Sound Blaster it is mixed with; game music sits lower still.
		// 0.6 brings it level with DOSBox's own output; the mix saturates,
		// so a loud passage clips instead of wrapping.
		fluid_settings_setnum (m_pSettings, "synth.gain", 0.6);
#if RASPPI <= 3
		fluid_settings_setint (m_pSettings, "synth.polyphony", 64);
#else
		fluid_settings_setint (m_pSettings, "synth.polyphony", 200);
#endif

		m_pSynth = new_fluid_synth (m_pSettings);
		if (m_pSynth == nullptr)
		{
			return false;
		}

		if (fluid_synth_sfload (m_pSynth, Path.c_str (), 1) == FLUID_FAILED)
		{
			MidiNote (LogWarning, "fluidsynth: cannot load %s", Path.c_str ());
			return false;
		}

		MidiNote (LogNotice, "fluidsynth: %s ready", Path.c_str ());
		return true;
	}

	void ShortMsg (u32 nMsg) override
	{
		const int nStatus = nMsg & 0xFF;
		const int nChannel = nStatus & 0x0F;
		const int nData1 = (nMsg >> 8) & 0x7F;
		const int nData2 = (nMsg >> 16) & 0x7F;

		switch (nStatus & 0xF0)
		{
		case 0x80: fluid_synth_noteoff (m_pSynth, nChannel, nData1);			break;
		case 0x90: fluid_synth_noteon (m_pSynth, nChannel, nData1, nData2);		break;
		case 0xA0: fluid_synth_key_pressure (m_pSynth, nChannel, nData1, nData2);	break;
		case 0xB0: fluid_synth_cc (m_pSynth, nChannel, nData1, nData2);			break;
		case 0xC0: fluid_synth_program_change (m_pSynth, nChannel, nData1);		break;
		case 0xD0: fluid_synth_channel_pressure (m_pSynth, nChannel, nData1);		break;
		case 0xE0: fluid_synth_pitch_bend (m_pSynth, nChannel, (nData2 << 7) | nData1);	break;
		default:
			if (nStatus == 0xFF)		// system reset
			{
				fluid_synth_system_reset (m_pSynth);
			}
			break;
		}
	}

	void SysEx (const u8 *p, std::size_t n) override
	{
		if (n >= 2)		// FluidSynth wants it without F0 and F7
		{
			fluid_synth_sysex (m_pSynth, reinterpret_cast<const char *> (p + 1),
					   static_cast<int> (n - 2), nullptr, nullptr, nullptr, 0);
		}
	}

	void Render (s16 *pFrames, unsigned nFrames) override
	{
		fluid_synth_write_s16 (m_pSynth, static_cast<int> (nFrames), pFrames, 0, 2, pFrames, 1, 2);
	}

	const char *Name (void) const override { return "fluidsynth"; }

private:
	fluid_settings_t *m_pSettings = nullptr;
	fluid_synth_t *m_pSynth = nullptr;
};

#endif // BARE_MIDI

} // namespace

#if BARE_MIDI

// --- hooks the patched FluidSynth leaves to the platform ------------------------
//
// patches/fluidsynth/fluidsynth-2.3.1-circle.patch removes FluidSynth's own
// memory, time and file layer (it is built on glib), and these are the symbols
// it expects in exchange.  mt32-pi defines the same set in soundfontsynth.cpp;
// these go through newlib's stdio, which is FatFs underneath in this port.

extern "C"
{
	void *fluid_alloc (size_t nSize)		{ return std::malloc (nSize); }
	void *fluid_realloc (void *p, size_t nSize)	{ return std::realloc (p, nSize); }
	void fluid_free (void *p)			{ std::free (p); }

	FILE *fluid_file_open (const char *pPath, const char **ppErrMsg)
	{
		FILE *pFile = std::fopen (pPath, "rb");
		if (pFile == nullptr && ppErrMsg != nullptr)
		{
			*ppErrMsg = "Failed to open file";
		}
		return pFile;
	}

	void fluid_msleep (unsigned int nMsecs)	{ CTimer::SimpleMsDelay (nMsecs); }
	double fluid_utime (void)		{ return static_cast<double> (CTimer::GetClockTicks ()); }

	void *default_fopen (const char *pPath)	{ return std::fopen (pPath, "rb"); }
	int default_fclose (void *pHandle)	{ return std::fclose (static_cast<FILE *> (pHandle)) == 0 ? FLUID_OK : FLUID_FAILED; }
	fluid_long_long_t default_ftell (void *pHandle) { return std::ftell (static_cast<FILE *> (pHandle)); }

	int safe_fread (void *pBuf, fluid_long_long_t nCount, void *pHandle)
	{
		return std::fread (pBuf, 1, static_cast<size_t> (nCount), static_cast<FILE *> (pHandle))
			== static_cast<size_t> (nCount) ? FLUID_OK : FLUID_FAILED;
	}

	int safe_fseek (void *pHandle, fluid_long_long_t nOffset, int nWhence)
	{
		return std::fseek (static_cast<FILE *> (pHandle), static_cast<long> (nOffset), nWhence) == 0
			? FLUID_OK : FLUID_FAILED;
	}
}

#endif // BARE_MIDI

// --- public interface (bare_env.h) -------------------------------------------------

boolean BareMidiInit (unsigned nSampleRate)
{
	s_nSampleRate = nSampleRate;

#if BARE_MIDI
	if (s_bCore1Running.load (std::memory_order_acquire))
	{
		return TRUE;
	}

	if (!BareCoresRun (1, Core1Loop))
	{
		MidiNote (LogWarning, "core 1 not available: MIDI off");
		return FALSE;
	}

	return s_bCore1Running.load (std::memory_order_acquire) ? TRUE : FALSE;
#else
	return FALSE;	// built without third_party/midi-rpi$(RASPPI)
#endif
}

// What is open now, so that MIDI_Init() called again with the same [midi]
// values does not reload the ROMs or the SoundFont.  dosbox-core re-runs a
// section's init whenever its libretro core options touch a variable of that
// section (update_dosbox_variable: ExecuteDestroy + ExecuteInit), and a
// SoundFont can be tens of megabytes off the card.
static std::string s_OpenDevice;
static std::string s_OpenConfig;

boolean BareMidiOpen (const char *pDevice, const char *pConfig)
{
	if (pDevice != nullptr && BareMidiActive ()
	    && s_OpenDevice == pDevice && s_OpenConfig == (pConfig != nullptr ? pConfig : ""))
	{
		return TRUE;
	}

	BareMidiClose ();

#if BARE_MIDI
	if (pDevice == nullptr || (std::strcmp (pDevice, "mt32") != 0
				   && std::strcmp (pDevice, "fluidsynth") != 0))
	{
		MidiNote (LogNotice, "midi: mididevice=%s - no synthesizer (mt32 | fluidsynth)",
			  pDevice != nullptr ? pDevice : "?");
		return FALSE;
	}

	if (!s_bCore1Running.load (std::memory_order_acquire))
	{
		MidiNote (LogWarning, "midi: %s asked for, but core 1 is not running", pDevice);
		return FALSE;
	}

	CBareSynth *pSynth = nullptr;
	if (std::strcmp (pDevice, "mt32") == 0)
	{
		auto *pMT32 = new CBareMT32;
		if (pMT32->Open (pConfig)) pSynth = pMT32; else delete pMT32;
	}
	else if (std::strcmp (pDevice, "fluidsynth") == 0)
	{
		auto *pFluid = new CBareSoundFont;
		if (pFluid->Open (pConfig)) pSynth = pFluid; else delete pFluid;
	}

	if (pSynth == nullptr)
	{
		return FALSE;
	}

	// Core 1 holds no synthesizer (BareMidiClose waited for it, or there was
	// none), and it touches the rings and the parser only while it holds one,
	// so all three can be reset from here without racing it.
	s_In.Reset ();
	s_Out.Reset ();
	s_Parser.Reset ();
	s_OpenDevice = pDevice;
	s_OpenConfig = pConfig != nullptr ? pConfig : "";
	s_pSynth.store (pSynth);
	BareCoresWake ();
	return TRUE;
#else
	(void) pConfig;
	MidiNote (LogWarning, "midi: %s asked for, but this kernel was built without MIDI",
		  pDevice != nullptr ? pDevice : "?");
	return FALSE;
#endif
}

void BareMidiClose (void)
{
	CBareSynth *pSynth = s_pSynth.exchange (nullptr);
	if (pSynth == nullptr)
	{
		return;
	}

	WaitCore1Released (pSynth);
	delete pSynth;
	s_OpenDevice.clear ();
	s_OpenConfig.clear ();
}

boolean BareMidiActive (void)
{
	return s_pSynth.load (std::memory_order_acquire) != nullptr ? TRUE : FALSE;
}

const char *BareMidiName (void)
{
	CBareSynth *pSynth = s_pSynth.load (std::memory_order_acquire);
	return pSynth != nullptr ? pSynth->Name () : "none";
}

// Every byte the MPU-401 sends, counted whether or not a synthesizer takes
// it: the panel shows the rate, which separates "the game sends nothing" (a
// setup problem in the game, or the MPU not seen) from "bytes arrive and
// nothing sounds" (a problem on this side).  Core 0 only.
static u64 s_nBytesIn = 0;

void BareMidiOutByte (u8 nByte)
{
	s_nBytesIn++;

	if (s_pSynth.load (std::memory_order_relaxed) == nullptr)
	{
		return;
	}

	if (s_In.Write (&nByte, 1) == 0)
	{
		s_nBytesDropped.fetch_add (1, std::memory_order_relaxed);
	}
}

void BareMidiMix (s16 *pFrames, unsigned nFrames)
{
	if (s_pSynth.load (std::memory_order_relaxed) == nullptr)
	{
		return;
	}

	s16 Chunk[512 * 2];
	while (nFrames != 0)
	{
		const unsigned nWant = nFrames < 512 ? nFrames : 512;
		const unsigned nGot = static_cast<unsigned> (s_Out.Read (Chunk, nWant * 2)) / 2;

		unsigned nPeak = s_nPeak.load (std::memory_order_relaxed);
		for (unsigned i = 0; i < nGot * 2; i++)
		{
			const unsigned nAbs = Chunk[i] < 0 ? -Chunk[i] : Chunk[i];
			if (nAbs > nPeak)
			{
				nPeak = nAbs;
			}

			int nSum = pFrames[i] + Chunk[i];
			if (nSum > 32767)  nSum = 32767;
			if (nSum < -32768) nSum = -32768;
			pFrames[i] = static_cast<s16> (nSum);
		}

		s_nPeak.store (nPeak, std::memory_order_relaxed);

		if (nGot < nWant)
		{
			// Core 1 fell behind (or is still filling at start-up).  The rest
			// of this batch goes out with DOSBox's audio only.
			s_nUnderruns.fetch_add (1, std::memory_order_relaxed);
			return;
		}

		pFrames += nGot * 2;
		nFrames -= nGot;
	}
}

void BareMidiStats (u64 *pUnderruns, u64 *pBytesDropped)
{
	if (pUnderruns != nullptr)    *pUnderruns = s_nUnderruns.load (std::memory_order_relaxed);
	if (pBytesDropped != nullptr) *pBytesDropped = s_nBytesDropped.load (std::memory_order_relaxed);
}

const char *BareMidiStatus (void)
{
	return s_MidiStatus;
}

u64 BareMidiBytesIn (void)
{
	return s_nBytesIn;
}

void BareMidiLevels (u64 *pRendered, unsigned *pPeak)
{
	if (pRendered != nullptr) *pRendered = s_nRendered.load (std::memory_order_relaxed);
	if (pPeak != nullptr)     *pPeak = s_nPeak.exchange (0, std::memory_order_relaxed);
}
