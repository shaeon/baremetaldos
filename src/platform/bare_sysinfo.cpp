//
// bare_sysinfo.cpp
//
// by @mmoroca & arena.ai 2026  -  https://arena.ai
//
// BM-DOS - SoC load and temperature, for the status panel.
//
// Why this file exists at all, and why it is so small
// ---------------------------------------------------
//
// The status panel used to say how many frames had been presented.  That is a
// count of work, not a measure of health: an emulator crawling at 3 fps and one
// idling at 60 look the same after a minute.  What tells them apart is how busy
// the CPU is and how hot it is getting.
//
// There are two hard constraints, and both of them dictate the design:
//
//   1. The status panel is redrawn from the frame loop, which is *the
//      emulator's own thread of control* here (retro_run returns once per
//      frame).  Anything slow done there directly slows DOS down.  Reading the
//      SoC temperature goes through the firmware mailbox - a call into the
//      VideoCore, with a spin waiting for the reply - and doing that once per
//      frame would be visible to the emulated machine.  So temperature is read
//      at most once every two seconds, and the panel shows the last value.
//
//   2. Circle is cooperative and this port runs the emulator on core 0.  We
//      cannot read another core's program counter, and there is no per-core
//      accounting in Circle.  So "load per core" cannot be a measured
//      utilisation.  What can be measured honestly, and cheaply, is the ARM
//      clock rate the firmware reports for the ARM domain and for the CORE
//      domain (this is a Pi 4: the VPU core clock is what moves when the SoC
//      throttles), plus the temperature.  The panel says "MHz" and not "%"
//      because that is what it is; a percentage nobody measured would be a
//      lie with a decimal point.
//
// Both mailbox tags are the ones Circle itself uses in lib/cputhrottle.cpp
// (PROPTAG_GET_CLOCK_RATE with CLOCK_ID_ARM / CLOCK_ID_CORE, and
// PROPTAG_GET_TEMPERATURE), so there is nothing private or guessed here.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
#include "bare_env.h"

#include <atomic>
#include "governor_policy.h"

#include <circle/machineinfo.h>

#include <circle/bcmpropertytags.h>
#include <circle/logger.h>
#include <circle/koptions.h>
#include <circle/multicore.h>
#include <circle/sched/scheduler.h>
#include <circle/timer.h>
#include <circle/types.h>

#include <cstdio>
#include <cstring>

