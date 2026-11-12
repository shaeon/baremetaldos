//
// governor_policy.h
//
// by @mmoroca & arena.ai 2026  -  https://arena.ai
//
//
// This file is original to BM-DOS unless stated otherwise, and is distributed
// under the GNU General Public License version 3.  The binary it is part of
// links Circle and DOSBox-core (both GPL), so GPL-3.0 is what it inherits.
// See LICENSE in the repository root.
//
// BM-DOS - the clock governor's *decision*, with no Circle in it.
//
// Why this file exists, and why it is a header with code.
//
// Until .65f, `check-governor` tested the level *table* (which MHz go in which
// slot) and nothing tested the *policy* (when to move).  Three behaviours the
// policy is responsible for were visible on the board and nowhere else:
//
//   * the clock starts at 600 MHz, jumps to 1500 almost at once and then to
//     1800, on the DOS text screen at boot;
//   * the panel keeps printing `busy overXXXX` and never once `comf`;
//   * a DOS prompt that is doing nothing still walks the clock to the top.
//
// The first two are policy, and the policy was only reachable from the board.
// So the three numbers that decide it live here: thresholds, and one function
// that says what the state of the world is.  bare_sysinfo.cpp keeps the state
// machine, the mailbox and the counters; this keeps the judgement, which is the
// part that was wrong.
//
// The same discipline as bare_coreopts.h: no Circle, no firmware, no globals -
// just enough to be compiled and executed by a host test with numbers that came
// off a screen.
//
#ifndef BM_DOS_GOVERNOR_POLICY_H
#define BM_DOS_GOVERNOR_POLICY_H
//
// This file is original to BM-DOS unless stated otherwise, and is distributed
// under the GNU General Public License version 3.  The binary it is part of
// links Circle and DOSBox-core (both GPL), so GPL-3.0 is what it inherits.
// See LICENSE in the repository root.

