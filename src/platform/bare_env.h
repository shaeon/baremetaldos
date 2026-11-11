//
// bare_env.h
//
// by @mmoroca & arena.ai 2026  -  https://arena.ai
//
// BM-DOS - host interface used by the rest of the port.
//
// This is the only header the frontend (kernel) and the platform layers share.
// Everything DOSBox-core expects from its host goes through the libretro API,
// so the port is really "a libretro frontend that happens to be a Circle
// kernel" plus a thin video/audio/input backend.  Keeping that boundary clean
// is what makes the next milestones (core 1 presentation, JIT, menus) local
// changes instead of rewrites.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
#ifndef _bare_env_h
#define _bare_env_h

// ---------------------------------------------------------------------------
// The contract between the kernel and the platform layer
// ---------------------------------------------------------------------------
//
// Every function below is implemented in one file of src/platform/, and this
// header is the whole of what "the application" (kernel.cpp: the frame loop, the
// watchdog, the panel) is allowed to know about "the machine" (DOSBox, USB, the
// frame buffer, the sound device).
//
// Two rules give the split its value, and both are worth keeping if you adapt
// this:
//
//  1. Decisions live below this line.  Key bindings, sound device priority,
//     panel contents - all of it is inside the bare_*.cpp files, so changing
//     platform behaviour never means touching the kernel.  The kernel asks;
//     the platform decides.
//
//  2. No DOSBox types cross it.  The libretro shim is bare_libretro.cpp and its
//     retro_* types stop at its door.  The one exception is the pair of
//     push/pull functions at the bottom shared with the input layer, and they
//     use plain integers for exactly that reason.
//
// The sections are ordered the way a boot uses them: core, video, audio, input,
// SoC information, time.
// ---------------------------------------------------------------------------

#include <circle/types.h>

// Circle's types.h gives u8..u64 and s8..s64 and nothing else - no <cstdint>,
// no int16_t.  The one function below whose signature is specified by libretro
// (an analogue axis is "int16") therefore cannot use its type name in this
// header, or every file that includes it fails to compile with
// "'int16_t' does not name a type" - which is exactly what happened on the
// first .60 build.  s16 is Circle's own name for the same type, so the header
// uses s16 and the definitions in bare_input.cpp and the call site in
// bare_libretro.cpp cast explicitly.  Do not "fix" this by including <cstdint>
// here: the port's headers stay free of libstdc++/libc++ dependencies, which is
// what lets them be compiled against the stand-ins.

class CInterruptSystem;

// ---------------------------------------------------------------------------
// Core lifecycle - implemented in bare_libretro.cpp
// ---------------------------------------------------------------------------

/// \brief Bring the frontend side of the core up.  Video, audio and input
///	   backends must be initialised first.
/// \return FALSE on failure
boolean BareCoreInit (void);

/// \brief Tell the core what to boot.
/// \param pImagePath  Path on the SD card: a directory (mounted as C:), a
///		      ".conf" (boot the shell with that config), or a disk
///		      image (.iso/.img).
/// \return FALSE on failure
boolean BareCoreLoadGame (const char *pImagePath);

/// \brief Run exactly one emulated frame and present it.
void BareCoreRunFrame (void);

/// \brief Tell the thread layer that the port is leaving.
///
/// Until this is called, every frame boundary inside the emulator hands the CPU
/// back and lets DOSBox carry on - which is correct while there is a frontend
/// and wrong once there is not.  dosbox-core's retro_deinit() joins its own
/// emulator thread, so a shutdown that does not say this waits for a thread that
/// has no reason to ever stop: the board hangs with the "flushing the card, then
/// halt" line on screen and never halts.  Call it before unloading the core.
///
/// One way only: there is no "resume", because the only caller is a shutdown.
void    BareThreadBeginExit (void);
boolean BareThreadExiting (void);

/// \brief Did the core ask to quit ("exit" in DOS, E_Exit, panic)?
boolean BareCoreWantsExit (void);

/// \brief TRUE while the emulated machine is paused (Alt+Pause).
///\t   The frame loop stops calling retro_run() and stops pacing; nothing is
///\t   reset, so resuming continues from the same emulated instant.
boolean BareCorePaused (void);

/// \brief Hook called at the end of every frame, from the frame thread.
typedef void TBareFrameHook (void *pParam);
void BareCoreSetFrameHook (TBareFrameHook *pHook, void *pParam);

/// \brief Tear the core down.
/// \brief retro_unload_game() + retro_deinit(), with a bound and a log line.
///\t   Joins the core's own emulator task, so it needs BareThreadBeginExit()
///\t   to have been called first: without it the join waits for ever.
void BareCoreDeinit (void);

// ---------------------------------------------------------------------------
// Video - bare_video.cpp
// ---------------------------------------------------------------------------

/// \brief Allocate the HDMI frame buffer (fixed mode, never resized).
boolean BareVideoInit (unsigned nWidth, unsigned nHeight);
void    BareVideoDeinit (void);

