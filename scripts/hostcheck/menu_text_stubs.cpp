//
// menu_text_stubs.cpp - the board-side functions the setup menu calls, for
// menu_text_test.cpp: each returns the LONGEST plausible value, so that the
// test sees the longest strings the board can put on the menu.
//
// Shaeon (Carlos Palmero) 2026, for BM-DOS by @mmoroca.  GPL-3.0-or-later.
//
#include "../../src/platform/bare_env.h"
#include <circle/logger.h>
#include <circle/memory.h>
#include <circle/sched/scheduler.h>
#include <circle/timer.h>
#include <circle/font.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
const char *BareAudioDeviceName (void) { return "PWM (jack)"; }
void BareAudioQueue (const s16 *, unsigned) {}
unsigned BareAudioSampleRate (void) { return 44100; }
void BareAudioSetVolume (unsigned) {}
void BareCoreReleaseAllKeys (void) {}
void BareCoreRequestDosRestart (void) {}
void BareCoreRequestReboot (void) {}
void BareCoreRequestShutdown (void) {}
boolean BareCoresIsRunning (unsigned) { return TRUE; }
boolean BareDiskAvailable (void) { return TRUE; }
boolean BareDiskEject (char) { return TRUE; }
boolean BareDiskInsert (const char *) { return TRUE; }
boolean BareInputCapsLock (void) { return FALSE; }
unsigned BareInputGamepads (void) { return 2; }
void BareInputSetMouseSpeed (unsigned) {}
u64 BareMicros (void) { return 123456789012ULL; }
boolean BareMidiActive (void) { return TRUE; }
const char *BareMidiName (void) { return "fluidsynth"; }
void BareMidiSendTestChord (boolean) {}
void BareMidiSetVolume (unsigned) {}
const char *BareMidiStatus (void) { return "fluidsynth: /dosbox/system/GeneralUser-GS.sf2 ready"; }
void BareNetInfo (TBareNetInfo *p) { std::memset (p, 0, sizeof *p); std::strcpy (p->IP, "192.168.100.200"); std::strcpy (p->Mask, "255.255.255.0"); std::strcpy (p->Gateway, "192.168.100.254"); std::strcpy (p->DNS, "192.168.100.254"); std::strcpy (p->MAC, "dc:a6:32:12:34:56"); std::strcpy (p->Hostname, "bmdos"); }
void BareNetLocalTime (char *p, unsigned n) { std::snprintf (p, n, "12/11/2026 23:59:59 CEST"); }
TBareNetMode BareNetMode (void) { return BareNetWiFi; }
boolean BareNetReady (void) { return TRUE; }
const char *BareNetState (void) { return "WiFi: conectada a MiRedWiFi_5G_Casa"; }
unsigned BareNetWiFiNetworks (TBareWiFiNetwork *p, unsigned n) { if (n == 0) return 0; std::strcpy (p[0].SSID, "MOVISTAR_PLUS_A1B2_5G_EXTENDER"); p[0].nRSSI = -88; p[0].bSecure = TRUE; return 1; }
const char *BareSysInfoBoardName (void) { return "Raspberry Pi 400"; }
unsigned BareSysInfoFirmwareMode (void) { return 0x10000; }
void BareSysInfoGet (unsigned *a, unsigned *b, unsigned *c) { if (a) *a = 1500; if (b) *b = 500; if (c) *c = 85; }
unsigned BareSysInfoGovernorLevels (unsigned *p) { p[0] = 600; p[1] = 1000; p[2] = 1500; return 3; }
const char *BareSysInfoGovernorState (void) { return "L2/2 busy over65535"; }
void BareSysInfoGovernorStats (TBareGovernorStats *p) { std::memset (p, 0, sizeof *p); p->nLevels = 3; p->nMHz[0] = 600; p->nMHz[1] = 1000; p->nMHz[2] = 1500; p->nPercentAt[0] = 33; p->nPercentAt[1] = 33; p->nPercentAt[2] = 34; p->nUps = 1234; p->nDowns = 1234; p->pLastReason = "arranque del governor"; p->nSecondsSinceChange = 86400; p->nLoadPercent = 199; }
void BareSysInfoRefresh (void) {}
void BareSysInfoSetMode (const char *) {}
void BareVideoModeSize (unsigned *w, unsigned *h) { *w = 1920; *h = 1080; }
void BareVideoPresent (const u32 *, unsigned, unsigned, unsigned) {}
void BareVideoPresenterStats (TBareVideoPresenterStats *p) { std::memset (p, 0, sizeof *p); p->bRunning = TRUE; p->bVsync = TRUE; p->nShown = 123456789; }
void BareVideoRestoreScreen (void) {}
void BareVideoSaveScreen (void) {}
void BareVideoSetSharpScaling (boolean) {}
void BareVideoFooterSetVisible (boolean) {}
boolean BareVideoSharpScaling (void) { return TRUE; }
boolean BareWebRunning (void) { return TRUE; }
static CLogger s_Logger;
CLogger *CLogger::Get (void) { return &s_Logger; }
void CLogger::Write (const char *, TLogSeverity, const char *, ...) {}
static CMemorySystem s_Mem;
CMemorySystem *CMemorySystem::Get (void) { return &s_Mem; }
u64 CMemorySystem::GetMemSize (void) const { return 4096ULL << 20; }
int32_t CPU_CycleMax = 600000;
bool CPU_CycleAutoAdjust = false;
static CScheduler s_Sched;
CScheduler *CScheduler::Get (void) { return &s_Sched; }
void CScheduler::Yield (void) {}
void CScheduler::MsSleep (unsigned) {}
void CTimer::SimpleMsDelay (unsigned) {}
const TFont Font8x16 = { 8, 16, 0, 0x21, 0xFF, nullptr };

// The profiler: the longest plausible channel list.
void BareProfileGet (TBareProfile *p)
{
	*p = TBareProfile {};
	p->bValid = TRUE;
	p->nCpu = 1000; p->nSound = 1000; p->nVideo = 1000; p->nAudio = 1000;
	p->nInput = 1000; p->nWait = 1000; p->nOther = 1000;
	static const char *const Names[] = { "FM", "SB", "GUS", "SPKR", "TANDY", "DISNEY", "CMS", "PS1DAC" };
	p->nChannels = 8;
	for (unsigned i = 0; i < 8; i++) { p->pChannelName[i] = Names[i]; p->nChannel[i] = 1000; }
}
