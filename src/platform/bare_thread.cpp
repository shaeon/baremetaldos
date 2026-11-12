//
// bare_thread.cpp
//
// by @mmoroca & arena.ai 2026  -  https://arena.ai
//
// BM-DOS - the single threaded replacement for DOSBox-core's
// emu_thread.cpp.
//
// DOSBox-core runs the emulator in a std::thread and ping-pongs control with
// the frontend thread through switchThread().  On a desktop std::thread is the
// OS.  On bare metal with circle-stdlib it is Circle's *cooperative,
// non-preemptive* scheduler, and a cooperative scheduler is a poor fit for
// that pattern: DOSBox's thread spins inside its own emulation loop and only
// yields at the end of a frame, so every hand-over becomes a scheduler round
// trip, and the shim's spinlock mode (useSpinlockThreadSync) has to be enabled
// to avoid deadlocks.
//
// Milestones 1 and 2 of this port run DOSBox on a *single* thread instead.
// That works because the two "threads" only hand control over at three well
// defined points, and all three can be expressed as a callback:
//
//   * the end of a frame, from RENDER_EndUpdate()      -> frame done
//   * a video mode change, from GFX_SetSize()          -> mode changed
//   * a fatal error, from E_Exit()                     -> abort
//
// So this file provides the *same* switchThread() symbol the shim calls, but
// instead of switching threads it services the frontend.  The emulation itself
// keeps running on the caller's stack - Circle's main task - and retro_run()
// consequently blocks for the duration of one emulated frame.
//
// IMPORTANT, and this cost a boot: dosbox-core does NOT honour that contract as
// written.  libretro.cpp starts the emulator with std::thread:
//
//     emu_thread = std::thread(start_dosbox, ...);
//     while (switchThread() != ThreadSwitchReason::VideoModeChange && !dosbox_exit);
//
// so once circle-stdlib's std::thread works (it needs a CScheduler, see
// kernel.h), the emulator runs in a Circle task, not on our stack.  From the
// main thread, switchThread() must then *actually give the CPU to that task*,
// or the wait above spins forever: it only returns when DOSBox reports a video
// mode change.  That is what m_seen[VideoModeChange] below is for - call
// CScheduler::Yield() until the emulator reaches its first mode change.
// That is exactly the contract the rest of the port is written against:
//
//     BareCoreRunFrame();      // one frame, video/audio already presented
//
// Note what is *not* here: no std::thread, no mutex, no condition variable.
// That also means no dependency on libc++'s threading support, which is the
// piece of circle-stdlib most likely to need care, and no risk of two threads
// touching DOSBox's non-reentrant globals.
//
// To go back to real threads (milestone 3, when the JIT wants the second
// core), build DOSBox-core's own emu_thread.cpp instead of this file - the
// makefile switch is DBC_SINGLE_THREADED=0 - and nothing else changes.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// NOTE: emu_thread.h is deliberately NOT included.  This file *replaces* that
// translation unit, and its header drags in <fmt/format.h> just to define a
// fmt::formatter for ThreadSwitchReason - which only the logging in the
// std::thread implementation ever uses.  All we need from it are the enum and
// the two function signatures, and they are reproduced here exactly.  If
// upstream ever changes them, the compiler will complain at the call sites in
// src/ (switchThread) and in libretro.cpp (useSpinlockThreadSync), which is the
// right place to find out.
#ifndef DBC_EMU_THREAD_H_REPLACED
#define DBC_EMU_THREAD_H_REPLACED
enum class ThreadSwitchReason
{
	None,
	VideoModeChange,
};

// Also from emu_thread.h, and for the same reason reproduced rather than
// included: the type the core's own shim throws to unwind its emulator task.
// dosbox-core's start_dosbox() catches it, so throwing it from here is how a
// task leaves DOSBox on purpose instead of merely stopping work - see the note
// on s_bExiting.  An empty struct, exactly as defined upstream.
struct EmuThreadCanceled
{
};
#endif

#include "bare_env.h"
#include "pace_policy.h"

#include <circle/sched/scheduler.h>
#include <circle/timer.h>
#include <circle/types.h>

// ---------------------------------------------------------------------------
// Interface with the rest of the port
// ---------------------------------------------------------------------------

