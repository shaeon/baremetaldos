//
// pace_test.cpp - host test for src/platform/pace_policy.h (make check-pace)
// Shaeon (Carlos Palmero) 2026, for BM-DOS by @mmoroca.  GPL-3.0-or-later.
//
#include "../../src/platform/pace_policy.h"

#include <cstdio>

using namespace barepace;

static unsigned s_nFailures = 0;
static void Check (bool b, const char *p)
{
	std::printf ("  %s  %s\n", b ? "ok    " : "FAILED", p);
	if (!b) s_nFailures++;
}

int main (void)
{
	const std::uint64_t T = 14286;		// a 70 Hz frame

	std::printf ("metronome\n");
	{
		// The case seen on the board (12-nov): each boundary takes 12.8 ms of work, and
		// two of them come per call of the frame loop.  Before: nobody waited
		// on the second one, 77 a second.  Now: every one waits for its time.
		TPacer P;
		std::uint64_t nNow = 1000000, nWaited = 0;
		unsigned nFrames = 0;
		while (nNow < 1000000 + 10000000)		// 10 s
		{
			nNow += 12800;				// the work
			const std::uint64_t w = Wait (P, nNow, T);
			nNow += w; nWaited += w;
			nFrames++;
		}
		Check (nFrames >= 699 && nFrames <= 701, "work under the budget: 70 frames a second, not 77");
		Check (nWaited > 900000, "and the spare time is spent waiting (about 10 %)");
	}
	{
		// A machine that is slower than real time: no waits, never paid back
		// as a burst once it speeds up.
		TPacer P;
		std::uint64_t nNow = 0;
		for (int i = 0; i < 100; i++) { nNow += 20000; Wait (P, nNow, T); }
		std::uint64_t nBurst = 0;
		for (int i = 0; i < 10; i++) { nNow += 1000; const std::uint64_t w = Wait (P, nNow, T); nNow += w; if (w == 0) nBurst++; }
		Check (nBurst <= 2, "after a slow stretch, at most a frame or so of catch-up, not the whole backlog");
	}
	{
		// Waits that overshoot a little (a busy-wait ends late) do not drift:
		// the next due time comes from the schedule, not from the late "now".
		TPacer P;
		std::uint64_t nNow = 0;
		unsigned nFrames = 0;
		while (nNow < 10000000)
		{
			nNow += 5000;
			nNow += Wait (P, nNow, T) + 150;	// 150 us late every time
			nFrames++;
		}
		Check (nFrames >= 699 && nFrames <= 701, "overshooting waits keep the rate at 70, not slower");
	}

	std::printf ("trim from the sound queue\n");
	Check (TrimPpm (2646, 5292) == 0, "half full: no trim");
	Check (TrimPpm (4734, 5292) > 3000, "the board's 89 % full queue: slow down (by ~0.4 %)");
	Check (TrimPpm (5292, 5292) == MaxTrimPpm && TrimPpm (0, 5292) == -MaxTrimPpm, "at most 0.5 % either way");
	Check (Trimmed (T, MaxTrimPpm) == T + T / 200 && Trimmed (T, -MaxTrimPpm) == T - T / 200, "the period moves by the trim");
	{
		// A closed loop: the device takes 44100 frames a second; the machine
		// makes 630 per frame at a period of 14286 us (a whole-number 70 Hz,
		// 0.12 % slow against 70.086), and the queue starts nearly full.
		// The trim must settle it near half, with no drops or gaps.
		const double nSize = 5292;
		double nQueued = 4734, t = 0;
		bool bDrop = false, bGap = false;
		TPacer P;
		std::uint64_t nNow = 0;
		for (int i = 0; i < 70 * 60; i++)		// a minute
		{
			const std::int64_t ppm = TrimPpm (static_cast<unsigned> (nQueued), static_cast<unsigned> (nSize));
			const std::uint64_t nPeriod = Trimmed (T, ppm);
			nNow += 3000;
			nNow += Wait (P, nNow, nPeriod);
			const double dt = (nNow - t) / 1e6; t = static_cast<double> (nNow);
			nQueued -= 44100.0 * dt;
			nQueued += 44100.0 / 70.086;
			if (nQueued > nSize) { nQueued = nSize; bDrop = true; }
			if (nQueued < 0) { nQueued = 0; bGap = true; }
		}
		Check (!bDrop && !bGap && nQueued > nSize * 0.3 && nQueued < nSize * 0.7,
		       "closed loop: a full queue settles near half in a minute, no drops, no gaps");
	}

	std::printf ("\n%u failures\n", s_nFailures);
	return s_nFailures == 0 ? 0 : 1;
}
