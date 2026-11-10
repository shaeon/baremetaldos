//
// bare_cores.cpp
//
// BM-DOS - the secondary cores, started once and handed work by name.
//
// Circle allows exactly one CMultiCoreSupport, and its Run(nCore) is the only
// way into cores 1..3.  Before this file the MIDI synthesizer owned that
// object, which left no way for anything else (the video presenter on core 2)
// to get a core.  Now one object starts all three cores early, and each core
// parks until a job is assigned to it:
//
//   core 0   DOSBox, the frontend, interrupts (Circle serves IRQs here only)
//   core 1   MIDI synthesizers                 (bare_midi.cpp)
//   core 2   video presentation + mailbox      (planned)
//   core 3   free
//
// A parked core sleeps in WFE, not in a delay loop.  SimpleMsDelay() is a busy
// wait, and three cores spinning at full clock are three cores heating the SoC
// whose temperature the governor on core 0 is reading.  BareCoresRun() stores
// the job and then executes SEV, which wakes every core waiting in WFE; the
// woken core re-reads its slot and either runs the job or goes back to sleep.
// A spurious wake-up costs one load.
//
// Starting the secondary cores also turns Circle's spin locks from no-ops into
// real locks (CSpinLock::Enable).  That is wanted: the heap, the mailbox and
// FatFs are spin-locked, and with more than one core running they must be.
//
// Shaeon (Carlos Palmero) 2026, for BM-DOS by @mmoroca.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
#include "bare_env.h"

#include <circle/logger.h>
#include <circle/memory.h>
#include <circle/multicore.h>
#include <circle/synchronize.h>
#include <circle/timer.h>

#include <atomic>

static const char FromCores[] = "cores";

namespace {

constexpr unsigned FirstCore = 1;
constexpr unsigned LastCore = 3;
constexpr unsigned Cores = LastCore + 1;

// One job per core.  Written once by core 0 (nullptr -> job), read by the
// core it belongs to.  A job never returns; if it does, the core parks again
// and the slot is cleared so that it reads as "not running".
std::atomic<TBareCoreJob> s_Job[Cores];

// Set by a core when it has taken its job, so that core 0 can tell "assigned"
// from "actually running".
std::atomic<bool> s_bRunning[Cores];

// Set by each secondary core as soon as it enters Run(), parked or not.
std::atomic<bool> s_bAlive[Cores];

// Circle's own WaitForEvent() / SendEvent() (circle/synchronize.h) are bare
// "wfe" / "sev".  SEV gets a DSB in front here: the job pointer must be
// visible to the other core before it is woken to read it.
inline void WakeAll (void)
{
	DataSyncBarrier ();
	SendEvent ();
}

class CBareCores : public CMultiCoreSupport
{
public:
	CBareCores (void) : CMultiCoreSupport (CMemorySystem::Get ()) {}

	void Run (unsigned nCore) override
	{
		if (nCore < FirstCore || nCore > LastCore)
		{
			return;
		}

		s_bAlive[nCore].store (true, std::memory_order_release);

		for (;;)
		{
			const TBareCoreJob pJob = s_Job[nCore].load (std::memory_order_acquire);
			if (pJob == nullptr)
			{
				WaitForEvent ();
				continue;
			}

			s_bRunning[nCore].store (true, std::memory_order_release);
			pJob ();			// normally never returns

			s_bRunning[nCore].store (false, std::memory_order_release);
			s_Job[nCore].store (nullptr, std::memory_order_release);
		}
	}
};

CBareCores *s_pCores = nullptr;

} // namespace

boolean BareCoresInit (void)
{
	if (s_pCores != nullptr)
	{
		return TRUE;
	}

	for (unsigned i = 0; i < Cores; i++)
	{
		s_Job[i].store (nullptr, std::memory_order_relaxed);
		s_bRunning[i].store (false, std::memory_order_relaxed);
		s_bAlive[i].store (false, std::memory_order_relaxed);
	}

	s_pCores = new CBareCores;
	if (s_pCores == nullptr || !s_pCores->Initialize ())
	{
		CLogger::Get ()->Write (FromCores, LogWarning,
					"secondary cores did not start: MIDI and core 2 video off");
		delete s_pCores;
		s_pCores = nullptr;
		return FALSE;
	}

	// Initialize() returns once the cores have been released; give them a
	// moment to reach Run() so that BareCoresAlive() means something.
	for (unsigned nWait = 0; nWait < 100; nWait++)
	{
		bool bAll = true;
		for (unsigned i = FirstCore; i <= LastCore; i++)
		{
			bAll = bAll && s_bAlive[i].load (std::memory_order_acquire);
		}
		if (bAll)
		{
			break;
		}
		CTimer::SimpleMsDelay (1);
	}

	unsigned nAlive = 0;
	for (unsigned i = FirstCore; i <= LastCore; i++)
	{
		nAlive += s_bAlive[i].load (std::memory_order_acquire) ? 1 : 0;
	}
	CLogger::Get ()->Write (FromCores, LogNotice, "%u secondary cores parked", nAlive);

	return nAlive != 0 ? TRUE : FALSE;
}

boolean BareCoresAlive (unsigned nCore)
{
	return nCore >= FirstCore && nCore <= LastCore
	       && s_bAlive[nCore].load (std::memory_order_acquire) ? TRUE : FALSE;
}

boolean BareCoresRun (unsigned nCore, TBareCoreJob pJob)
{
	if (pJob == nullptr || !BareCoresAlive (nCore))
	{
		return FALSE;
	}

	TBareCoreJob pNone = nullptr;
	if (!s_Job[nCore].compare_exchange_strong (pNone, pJob, std::memory_order_acq_rel))
	{
		return FALSE;		// that core already has a job
	}

	WakeAll ();

	// Wait (bounded) until the core has picked it up, so that the caller can
	// rely on the job running when this returns TRUE.
	for (unsigned nWait = 0; nWait < 1000; nWait++)
	{
		if (s_bRunning[nCore].load (std::memory_order_acquire))
		{
			return TRUE;
		}
		CTimer::SimpleusDelay (100);
	}

	CLogger::Get ()->Write (FromCores, LogWarning, "core %u did not take its job", nCore);
	return FALSE;
}

void BareCoresWait (void)
{
	WaitForEvent ();
}

void BareCoresWake (void)
{
	WakeAll ();
}

boolean BareCoresIsRunning (unsigned nCore)
{
	return nCore < Cores && s_bRunning[nCore].load (std::memory_order_acquire) ? TRUE : FALSE;
}