namespace {

typedef void TBareFrameNotify (void *pParam, boolean bFrameComplete);

TBareFrameNotify *s_pNotify = nullptr;
void *s_pNotifyParam = nullptr;

boolean s_bAborted = FALSE;

// Number of times DOSBox has handed control back (see switchThread), and when
// that last happened.  This is the port's one honest measure of emulator
// progress: a frame can be presented from a stale buffer, but switchThread()
// only runs when DOSBox's own loop reaches a frame boundary.
unsigned s_nHandBacks = 0;
u64 s_nLastHandBackMicros = 0;

// Which side is calling, and the reason the emulator last gave.  Both live in
// static storage because switchThread() is called from two tasks.
// The Circle task that runs the frontend.  Cached the first time switchThread()
// is called, which is always from the main task (retro_load_game is called from
// kernel.cpp, and the emulator task does not exist until later in that same
// call).  Comparing task pointers is reliable here because CScheduler hands out
// stable CTask objects, and it needs no cooperation from libretro.cpp.
CTask *s_pMainTask = nullptr;
ThreadSwitchReason s_LastReason = ThreadSwitchReason::None;
boolean s_bLastReasonValid = FALSE;

// ---------------------------------------------------------------------------
// .65e: the exit that never finished
// ---------------------------------------------------------------------------
//
// On Ctrl+F9 the panel could say "flushing the card, then halt" and stay there
// for ever: the screen never went black and the board never halted.  This is
// why, and it is a race that only exists on shutdown:
//
//   dosbox-core's retro_deinit() sets frontend_exit = true and then joins its
//   own emulator thread.  That thread is inside switchThread(), which on this
//   port is the function below - and the branch it takes is "give the CPU back
//   and return at once, so DOSBox carries on emulating".  There is no reason
//   anywhere in that branch to stop.  So the emulator task keeps emulating for
//   ever, the join never returns, retro_deinit() never returns, kernel.cpp
//   never reaches BareAudioDrain(), and halt() is never called.
//
//   Worse, it is a *race*: whether the emulator lands in the wait (returns) or
//   in the drive path (keeps spinning) at that instant decides whether the
//   board shuts down or hangs.  A shutdown that works once and not the next
//   time is this.
//
// The fix is the flag below.  Once the port has decided to leave, every
// boundary takes the short path, and the emulator task parks itself in Yield()
// where the scheduler can keep serving it - it does not need to stop, it needs
// to stop *working*, which is a different and much easier thing to guarantee.
boolean s_bExiting = FALSE;

// Whose turn it is.  This is the part that took three boots to get right.
//
// The two sides have opposite needs and the same scheduler, so a single
// "is there a reason waiting" flag cannot serve both:
//
//   * the FRONTEND must be able to give the CPU away and be sure it will get
//     it back.  If the emulator has nothing to do it reports nothing, and a
//     frontend that waits for a report waits for ever.
//   * the EMULATOR must be able to report and then run.  If it waits for the
//     frontend to acknowledge - which is what this did at .45 - and the
//     frontend has already given up waiting and gone off to render, the
//     emulator sleeps until the frontend happens to call it again.  That is
//     not a hang, it is a *throttle*: the emulator advances one frame per
//     retro_run() at best, and only when the two sides happen to meet.  DOS's
//     clock (PIT 0, one interrupt every 1 ms of emulated time) then appears to
//     stop, and the machine looks frozen with the last picture still up.
//
// So: no acknowledgements at all.  Whoever has the CPU uses a counter to see
// that the other side got somewhere, then takes it back.
boolean s_bHandedOver = FALSE;      // frontend is part way through a Yield
boolean s_bEmulatorDriving = FALSE; // emulator is between report and the
				    // next time it gets the CPU (see below)

// Counters, so neither side has to guess.  Cleared once per frame by the
// frontend, read by the watchdog in kernel.cpp.
unsigned s_nHandBacksSinceReport = 0;   // DOSBox frame boundaries since then
unsigned s_nEmulatorYields = 0;         // CPU grants the emulator asked for

// Frame boundaries the emulator drove *by itself* - i.e. cases where it came
// back round to switchThread() without the frontend having asked for anything.
//
// This is the .57 diagnosis, and it is worth spelling out because the symptom
// ("everything runs fast, and cycles do not help") points at the wrong place.
// In DOSBox-core the frontend's own switchThread() wait is *unbounded*:
//
//     while (switchThread() != VideoModeChange && !dosbox_exit);
//
// so when the emulator returns to that loop on its own - which is what happens
// whenever it draws faster than the display refreshes, and again whenever
// render.updating is false so that RENDER_EndUpdate never switches - the
// frontend sits inside retro_run() while the emulator keeps running at full
// speed.  The frame loop in kernel.cpp only paces the calls it *makes*; it
// cannot pace calls the emulator makes on its own.  So the port has to hold
// the emulator here instead: if it came round on its own and its budget has
// not elapsed, it waits for what is left of the budget.
//
// Doing this unconditionally would be wrong in the other direction: a game
// that presents several times inside one real frame (mode changes, a redraw
// after a palette change) must be allowed to, or the port would invent a
// stutter.  So the wait is bounded by the frame budget and skipped while the
// frontend is inside its own wait (s_bHandedOver), which is the case where the
// frontend's own pacer is already in charge.
unsigned s_nFrameDrives = 0;
u64 s_nNextDriveMicros = 0;             // earliest time for an emulator-driven frame

// ---------------------------------------------------------------------------
// .65g: the two numbers that end the argument
// ---------------------------------------------------------------------------
//
// `pace` is the line's headline measurement and it cannot answer the question
// it looks like it answers.  It counts pictures that
// reached the video callback, so it says how many frames the emulator DREW - not
// how many times the emulator advanced, and not whether the two agree.
//
// What the panel can actually show, read strictly, is this: at a 70 Hz mode
// a line reading `pace 33/70 w0%` with `fr 29700us`.  If one retro_run() produced one frame,
// then 33 frames took 33 x 29.7 ms = 0.98 s, and the loop would have had to
// wait for nothing: those numbers are consistent with a machine that is simply
// too slow.  The machine is not necessarily too slow - the same board reaches
// `pace 70/70` - so the other possibility has to be measured, not argued:
// several frames per call, presented from inside the emulator, with the loop
// seeing one call's worth of time and crediting it with one frame.
//
//   nBoundaries   frame boundaries DOSBox reached per second - the rate at
//                 which the emulated machine advances.  This is what "the
//                 action is fast" means, and nothing else measures it.
//   nDrives       of those, the ones the frontend did NOT ask for: the
//                 emulator running on its own, which kernel.cpp's pacer cannot
//                 meter no matter where the wait is placed.
//
// The panel prints them next to `pace`, so the diagnosis becomes one reading:
// boundaries ~ presents  -> the loop is the metronome and `pace` is honest;
// boundaries > presents  -> the emulator is advancing faster than it draws;
// boundaries > the mode's own rate -> the machine is running fast, whoever is
// producing the pictures.
unsigned s_nBoundaries = 0;
unsigned s_nTrueDrives = 0;

// How long a frame is allowed to take, in microseconds, as the core last
// announced it (1e6 / the rate).  16666 until it says something: 60 Hz, which
// is what this port assumed everywhere before it started listening.
u64 s_nDriveBudgetMicros = 16666;

// The metronome (pace_policy.h), its trim from the sound queue in parts per
// million (set by kernel.cpp), and the time spent waiting on it - which the
// frame loop takes off what it gives the governor: waiting is not work.
barepace::TPacer s_Pacer;
s64 s_nPaceTrimPpm = 0;
u64 s_nPaceWaitMicros = 0;
unsigned s_nCallBoundaries = 0;	// boundaries since the frame loop last asked

// Diagnostic: how many times the frontend came back empty-handed.  The first
// five are reported on screen and then one in three hundred, so that a repeated
// failure does not bury the rest of the message.
unsigned s_nIdleReports = 0;

} // namespace

