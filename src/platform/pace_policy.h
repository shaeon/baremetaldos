//
// pace_policy.h
//
// BM-DOS - the metronome for the emulated machine's frames, without Circle:
// when each frame boundary is due, and how the period is trimmed to the sound
// output.  bare_thread.cpp applies it to every boundary DOSBox reaches;
// scripts/hostcheck/pace_test.cpp checks it on the PC (make check-pace).
//
// Why one metronome: the frame loop paced its own calls ("one call = one
// frame"), and the emulator also reached boundaries on its own whenever the
// loop yielded the CPU (USB polling, the network's tasks), paced only against
// its own previous one.  Two boundaries per call, no wait anywhere: on the
// board (12-nov), a game's 70 Hz mode ran at 77 frames a second - 10 % fast -
// and the surplus sound, about 11 % of it, was thrown away at a full queue.
//
// Shaeon (Carlos Palmero) 2026, for BM-DOS by @mmoroca.  GPL-3.0-or-later.
//
#ifndef BARE_PACE_POLICY_H
#define BARE_PACE_POLICY_H

#include <cstdint>

namespace barepace {

struct TPacer
{
	std::uint64_t nNextDue = 0;	// 0: no boundary yet
};

/// A boundary at nNow: how long to wait before letting it through.  The next
/// one is due one period after THIS one's due time, not after "now", so that
/// waits that run a little long do not slow the machine down over time.  A
/// machine more than one period late keeps one period of catch-up at most:
/// running a backlog back to back would come out as a burst.
inline std::uint64_t Wait (TPacer &P, std::uint64_t nNow, std::uint64_t nPeriod)
{
	if (P.nNextDue == 0 || nPeriod == 0)
	{
		P.nNextDue = nNow + nPeriod;
		return 0;
	}

	const std::uint64_t nWait = P.nNextDue > nNow ? P.nNextDue - nNow : 0;
	if (nNow > P.nNextDue + nPeriod)
	{
		P.nNextDue = nNow;			// forget the backlog but one period
	}
	P.nNextDue += nPeriod;
	return nWait;
}

/// The period trimmed by the sound output's queue: fuller than half slows the
/// machine a little, emptier speeds it up, at most MaxTrimPpm either way.  The
/// display's audio clock and the board's timer never agree exactly, and a
/// 70 Hz mode is announced as a whole number of hertz (70.086 really), so a
/// machine paced by the timer alone fills or drains the queue slowly - drops
/// one way, gaps the other.  This is the dynamic rate control RetroArch uses.
constexpr std::int64_t MaxTrimPpm = 5000;	// 0.5 %

inline std::int64_t TrimPpm (unsigned nQueued, unsigned nSize)
{
	if (nSize == 0)
	{
		return 0;
	}
	// -1 .. +1 around the half-full target, then scaled.
	const std::int64_t nOff = static_cast<std::int64_t> (nQueued) * 2 - nSize;
	std::int64_t nPpm = nOff * MaxTrimPpm / static_cast<std::int64_t> (nSize);
	if (nPpm > MaxTrimPpm) nPpm = MaxTrimPpm;
	if (nPpm < -MaxTrimPpm) nPpm = -MaxTrimPpm;
	return nPpm;
}

inline std::uint64_t Trimmed (std::uint64_t nPeriod, std::int64_t nPpm)
{
	return static_cast<std::uint64_t> (static_cast<std::int64_t> (nPeriod)
					   + static_cast<std::int64_t> (nPeriod) * nPpm / 1000000);
}

} // namespace barepace

#endif