namespace {

// How often the mailbox may be touched.  Two seconds: fast enough to watch the
// SoC heat up under load, slow enough that the emulator never notices.
constexpr u64 RefreshMicros = 2000000;

static const char FromSysInfo[] = "sysinfo";

unsigned s_nArmClockHz = 0;	// last ARM clock rate the firmware reported
unsigned s_nCoreClockHz = 0;	// last VPU/core clock rate
unsigned s_nTemperature = 0;	// degrees Celsius, 0 = not available
u64 s_nLastRefreshMicros = 0;

u32 s_nFirmwareMode = 0;	// PROPTAG_GET_THROTTLED: bits, see below

// ---------------------------------------------------------------------------
// Mailbox proxy
// ---------------------------------------------------------------------------
//
// Why core 0 hands the mailbox to another core while the emulator runs:
// CBcmMailBox::WriteRead() holds a spin lock for the whole round trip, and the
// video presenter on core 2 waits for vertical sync through the same mailbox
// (PROPTAG_WAIT_FOR_VSYNC answers at the next vsync, up to 16.7 ms later).  A
// clock change or a temperature read from core 0 during that wait would spin
// on the lock and stall the emulated machine for up to a frame.
//
// So with the proxy on, core 0 never touches the mailbox:
//
//   * SetArmClock() posts the target rate (latest wins) and returns;
//   * BareSysInfoRefresh() copies the values the owner core published;
//   * the owner core calls BareSysInfoServiceTick() between frames, which
//     applies a pending clock change (including the settling wait) and
//     re-reads clocks, temperature and the throttling bits every
//     RefreshMicros.
//
// With the proxy off - before the presenter starts, after it stops, or on a
// build where core 2 never came up - everything is as it was: direct calls
// on core 0.
std::atomic<bool> s_bProxy {false};
std::atomic<unsigned> s_nClockRequestHz {0};	// 0 = nothing pending
std::atomic<unsigned> s_nPubArmHz {0};
std::atomic<unsigned> s_nPubCoreHz {0};
std::atomic<unsigned> s_nPubCelsius {0};
std::atomic<u32> s_nPubFirmwareMode {0};
u64 s_nServiceLastReadMicros = 0;		// owner core only

// What the firmware did NOT give us, and why the panel lost its per-core
// figure: PROPTAG_GET_CLOCK_RATE_MEASURED (0x00030047) is defined in Circle's
// header, but the VideoCore does not always answer it - the first hardware run
// of this port showed "core?" for exactly that reason - and when it does answer
// it gives one rate for the whole ARM domain on a Pi 4, because there is one
// clock tree.  So the panel shows the domain rate once (BareSysInfoGet) and
// this file no longer pretends to have four numbers.

// The firmware's own limits for the ARM domain.  These are what the governor
// builds its levels from, so that a board with a lower rating (or a config.txt
// with arm_freq set) is respected instead of assumed.
unsigned ReadMaxClockRate (u32 nClockId)
{
	CBcmPropertyTags Tags;
	TPropertyTagClockRate Tag = {};
	Tag.nClockId = nClockId;
	if (!Tags.GetTag (PROPTAG_GET_MAX_CLOCK_RATE, &Tag, sizeof Tag, 4))
	{
		return 0;
	}

	return Tag.nRate;
}

// The firmware's *minimum* for the ARM domain (PROPTAG_GET_MIN_CLOCK_RATE).
//
// .60: this did not exist, and BuildLevels() filled s_nMinHz from
// ReadClockRate() - the tag that reports the rate the board is running at
// *right now*.  At boot that is the firmware's default (1500 MHz on a Pi 4),
// so the governor built its table around 1500, believed it had two levels
// (1500 and 2000) and had no 600 MHz anywhere in it.  "Never reduces the
// clock" was therefore not a symptom of the decision logic at all: there was
// nothing to reduce it to.
unsigned ReadMinClockRate (u32 nClockId)
{
	CBcmPropertyTags Tags;
	TPropertyTagClockRate Tag = {};
	Tag.nClockId = nClockId;
	if (!Tags.GetTag (PROPTAG_GET_MIN_CLOCK_RATE, &Tag, sizeof Tag, 4))
	{
		return 0;
	}

	return Tag.nRate;
}

// One mailbox transaction, cached.  Returns 0 on failure - the firmware on a
// given board may refuse a tag, and "unknown" has to be representable.
unsigned ReadClockRate (u32 nClockId)
{
	CBcmPropertyTags Tags;
	TPropertyTagClockRate TagClockRate;
	TagClockRate.nClockId = nClockId;
	if (!Tags.GetTag (PROPTAG_GET_CLOCK_RATE, &TagClockRate,
			  sizeof TagClockRate, 8))
	{
		return 0;
	}

	return TagClockRate.nRate;
}

unsigned ReadTemperature (void)
{
	CBcmPropertyTags Tags;
	TPropertyTagTemperature TagTemperature;
	TagTemperature.nTemperatureId = TEMPERATURE_ID;
	if (!Tags.GetTag (PROPTAG_GET_TEMPERATURE, &TagTemperature,
			  sizeof TagTemperature, 8))
	{
		return 0;
	}

	// The firmware reports milli-degrees Celsius.
	return (TagTemperature.nValue + 500) / 1000;
}

// ---------------------------------------------------------------------------
// The clock governor
// ---------------------------------------------------------------------------
//
// Why this is written here and not used from Circle: Circle's CCPUThrottle has
// exactly two speeds (the firmware's minimum and its maximum) and, if you add
// it to the system, it sets the maximum at construction and leaves it there.
// That is 2 GHz on a Pi 4 - the board gets hot, the fan runs, and the emulator
// is not one bit faster, because DOSBox's interpreter is not clock-bound at
// 600 MHz either.  What is needed is three levels and a reason to move between
// them.
//
// The three levels, and why exactly these:
//
//   low   600 MHz   what the SoC idles at.  Enough for an idle prompt.
//   mid  1500 MHz   the Pi 4's nominal rate.  DOSBox at its default cycles
//                   (23880, a 486DX2/66) fits in this on the interpreter.
//   max  2000 MHz   the firmware's maximum, for when the emulator's own
//                   speed test says 1500 is not enough.
//
// The decision comes from the *latency of an emulated frame*, which is the one
// number that says "the host is not keeping up".  DOSBox has its own governor
// for its cycle count, but it knows nothing about the host's clock, so this one
// watches the wall clock instead: how long retro_run() took against the 16.6 ms
// a frame has.  Frames that consistently take longer than their budget mean the
// interpreter wants more MHz, and it gets them.
//
// Three details that make it behave rather than oscillate:
//
//  * Escalation is fast (mid after 2 s of overrun, max after 3 s more) and
//    de-escalation is slow (30 s of comfort before dropping, 60 s more before
//    dropping again).  A game that is just at the edge stays at mid instead of
//    flapping between levels - and every change costs a 355 us transition that
//    the sound device would rather not see.
//  * The temperature can veto everything: above ThrottleCelsius the governor
//    drops one level regardless of load, and it never goes back up while the
//    SoC is still that hot.  Override the number with "socmaxtemp=" in
//    cmdline.txt; the firmware's own limit (85 C) stays as the last line of
//    defence and is not touched.
//  * The governor is *only* trusted where it has been told the clock can be
//    changed.  That is checked once, at start-up, by reading the firmware's
//    minimum and maximum and asking for a rate in between and back - see
//    BareSysInfoSetPerformance().  If the check fails - some firmware builds
//    refuse SET_CLOCK_RATE on some boards - the governor stays put at whatever
//    the firmware gives it, and the panel still reports the truth.

constexpr unsigned LowHz = 600000000;
constexpr unsigned MidHz = 1500000000;
constexpr unsigned DefaultMaxHz = 2000000000;	// what a Pi 4 is rated at
constexpr unsigned ThrottleCelsius = 80;

// How the judgement is made, which is the .57 correction: as a *fraction* of
// the frame budget, not against a fixed number of microseconds.
//
// The budget is not always 16.6 ms.  A DOS text screen refreshes at 70 Hz
// (14.3 ms per frame) and a 320x200 game mode at 70 Hz too; a VGA mode at
// 59.9 Hz.  The previous version compared every frame against a hard-coded
// 16.6 ms, so a perfectly paced 70 Hz machine was over budget on every single
// frame and the governor sat at the top level for ever - which is the panel
// showing a frequency that "does not go up or down".
//
// So: a frame that used more than 90 % of its budget has no margin and is
// evidence that a faster clock would help; one under 75 % has room.
//
// .65f: and two dead zones, because the panel shows the counter-example that
// proves the numbers above are too tight for a big blitter:
//
//     fr 28ms  pace 34/60  over 3614
//
// 28 ms against a 16.7 ms budget is 168 % - a frame that takes nearly two
// frames' worth of time.  On a fixed-clock machine that is simply "it cannot",
// and asking the governor to raise the clock for it is asking the wrong
// question: 1800 MHz is the top of the range, and it is already there.  What the
// counter then does is stop *ever* looking comfortable, so the board can never
// step down even after the game stops, which is the "nunca baja" report again
// with a different cause.
//
// So:
//   * at or above HopelessPercent the frame is not evidence for anything - it is
//     the emulator being too slow for this hardware, and no clock fixes it;
//   * at or below ComfortPercent the frame is evidence of room.
// The thresholds themselves are in governor_policy.h, where check-governor can
// execute them against numbers off a screen.  These aliases exist so that the
// rest of this file reads as it did; there is one definition.
constexpr unsigned OverrunPercent = governor::OverrunPercent;
constexpr unsigned ComfortPercent = governor::ComfortPercent;

// How long each decision takes, in frames at 60 Hz.  Escalation is quick - a
// game that needs more MHz should get them within two seconds - and
// de-escalation is slow, because dropping the clock during a momentary quiet
// spell costs a transition and gets undone immediately.
constexpr unsigned EscalateFrames = 120;	// 2 s of overrun to climb

// .65g: how many frames a level must go by WITHOUT a single repeated frame
// before an overrun counts as evidence that the clock should go up.
//
// A panel line reading `max over803` or `max over2118` - hundreds of "overrun"
// frames accumulated at the TOP level - on a board drawing 33 frames a second
// of a 70 Hz mode is what this exists for.  A frame cannot be over its
// budget *and* arrive at half the rate unless the measured "frame" is not the
// thing being paced, and until that is settled a governor that acts on those
// numbers is acting on a measurement nobody has verified.
//
// So: overrun evidence only counts on a stretch of frames where the picture is
// actually changing.  A prompt, a still menu, a paused game - the frames that
// repeat - can no longer walk the clock up.  That is the "1800 MHz while the
// prompt sits still" behaviour, fixed at the source rather than by banning
// escalation at boot, which was .65f's attempt and did not hold.
constexpr unsigned ComfortFrames = 600;	// 10 s of comfort to consider dropping

// How long a level has to last before it may step down.  Two numbers: the
// ordinary one, and a longer one for the case where there is no positive
// evidence of comfort but also no overrun (see BareSysInfoGovernorTick).
constexpr u64 StepDownMicros = 15000000;	// 15 s
constexpr u64 StepDownIdleMicros = 30000000;	// 30 s

// How long the "nothing is presenting frames" state has to last before it costs
// a level.  Short, because nothing is running: there is no work to be late for.
constexpr u64 IdleStepMicros = 3000000;		// 3 s


/// \brief The three levels the governor uses, low first.
struct TLevel
{
	unsigned nHz;
};

TLevel s_Levels[3] = { { LowHz }, { MidHz }, { DefaultMaxHz } };
unsigned s_nLevels = 3;
unsigned s_nLevel = 0;			// index into s_Levels

// boot.conf mode: 0 auto (the governor decides), 1 performance (the top
// level), 2 powersave (the bottom level).  The temperature veto applies in all
// three: a pinned level is where the clock goes back to once the SoC has
// cooled, not a reason to cook it.
unsigned s_nPinned = 0;
boolean s_bGovernorReady = FALSE;	// TRUE once the firmware accepted a change
// Starts as TRUE on purpose: the first call with FALSE - "no game is presenting
// frames" - has to be a *transition*, and it is the one that takes the board
// from the middle level down to 600 MHz at the DOS prompt.  Starting at FALSE
// makes that first call a no-op and leaves the prompt running at 1500.
boolean s_bCLIActive = TRUE;		// DOS prompt on screen: nothing to run
boolean s_bInTransition = FALSE;	// between level changes
u64 s_nLevelSince = 0;			// when the current level was entered
unsigned s_nOverrunFrames = 0;
// Frames in a row whose picture was identical to the previous one.  What makes
// this an idle detector is how many of them are required: see
// governor::IdleFramesForPrompt, which is the whole rule and explains the two
// rules before it that were wrong.
unsigned s_nStillFrames = 0;		// consecutive slow frames
unsigned s_nComfortFrames = 0;		// consecutive comfortable frames

// What the last frame said, kept for the panel's word.  See governor::Label():
// "busy" was being printed at 1800 MHz, i.e. on a board with no level above it,
// which is why "comf" never appeared on the panel at all.
governor::TVerdict s_nLastVerdict = governor::TVerdict::Borderline;
unsigned s_nMaxHz = DefaultMaxHz;	// from the firmware, not assumed
unsigned s_nMinHz = LowHz;
unsigned s_nVetoCelsius = ThrottleCelsius;	// socmaxtemp= overrides

u64 NowMicros (void)
{
	// BareMicros(), and NOT "the clock counter times 10000".
	//
	// This used to multiply GetClockTicks64() by (1000000 / HZ), on the reading
	// that the counter ticks at Circle's 100 Hz task rate.  It does not: it is
	// the CPU's physical counter, 54 MHz on a Pi 4, so every millisecond this
	// governor measured was really 540 of them.  The consequences were exactly
	// the ones observed: the clock-change settling time looked long, and the
	// "has it been 30 s at this level" test for stepping *down* was comparing
	// against a number growing 540 times too fast, so the governor escalated on
	// the first frame and never came back.
	//
	// One clock for the whole port, calibrated in one place (bare_time.cpp).
	return BareMicros ();
}

/// \brief Ask the firmware for one ARM clock rate.  Returns FALSE on failure.
///\t   The mailbox call itself; SetArmClock() decides who makes it.
boolean MailboxSetArmClock (unsigned nHz)
{
	CBcmPropertyTags Tags;
	TPropertyTagSetClockRate Tag = {};
	Tag.nClockId = CLOCK_ID_ARM;
	Tag.nRate = nHz;
	Tag.nSkipSettingTurbo = 0;	// let the firmware apply its own turbo rules

	const u64 nStart = NowMicros ();

	if (!Tags.GetTag (PROPTAG_SET_CLOCK_RATE, &Tag, sizeof Tag, 12))
	{
		return FALSE;
	}

	// The rate has to settle before anything is measured against it: 355 us is
	// the figure Linux uses for this SoC family (bcm2835-cpufreq.c) and
	// Circle's own CCPUThrottle uses the same one.  It is a busy wait of a
	// third of a millisecond, which is why the governor changes level in
	// *seconds*, not frames.
	(void) nStart;
	CTimer::SimpleusDelay (400);

	return TRUE;
}

/// \brief Change the ARM clock: directly, or through the owner core when the
///\t   mailbox proxy is on (then it is applied within one frame and this
///\t   reports success at once, which is what the governor's bookkeeping
///\t   needs - it measures in seconds, not frames).
boolean SetArmClock (unsigned nHz)
{
	if (s_bProxy.load (std::memory_order_acquire))
	{
		s_nClockRequestHz.store (nHz, std::memory_order_release);
		return TRUE;
	}

	return MailboxSetArmClock (nHz);
}

/// \brief Move to level nTo, if the firmware agrees.  Resets the counters.
// What the governor did, for the setup menu's Rendimiento page (item 2 of the
// list: "see what it does").  Times per level are accumulated when the level
// changes, and the current level's share is added when they are read.
u64 s_nLevelMicros[8] = {};
unsigned s_nUps = 0;
unsigned s_nDowns = 0;
const char *s_pLastReason = "";
u64 s_nLastChangeMicros = 0;
unsigned s_nLoadPermille = 0;		// work / budget, smoothed (1000 = all of it)

boolean SetLevel (unsigned nTo, const char *pReason)
{
	if (nTo >= s_nLevels || nTo == s_nLevel)
	{
		return FALSE;
	}

	s_bInTransition = TRUE;
	const boolean bOk = SetArmClock (s_Levels[nTo].nHz);
	s_bInTransition = FALSE;

	if (bOk)
	{
		const u64 nNow = NowMicros ();
		if (s_nLevelSince != 0 && s_nLevel < 8)
		{
			s_nLevelMicros[s_nLevel] += nNow - s_nLevelSince;
		}
		if (nTo > s_nLevel) s_nUps++; else s_nDowns++;
		s_pLastReason = pReason;
		s_nLastChangeMicros = nNow;

		s_nLevel = nTo;
		s_nLevelSince = nNow;
	}

	s_nOverrunFrames = 0;
	s_nComfortFrames = 0;

	return bOk;
}

void BuildLevels (void)
{
	// The firmware is the authority on the *ceiling* - a Pi 4 on a weak supply
	// and a Pi 5 do not share limits - so the top of the range is its number.
	//
	// The bottom is not, and this is the .60 correction.  It used to be filled
	// from ReadClockRate(), i.e. from "what is the clock right now": at boot
	// that is 1500 MHz on a Pi 4, so the table came out as {1500, 1500, 2000},
	// the "is the middle strictly inside the range" test below failed, and the
	// governor ran with two levels and no 600 MHz in either of them.  The
	// panel showed a level that never dropped because there was no lower level
	// to drop to.  ReadMinClockRate() asks the tag that actually means minimum,
	// and LowHz is the floor underneath it.
	s_nMinHz = ReadMinClockRate (CLOCK_ID_ARM);
	if (s_nMinHz == 0)
	{
		s_nMinHz = LowHz;
	}

	s_nMaxHz = ReadMaxClockRate (CLOCK_ID_ARM);
	if (s_nMaxHz == 0 || s_nMaxHz <= s_nMinHz)
	{
		s_nMaxHz = DefaultMaxHz;
	}

	// The floor is LowHz (600 MHz): it is the figure the whole design is
	// written around, and every Pi this port targets can run there.  The
	// firmware's minimum only *raises* it, which covers the one case that
	// should be respected - a config.txt that really did pin the clock.
	if (s_nMinHz < LowHz)
	{
		s_nMinHz = LowHz;
	}

	s_Levels[0].nHz = s_nMinHz;
	s_Levels[1].nHz = MidHz;
	s_Levels[2].nHz = s_nMaxHz;

	// Three levels only make sense with the middle one strictly inside the
	// range.  No Pi 4/5 fails that test, but a board with arm_freq pinned low
	// would, and then two levels are the honest answer.
	if (s_nMinHz < MidHz && MidHz < s_nMaxHz)
	{
		s_nLevels = 3;
	}
	else
	{
		s_Levels[1].nHz = s_nMaxHz;
		s_nLevels = 2;
	}

	// .77: and the table has to be STRICTLY increasing, which the block above
	// does not guarantee.  It is the bug a `cpu_max_mhz = 1000` produced on a
	// Pi 3: BuildLevels() filled {600, 1500, 1000}, the middle-in-range test
	// failed (1500 < 1000 is false), the else branch wrote s_Levels[1] = the
	// maximum, and the levels came out {600, 1000, 1000} with s_nLevels = 2.
	//
	// Two identical levels are worse than useless: a ceiling below LowHz - 600,
	// i.e. 0 in boot.conf's terms - and a maximum the firmware reports as lower
	// than its own minimum both collapse the two remaining slots onto the same
	// number, and then SetLevel(s_nLevel - 1) asks the firmware for the rate it
	// already has, gets the level clock reset, and the governor believes it
	// stepped down while nothing moved.
	//
	// So the last word on the table is here, and it is one rule: drop the
	// duplicates, keep it sorted.  With {600, 1000, 1000} the answer is {600,
	// 1000} and the governor has somewhere to go.  With a single rate the table
	// is one level and the governor simply has no decision to make - which is
	// the truth for a board whose clock is pinned.
	{
		unsigned nOut = 1;
		for (unsigned nIn = 1; nIn < s_nLevels; nIn++)
		{
			if (s_Levels[nIn].nHz > s_Levels[nOut - 1].nHz)
			{
				s_Levels[nOut].nHz = s_Levels[nIn].nHz;
				nOut++;
			}
		}

		s_nLevels = nOut;
	}

	// socmaxtemp= from cmdline.txt moves the veto point; the firmware's own
	// limit (85 C) is untouched above it either way.
	const unsigned nOption = CKernelOptions::Get ()->GetSoCMaxTemp ();
	if (nOption >= 40 && nOption <= 85)
	{
		s_nVetoCelsius = nOption;
	}
}

void ReadFirmwareMode (void)
{
	// 0x00030046 returns a *bitmask*, not a level:
	//   bit 0  under-voltage now           bit 16 under-voltage has occurred
	//   bit 1  frequency capped now        bit 17 frequency capping occurred
	//   bit 2  throttled now               bit 18 throttling occurred
	//   bit 3  soft temperature limit now  bit 19 soft temperature limit occurred
	// The "now" bits are what the panel shows while it happens; the sticky ones
	// are what to look at after a session that felt slow.  Under-voltage is the
	// common one on a Pi 4 with a weak supply, and it is what makes the board
	// silently ignore the clock the governor asked for.
	CBcmPropertyTags Tags;
	TPropertyTagSimple Tag = {};
	if (Tags.GetTag (PROPTAG_GET_THROTTLED, &Tag, sizeof Tag, 4))
	{
		s_nFirmwareMode = Tag.nValue;
	}
}

} // namespace