/// \brief Installed by bare_libretro.cpp: where to go when DOSBox yields.
extern "C" void BareThreadSetFrameNotify (TBareFrameNotify *pNotify, void *pParam)
{
	s_pNotify = pNotify;
	s_pNotifyParam = pParam;
}

/// \brief TRUE once DOSBox has called E_Exit (fatal error / "exit" command).
extern "C" boolean BareThreadAborted (void)
{
	return s_bAborted;
}

/// \brief How many times DOSBox has handed control back so far.
extern "C" unsigned BareThreadHandBacks (void)
{
	return s_nHandBacks;
}

/// \brief Milliseconds since DOSBox last handed control back, 0 if never.
extern "C" u64 BareThreadMicrosSinceHandBack (void)
{
	if (s_nLastHandBackMicros == 0)
	{
		return 0;
	}

	const u64 nNow = BareMicros ();

	return nNow > s_nLastHandBackMicros ? nNow - s_nLastHandBackMicros : 0;
}

/// \brief DOSBox frame boundaries since the last time this was called (the
///\t   frontend calls it once per frame).  Zero means DOSBox did not advance.
extern "C" unsigned BareThreadHandBacksSinceReport (void)
{
	const unsigned n = s_nHandBacksSinceReport;
	s_nHandBacksSinceReport = 0;

	return n;
}

