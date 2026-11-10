//
// dcblock_test.cpp - host test for src/platform/audio_dcblock.h (make check-audio)
//
// Shaeon (Carlos Palmero) 2026, for BM-DOS by @mmoroca.  GPL-3.0-or-later.
//
#include "../../src/platform/audio_dcblock.h"

#include <cmath>
#include <cstdio>
#include <vector>

static unsigned s_nFailures = 0;
static void Check (bool b, const char *p)
{
	std::printf ("  %s  %s\n", b ? "ok    " : "FAILED", p);
	if (!b) s_nFailures++;
}

static double Rms (const std::vector<std::int16_t> &v, size_t nFrom, size_t nTo)
{
	double s = 0;
	for (size_t i = nFrom; i < nTo; i++) s += double (v[i]) * v[i];
	return std::sqrt (s / double (nTo - nFrom));
}

int main (void)
{
	const unsigned SR = 44100;

	// What the board produced in DOOM's SETUP: a level of -0.8 of full scale
	// held by the emulator, and the audio device playing zeros for 5 ms of
	// every 33 (the emulator behind real time).  The gaps are inserted AFTER
	// this port's code - the device fills them - so the filter sees only the
	// held level, and the gaps are added to its output.
	{
		const unsigned nFrames = SR * 2;
		std::vector<std::int16_t> Raw (nFrames * 2, -26214), Out = Raw;
		bareaudio::CDCBlocker Block;
		Block.Process (Out.data (), nFrames);
		for (unsigned i = 0; i < nFrames; i++)
		{
			if ((i * 1000 / SR) % 33 >= 28)
			{
				Raw[2 * i] = Raw[2 * i + 1] = 0;	// the gap, unfiltered
				Out[2 * i] = Out[2 * i + 1] = 0;	// the gap, filtered
			}
		}
		// Measure the second second: what is heard once the level settled.
		const double fBefore = Rms (Raw, nFrames, nFrames * 2);
		const double fAfter = Rms (Out, nFrames, nFrames * 2);
		std::printf ("         buzz rms %.0f -> %.0f\n", fBefore, fAfter);
		Check (fAfter < fBefore / 100, "the frame-rate buzz from a held level is gone (-40 dB)");
	}

	// A held level alone decays to nothing.
	{
		std::vector<std::int16_t> v (SR * 2, 0);
		for (auto &s : v) s = -26214;
		bareaudio::CDCBlocker Block;
		Block.Process (v.data (), SR);
		Check (std::abs (v[2 * (SR - 1)]) < 50, "a constant level decays to silence within a second");
	}

	// Music passes: a 440 Hz tone keeps its level.
	{
		std::vector<std::int16_t> v (SR * 2);
		for (unsigned i = 0; i < SR; i++)
			v[2 * i] = v[2 * i + 1] = std::int16_t (10000 * std::sin (2 * 3.14159265 * 440 * i / SR));
		const double fBefore = Rms (v, SR, SR * 2);
		bareaudio::CDCBlocker Block;
		Block.Process (v.data (), SR);
		const double fAfter = Rms (v, SR, SR * 2);
		Check (std::fabs (fAfter / fBefore - 1.0) < 0.01, "a 440 Hz tone passes within 1 %");
	}

	// So does a 50 Hz bass note, within 5 %.
	{
		std::vector<std::int16_t> v (SR * 2);
		for (unsigned i = 0; i < SR; i++)
			v[2 * i] = v[2 * i + 1] = std::int16_t (10000 * std::sin (2 * 3.14159265 * 50 * i / SR));
		const double fBefore = Rms (v, SR, SR * 2);
		bareaudio::CDCBlocker Block;
		Block.Process (v.data (), SR);
		const double fAfter = Rms (v, SR, SR * 2);
		Check (std::fabs (fAfter / fBefore - 1.0) < 0.05, "a 50 Hz bass note passes within 5 %");
	}

	// The soft limiter: transparent below the knee, never past full scale,
	// monotonic, and continuous at the knee (no step where it starts).
	{
		using bareaudio::SoftLimit;
		Check (SoftLimit (10000.0f) == 10000.0f && SoftLimit (-20000.0f) == -20000.0f,
		       "soft limiter: below 75 % of full scale nothing changes");
		Check (SoftLimit (1e9f) <= 32767.0f && SoftLimit (-1e9f) >= -32767.0f,
		       "soft limiter: never past full scale, however large the sum");
		bool bMonotonic = true;
		for (float x = 0.0f; x < 100000.0f; x += 7.0f)
			bMonotonic = bMonotonic && SoftLimit (x + 7.0f) >= SoftLimit (x);
		Check (bMonotonic, "soft limiter: monotonic (a louder input is never quieter)");
		const float fKnee = 0.75f * 32767.0f;
		Check (std::fabs (SoftLimit (fKnee + 1.0f) - (fKnee + 1.0f)) < 0.01f,
		       "soft limiter: continuous with slope 1 at the knee");
		// MIDI at 0.8 of full scale on top of a Sound Blaster at 0.6: a hard
		// clip flattens 0.4 of full scale; the limiter keeps it below full
		// scale and still rising.
		Check (SoftLimit (1.4f * 32767.0f) < 32767.0f
		       && SoftLimit (1.4f * 32767.0f) > SoftLimit (1.2f * 32767.0f),
		       "soft limiter: an overshoot of 40 % is rounded, not cut flat");
	}

	std::printf ("\n%u failures\n", s_nFailures);
	return s_nFailures == 0 ? 0 : 1;
}
