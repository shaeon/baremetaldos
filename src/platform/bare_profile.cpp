//
// bare_profile.cpp
//
// BM-DOS - core 0's time, in one-second windows (profile_core.h): the port's
// own parts, timed where they run, and DOSBox's sound channels, timed in
// MixerChannel::Mix through the weak hooks of
// patches/dosbox-core/mixer-profile.patch.  Everything here runs on core 0 -
// the frame loop and the emulator thread take turns on it - so no locking.
//
// The clock is the ARM generic timer's count (CNTPCT_EL0): 54 MHz on a Pi 4,
// 19 ns a tick, read in a couple of cycles; a sound channel's handler takes
// a few microseconds a call, too short for BareMicros().
//
// Shaeon (Carlos Palmero) 2026, for BM-DOS by @mmoroca.  GPL-3.0-or-later.
//
#include "bare_env.h"
#include "profile_core.h"

namespace {

bareprofile::CProfile s_Profile;
u64 s_nFrequency = 0;

inline u64 Count (void)
{
	u64 n;
	asm volatile ("isb; mrs %0, cntpct_el0" : "=r" (n));
	return n;
}

u64 Frequency (void)
{
	if (s_nFrequency == 0)
	{
		u64 n;
		asm volatile ("mrs %0, cntfrq_el0" : "=r" (n));
		s_nFrequency = n != 0 ? n : 54000000;
	}
	return s_nFrequency;
}

} // namespace

// The hooks DOSBox's mixer calls (mixer-profile.patch).  Their declarations
// there are weak; these definitions make them real.
unsigned long long BARE_ProfileClock (void)
{
	return Count ();
}

void BARE_ProfileChannel (const char *pName, unsigned long long nTicks)
{
	s_Profile.AddChannel (pName, nTicks);
}

u64 BareProfileClock (void)
{
	return Count ();
}

void BareProfileAdd (unsigned nPart, u64 nTicks)
{
	if (nPart < bareprofile::PartCount)
	{
		s_Profile.Add (static_cast<bareprofile::TPart> (nPart), nTicks);
	}
}

void BareProfileAddMicros (unsigned nPart, u64 nMicros)
{
	BareProfileAdd (nPart, nMicros * Frequency () / 1000000);
}

void BareProfileTick (void)
{
	s_Profile.Tick (Count (), Frequency ());
}

void BareProfileRestart (void)
{
	s_Profile.Restart (Count ());
}

void BareProfileGet (TBareProfile *pOut)
{
	if (pOut == nullptr)
	{
		return;
	}
	*pOut = TBareProfile {};
	const bareprofile::TSnapshot &S = s_Profile.Last ();
	if (S.nWindowTicks == 0)
	{
		return;				// no complete window yet
	}
	pOut->bValid = TRUE;
	pOut->nCpu = S.Permille (S.Cpu ());
	pOut->nSound = S.Permille (S.Sound ());
	pOut->nVideo = S.Permille (S.Parts[bareprofile::PartVideo]);
	pOut->nAudio = S.Permille (S.Parts[bareprofile::PartAudio]);
	pOut->nInput = S.Permille (S.Parts[bareprofile::PartInput]);
	pOut->nWait = S.Permille (S.Parts[bareprofile::PartWait]);
	const u64 nCall = S.Parts[bareprofile::PartCall];
	pOut->nOther = nCall < S.nWindowTicks ? S.Permille (S.nWindowTicks - nCall) : 0;
	pOut->nChannels = S.nChannels < 8 ? S.nChannels : 8;
	for (unsigned i = 0; i < pOut->nChannels; i++)
	{
		pOut->pChannelName[i] = S.ChannelNames[i] != nullptr ? S.ChannelNames[i] : "?";
		pOut->nChannel[i] = S.Permille (S.Channels[i]);
	}
}