/// \brief CPU grants the emulator has asked for since the last reset.
extern "C" unsigned BareThreadEmulatorYields (void)
{
	const unsigned n = s_nEmulatorYields;
	s_nEmulatorYields = 0;

	return n;
}

/// \brief TRUE while the emulator is between its last report and the next
///\t   time it is given the CPU.
extern "C" boolean BareThreadEmulatorDriving (void)
{
	return s_bEmulatorDriving;
}

/// \brief Called by E_Exit through the dosbox_exit flag path: record the abort.
extern "C" void BareThreadNoteAbort (void)
{
	s_bAborted = TRUE;
}

/// \brief "We are leaving: stop driving frames."  See s_bExiting above.
///
/// Idempotent and callable from either task, because it is called from the
/// frontend's shutdown path and read from the emulator's frame boundary.  It is
/// deliberately NOT the same flag as s_bAborted: an abort means the emulator
/// died and the frontend must report it, an exit means shutdown was asked for
/// and nothing went wrong.  Conflating them would make every clean shutdown
/// look like a crash in the log.
void BareThreadBeginExit (void)
{
	s_bExiting = TRUE;
}

boolean BareThreadExiting (void)
{
	return s_bExiting;
}

/// \brief After a DOS restart's unload: the state of a fresh boot again.  The
///\t   emulator task has ended (retro_deinit joined it), so nothing reads
///\t   these while they change.
void BareThreadResetForRestart (void)
{
	s_bExiting = FALSE;
	s_bAborted = FALSE;
	s_bHandedOver = FALSE;
	s_bEmulatorDriving = FALSE;
	s_LastReason = ThreadSwitchReason::None;
	s_bLastReasonValid = FALSE;
	s_nHandBacksSinceReport = 0;
	s_nFrameDrives = 0;
	s_nNextDriveMicros = 0;

	// "Microseconds since the emulator last handed back" must not count the
	// restart itself: 0 means "no hand-back yet", as at power-on.
	s_nLastHandBackMicros = 0;
	s_nIdleReports = 0;
	s_Pacer = barepace::TPacer {};
	s_nPaceWaitMicros = 0;
	s_nCallBoundaries = 0;
}

// ---------------------------------------------------------------------------
// The interface DOSBox-core's shim calls
// ---------------------------------------------------------------------------