unsigned BareSysInfoCores (void)
{
	// Four, and this is the honest number rather than a guess: every model
	// this port targets (Pi 2B, 3B/3B+, 4B, 400, CM4, 5, 500) has four, and
	// the one 2 GB Pi 4 that shipped with a different core count does not
	// exist.  RASPPI, which would be the compile-time answer, says how many
	// cores the code was *built* for - a different question, and the one that
	// made the panel print a number the board did not match.
	const unsigned nCores = 4;

	return nCores;
}

const char *BareSysInfoBoardName (void)
{
	// Circle knows the model from the firmware (CMachineInfo asks
	// PROPTAG_GET_BOARD_MODEL); the port does not need to.  This exists
	// because of one concrete confusion: a Pi 400 runs its ARM domain at
	// 1800 MHz and a Pi 4B at 1500, and two rounds of clock work were done
	// against "a Pi 4" while the log said "Raspberry Pi 400".  The panel says
	// which board it is so the numbers next to it can be read correctly.
	switch (CMachineInfo::Get ()->GetMachineModel ())
	{
	case MachineModel400:
		return "Pi 400";
	case MachineModel4B:
		return "Pi 4B";
	case MachineModelCM4:
		return "CM4";
	case MachineModelCM4S:
		return "CM4S";
	case MachineModel5:
		return "Pi 5";
	case MachineModel500:
		return "Pi 500";
	case MachineModelCM5:
		return "CM5";
	case MachineModelCM5Lite:
		return "CM5L";
	case MachineModel3BPlus:
		return "Pi 3B+";
	case MachineModel3B:
		return "Pi 3B";
	case MachineModel3APlus:
		return "Pi 3A+";
	case MachineModel2B:
		return "Pi 2B";
	case MachineModelZero2W:
		return "Pi Zero 2W";
	default:
		return "Pi";
	}
}