/// \brief Present one XRGB8888 frame, scaled to the HDMI mode.
void    BareVideoPresent (const u32 *pPixels, unsigned nWidth, unsigned nHeight,
			  unsigned nPitchBytes);

/// \brief OSD text (used for the core's own notices).
void    BareVideoShowMessage (const char *pText);

/// \brief Put a single line of status text on the frame buffer, at the top,
///\t   in the built-in font.  Once the frame buffer exists this is the only
///\t   output that is visible: Circle's screen logger writes into the
///\t   firmware console buffer, which is no longer what the display scans
///\t   out.  Safe to call with the frame buffer not yet initialised (it
///\t   does nothing then).
void    BareVideoStatus (unsigned nLine, const char *pText);

/// \brief Wipe the visible page and start the status text again from the
///\t   first line.
void    BareVideoStatusReset (void);

/// \brief TRUE while the overlay is drawing (i.e. the emulator has not taken
///\t   the screen yet, or the watchdog has taken it back).
boolean BareVideoStatusIsEnabled (void);

/// \brief Take the screen back from the emulator for the status overlay.
///
/// The overlay steps aside on the first real frame (the emulator owns the
/// display from then on).  If frames stop arriving, this brings it back, so a
/// stalled emulator does not leave a frozen picture and nothing else.
void    BareVideoStatusEnable (void);

/// \brief Frames presented since boot, and how long ago the last one was.
///
/// The frame loop uses these to notice that the emulator has stopped producing
/// output: a frozen picture with no message is the one diagnostic this port
/// cannot afford.
u64     BareVideoFramesPresented (void);
u64     BareVideoMicrosSinceLastFrame (void);

/// \brief Frames that were not identical to the one before them.
///
/// The companion of BareVideoFramesPresented(), and the difference between
/// them is what "idle" means on this machine.  A DOS prompt presents seventy
/// frames a second and changes none of them: presenting is running, changing is
/// working.  The clock governor uses this one, because "nothing has been
/// presented for a second" never becomes true on a text screen and a governor
/// that waits for it can only ever stay at the top.
u64     BareVideoFramesChanged (void);

/// \brief Write the visible page to SD:/DOSBOX/SHOTnnnn.BMP (24-bit BMP, no
///\t   library needed).  Bound to Ctrl+F5.  Returns TRUE if a file was
///\t   written and closed successfully.
boolean BareVideoScreenshot (void);

/// \brief Show or hide the bottom status line.  Hidden at boot; the PetSis key
///\t   (ImpPt / Print Screen) toggles it.  Hiding it also clears the strip,
///\t   because the emulator never repaints the part of the screen its own
///\t   picture does not cover.
void    BareVideoFooterSetVisible (boolean bVisible);
boolean BareVideoFooterVisible (void);

/// \brief One line of status text, ON TOP of the emulator's picture.
///\t   nRow is the line slot (0 = the panel, 1 = a message from the core);
///\t   the name is historical - these used to be drawn below the picture.
///
///\t   The text is stored in the slot and repainted on every presented frame,
///\t   which is what keeps it above a screen the emulator rewrites.  Erasing a
///\t   slot (pText == nullptr) also repaints the picture underneath from the
///\t   last source frame - the emulator never drew there, so it cannot.
void    BareVideoFooter (unsigned nRow, const char *pText);

/// \brief Draw (or erase, with pText == nullptr) one line of the top-right
///\t   corner.  Slot 0 is the status panel and follows BareVideoFooterSetVisible;
///\t   slot 1 is a transient message from the core.  Both remember the rectangle
///\t   they drew and erase exactly that, which is what makes PetSis and the
///\t   message timeout leave a clean screen behind.
void    BareVideoLine (unsigned nSlot, const char *pText);

/// \brief The rate the core is emulating at, in millihertz.  0 = it has not said.
///
/// One thing writes it: \ref BareVideoSetRefreshHz, from the core's own
/// announcement (retro_system_av_info::timing::fps), which is the mode's real
/// rate.  There is no second writer any more - .65 added a "pin" that could
/// lower it, and .65g removed it because it only ever made the emulated machine
/// slower without ever making it faster (see bare_coreopts.h).
///
/// The frame loop paces against this, not against a hard-coded 60: with
/// RUN_SYNC the core returns from retro_run() once per emulated frame, so a
/// 70 Hz text mode returns 70 times a second and pacing it at 60 makes the
/// machine run 17 % fast - which no amount of "cycles" can fix, because cycles
/// decide how much work fits in a frame, not how many frames happen per second.
unsigned BareVideoRefreshMilliHz (void);

/// \brief Ask the core what rate its current mode refreshes at, and apply it.
///
/// A wrapper rather than a direct call from kernel.cpp, for one reason worth a
/// line: dosbox-core announces its rate with SET_SYSTEM_AV_INFO **only when the
/// rate changes**, so a machine that boots straight into a text mode never
/// announces anything at all, and the pacer falls back to its sanity ceiling
/// (240 Hz).  The governor then sees a 4.2 ms budget on a 14.3 ms frame and
/// pins the board at its top clock.  libretro's retro_get_system_av_info() can
/// be asked at any time; this asks it, feeds the answer through the same
/// BareVideoSetRefreshHz() path a real announcement uses (so the pin applies),
/// and returns the rate in millihertz, or 0 when the core has nothing to say.
///
/// Called once, after retro_load_game() has returned and before the frame loop.
/// Not in the frame loop: the answer only changes with the mode, and the mode
/// announcements cover that case.
unsigned BareCorePollRefreshMilliHz (void);