auto switchThread (const ThreadSwitchReason reason) -> ThreadSwitchReason
{
	if (s_pMainTask == nullptr)
	{
		s_pMainTask = CScheduler::Get ()->GetCurrentTask ();
	}

	if (CScheduler::Get ()->GetCurrentTask () == s_pMainTask)
	{
		// ------------------------------------------------------------------
		// The frontend is asking.  Give the CPU to the emulator task and get
		// it back.  See the comment on s_bHandedOver for why there is no
		// acknowledgement: this side must never wait for a report, because a
		// working emulator has nothing to report most of the time.
		// ------------------------------------------------------------------
		const unsigned nHandBacks = s_nHandBacks;
		const unsigned nEmulatorYields = s_nEmulatorYields;

		s_bHandedOver = TRUE;

		// Bounded, and SHORT.  The bound is not for pacing and not for
		// patience: it is the point at which this side stops waiting and lets
		// the caller go round again.  Every microsecond spent waiting here is
		// a microsecond the emulator is not running, because this task holds
		// the CPU between yields.  50 ms is three video frames at 60 Hz -
		// enough for the emulator to reach a boundary on any frame this
		// machine can produce, and short enough that a slow emulator still
		// gets most of the CPU across successive calls.
		const u64 nDeadline = BareMicros () + 50000ULL;
		while (BareMicros () < nDeadline)
		{
			CScheduler::Get ()->Yield ();

			// Done when the emulator reported (there is something for the
			// caller to act on) or when it asked for the CPU but chose not to
			// report - both mean it ran.
			if (s_nHandBacks != nHandBacks || s_nEmulatorYields != nEmulatorYields)
			{
				break;
			}

			// Nothing happened: the emulator is not ready yet.  If it is not
			// between reports either, it will never run on its own, and
			// waiting is pure loss.
			if (!s_bEmulatorDriving)
			{
				break;
			}
		}

		s_bHandedOver = FALSE;

		if (!s_bLastReasonValid)
		{
			// No report: the emulator did not reach a frame boundary in time,
			// or it is not in a state where it can run at all.  Say so - a
			// silent "nothing" is what made three boots unreadable - and hand
			// the caller "nothing to do".  The frame loop of kernel.cpp treats
			// that as a frame and carries on, so the watchdog and the USB poll
			// keep running either way.  The first five times, and then one in
			// three hundred: an emulator that never advances must not fill the
			// screen with the same sentence.
			// Only worth a line while the overlay is on screen.  Once the
			// emulator has taken the display, a line written here would be
			// thrown away - and taking the display back is the watchdog's
			// decision, not this function's: an emulator that is merely slow
			// must not have its picture wiped once per frame.
			s_nIdleReports++;
			if ((s_nIdleReports <= 5 || s_nIdleReports % 300 == 0)
			    && BareVideoStatusIsEnabled ())
			{
				BareVideoStatusPrint ("core: switchThread timeout, no report");
			}

			return ThreadSwitchReason::None;
		}

		const ThreadSwitchReason reason_reported = s_LastReason;
		s_bLastReasonValid = FALSE;
		s_LastReason = ThreadSwitchReason::None;

		return reason_reported;
	}

	// ----------------------------------------------------------------------
	// The emulator is asking.  Record the reason, tell the frontend, and hand
	// the CPU over *once*, so that it can present the frame.  Then return at
	// once - do not wait for anything - so that DOSBox carries on emulating.
	// Whatever CPU time the frontend needs it takes by yielding, and this side
	// is back at the top of DOSBox's loop before the next frame boundary.
	// ----------------------------------------------------------------------

	// ---- .65e: leaving, so do not drive anything --------------------------
	// Placed before the drive logic on purpose: the drive path below is the
	// one that spends real time (up to a full frame budget per boundary) and
	// the one that would keep DOSBox running after the frontend has gone.
	// Record the boundary - the counters and the "the core is alive" timestamp
	// are read by the frontend while it waits, and a shutdown that stops
	// updating them looks exactly like the stall this file exists to tell
	// apart from a shutdown - and hand the CPU over without emulating further.
	if (s_bExiting)
	{
		++s_nHandBacks;
		++s_nHandBacksSinceReport;
		s_nLastHandBackMicros = BareMicros ();
		s_bEmulatorDriving = FALSE;
		++s_nEmulatorYields;

		// ------------------------------------------------------------------
		// .65f: yield, then LEAVE - do not come back
		// ------------------------------------------------------------------
		//
		// The first version of this branch returned to DOSBox after a Yield(),
		// on the reasoning that "the emulator does not have to stop, it has to
		// stop working".  That was wrong, and a screen showing it proves it:
		// the panel stopped at "shutting down: stopping the emulator", which is
		// the line kernel.cpp writes *before* BareCoreDeinit() - so the unload
		// had started and never returned.
		//
		// Why: dosbox-core's retro_deinit() sets frontend_exit and then joins
		// this task.  Returning into DOSBox sends it round its own loop again,
		// where it keeps taking frame boundaries and keeps landing here: a live
		// task that never terminates, which the join waits for for ever.  On a
		// cooperative scheduler the join needs this task to *end*, so the only
		// correct thing to do here is Yield() - to let the frontend run, since
		// it is the one that is about to join us - and then throw the exception
		// the core's own shim already expects for exactly this case.
		//
		// EmuThreadCanceled is what libretro.cpp's emu_thread.cpp raises when it
		// wants its thread to unwind (switchToMainWait does it when
		// frontend_exit is set), and start_dosbox() catches it.  This is that
		// same mechanism, driven by the port's own exit flag instead of by a
		// condition variable that this scheduler cannot block on.
		CScheduler::Get ()->Yield ();

		throw EmuThreadCanceled ();
	}

	// ---- .57: the frame the frontend never gets to pace -------------------
	// The frontend is in its own switchThread() wait only when it asked for
	// this frame.  If it is NOT waiting, this boundary came from the emulator
	// driving itself - and the frontend will not return from retro_run() at
	// all, so nothing in kernel.cpp can slow the machine down.  See the note
	// on s_nFrameDrives: that is the case this wait exists for.
	//
	// Two conditions, both cheap, both necessary:
	//   * the frontend is not inside its own Yield (then its pacer decides);
	//   * this is not a video mode change (that one must reach the frontend at
	//     once: it is how the frontend hears about a new mode).
	const boolean bDriven = !s_bHandedOver && reason != ThreadSwitchReason::VideoModeChange;

	// ---- one metronome for every boundary (pace_policy.h) ------------------
	// Every frame boundary waits for its time, whether the frontend asked for
	// it or the emulator reached it on its own.  It used to be only the
	// latter, against the previous one of its own: with two boundaries per
	// call of the frame loop, the one the loop asked for was never held, and
	// a game's 70 Hz mode ran at 77 frames a second (the panel on the board,
	// 12-nov) - 10 % fast, its surplus sound thrown away at a full queue.  The period is the mode's,
	// trimmed by the sound queue (BareThreadSetPaceTrim).  A video mode change
	// is not held: it is how the frontend hears about a new mode.
	if (reason != ThreadSwitchReason::VideoModeChange)
	{
		const u64 nPeriod = barepace::Trimmed (s_nDriveBudgetMicros, s_nPaceTrimPpm);
		const u64 nWait = barepace::Wait (s_Pacer, BareMicros (), nPeriod);
		if (nWait >= 1000)
		{
			// Below 1 ms there is nothing worth the call: SimpleusDelay()
			// polls a register and would spend more time entering than
			// waiting.  (The schedule still advances: the next boundary is
			// due one period after this one's due time.)
			CTimer::SimpleusDelay (static_cast<unsigned> (nWait));
			s_nPaceWaitMicros += nWait;
		}
	}
	s_nNextDriveMicros = s_Pacer.nNextDue;

	++s_nHandBacks;
	++s_nHandBacksSinceReport;
	++s_nBoundaries;
	++s_nCallBoundaries;
	s_nLastHandBackMicros = BareMicros ();

	// A drive is an emulator boundary the frontend did not ask for.  The old
	// counter incremented on *every* boundary and lived thirty lines below the
	// only place that knew the difference, so `pace ... +` - "the core is
	// driving itself" - was true of every frame, including the ones the loop
	// had just asked for.  A field that is always set says nothing, and this
	// one was read as evidence for two rounds.  Now it means what it says.
	if (bDriven)
	{
		++s_nTrueDrives;
	}

	s_LastReason = reason;
	s_bLastReasonValid = TRUE;
	s_bEmulatorDriving = FALSE;

	switch (reason)
	{
	case ThreadSwitchReason::VideoModeChange:
		// GFX_SetSize() got a new mode.  In this port the frame buffer is a
		// fixed mode and everything is scaled into it, so there is nothing to
		// reallocate - but the frontend may want to know (it clears the OSD,
		// for instance).
		if (s_pNotify != nullptr)
		{
			s_pNotify (s_pNotifyParam, /*frame complete*/ FALSE);
		}
		break;

	case ThreadSwitchReason::None:
	default:
		// End of an emulated frame.  This is the once-per-frame point where
		// the frontend gets to present, poll USB, service the OSD and run its
		// own bookkeeping.  Video and audio were already handed over inside
		// the frame through their callbacks.
		if (s_pNotify != nullptr)
		{
			s_pNotify (s_pNotifyParam, /*frame complete*/ TRUE);
		}
		break;
	}

	// Give the CPU back, unconditionally, once per frame boundary.  This is not
	// politeness and it is NOT optional: Circle's scheduler is cooperative and
	// this function is the emulator task's *only* yield point.  Skip it and the
	// emulator task never returns to the scheduler, so the frontend never
	// resumes: it stays inside its own Yield (or inside retro_run) while DOSBox
	// keeps emulating behind a picture nobody updates.  Making this conditional
	// on "the frontend is waiting" was tempting and wrong - the frontend may be
	// between calls, and then it never gets scheduled again.
	++s_nEmulatorYields;
	s_bEmulatorDriving = TRUE;
	CScheduler::Get ()->Yield ();

	// NO tope here (see the .57 note below), but a count, so the frontend can
	// tell "DOSBox is drawing faster than the display" from "DOSBox is
	// holding the CPU and the frontend is asleep".
	++s_nFrameDrives;

	// Echo the reason back.  The shim's call sites read the return value as
	// "what did the other side say"; there is no other side yet at this point,
	// so returning what we were given is the honest answer.
	return reason;
}