void BareSysInfoSetCPUMax (unsigned nMHz)
{
	// A ceiling from boot.conf: the top level becomes this instead of the
	// firmware's maximum, which is how you tell a Pi 4 in a small case to stay
	// at 1500 without touching anything else.
	if (nMHz >= 600 && nMHz < s_nMaxHz / 1000000)
	{
		s_nMaxHz = nMHz * 1000000;
	}
}

void BareSysInfoSetPerformance (void)
{
	BuildLevels ();

	// One change, and then a read-back: a firmware that accepts
	// SET_CLOCK_RATE but ignores it would let the governor believe it had
	// lowered the clock while the board cooked.  Read-back is the only test
	// that distinguishes the two.
	// .65f: start at the BOTTOM, not in the middle.
	//
	// This used to be the middle level when there were three, and the panel
	// shows what that looks like from the outside: the board starts at 600,
	// jumps to 1500 and then to 1800.  The 600 is the
	// firmware; the jump to 1500 was this line, made *before any frame had been
	// measured*; and the climb to 1800 was the governor reacting to the text
	// screen it had just been handed.
	//
	// Booting is the one moment when nothing is known and nothing is being
	// asked for, so the honest place to start is the lowest level: the frame
	// timings of the first two seconds will argue the machine up if they need
	// to, and 355 us is what that costs.  It is also the level a DOS prompt is
	// entitled to, and it makes the governor's behaviour observable - a board
	// that starts low and climbs is a governor that can be watched, and a
	// governor that cannot be watched is a governor that cannot be trusted.
	const unsigned nStart = 0;
	if (!SetArmClock (s_Levels[nStart].nHz))
	{
		CLogger::Get ()->Write (FromSysInfo, LogWarning,
					"clock: firmware refused clock changes, staying at %u MHz",
					ReadClockRate (CLOCK_ID_ARM) / 1000000);
		return;
	}

	// 20 ms, not 2.  The clock change itself settles in 355 us, but the rate
	// the mailbox reports is not instantaneous: with a 2 ms wait this read came
	// back with the *old* rate on the board, which made the governor believe the
	// firmware had refused and give up - the panel then showed a fixed frequency
	// for ever.  A margin of two orders of magnitude over the hardware figure
	// costs nothing at boot and removes a whole class of "it silently did
	// nothing".
	CTimer::SimpleusDelay (20000);

	const unsigned nNow = ReadClockRate (CLOCK_ID_ARM);
	if (nNow == 0)
	{
		CLogger::Get ()->Write (FromSysInfo, LogWarning,
					"clock: the firmware does not report the ARM rate");
		return;
	}

	// A 5 % tolerance: the firmware rounds to its own PLL steps and reporting
	// 1499 where we asked 1500 is not a failure.
	const unsigned nWant = s_Levels[nStart].nHz;
	if (nNow + nNow / 20 < nWant || nNow > nWant + nWant / 20)
	{
		CLogger::Get ()->Write (FromSysInfo, LogWarning,
					"clock: asked for %u MHz, firmware reports %u MHz",
					nWant / 1000000, nNow / 1000000);
		// It accepted *something*, so the governor can still steer; it just
		// starts from wherever the firmware put it.
	}

	s_bGovernorReady = TRUE;
	s_nLevel = nStart;
	s_nLevelSince = NowMicros ();

	CLogger::Get ()->Write (FromSysInfo, LogNotice,
				"clock: %u MHz now, levels %u/%u/%u MHz, veto at %u C",
				nNow / 1000000,
				s_Levels[0].nHz / 1000000,
				s_nLevels > 2 ? s_Levels[1].nHz / 1000000 : 0,
				s_Levels[s_nLevels - 1].nHz / 1000000,
				s_nVetoCelsius);
}

