//
// bare_audio.cpp
//
// by @mmoroca & arena.ai 2026  -  https://arena.ai
//
// BM-DOS - audio backend.
//
// DOSBox-core mixes its own audio (SB, Adlib, PC speaker, and optionally
// MT-32) and hands us finished 16-bit signed stereo blocks through the
// libretro audio batch callback.  We push those into a Circle sound device.
//
// Two properties matter:
//
//  * A batch larger than the free queue space is split, and when there is no
//    space at all the rest of it is DROPPED.  It used to block instead - that
//    is how an audio device paces a program on a desktop with a preemptive
//    OS, and it was the first thing to suspect when the emulator froze with
//    the picture already on screen.  But this port runs everything on Circle's
//    *cooperative* scheduler: a task that blocks in a delay loop keeps the CPU
//    for ever unless it yields, and the emulator task is exactly the task that
//    would have to run to drain the queue.  A single empty slot was enough to
//    stop the machine for good.  Audio is worth less than a running emulator,
//    so the queue gets what it has room for and the rest is counted and
//    discarded.  If that ever turns into audible dropouts on real hardware,
//    the fix is a rate match (the mixer should run at the device rate) or a
//    bigger queue, not a blocking wait.
//
//  * The hardware runs at one rate.  DOSBox's mixer is configured to that
//    same rate, so no resampling happens in this file: what the core mixes
//    at 44.1 or 48 kHz goes out untouched.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
#include "bare_env.h"
#include "audio_dcblock.h"

#include <circle/interrupt.h>
#include <circle/logger.h>
#include <circle/sound/hdmisoundbasedevice.h>
#include <circle/sound/i2ssoundbasedevice.h>
#include <circle/sound/pwmsoundbasedevice.h>
#include <circle/sound/soundbasedevice.h>
#include <circle/util.h>

static const char FromAudio[] = "audio";

namespace {

CSoundBaseDevice *s_pSound = nullptr;

// Which device won, by the name given in the list below.  Reported in the boot
// log and printed in the status panel: "there is no sound" has three very
// different causes (no device started, HDMI with a silent display, jack with
// nothing plugged in) and this is the number that tells them apart.
const char *s_pDeviceName = "none";

// Audio traffic, kept so the frame loop can *prove* the queue drains instead of
// guessing.  s_nDroppedFrames is the interesting one: non-zero means the device
// is not keeping up, which is audible but harmless now that queueing never
// blocks - and it is also the number that would justify a bigger queue or a
// mixer rate that matches the device.
u64 s_nQueuedFrames = 0;
u64 s_nDroppedFrames = 0;
u64 s_nLastDrainMicros = 0;
// Set before BareAudioInit (boot.conf audio_out): try the 3.5 mm jack first.
bool s_bPreferJack = false;

// Whole-output volume, 0..256 for 0..100 % (boot.conf volume, the menu).
unsigned s_nVolume256 = 256;

u64 s_nGaps = 0;			// writes that found the device queue empty
unsigned s_nSampleRate = 44100;	// set by BareAudioInit

// How much audio we let Circle queue ahead, in milliseconds.  Too small and
// a long emulated frame underruns (clicks); too large and the sound lags the
// picture.  120 ms is a compromise that survives the slow interpreter core.
constexpr unsigned QueueMsecs = 120;

// HDMI needs chunk sizes that are a multiple of 384 words (stereo samples).
constexpr unsigned ChunkSize = 384 * 8;

// Take the first device that can actually be started, not the first that can
// be constructed.  An HDMI device on a display with no audio path constructs
// happily and then never leaves silence, so the test has to be Start().
void TryDevice (CSoundBaseDevice *pDevice, const char *pName)
{
	if (pDevice == nullptr)
	{
		return;
	}

	if (s_pSound != nullptr)
	{
		delete pDevice;
		return;
	}

	if (!pDevice->AllocateQueue (QueueMsecs))
	{
		CLogger::Get ()->Write (FromAudio, LogWarning,
					"%s: cannot allocate queue", pName);
		delete pDevice;
		return;
	}

	pDevice->SetWriteFormat (SoundFormatSigned16, 2);

	if (!pDevice->Start ())
	{
		CLogger::Get ()->Write (FromAudio, LogWarning,
					"%s: cannot start", pName);
		pDevice->Cancel ();
		delete pDevice;
		return;
	}

	s_pSound = pDevice;
	s_pDeviceName = pName;

	CLogger::Get ()->Write (FromAudio, LogNotice, "Using %s", pName);
}

} // namespace