/// \brief The frame budget to hold emulator-driven frames to.  Fed from the
///\t   core's announced rate (by way of bare_video.cpp), so that a 70 Hz mode
///\t   is metered at 14.3 ms and not at 16.6.
extern "C" void BareThreadSetFrameBudget (unsigned nMicros)
{
	if (nMicros >= 5000 && nMicros <= 100000)
	{
		s_nDriveBudgetMicros = nMicros;

		// A budget change (a new video mode) invalidates the pending
		// deadline: the next frame is due one *new* budget from now.
		s_nNextDriveMicros = 0;
		s_Pacer = barepace::TPacer {};
	}
}

/// \brief Emulator-driven frames since the last call (the frontend calls it
///\t   once per frame, next to BareThreadHandBacksSinceReport).
extern "C" unsigned BareThreadFrameDrivesSinceReport (void)
{
	const unsigned n = s_nFrameDrives;
	s_nFrameDrives = 0;

	return n;
}

/// \brief Frame boundaries since the last call: how often the EMULATED MACHINE
///\t   advanced, which is the only direct measure of "va acelerado".
extern "C" unsigned BareThreadBoundariesSinceReport (void)
{
	const unsigned n = s_nBoundaries;
	s_nBoundaries = 0;

	return n;
}

/// \brief Of those boundaries, how many the frontend did not ask for.  Zero
///\t   means kernel.cpp's pacer is the only metronome there is.
extern "C" unsigned BareThreadTrueDrivesSinceReport (void)
{
	const unsigned n = s_nTrueDrives;
	s_nTrueDrives = 0;

	return n;
}