void BareSysInfoSetCLIActivity (boolean bActive)
{
	if (!s_bGovernorReady || bActive == s_bCLIActive)
	{
		return;
	}

	// Measure before switching, because SetLevel() resets the level clock and
	// the *previous* level is what has to be judged.
	const u64 nWasAtLevelFor = NowMicros () - s_nLevelSince;

	s_bCLIActive = bActive;

	if (bActive)
	{
		// A game (or anything) is presenting frames again: go to the middle
		// level and let the frame timings argue for the rest.
		//
		// .59: but only if the board is *cold*, i.e. it was idling rather than
		// already working.  This used to move unconditionally, and that alone
		// was enough to keep the board at 1500: a screen that presents nothing
		// for one second (a load, a static menu, the brief gap when a mode
		// changes) flipped the flag to idle and then straight back to busy, and
		// every one of those transitions reset the level to the middle and threw
		// away the evidence the governor had gathered.  With a flapping flag the
		// level could never walk anywhere, least of all down.
		//
		// "Cold" is measured, not guessed: the idle rule in
		// BareSysInfoGovernorTick() steps down every IdleStepMicros, so being at
		// the bottom means the board has been quiet for at least that long.  If
		// it is not at the bottom it was already running and the level it has
		// earned is kept.
		const boolean bWasIdleLongEnough =
		s_nLevel == 0 || nWasAtLevelFor >= IdleStepMicros;

		if (bWasIdleLongEnough)
		{
			SetLevel (s_nLevels > 2 ? 1 : 0, "arranque del governor");
		}
	}
	else
	{
		// DOS prompt: no emulated frame is being presented, so there is
		// nothing to be slow at.  This is the 600 MHz the board should sit at
		// when it is doing nothing, and coming back costs 355 us.
		//
		// .58: this fires when nothing has been presented for ONE SECOND - so
		// it also fires while a game is loading, or on a level that holds a
		// static picture.  That is the case where the clock never came down:
		// the state machine went busy -> idle -> busy on the same
		// screen, and each busy transition reset the level to the middle and
		// threw away the evidence the governor had gathered.  The clock ended
		// up wherever the flapping left it, which is the top.
		//
		// A game that presents nothing for a second is *quiet*, not gone, so
		// the drop is now one level rather than straight to the bottom, and
		// coming back does not clobber the level unless the frame timings say
		// it should.  The bottom is still where a real DOS prompt ends up:
		// one second of quiet, then another, then another.
		// Only the counters are reset here any more; the walk down to the
		// bottom is BareSysInfoGovernorTick()'s job, because that one is
		// called on every frame and this one only when the flag flips (see
		// the long note there - a single step down was the whole fault).
		s_nOverrunFrames = 0;
		s_nComfortFrames = 0;
	}
}