void    BareVideoSetRefreshHz (double fHz);

/// \brief The core announcing the aspect ratio of its picture, as a fraction.
///\t   Only the core knows what the VGA is doing; when it says nothing the
///\t   video layer falls back to 4:3.
void    BareVideoSetSourceAspect (unsigned nAspectX, unsigned nAspectY);

/// \brief Rebuild the visible parts of the frame from the last source frame.
///\t   Used when a status line is erased over a picture the emulator is no
///\t   longer drawing: the pixels underneath have to come from somewhere.
void    BareVideoRepaint (void);

/// \brief Presents with a null buffer: the core says "nothing changed".
u64     BareVideoDuplicatePresents (void);

/// \brief Append one line of status text and advance to the next one.
///\t   Anything that goes wrong after this point has to come out here,
///\t   because there is no other output left.
void    BareVideoStatusPrint (const char *pText);

/// \brief How many status lines fit on the display at the current mode.
unsigned BareVideoStatusLines (void);

/// \brief Fill the visible page with one colour (used to wipe the display
///\t   before status text is written).
void    BareVideoFill (u32 nColor);

/// \brief One-line report on whether a file is reachable: one line saying
///\t   what fopen() thinks and what std::filesystem::canonical() thinks.
///\t   The two disagreeing is the interesting case, and it is exactly what
///\t   retro_load_game() trips over (it canonicalises before anything else).
///\t   Returns a pointer to a static buffer; not thread safe, not reentrant.
const char *BareProbeFile (const char *pPath);

/// \brief Same for a directory: does it exist, and can it be listed.
const char *BareProbeDir (const char *pPath);

// ---------------------------------------------------------------------------
// Audio - bare_audio.cpp
// ---------------------------------------------------------------------------

boolean BareAudioInit (unsigned nSampleRate, CInterruptSystem *pInterrupt);
void    BareAudioDeinit (void);

/// \brief Queue stereo frames.  NEVER blocks: whatever does not fit in the
///\t   device queue is dropped and counted (see BareAudioStats).
void    BareAudioQueue (const s16 *pFrames, unsigned nFrames);

/// \brief Kept for symmetry with the old blocking design; now a no-op.
void    BareAudioDrain (void);

/// \brief Which sound device was started ("none" if audio is muted).
const char *BareAudioDeviceName (void);

/// \brief Room in the device queue, in the device's own frames.  Returns 0 if
///\t   there is no device.  This is the one number that answers "is the sound
///\t   device actually consuming what we give it": free stuck at 0 with drops
///\t   climbing means it is not, and free oscillating near the queue size
///\t   means it is.
unsigned BareAudioQueueFrames (unsigned *pFree, unsigned *pSize);

/// \brief Audio traffic so far, for the frame loop's watchdog.
///\t   pMicrosSinceWrite is 0 if nothing has ever been queued.
void    BareAudioStats (u64 *pQueuedFrames, u64 *pDroppedFrames,
			u64 *pMicrosSinceWrite);

/// \brief Samples discarded since start-up, monotonically.  In SAMPLES, not
///\t   frames and not bytes: the caller turns the difference between two
///\t   readings into a rate, and a rate is the only form of this number that
///\t   can be judged.  See BareAudioQueue for why it is never zero.
u64     BareAudioDroppedSamples (void);

/// \brief Writes that found the device's queue empty: the device had been
///\t   playing silence, i.e. the emulator was behind real time.
u64     BareAudioGaps (void);

/// \brief boot.conf audio_out = jack: try the 3.5 mm jack before HDMI.  Before
///\t   BareAudioInit.
void    BareAudioPreferJack (boolean bJack);

/// \brief Whole-output volume, 0..100 %, applied after DOSBox's mixer.
void    BareAudioSetVolume (unsigned nPercent);

/// \brief The output rate, or 0 when running muted.
unsigned BareAudioSampleRate (void);

// ---------------------------------------------------------------------------
// Video presentation on core 2 - bare_video.cpp
// ---------------------------------------------------------------------------

/// \brief Hand presentation to core 2: core 0 then only publishes frames, and
///\t   core 2 scales them into the hidden page, flips and (bVsync) waits for
///\t   vertical sync.  Also makes core 2 the mailbox owner.  FALSE if core 2
///\t   is not available; core 0 then keeps presenting as before.
boolean BareVideoPresenterStart (boolean bVsync);

/// \brief From boot.conf: start the presenter by itself once the emulator's
///\t   picture owns the screen (first frames drawn, boot text gone).
void    BareVideoPresenterConfigure (boolean bEnable, boolean bVsync);