boolean BareAudioInit (unsigned nSampleRate, CInterruptSystem *pInterrupt)
{
	s_pSound = nullptr;
	s_nSampleRate = nSampleRate;

	// Device priority, and it is a decision rather than a default: HDMI first
	// (it is what the board is plugged into nine times out of ten), then the
	// 3.5 mm jack through PWM, then I2S.  PWM before I2S because PWM is what
	// the jack physically is on a Pi 4 and because it needs no configuration;
	// I2S is only useful with a HAT or an external DAC, so it goes last.
	//
	// Choosing by "which one constructs without throwing" has a hole in it, and
	// it is worth knowing exactly how big: on a Pi 4, the PWM device constructs
	// fine whether or not anything is plugged into the jack, and the HDMI
	// device constructs fine whether or not the display has speakers - but
	// its Start() is honest.  Circle's CHDMISoundBaseDevice::Start() checks the
	// RAM packet config register and fails (with the log line "Requires HDMI
	// display with audio support") when the display does not announce audio.
	// So the list is tried in order and the *first device that actually
	// starts* wins: a constructor is not a device, a Start() is.
	//
	// What this cannot detect: an HDMI display that announces audio and has no
	// speakers (quite common on monitors - they pass the audio through to
	// nothing).  If that is your setup, sound is being sent and not heard.
	// Nothing can be queried to choose automatically (a display can announce
	// audio it cannot play, and a jack can be empty), so it is a setting:
	// boot.conf audio_out = jack, or the setup menu's Sonido page.
#if BARE_AUDIO_DEVICE == 1
	// make AUDIO_DEVICE=pwm still forces the jack first, as it always did.
	s_bPreferJack = true;		// the build-time switch still forces it
#endif

	// boot.conf audio_out = jack: the jack first, HDMI as the fallback;
	// otherwise HDMI first.  I2S last either way.
	if (s_bPreferJack)
	{
		TryDevice (new CPWMSoundBaseDevice (pInterrupt, nSampleRate, ChunkSize),
			   "PWM (jack)");
		TryDevice (new CHDMISoundBaseDevice (pInterrupt, nSampleRate, ChunkSize),
			   "HDMI");
	}
	else
	{
		TryDevice (new CHDMISoundBaseDevice (pInterrupt, nSampleRate, ChunkSize),
			   "HDMI");
		TryDevice (new CPWMSoundBaseDevice (pInterrupt, nSampleRate, ChunkSize),
			   "PWM (jack)");
	}
	TryDevice (new CI2SSoundBaseDevice (pInterrupt, nSampleRate, ChunkSize, FALSE), "I2S");

	if (s_pSound == nullptr)
	{
		CLogger::Get ()->Write (FromAudio, LogWarning,
					"No sound device could be started, running muted");
		return TRUE;		// audio is not required to boot DOS
	}

	CLogger::Get ()->Write (FromAudio, LogNotice,
				"%u Hz, %u ms queue, %u frames per chunk",
				nSampleRate, QueueMsecs, s_pSound->GetQueueSizeFrames ());

	return TRUE;
}

void BareAudioDeinit (void)
{
	if (s_pSound != nullptr)
	{
		s_pSound->Cancel ();
		delete s_pSound;
		s_pSound = nullptr;
	}
}