const char *BareSysInfoGovernorState (void)
{
	static char State[96];

	if (!s_bGovernorReady)
	{
		snprintf (State, sizeof State, "off");
		return State;
	}

	// The label, chosen so that each word means one thing:
	//   idle  nothing is being presented; no clock is justified
	//   slow  the frames do not fit and no faster clock would help
	//   busy  over budget with room above: the governor should be climbing
	//   comf  comfortable: there is room, the governor may step down
	// Before .65f only idle/busy existed, and "busy over3614" was the panel
	// saying "climbing" about a machine that had nowhere left to climb.
	const char *pState = governor::Label (s_bCLIActive,
					     s_nLevel + 1 >= s_nLevels,
					     s_nLastVerdict);

	snprintf (State, sizeof State, "L%u/%u %s over%u",
		  s_nLevel, s_nLevels, pState, s_nOverrunFrames);

	// The comfort counter used to be here too ("comf%u").  It answered a
	// question the panel is not the place for - the governor's own state
	// machine - and it cost fifteen characters of a line that has to fit on a
	// 800-pixel-wide mode.  It is still in the log, where a person debugging
	// the governor will look.

	return State;
}

void BareSysInfoGovernorTick (unsigned nFrameMicros, unsigned nBudgetMicros,
			      boolean bFrameChanged)
{
	if (!s_bGovernorReady || s_bInTransition)
	{
		return;
	}

	// mode = performance / powersave: no judging, no idle rules - the pinned
	// level, except that the temperature veto still takes the clock down one
	// level at a time while the SoC is at or above its limit.
	if (s_nPinned != 0)
	{
		const bool bHot = s_nTemperature != 0 && s_nTemperature >= s_nVetoCelsius;
		const unsigned nTarget = s_nPinned == 1 ? s_nLevels - 1 : 0;
		if (bHot && s_nLevel > 0)
		{
			SetLevel (s_nLevel - 1, "temperatura");
		}
		else if (!bHot && s_nLevel != nTarget && NowMicros () - s_nLevelSince >= IdleStepMicros)
		{
			SetLevel (nTarget, s_nPinned == 1 ? "modo Máximo" : "modo Ahorro");
		}
		return;
	}

	// ---------------------------------------------------------------------
	// .59: the prompt never got back to 600 MHz either
	// ---------------------------------------------------------------------
	//
	// Stepping down when the emulator goes quiet was handled in
	// BareSysInfoSetCLIActivity(), which is *edge triggered*: it acts when the
	// idle/busy flag changes and does nothing afterwards.  So a game that ends
	// at level 2 dropped exactly one level, to 1500, and stopped there for ever,
	// because the flag stayed idle.  The board then sat at 1500 MHz over a DOS
	// prompt - and the panel showed it, which is the report: "el governor nunca
	// disminuye los mhz".
	//
	// Idle is a *state*, not an event, so the rule belongs here, where it is
	// evaluated every frame: while nothing is being presented, keep walking down
	// one level every IdleStepMicros until the bottom.  Three seconds per step,
	// so a game that pauses for a moment does not lose the clock it will want
	// back in a second - and going back up costs 355 us.
	// ---------------------------------------------------------------------
	// .65g: idle is "the picture is not changing", not "no frame arrived"
	// ---------------------------------------------------------------------
	//
	// This runs before the CLI branch on purpose.  A DOS prompt presents seventy
	// frames a second, so `s_bCLIActive` is TRUE there and always will be: the
	// flag means "the emulator is presenting", and on a text screen that is the
	// normal state of an idle machine.  That is the whole bug: the board
	// reported `busy` (later `max`) at 1800 MHz over a prompt whose only
	// activity was repeating its own picture.
	//
	// A repeated frame is not work.  After a second of them the board is idle
	// whatever else says otherwise, and it walks down one level per
	// IdleStepMicros exactly as a quiet screen always did - the walk, the step
	// and the hysteresis are unchanged; only the trigger is now measurable.
	// .77: and the two fixes before this one are both withdrawn, because both were
	// built on a measurement that cannot see the difference.  Read the note on
	// governor::IdleFramesForPrompt for the two photographs and the two failed
	// rules; the short version is that a game repeats its picture (menus, status
	// bars, the gap between tics) and a prompt looks like a slow frame (most of it
	// is the emulated machine sleeping on its own timer), so neither "identical"
	// nor "identical and cheap" nor "advanced less often than the mode asks" is
	// idle.  What is idle is a screen that has not changed in TEN SECONDS, which a
	// prompt does and no game does.
	if (governor::IsIdleFrame (bFrameChanged))
	{
		if (s_nStillFrames < 0xFFFF)
		{
			s_nStillFrames++;
		}

		if (governor::IsIdleAfter (s_nStillFrames))
		{
			s_nOverrunFrames = 0;   // a picture that has not changed in 10 s is not work

			if (s_nLevel > 0 && NowMicros () - s_nLevelSince >= IdleStepMicros)
			{
				SetLevel (s_nLevel - 1, "pantalla quieta");
			}
		}
	}
	else
	{
		s_nStillFrames = 0;
	}

	if (s_bCLIActive)
	{
		if (s_nLevel > 0 && NowMicros () - s_nLevelSince >= IdleStepMicros)
		{
			SetLevel (s_nLevel - 1, "prompt de DOS");
		}

		return;
	}

	// No budget: the caller does not know what the mode refreshes at.
	//
	// .65e: this used to fall back to 16666 and carry on, and the DOS prompt is
	// what showed why that is not neutral.
	// Before the rate is known, kernel.cpp paces at its sanity ceiling, so the
	// budget it passes is 1/240 s = 4167 us - and a 14.3 ms text frame against a
	// 4.2 ms budget is a 340 % overrun on every single frame, which walks the
	// governor to its top level within the four seconds of escalation.  That is
	// exactly the behaviour of starting at 600, jumping to 1500 and then to
	// 1800 on a text screen.
	//
	// The root cause is fixed where it belongs (kernel.cpp asks the core for its
	// rate right after the load, so this case should not happen any more), and
	// this is the belt: with no budget there is no measurement, and a governor
	// with no measurement must not *escalate*.  It keeps the level it has -
	// which is 600 for a DOS prompt - and waits for a number it can trust.
	if (nBudgetMicros == 0)
	{
		return;
	}

	// The judgement itself is governor::Judge(), in governor_policy.h, so that
	// check-governor can execute it with the numbers this board actually
	// produced (a 28 ms frame in a 16.7 ms budget, for instance) instead of
	// only on the board.  Everything below is the state machine that acts on
	// the verdict.

	// The temperature is not read here: a mailbox call is a round trip to the
	// VideoCore and this runs 60 times a second.  BareSysInfoRefresh() reads it
	// once a second and the value it leaves behind is at most a second old,
	// which is exactly the right resolution for something that takes 30 s to
	// cool down.
	// ---------------------------------------------------------------------
	// .59: the thermal veto was firing on every single frame
	// ---------------------------------------------------------------------
	//
	// It read `s_nTemperature >= s_nVetoCelsius`, and s_nTemperature is 0 until
	// my own BareSysInfoRefresh() first succeeds - which happens once a second,
	// and only from the frame loop.  So for the first second the comparison was
	// `0 >= 80`, which is false... and then the *firmware* is queried by
	// ReadTemperature() which returns 0 when the mailbox call fails, and 0
	// again passes.  That part is fine.
	//
	// What is not fine is the direction of the whole rule: on a Pi 4 idling at
	// 40-42 C, this never fires - while the board reported by the panel was one
	// whose clock never came down.  So the veto is not the cause of *that*, but
	// it was untestable: with 0 meaning both
	// "0 degrees" and "no reading", a firmware that answers nothing would look
	// like a board at absolute zero.  It is now explicit, and the governor
	// refuses to use a temperature it does not have.
	//
	// The real fix for "never comes down" is elsewhere - it was the level
	// bookkeeping, see the step-down rule below - but this had to be honest
	// first, otherwise the next person to read this would chase it again.
	if (s_nTemperature != 0 && s_nTemperature >= s_nVetoCelsius && s_nLevel > 0)
	{
		SetLevel (s_nLevel - 1, "temperatura");
		return;
	}

	const governor::TVerdict Verdict = governor::Judge (nFrameMicros, nBudgetMicros);
	s_nLastVerdict = Verdict;

	// The load, smoothed over about a second of frames (1/64 per frame), for
	// the menu: how much of each frame's time the work takes.
	{
		const unsigned nNow = nBudgetMicros != 0
			? static_cast<unsigned> (static_cast<u64> (nFrameMicros) * 1000 / nBudgetMicros) : 0;
		const unsigned nClamped = nNow > 2000 ? 2000 : nNow;
		s_nLoadPermille = s_nLoadPermille + (static_cast<int> (nClamped) - static_cast<int> (s_nLoadPermille)) / 64;
	}

	if (Verdict == governor::TVerdict::Overrun)
	{
		if (s_nOverrunFrames < 0xFFFF)
		{
			s_nOverrunFrames++;
		}
		s_nComfortFrames = 0;
	}
	else if (Verdict == governor::TVerdict::Slow)
	{
		if (s_nComfortFrames < 0xFFFF)
		{
			s_nComfortFrames++;
		}
		s_nOverrunFrames = 0;
	}
	else
	{
		// In between: neither evidence.  Not resetting is deliberate - one
		// borderline frame in the middle of a slow stretch should not undo it.
	}

	// Both reasons to go up have to be about *overrun*.  A frame count alone
	// does not work: a slow game presents fewer than 60 frames a second, so
	// "120 slow frames" can take ten seconds to arrive; and elapsed time alone
	// would escalate a machine that is comfortably idle.  Together they say "it
	// has been over budget for a while", which is the thing worth acting on.
	const u64 nElapsed = NowMicros () - s_nLevelSince;
	const boolean bSustainedOverrun =
		s_nOverrunFrames >= EscalateFrames
		|| (s_nOverrunFrames != 0 && nElapsed >= 3000000);

	// ---------------------------------------------------------------------
	// .58: why it never came down, and what the rule is now
	// ---------------------------------------------------------------------
	//
	// The old test was `s_nLevel > 1`, i.e. only levels 2 and 3 could ever step
	// down.  The governor climbs to level 3 as soon as a game overruns for two
	// seconds - which with an interpreter on a Pi 4 happens immediately - and
	// then, to come back, it needed 1800 comfortable frames *and* 30 s.  On a
	// short text screen the frames are already at the top of the budget, so
	// "comfortable" (under 75 %) almost never accumulates... and when the level
	// finally did try to drop, 3 -> 2 worked and 2 -> 1 could not.  The result
	// is exactly that: a clock that never comes down, and a board sitting at
	// 1800 MHz over a DOS prompt, heating up.
	//
	// The rule now: **any** level above the bottom may step down, the wait is
	// shorter, and the evidence is allowed to include "comfortable *and*
	// overrun" over a long enough window - because a level that is half
	// comfortable is a level the machine does not need.  Dropping costs 355 us;
	// staying high costs watts on every frame after that.
	//
	// Asymmetric on purpose, still: up in 2 s of overrun, down in 15 s of
	// comfort.  A governor that flaps is worse than one that is a level too
	// high, so down is slower - just not forbidden.
	if (s_nLevel + 1 < s_nLevels && bSustainedOverrun)
	{
		SetLevel (s_nLevel + 1, "no llega");
	}
	else if (s_nLevel > 0 && nElapsed >= StepDownMicros)
	{
		const boolean bComfortable = s_nComfortFrames >= ComfortFrames;
		const boolean bNotOverrunning = s_nOverrunFrames == 0;

		// Comfortable evidence, or a long enough window with no overrun at
		// all.  The second clause is what covers a mode whose frames sit
		// right at the budget (a 70 Hz text screen on the interpreter): it
		// never looks "comfortable" and it never overruns either, and that
		// used to be a permanent reason to stay at the top.
		if (bComfortable || (bNotOverrunning && nElapsed >= StepDownIdleMicros))
		{
			SetLevel (s_nLevel - 1, "va holgado");
		}
	}
}