/// \brief From boot.conf scaler = sharp | smooth: area sampling of rows when
///\t   the picture is enlarged by a fraction (sharp, the default), or the
///\t   linear interpolation used before (smooth).  Before the first frame.
void    BareVideoSetSharpScaling (boolean bSharp);
boolean BareVideoSharpScaling (void);

/// \brief The frame buffer's size (the display mode the port uses).
void    BareVideoModeSize (unsigned *pWidth, unsigned *pHeight);

/// \brief Keep a copy of the last frame DOSBox presented, and present it
///\t   again: the setup menu covers the screen while the core is paused, and
///\t   DOSBox only redraws when its picture changes.
void    BareVideoSaveScreen (void);
void    BareVideoRestoreScreen (void);

// ---------------------------------------------------------------------------
// Setup menu - bare_menu.cpp (Win+F12)
// ---------------------------------------------------------------------------

/// \brief Every key goes here first.  TRUE when the menu took it (it opened,
///\t   or it is open): the key must not reach DOS.  bWinDown: a Windows key
///\t   is held, counting this event.
boolean BareMenuKey (unsigned nRetroKey, boolean bDown, boolean bWinDown);
boolean BareMenuActive (void);

/// \brief From the frame loop while the menu is open, instead of running the
///\t   core: key repeat, live values, drawing.
void    BareMenuTick (void);

/// \brief At boot, before DOSBox starts: wait nSeconds for SUPR or F2, and if
///\t   one is pressed run the menu until it is closed.  TRUE if it was opened
///\t   (the boot screen then needs redrawing).  0 seconds returns at once.
boolean BareMenuBootWindow (unsigned nSeconds);

/// \brief For the menu (bare_libretro.cpp): release every key DOS thinks is
///\t   down; and the two ways out that Ctrl+F9 and Ctrl+Alt+PetSis use.
void    BareCoreReleaseAllKeys (void);
void    BareCoreRequestShutdown (void);
void    BareCoreRequestReboot (void);

/// \brief Disk images through the core's disk control (bare_libretro.cpp):
///\t   insert a .img (mounted as A:) or an .iso / .cue (as D:) in place of
///\t   what this frontend put in that drive, or eject drive 'A' or 'D'.  From
///\t   the frame loop, between frames.
boolean BareDiskInsert (const char *pPath);
boolean BareDiskEject (char cDrive);
boolean BareDiskAvailable (void);

/// \brief Take presentation back to core 0 (before anything else draws on
///\t   the screen, e.g. at shutdown).  Waits for core 2 to stop; leaves the
///\t   last picture on page 0.
void    BareVideoPresenterStop (void);
boolean BareVideoPresenterActive (void);

struct TBareVideoPresenterStats
{
	boolean bRunning;	///< core 2 is in its loop
	boolean bTwoPages;	///< the firmware granted the second page
	boolean bVsync;		///< vsync asked for and the firmware honours it
	u64 nShown;		///< frames flipped onto the screen
	u64 nOverwritten;	///< frames replaced before core 2 took them
	unsigned nLastMicros;	///< scaling time of the last frame
	unsigned nPeakMicros;	///< worst scaling time since the previous call
};
void    BareVideoPresenterStats (TBareVideoPresenterStats *pStats);

// ---------------------------------------------------------------------------
// The board's network - bare_net.cpp (Ethernet or WiFi, Circle's TCP/IP)
// ---------------------------------------------------------------------------

enum TBareNetMode { BareNetOff, BareNetEthernet, BareNetWiFi };

struct TBareNetConfig
{
	TBareNetMode Mode;
	const char *pHostname;
	boolean bDHCP;
	u8 IP[4], Mask[4], Gateway[4], DNS[4];	// with bDHCP FALSE
};

struct TBareNetInfo
{
	char IP[16], Mask[16], Gateway[16], DNS[16];	// "" until there is an address
	char MAC[18];
	char Hostname[32];
};

/// \brief Start the network from boot.conf, without waiting for the link: the
///\t   WiFi association and DHCP finish while DOS runs.  FALSE if it could not
///\t   start (BareNetState says why).  Off returns TRUE at once.
boolean BareNetInit (const TBareNetConfig *pConfig);
TBareNetMode BareNetMode (void);

/// \brief Connected and with an address.
boolean BareNetReady (void);

/// \brief One line, in Spanish, for the menu: "apagada", "WiFi: conectando con
///\t   la red", "esperando dirección (DHCP)", "conectada", or what failed.
const char *BareNetState (void);
void    BareNetInfo (TBareNetInfo *pInfo);

/// \brief The time zone for DOS's clock, as a POSIX TZ rule
///\t   ("CET-1CEST,M3.5.0,M10.5.0/3").  Before DOSBox starts: its BIOS reads
///\t   the local time once.
void    BareNetSetTimeZone (const char *pPosixTZ);

/// \brief Circle's clock holds a real time (NTP answered; the board has no clock).
boolean BareNetTimeValid (void);

/// \brief Wait (yielding to the network's tasks) until NTP has set the clock, at
///\t   most nMaxMillis.  TRUE if the time is valid.  Returns at once with the
///\t   network off or failed.
boolean BareNetWaitForTime (unsigned nMaxMillis);