void BareAudioQueue (const s16 *pFrames, unsigned nFrames)
{
	// The profiler's "Audio y MIDI": this function's time on core 0, the MIDI
	// mix included.
	struct TProfileScope
	{
		const u64 nStart = BareProfileClock ();
		~TProfileScope () { BareProfileAdd (BareProfileAudio, BareProfileClock () - nStart); }
	} ProfileScope;

	if (s_pSound == nullptr || nFrames == 0)
	{
		// Muted build.  Nothing to wait for and nothing to pace: the frame
		// loop is responsible for running the emulator at real time, and a
		// delay here made the emulator run at whatever fraction of speed the
		// sound card would have imposed.
		s_nDroppedFrames += nFrames;
		return;
	}

	// Frames, not bytes, and in the unit the *device* counts in.  This used to
	// divide both counters by the size of an input frame (2 channels x s16),
	// which is wrong for a device whose hardware format differs from the write
	// format - HDMI takes 32-bit words, so its frame is 8 bytes and every
	// "frame" counted here was half a frame in reality.  GetQueueSizeFrames()
	// and GetQueueFramesAvail() both speak hardware frames, so these counters
	// are in the same unit as the two numbers shown next to them.
	constexpr unsigned WriteFrameBytes = 2 * sizeof (s16);	// 16-bit stereo in

	// A gap: the device's queue ran dry since the previous write, so it played
	// silence for a while.  (GetQueueFramesAvail() is the number of frames IN
	// the queue - see the correction noted below.)  That is the emulator
	// behind real time, and each gap is heard as a click, or, with a held
	// level in the mix, as a buzz at the frame rate (audio_dcblock.h).
	if (s_pSound->GetQueueFramesAvail () < 16)
	{
		s_nGaps++;
	}

	// The batch belongs to DOSBox, so everything below works on a copy, in
	// chunks of the copy's size: MIDI from core 1 is added, then the DC
	// blocker - the AC coupling of a real sound card's line output, which is
	// what keeps a PC speaker left on, or a DAC parked at an extreme, from
	// reaching the HDMI as a constant level.
	static s16 s_Mixed[2048 * 2];
	static s16 s_Midi[2048 * 2];
	static bareaudio::CDCBlocker s_DCBlock (10.0f, static_cast<float> (s_nSampleRate));

	while (nFrames != 0)
	{
		const unsigned nChunk = nFrames < 2048 ? nFrames : 2048;

		// The MIDI from core 1 arrives in a buffer of its own (added onto
		// silence, so it is a copy), and the sum with DOSBox's output is
		// made in float: no clipping until the very end, where the soft
		// limiter rounds the overshoot off instead of cutting it flat.
		const bool bMidi = BareMidiActive () != FALSE;
		if (bMidi)
		{
			memset (s_Midi, 0, nChunk * WriteFrameBytes);
			BareMidiMix (s_Midi, nChunk);
		}

		for (unsigned i = 0; i < nChunk * 2; i++)
		{
			float x = pFrames[i];
			if (bMidi)
			{
				x += s_Midi[i];
			}
			x = bareaudio::SoftLimit (s_DCBlock.Step (i & 1, x));
			if (s_nVolume256 != 256)
			{
				x = x * static_cast<float> (s_nVolume256) * (1.0f / 256.0f);
			}
			s_Mixed[i] = static_cast<s16> (x);
		}

		size_t nBytes = static_cast<size_t> (nChunk) * WriteFrameBytes;

		// No guessing about free space: Write() is the authority.  It takes the
		// device's spin lock, converts format if it has to, and copies as many
		// whole frames as fit, returning the number of *write-format* bytes it
		// accepted.  Whatever it does not take is dropped and counted.
		//
		// This is a correction of a real bug.  The previous version asked
		// GetQueueFramesAvail() how much room was left, and that function returns
		// the frames *currently in* the queue - the opposite.  So the queue always
		// looked full, every frame was discarded, Write() was never called with a
		// single sample, and the panel showed "drop" climbing for ever while
		// nothing came out of either the HDMI or the jack.  There was no sound
		// because there was no data, not because of the device.
		const int nResult = s_pSound->Write (s_Mixed, nBytes);
		if (nResult > 0)
		{
			nBytes -= static_cast<size_t> (nResult);
			s_nQueuedFrames += static_cast<u64> (nResult) / WriteFrameBytes;
			s_nLastDrainMicros = BareMicros ();
		}

		if (nBytes != 0)
		{
			// Counted in *samples this call could not hand over*, not in frames.
			//
			// .58: the number in the panel was 2 285 317 after a few minutes of a
			// pinball table - more drops than audio frames that exist in that time
			// - because the old code divided the refused bytes by the size of one
			// 16-bit stereo frame (4) and then the panel called the result "drop".
			// Two errors stacked: the unit was frames-not-samples, and a
			// half-filled queue produced a drop figure that looked like failure
			// when it was the normal steady state (see below).
			//
			// What it is now: one unit per *sample that was discarded*.  Small
			// fractions during normal operation, which is the truth: a queue that
			// runs near full discards a few samples per call, and the sound is
			// fine.
			s_nDroppedFrames += static_cast<u64> (nBytes) / sizeof (s16);
		}

		pFrames += nChunk * 2;
		nFrames -= nChunk;
	}

	// And a note on why it is never zero, which is the question worth
	// ("siguen apareciendo drops constantes al iniciar y sin hacer nada, es
	// normal?").
	//
	// It IS normal, and it is structural rather than a bug: Circle's device
	// asks for data in chunks (384 x 8 stereo frames for HDMI) against a queue
	// of 120 ms, while DOSBox hands us whatever its mixer produced for one
	// emulated frame.  The two sizes are unrelated, so the queue hovers near
	// full and the tail of almost every batch does not fit.  Dropping the tail
	// is the design decision taken back in .43 (blocking here froze the machine
	// on a cooperative scheduler), and it costs a few samples of a buffer that
	// is 120 ms long - inaudible.
	//
	// The number to watch is not "drop" but "free": if the free column
	// oscillates, the device is consuming.  A free count stuck at 0 with drop
	// climbing fast is the failure.  The panel says both.
}