void BareSysInfoRefresh (void)
{
	const u64 nNow = BareMicros ();
	if (s_nLastRefreshMicros != 0 && nNow - s_nLastRefreshMicros < RefreshMicros)
	{
		return;
	}

	s_nLastRefreshMicros = nNow;

	if (s_bProxy.load (std::memory_order_acquire))
	{
		s_nArmClockHz = s_nPubArmHz.load (std::memory_order_relaxed);
		s_nCoreClockHz = s_nPubCoreHz.load (std::memory_order_relaxed);
		s_nTemperature = s_nPubCelsius.load (std::memory_order_relaxed);
		s_nFirmwareMode = s_nPubFirmwareMode.load (std::memory_order_relaxed);
		return;
	}

	s_nArmClockHz = ReadClockRate (CLOCK_ID_ARM);
	s_nCoreClockHz = ReadClockRate (CLOCK_ID_CORE);
	s_nTemperature = ReadTemperature ();
	ReadFirmwareMode ();
}

void BareSysInfoGet (unsigned *pArmMHz, unsigned *pCoreMHz, unsigned *pCelsius)
{
	if (pArmMHz != nullptr)
	{
		*pArmMHz = s_nArmClockHz / 1000000;
	}

	if (pCoreMHz != nullptr)
	{
		*pCoreMHz = s_nCoreClockHz / 1000000;
	}

	if (pCelsius != nullptr)
	{
		*pCelsius = s_nTemperature;
	}
}