/// \brief The network is up and circle-newlib's BSD sockets can be used.
boolean BareNetSocketsReady (void);

/// \brief "dd/mm/yyyy hh:mm:ss ZONE" in the configured zone, for the menu.
void    BareNetLocalTime (char *pOut, unsigned nSize);

/// A WiFi network wpa_supplicant's scans have seen (bare_net.cpp).
struct TBareWiFiNetwork
{
	char SSID[33];
	int nRSSI;		///< dBm, strongest access point with that name
	boolean bSecure;	///< needs a password
};

/// \brief The networks seen so far, strongest first; at most nMax.  Only with
///\t   network = wifi (the scans are wpa_supplicant's own).
unsigned BareNetWiFiNetworks (TBareWiFiNetwork *pOut, unsigned nMax);

/// \brief SD:/wpa_supplicant.conf names a network that is not the template's.
boolean BareNetWiFiConfigured (void);

// ---------------------------------------------------------------------------
// Secondary cores - bare_cores.cpp
// ---------------------------------------------------------------------------

/// A job for a secondary core.  Normally never returns.
typedef void (*TBareCoreJob) (void);

/// \brief Start cores 1..3 and park them (WFE) until a job is assigned.  Once,
///\t   early.  FALSE if no secondary core came up.
boolean BareCoresInit (void);

/// \brief The core reached its parking loop.
boolean BareCoresAlive (unsigned nCore);

/// \brief Give a parked core its job.  TRUE once the core is running it;
///\t   FALSE if the core is not alive or already has a job.
boolean BareCoresRun (unsigned nCore, TBareCoreJob pJob);

/// \brief The core is running a job right now.
boolean BareCoresIsRunning (unsigned nCore);

/// \brief Sleep the calling core until some core calls BareCoresWake() (WFE).
///\t   May return early; callers re-check their condition in a loop.
void    BareCoresWait (void);

/// \brief Wake every core sleeping in BareCoresWait() (DSB + SEV).
void    BareCoresWake (void);

// ---------------------------------------------------------------------------
// MIDI - bare_midi.cpp (MT-32 / General MIDI rendered on core 1)
// ---------------------------------------------------------------------------

/// \brief Give core 1 the synthesizer loop (BareCoresInit() must have run).
///\t   FALSE if the build has no MIDI libraries (scripts/build-midi-libs.sh
///\t   was not run) or core 1 is not available; everything else in this
///\t   section is then a harmless no-op.
boolean BareMidiInit (unsigned nSampleRate);

/// \brief Load and publish a synthesizer: pDevice is the [midi] mididevice
///\t   value ("mt32" or "fluidsynth"), pConfig its midiconfig.  Core 0 only:
///\t   it reads the card and allocates.
boolean BareMidiOpen (const char *pDevice, const char *pConfig);
void    BareMidiClose (void);
boolean BareMidiActive (void);
const char *BareMidiName (void);

/// \brief One byte from the MPU-401.  Never blocks; a full ring drops it.
void    BareMidiOutByte (u8 nByte);

/// \brief Add the synthesizer's output into a batch of stereo frames, in
///\t   place, with saturation.  Called by BareAudioQueue().
void    BareMidiMix (s16 *pFrames, unsigned nFrames);

/// \brief Times the mixer found core 1 behind, and MIDI bytes dropped.
void    BareMidiStats (u64 *pUnderruns, u64 *pBytesDropped);

/// \brief The last thing the MIDI side reported (device opened, or why not),
///\t   for the screen: Circle's log is not on the port's frame buffer.
const char *BareMidiStatus (void);

/// \brief MIDI bytes received from the MPU-401 so far, synthesizer or not.
u64     BareMidiBytesIn (void);

/// \brief MIDI volume relative to DOSBox's sound, 25..200 %.  Core 0.
void    BareMidiSetVolume (unsigned nPercent);

/// \brief The menu's MIDI test: a chord on, or off.
void    BareMidiSendTestChord (boolean bOn);

/// \brief Frames core 1 has rendered so far, and the loudest MIDI sample
///\t   mixed since the previous call (reset by the call).  For the panel:
///\t   bytes arriving with nothing rendered, or rendered at peak 0, are two
///\t   different faults.
void    BareMidiLevels (u64 *pRendered, unsigned *pPeak);

// ---------------------------------------------------------------------------
// Input - bare_input.cpp
// ---------------------------------------------------------------------------

boolean BareInputInit (void);
void    BareInputDeinit (void);

/// \brief Service USB and turn it into libretro key/mouse state.  Called once
///	   per frame by the core's input poll callback.
void    BareInputPoll (void);

// ---------------------------------------------------------------------------
// SoC information - bare_sysinfo.cpp
// ---------------------------------------------------------------------------

/// \return Number of ARM cores on the board (4 on every model this targets).
unsigned BareSysInfoCores (void);

