//
// profile_test.cpp - host test for src/platform/profile_core.h (make check-profile)
// Shaeon (Carlos Palmero) 2026, for BM-DOS by @mmoroca.  GPL-3.0-or-later.
//
#include "../../src/platform/profile_core.h"

#include <cstdio>

using namespace bareprofile;

static unsigned s_nFailures = 0;
static void Check (bool b, const char *p)
{
	std::printf ("  %s  %s\n", b ? "ok    " : "FAILED", p);
	if (!b) s_nFailures++;
}

int main (void)
{
	static const char FM[] = "FM", SB[] = "SB";
	CProfile P;
	P.Tick (1000, 1000);			// starts the window
	// One second (1000 ticks): the call took 800, of which 100 waiting,
	// 50 video, 30 audio, 40 input, FM 120, SB 20 -> x86 CPU 440.
	P.Add (PartCall, 800); P.Add (PartWait, 100); P.Add (PartVideo, 50); P.Add (PartAudio, 30);
	P.AddChannel (FM, 70); P.AddChannel (SB, 20); P.AddChannel (FM, 50);
	P.Add (PartInput, 40);
	P.Tick (1500, 1000);
	Check (P.Last ().nWindowTicks == 0, "no snapshot before the window ends");
	P.Tick (2000, 1000);
	const TSnapshot &S = P.Last ();
	Check (S.nWindowTicks == 1000, "a one-second window closes");
	Check (S.nChannels == 2 && S.Channels[0] == 120 && S.Channels[1] == 20,
	       "a channel's times add up, each channel once (by its name)");
	Check (S.Cpu () == 440, "x86 CPU = the call less the waits, video, audio, input and sound inside it");
	Check (S.Permille (S.Sound ()) == 140 && S.Permille (S.Cpu ()) == 440, "shares of the window, in tenths of a percent");
	Check (P.Last ().Parts[PartCall] == 800, "the snapshot keeps the window's figures");

	P.Add (PartCall, 999);
	P.Restart (2100);
	P.Tick (3100, 1000);
	Check (P.Last ().Parts[PartCall] == 0 && P.Last ().nChannels == 2,
	       "a pause drops the window in progress; the channel names stay");

	CProfile Q;
	Q.Tick (0, 10);
	Q.Add (PartCall, 5); Q.Add (PartWait, 9);
	Q.Tick (5, 10); Q.Tick (20, 10);
	Check (Q.Last ().nWindowTicks == 20 && Q.Last ().Cpu () == 0,
	       "a window may start at tick 0; waits larger than the call give 0, not a wrap-around");
	std::printf ("\n%u failures\n", s_nFailures);
	return s_nFailures == 0 ? 0 : 1;
}