unsigned BareSysInfoGovernorLevel (void)
{
	return s_bGovernorReady ? s_nLevel : 0;
}

unsigned BareSysInfoGovernorLevels (unsigned *pMHz)
{
	for (unsigned i = 0; i < s_nLevels && pMHz != nullptr; i++)
	{
		pMHz[i] = s_Levels[i].nHz / 1000000;
	}

	return s_bGovernorReady ? s_nLevels : 0;
}

unsigned BareSysInfoFirmwareMode (void)
{
	return s_nFirmwareMode;
}

unsigned BareSysInfoThisCore (void)
{
	// Which core the caller is running on.  With a single threaded port this
	// is always 0, but the panel prints it so that the day a second task is
	// moved to another core, the number changes by itself.
	return CMultiCoreSupport::ThisCore ();
}

// ---------------------------------------------------------------------------
// Mailbox proxy - public side
// ---------------------------------------------------------------------------

void BareSysInfoSetMailboxProxy (boolean bOn)
{
	if (bOn)
	{
		// Seed the published values from core 0's cache, so that the panel
		// does not show zeros until the owner's first read.
		s_nPubArmHz.store (s_nArmClockHz, std::memory_order_relaxed);
		s_nPubCoreHz.store (s_nCoreClockHz, std::memory_order_relaxed);
		s_nPubCelsius.store (s_nTemperature, std::memory_order_relaxed);
		s_nPubFirmwareMode.store (s_nFirmwareMode, std::memory_order_relaxed);
		s_nServiceLastReadMicros = 0;
		s_bProxy.store (true, std::memory_order_release);
		return;
	}

	// Off: the caller guarantees the owner core no longer calls the service.
	// A clock change it had not applied yet is applied here, directly.
	s_bProxy.store (false, std::memory_order_release);
	const unsigned nPending = s_nClockRequestHz.exchange (0, std::memory_order_acq_rel);
	if (nPending != 0)
	{
		(void) MailboxSetArmClock (nPending);
	}
}

boolean BareSysInfoMailboxProxy (void)
{
	return s_bProxy.load (std::memory_order_acquire) ? TRUE : FALSE;
}

void BareSysInfoServiceTick (void)
{
	if (!s_bProxy.load (std::memory_order_acquire))
	{
		return;
	}

	const unsigned nPending = s_nClockRequestHz.exchange (0, std::memory_order_acq_rel);
	const u64 nNow = BareMicros ();

	if (nPending != 0)
	{
		(void) MailboxSetArmClock (nPending);

		// Read the result back 20 ms from now, not at once: the mailbox
		// reports the OLD rate for a while after a change (see the 20 ms in
		// BareSysInfoSetPerformance), and a read right after the change
		// published that old rate for a whole RefreshMicros - the panel
		// said 600 MHz for two seconds after the clock had gone up.
		s_nServiceLastReadMicros = nNow > RefreshMicros ? nNow - RefreshMicros + 20000 : 1;
		return;
	}

	if (s_nServiceLastReadMicros != 0 && nNow - s_nServiceLastReadMicros < RefreshMicros)
	{
		return;
	}
	s_nServiceLastReadMicros = nNow != 0 ? nNow : 1;

	s_nPubArmHz.store (ReadClockRate (CLOCK_ID_ARM), std::memory_order_relaxed);
	s_nPubCoreHz.store (ReadClockRate (CLOCK_ID_CORE), std::memory_order_relaxed);
	s_nPubCelsius.store (ReadTemperature (), std::memory_order_relaxed);

	CBcmPropertyTags Tags;
	TPropertyTagSimple Tag = {};
	if (Tags.GetTag (PROPTAG_GET_THROTTLED, &Tag, sizeof Tag, 4))
	{
		s_nPubFirmwareMode.store (Tag.nValue, std::memory_order_relaxed);
	}
}

void BareSysInfoSetMode (const char *pMode)
{
	s_nPinned = pMode == nullptr ? 0
		  : strcmp (pMode, "performance") == 0 ? 1
		  : strcmp (pMode, "powersave") == 0 ? 2 : 0;

	// Applied on the next governor tick (the next frame), which is also where
	// the temperature veto is checked first.
}

void BareSysInfoGovernorStats (TBareGovernorStats *pStats)
{
	if (pStats == nullptr)
	{
		return;
	}
	*pStats = TBareGovernorStats {};
	pStats->nLevels = s_bGovernorReady ? s_nLevels : 0;
	pStats->nLevel = s_nLevel;
	pStats->nUps = s_nUps;
	pStats->nDowns = s_nDowns;
	pStats->pLastReason = s_pLastReason;
	pStats->nSecondsSinceChange = s_nLastChangeMicros != 0
		? static_cast<unsigned> ((NowMicros () - s_nLastChangeMicros) / 1000000) : 0;
	pStats->nLoadPercent = s_nLoadPermille / 10;

	const u64 nNow = NowMicros ();
	u64 nTotal = 0;
	u64 Shares[8] = {};
	for (unsigned i = 0; i < s_nLevels && i < 8; i++)
	{
		Shares[i] = s_nLevelMicros[i];
		if (i == s_nLevel && s_nLevelSince != 0) Shares[i] += nNow - s_nLevelSince;
		nTotal += Shares[i];
	}
	for (unsigned i = 0; i < s_nLevels && i < 8; i++)
	{
		pStats->nMHz[i] = static_cast<unsigned> (s_Levels[i].nHz / 1000000);
		pStats->nPercentAt[i] = nTotal != 0 ? static_cast<unsigned> (Shares[i] * 100 / nTotal) : 0;
	}
}