/// \brief Which board this is, as Circle read it from the firmware.
///
/// There is a reason this is in the panel and not only in the boot log: a
/// Pi 400 runs its ARM domain at 1800 MHz where a Pi 4B runs at 1500, so the
/// frequency printed next to it is only readable if you know which board is
/// on the other end of the cable.
/// \return "Pi 400", "Pi 4B", "CM4", "Pi 5", ... never NULL.
const char *BareSysInfoBoardName (void);

/// \brief Ask the firmware for the maximum CPU clock rate, once, at boot.
///
/// Without this the SoC stays at its low rate (600 MHz on a Pi 4) because
/// nothing asks for more - see the comment in bare_sysinfo.cpp.  Safe to call
/// on boards where the firmware refuses: it is then a no-op.
void BareSysInfoSetPerformance (void);

/// \brief Read the ARM and core clock rates and the SoC temperature, at most
///\t   once every two seconds (the reads go through the firmware mailbox,
///\t   which is a call into the VideoCore and too slow to do per frame).
void BareSysInfoRefresh (void);

/// \brief Mailbox proxy: with it on, core 0 never calls the firmware mailbox -
///\t   clock changes are posted and the clocks, temperature and throttling
///\t   bits are read by the core that calls BareSysInfoServiceTick() (the
///\t   video presenter, which waits for vsync through the mailbox).  Turning
///\t   it off requires that core to have stopped calling the service.
void    BareSysInfoSetMailboxProxy (boolean bOn);
boolean BareSysInfoMailboxProxy (void);

/// \brief On the mailbox owner core, between frames: apply a posted clock
///\t   change and refresh the published readings.  No-op with the proxy off.
void    BareSysInfoServiceTick (void);

/// \brief Last values read.  Any pointer may be null.  MHz and degrees
///\t   Celsius; 0 means "the firmware did not answer".
void BareSysInfoGet (unsigned *pArmMHz, unsigned *pCoreMHz, unsigned *pCelsius);

/// \brief Which ARM core the caller is running on (0 for the emulator task).
unsigned BareSysInfoThisCore (void);

/// \brief Which governor level is active right now (0 = lowest), and how many
///\t   levels there are.  Returns 0 levels if the firmware refused to change the
///\t   clock, in which case the board runs at whatever the firmware gives it.
unsigned BareSysInfoGovernorLevel (void);
unsigned BareSysInfoGovernorLevels (unsigned *pMHz);

/// \brief One short line for the panel: "L1/3 busy over0 comf120", or "off" if
///\t   the firmware refused to change the clock.  This exists because the
///\t   governor can be perfectly correct and still look dead: a panel that only
///\t   shows the frequency cannot distinguish "it decided not to move" from
///\t   "it never ran".
const char *BareSysInfoGovernorState (void);

/// \brief Cap the top governor level, from boot.conf (0 = no cap).
void BareSysInfoSetCPUMax (unsigned nMHz);

/// \brief boot.conf mode: "auto" (the governor decides), "performance" (the
///\t   top level) or "powersave" (the bottom one).  The temperature veto
///\t   applies in all three.  Takes effect on the next frame.
void BareSysInfoSetMode (const char *pMode);

/// \brief Feed the governor the duration of one emulated frame and its budget.
///\t   Called once per frame from the frame loop; it decides on its own
///\t   schedule whether to change the ARM clock rate.
///
/// \param nFrameMicros  how long the frame took.
/// \param nBudgetMicros how long it was allowed to take (1e6 / the core's
///\t      rate).  The *fraction* used is what decides, because a fixed
///\t      threshold cannot be right across 50/60/70 Hz modes - and with one,
///\t      a perfectly paced 70 Hz screen looked like a permanent overrun and
///\t      the governor pinned itself at the top level.
/// \param bFrameChanged   whether this frame's picture differs from the one
///\t      before it.  FALSE for a stretch of frames means the emulator is
///\t      running and producing nothing - a DOS prompt, a still menu - and
///\t      that outranks the two numbers above: a repeated frame is not work,
///\t      and until .65g the governor had no way to be told so.  It counted
///\t      presented frames, and a prompt presents seventy a second.
///
/// `nPresents`, `nWanted` and `nAdvanced` (the panel's `pace` and `bd`) were
/// passed here for one round, to decide idleness from them.  They are gone: a
/// healthy machine reads `30/60` or `35/70`, and DOOM at `pace 15/70 bd 47` is
/// working hard, so those three numbers cannot tell idle from working.  What can
/// is duration - see governor::IdleFramesForPrompt - and the duration is counted
/// inside this file, from `bFrameChanged`.
void BareSysInfoGovernorTick (unsigned nFrameMicros, unsigned nBudgetMicros,
			      boolean bFrameChanged);

/// \brief Tell the governor whether the emulator is presenting frames at all.
///\t   FALSE means "DOS prompt": there is nothing to be slow at, so the clock
///\t   goes to the bottom level immediately.
void BareSysInfoSetCLIActivity (boolean bActive);

/// \brief The firmware's throttling bitmask (PROPTAG_GET_THROTTLED).  It is a
///\t   mask, not a level: bit 0 under-voltage now, bit 1 frequency capped now,
///\t   bit 2 throttled now, bit 3 soft temperature limit now; bits 16-19 are the
///\t   same four as "has happened since boot".
unsigned BareSysInfoFirmwareMode (void);