void BareAudioDrain (void)
{
	// Nothing to do any more.  This used to wait (bounded, but still waiting)
	// for the queue to empty at shutdown.  Since queueing never blocks, the
	// only thing a wait here can achieve is delaying the shutdown; the device
	// keeps playing what it already has while it is being deleted.
}

unsigned BareAudioQueueFrames (unsigned *pQueued, unsigned *pSize)
{
	if (s_pSound == nullptr)
	{
		return 0;
	}

	if (pQueued != nullptr)
	{
		*pQueued = s_pSound->GetQueueFramesAvail ();	// waiting to be sent
	}

	if (pSize != nullptr)
	{
		*pSize = s_pSound->GetQueueSizeFrames ();
	}

	return 1;
}

const char *BareAudioDeviceName (void)
{
	return s_pDeviceName;
}

u64 BareAudioDroppedSamples (void)
{
	return s_nDroppedFrames;
}

void BareAudioStats (u64 *pQueuedFrames, u64 *pDroppedFrames, u64 *pMicrosSinceWrite)
{
	if (pQueuedFrames != nullptr)
	{
		*pQueuedFrames = s_nQueuedFrames;
	}

	if (pDroppedFrames != nullptr)
	{
		*pDroppedFrames = s_nDroppedFrames;
	}

	if (pMicrosSinceWrite != nullptr)
	{
		*pMicrosSinceWrite = s_nLastDrainMicros == 0
			? 0 : BareMicros () - s_nLastDrainMicros;
	}
}

u64 BareAudioGaps (void)
{
	return s_nGaps;
}

void BareAudioPreferJack (boolean bJack)
{
	s_bPreferJack = bJack != FALSE;
}

void BareAudioSetVolume (unsigned nPercent)
{
	s_nVolume256 = (nPercent > 100 ? 100 : nPercent) * 256 / 100;
}

unsigned BareAudioSampleRate (void)
{
	return s_pSound != nullptr ? s_nSampleRate : 0;
}