namespace governor
{

// A frame that used more than 90 % of its budget has no margin: a faster clock
// would help.  Under 75 % there is room, and the clock may come down.
constexpr unsigned OverrunPercent = 90;
constexpr unsigned ComfortPercent = 75;

/// What one frame said about the machine.
/// What one frame said about the machine.
enum class TVerdict
{
	Slow,		///< under ComfortPercent: there is room
	Borderline,	///< between comfort and overrun: no evidence either way
	Overrun,	///< over the budget: a faster clock would help, if there is one
};

/// Judge one frame.  0 of budget means "no measurement", which is not a verdict:
/// that case must be handled by the caller before it gets here (see the note in
/// BareSysInfoGovernorTick - a governor with no measurement must not escalate).
constexpr TVerdict Judge (unsigned nUsedMicros, unsigned nBudgetMicros)
{
	if (nBudgetMicros == 0)
	{
		return TVerdict::Borderline;
	}

	// Integer arithmetic, in percent, in 64 bits: a slow frame can be several
	// times the budget and the product overflows 32 bits above 42 ms.
	const unsigned long long nPercent =
		static_cast<unsigned long long> (nUsedMicros) * 100 / nBudgetMicros;

	if (nPercent > OverrunPercent)
	{
		return TVerdict::Overrun;
	}

	if (nPercent < ComfortPercent)
	{
		return TVerdict::Slow;
	}

	return TVerdict::Borderline;
}

/// \brief How long a picture has to be literally identical before the machine
///\t   counts as idle.  Ten seconds at 70 Hz.
///
/// This constant is the whole idle detector, and it is a *duration* because the
/// two cases that broke the previous attempts differ in duration and in nothing
/// else that the panel can show.
///
/// The history matters, because two attempts in a row got this wrong:
///
///   `.72`  "identical frame = idle", 60 frames (~1 s).  A DOS prompt stepped
///          down to 600 MHz, which was the point - and DOOM stepped down too,
///          because a game repeats its picture all the time: menus, status bars,
///          the gap between two tics.  The audio broke up mid-game.
///   `.74`  "identical AND cheap = idle".  That fixed DOOM and broke the prompt:
///          at a prompt most of a 42 ms frame is the emulated machine sleeping on
///          its own timer, so the frame is never "cheap", the clock stayed at the
///          top and the board reached 63 C over a screen doing nothing.
///   `.77`  "the emulated machine advanced fewer times than the mode asks".  Off
///          the board: a healthy machine reads `30/60` or `35/70`, and DOOM at
///          `pace 15/70 bd 47` is doing plenty.  Those counters cannot separate
///          idle from working, and no amount of comparing them will make them.
///
/// What does separate them is time, and it is the one thing that is not a
/// heuristic: a DOS prompt leaves the screen *literally identical for minutes*.
/// A game does not - even a static menu repaints something within a few seconds
/// (a blinking cursor, an animated face, a status bar clock).  So the rule does
/// not try to judge one frame; it waits long enough that "still" means still.
///
/// Ten seconds costs nothing when it is wrong: one level, which is 3 s to reach
/// and 355 us plus two seconds of overrun to undo as soon as the picture moves.
constexpr unsigned IdleFramesForPrompt = 700;

/// \brief TRUE when a stretch of identical frames is long enough to be a prompt.
///
/// One condition, as before: counting the frames is the caller's, deciding what
/// the count means is here.
constexpr bool IsIdleAfter (unsigned nStillFrames)
{
	return nStillFrames >= IdleFramesForPrompt;
}


/// \brief TRUE when this frame proves the emulated machine is doing nothing.
///
/// The .65g addition, and it is a *measurement*, not a heuristic: a frame that
/// is identical to the one before it is an emulator that ran and produced
/// nothing new.  On a DOS prompt that is every frame - seventy a second of the
/// same text - which is why the governor's old idle test ("nothing presented for
/// a second") could never fire on a text screen and the board sat at its top
/// level over a prompt that was doing nothing at all.
///
/// One condition, on purpose: whether "identical for long enough" has been true
/// is the caller's clock, not this function's.  What belongs here is the
/// judgement, and the judgement is that a repeated frame is not work.
constexpr bool IsIdleFrame (bool bFrameChanged)
{
	return !bFrameChanged;
}


/// The word the panel prints, and it needs to know what the governor can still
/// do - not only what the last frame cost.
///
/// A panel reading `busy over3614` at 1800 MHz is the top of the board's range:
/// "busy" means "climbing", and there is nowhere to climb.  So the *level* is as much a part of the answer as the
/// frame, and the four words become five:
///
///     idle   nothing is being presented; no clock is justified
///     max    over budget, at the top level: no clock will help (the old "busy")
///     busy   over budget with a level above: the governor is climbing
///     comf   comfortable: there is room, and the governor may step down
///     -      between comfort and overrun: no evidence either way
///
/// `comf` is deliberately not used for the max case even though it is what a
/// person waiting for the board to cool down would like to read: frames of
/// 28 ms in a 16.7 ms budget are not comfortable, they are the emulator being
/// too slow here.  Saying "comf" would be the panel lying to make a number look
/// better, which is the one thing this port has spent three rounds undoing.
constexpr const char *Label (bool bIdle, bool bAtTop, TVerdict Verdict)
{
	if (bIdle)
	{
		return "idle";
	}

	switch (Verdict)
	{
	case TVerdict::Slow:
		return "comf";

	case TVerdict::Overrun:
		return bAtTop ? "max" : "busy";

	case TVerdict::Borderline:
	default:
		return bAtTop ? "max" : "-";
	}
}

// ---------------------------------------------------------------------------
// Rising early (item 3 of the list)
// ---------------------------------------------------------------------------
//
// The frame-time rules react after the fact: a few frames over budget, or 3 s
// of it.  Two earlier signals:
//
//   * A program starts (DOS's current PSP changes to one that is not the
//     shell's): its first seconds - unpacking, loading, a DOS extender, the
//     intro - are the heaviest.  Go to the top at once and stay there for
//     WarmupMicros.
//   * The sound output ran dry (an audio gap: a click you hear).  That is the
//     emulator already late, not about to be: one level up at once, and no
//     stepping down for a while.  Not right after a pause, the menu or a DOS
//     restart, when the queue is empty for reasons that are not load.
//
// The temperature veto overrides all of it, as it overrides everything else.

constexpr unsigned long long WarmupMicros = 10000000;		// 10 s at the top
constexpr unsigned long long GapGraceMicros = 2000000;		// after a resume
constexpr unsigned long long GapStepMicros = 500000;		// between gap steps
constexpr unsigned long long NoDownAfterGapMicros = 15000000;	// 15 s

/// May the governor step DOWN now (for comfort or idle)?  Not while a program
/// warms up, nor soon after an audio gap.
constexpr bool MayStepDown (unsigned long long nNow, unsigned long long nWarmUntil,
			    unsigned long long nNoDownUntil)
{
	return nNow >= nWarmUntil && nNow >= nNoDownUntil;
}

/// Should an audio gap raise the clock one level now?
constexpr bool GapStepsUp (bool bPinned, unsigned nLevel, unsigned nTop,
			   unsigned long long nSinceLevelChange, unsigned long long nSinceResume)
{
	return !bPinned && nLevel < nTop && nSinceLevelChange >= GapStepMicros
	       && nSinceResume >= GapGraceMicros;
}

/// Is this PSP a program starting?  nHome is the shell's (the first PSP seen);
/// going back to it is a program ending, not one starting.
constexpr bool ProgramStarted (unsigned nLastPsp, unsigned nPsp, unsigned nHome)
{
	return nPsp != 0 && nPsp != nLastPsp && nPsp != nHome;
}

} // namespace governor

#endif // BM_DOS_GOVERNOR_POLICY_H