// ---------------------------------------------------------------------------
// Misc - bare_time.cpp
// ---------------------------------------------------------------------------

/// \brief Set how much of the boot's log goes to the screen (boot_log=).
///
/// 2 = everything (default), 1 = this port's own lines and nothing from the
/// emulation core, 0 = nothing at all.  Called once by the kernel from
/// boot.conf; bare_time.cpp consults BareLogDisplayEnabled() before painting
/// anything the core said.  The log itself is never suppressed - CLogger gets
/// every message either way, so tty1 and the serial port keep the full record.
void BareLogSetDisplayLevel (unsigned nLevel);

/// \return FALSE when boot.conf asked for a quiet or silent boot.
boolean BareLogDisplayEnabled (void);

/// \brief Measure the counter BareMicros() divides by.  Call once, early.
///
/// Circle's CTimer::GetClockTicks64() is the CPU's *physical* counter, which
/// runs at CNTFRQ_EL0 (54 MHz on a Pi 4), not at 1 MHz - and until .57 this
/// port used it as if it were microseconds.  Every timing constant in the port
/// was then wrong by that ratio, and the visible symptom was a machine that ran
/// as fast as the interpreter could go.  See the file header of bare_time.cpp.
///
/// It is a separate call and not a lazy initialiser inside BareMicros() because
/// measuring it needs a delay, and a delay inside a function called from the
/// core's frame path would be a trap for the next reader.  BareMicros() works
/// before this is called (it returns raw ticks, i.e. the old broken scale)
/// rather than returning zeros, so nothing can divide by an unknown.
void BareTimeCalibrate (void);

/// \return Monotonic microseconds since boot.
///
/// Microseconds since BareTimeCalibrate(); *ticks* of the same source before
/// it, which is harmless during boot and wrong afterwards.  It is calibrated
/// before anything on a timer starts, so in practice this is always real
/// microseconds - and if the calibration itself failed it says so in the log
/// and falls back to ticks instead of pretending.
u64  BareMicros (void);

/// \brief Log a line.  nLevel: 1=debug 2=info 3=warning 4=error
void BareLog (int nLevel, const char *pText);

// --- the C-linkage hand-over between the input layer and the core ----------
// bare_input.cpp decodes USB HID into "key/counter" state; the callbacks those
// land in live in bare_libretro.cpp (they are what DOSBox's libretro shim reads
// through input_cb).  Declared here so both halves agree on the signature.

/// \brief Set the state of one libretro RETROK_* key.
extern "C" {

/// \brief Microseconds since DOSBox last handed control back to the frontend
///\t   (0 if it never has).  Defined in bare_thread.cpp, which *replaces*
///\t   DOSBox-core's emu_thread.cpp, so it speaks C linkage like the rest of
///\t   that file's interface.  C++ linkage here would be a different symbol
///\t   and the link would fail - the compiler caught exactly that at .45.
u64 BareThreadMicrosSinceHandBack (void);

/// \brief DOSBox frame boundaries since the last call (the frontend calls it
///\t   once a frame).  Zero means the emulator did not advance at all.
unsigned BareThreadHandBacksSinceReport (void);

/// \brief CPU grants the emulator has asked for since the last call.
///\t   Non-zero while DOSBox is running; frozen means the emulator is not
///\t   even reaching its own loop.
unsigned BareThreadEmulatorYields (void);

/// \brief TRUE while the emulator is between its last report and the next
///\t   time the scheduler gives it the CPU.
boolean BareThreadEmulatorDriving (void);

// --- the dynamic recompiler (bare_dynarec.cpp) --------------------------
//
// One job: make the code cache executable.  Circle maps everything above the
// kernel's text with PXN = 1, which is right for data and fatal for a JIT, and
// there is no Circle API to change one page's permission - so the page tables
// are walked directly.  See the file header of bare_dynarec.cpp.

/// \brief Map the given buffer executable (clear PXN on the pages it covers).
/// \param pCache The buffer the recompiler writes its blocks into.
/// \param nBytes How much of it, from pCache.  Rounded outwards to whole pages.
/// \return FALSE if the page tables were not what this code expects, in which
///\t   case nothing was changed and the caller must stay on the interpreter.
///
/// Called with the *engine's own* buffer, which is why this takes a pointer and
/// does not allocate: dyn_cache.h keeps cache_code_start_ptr in file-static
/// storage, so there is no symbol to look up from here.  The call site has to
/// be DOSBox's side, which is why this is declared in the contract and called
/// from a hook the engine sets.  Until that hook exists the dynarec stays off.
boolean BareDynarecPrepareCodeCache (void *pCache, unsigned nBytes);

// --- the emulator's own frame accounting -------------------------------
//
// Why these three exist at all: the frame loop in kernel.cpp paces the frames
// *it* asks DOSBox for, and DOSBox-core does not always come back to it.  With
// RUN_SYNC the core returns to the frontend only from RENDER_EndUpdate and only
// while run_synced; when a game draws faster than the announced rate, or when
// render.updating is false, the emulator goes round its own loop (this file's
// switchThread) and the frontend stays inside retro_run().  Nothing in
// kernel.cpp can slow that down, so the metering has to happen here.
//
// See the note on s_nFrameDrives in bare_thread.cpp.

/// \brief How long a frame may take, in microseconds, from the core's rate.
///\t   Written by bare_video.cpp when the core announces its rate.
void BareThreadSetFrameBudget (unsigned nMicros);

/// \brief Frame boundaries the emulated machine reached since the last call.
///
/// The rate of these is what "the action runs fast" actually means: it is the
/// emulated machine advancing, not the pictures appearing.  `pace` counts the
/// pictures, and a core that draws several frames inside one retro_run() makes
/// those two numbers different - which is the one thing the panel could not
/// show until .65g.
unsigned BareThreadBoundariesSinceReport (void);

/// \brief Of those boundaries, the ones the frontend did NOT ask for.
///
/// Non-zero means DOSBox went round on its own, inside a retro_run() the
/// frontend is still waiting on; nothing in kernel.cpp can meter those.  Zero
/// means the frontend's pacer is the only metronome there is.
///
/// This is the number the `+` in the panel used to pretend to be.  It counted
/// every boundary, asked-for or not, so it was set on every single frame - a
/// field that always says yes, read for two rounds as if it said something.
unsigned BareThreadTrueDrivesSinceReport (void);

/// \brief Microseconds until the next frame is due, as this layer sees it.
///\t   0 means no deadline is armed yet.
u64 BareThreadNextDriveMicros (void);

} // extern "C"