/// \brief Microseconds until the emulator's next frame is due, 0 if not yet
///\t   known.  For the panel: a number that keeps coming back as "0" while
///\t   the picture moves means the emulator is not being metered at all.
extern "C" u64 BareThreadNextDriveMicros (void)
{
	return s_nNextDriveMicros;
}

void useSpinlockThreadSync (bool /*use_spinlock*/)
{
	// No threads, no locks.  A no-op rather than an assert: the shim calls
	// this whenever a core option changes, and aborting there would be a worse
	// outcome than ignoring it.
}

/// \brief The metronome's trim, in parts per million of the frame period, from
///\t   the sound queue's fill (barepace::TrimPpm); positive slows the machine.
extern "C" void BareThreadSetPaceTrim (s64 nPpm)
{
	s_nPaceTrimPpm = nPpm;
}

/// \brief Microseconds the emulator spent waiting on the metronome since the
///\t   last call.
extern "C" u64 BareThreadTakePaceWaitMicros (void)
{
	const u64 n = s_nPaceWaitMicros;
	s_nPaceWaitMicros = 0;
	return n;
}

/// \brief Frame boundaries since the last call - how many emulated frames the
///\t   frame loop's last call contained (one, two, sometimes none).
extern "C" unsigned BareThreadTakeCallBoundaries (void)
{
	const unsigned n = s_nCallBoundaries;
	s_nCallBoundaries = 0;
	return n;
}