extern "C" void BarePushKey (unsigned nRetroKey, boolean bDown);

/// \brief A key changed state.  Records it AND pushes it to DOSBox-core
///\t   through the keyboard event callback the core installed
///\t   (RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK).
///\t   This is the only path a key takes into the emulated machine; there is
///\t   no point in answering input_cb(RETRO_DEVICE_KEYBOARD) because the core
///\t   never asks.
extern "C" void BareDeliverKey (unsigned nRetroKey, boolean bDown);

/// \brief Was a screenshot asked for since the last call?  The frame loop calls
///\t   this and does the writing itself: taking a screenshot inside the
///\t   keyboard callback would stall the emulator, and with it the audio.
extern "C" boolean BareTakeScreenshotRequest (void);

/// \brief Keys delivered since the last call, for the status line.  Proves
///\t   that USB reports are arriving even when the emulated machine ignores
///\t   them.
unsigned BareInputKeyEvents (void);

/// \brief Last key delivered: *pKey gets the RETROK_* code, *pDown whether it
///\t   went down.  Used by the status panel to name the key.
void BareInputLastKey (unsigned *pKey, boolean *pDown);

/// \brief TRUE once the core has installed its keyboard event callback.
///\t   Until then nothing this port does can put a key into the emulated
///\t   machine, and the status panel says so.
boolean BareInputCoreCallbackInstalled (void);

/// \brief Hand over the mouse delta and button mask accumulated this frame.
extern "C" void BarePushMouse (int nDX, int nDY, u32 nButtons);

// ---------------------------------------------------------------------------
// Gamepad - bare_input.cpp
// ---------------------------------------------------------------------------
//
// A *joystick* to DOSBox, a *gamepad* to libretro, and the difference is not
// cosmetic: dosbox-core only switches the emulated game port on when the
// frontend tells it a pad is connected (retro_set_controller_port_device), and
// it then reads it through input_cb(RETRO_DEVICE_JOYPAD) and
// input_cb(RETRO_DEVICE_ANALOG).  Until .60 this port answered 0 to both, so a
// pad that Circle detected perfectly was invisible inside DOS.
//
// The mapping is "one modern pad is a two-axis, two-button DOS joystick", which
// is what the overwhelming majority of DOS games expect: left stick to the
// axes, the bottom two face buttons to the two buttons, the d-pad to the same
// axes as a digital fallback (many games want that and it costs nothing).
// Buttons 3 and 4 of a 4-axis/4-button joystick are the second stick and the
// shoulder buttons; see bare_input.cpp for the exact table.

/// \brief TRUE if a gamepad has been found and attached.
boolean BareInputHasGamepad (void);

/// \brief Number of pads attached (0 if none).
unsigned BareInputGamepads (void);

/// \brief Mouse speed in percent, 25..400 (boot.conf mouse_speed, the menu).
void    BareInputSetMouseSpeed (unsigned nPercent);

/// \brief Current buttons, as a libretro RETRO_DEVICE_ID_JOYPAD_* bitmask.
extern "C" u32 BareJoypadBits (void);

/// \brief One analogue axis, in libretro's units: -32768..32767.
/// \param nIndex RETRO_DEVICE_INDEX_ANALOG_LEFT or _RIGHT
/// \param nId    RETRO_DEVICE_ID_ANALOG_X or _Y
extern "C" s16 BareJoypadAxis (unsigned nIndex, unsigned nId);


#endif
