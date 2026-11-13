//
// kernel.h
//
// by @mmoroca & arena.ai 2026  -  https://arena.ai
//
// BM-DOS - the Circle application around the DOSBox core.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
#ifndef _kernel_h
#define _kernel_h

#include <circle_stdlib_app.h>
#include <circle/sched/scheduler.h>
#include <circle/types.h>

// circle_stdlib_app.h uses strcmp() in an inline method but does not include
// <cstring> itself; with libc++ the transitive includes usually cover it, with
// other configurations they do not.  Cheap insurance, and it documents why the
// include is here.
#include <cstring>

// Where the emulator looks for things on the SD card.  One place, so the
// kernel and the core cannot disagree.
//
// Two spellings, and which one is used depends on who is asking:
//
//   DOSBOX_*      with the volume name.  newlib's fopen()/opendir() go through
//                 Circle's FatFs glue, which needs "SD:" to know which drive.
//   DOSBOX_*_ABS  without it.  std::filesystem (libc++ over newlib) has no idea
//                 what a volume name is: a path starting with "SD:/" is a
//                 relative path whose first component is a directory called
//                 "SD:", and std::filesystem::canonical() on it throws or
//                 resolves to nonsense.  dosbox-core canonicalises the path it
//                 is given before doing anything else, so the path handed to
//                 retro_load_game() must be the absolute one.
#define DOSBOX_ROOT		"SD:"
#define DOSBOX_CONFIG		"SD:/dosbox/dosbox.conf"
#define DOSBOX_GAME_DIR		"SD:/dos"

#define DOSBOX_CONFIG_ABS	"/dosbox/dosbox.conf"
#define DOSBOX_GAME_DIR_ABS	"/dos"

// ---------------------------------------------------------------------------
// CKernel - what the rest of the port is attached to
// ---------------------------------------------------------------------------
//
// This class is the whole application in the Circle sense: Circle's startup code
// constructs it (which initialises the hardware, see the constructor in
// kernel.cpp), calls Initialize() once and Run() once, and halts or reboots when
// Run() returns.
//
// The three things worth knowing before reading the implementation:
//
//  * Initialize() is where BM-DOS decides its own shape: it takes the frame
//    buffer first, so that every later message can be *seen*, and only then
//    starts the emulator.  Order matters here and the comments say why.
//
//  * Run() is the emulator's frame loop.  It calls retro_run() in a loop, paces
//    it to 60 Hz, and runs the watchdog and the status panel between frames.
//    Everything that has to happen "while DOS is running" happens there, in
//    one place, on the emulator's own thread of control - which is what makes
//    this port free of scheduling assumptions (see bare_thread.cpp).
//
//  * Nothing here touches DOSBox directly: this class talks to the bare_*
//    platform functions declared in platform/bare_env.h, which are the layer
//    that knows about DOSBox.  Keeping the boundary there is what lets the
//    platform layer be read on its own.
class CKernel : public CStdlibAppStdio
{
public:
	CKernel (void);
	~CKernel (void);

	virtual bool Initialize (void) override;
	virtual TShutdownMode Run (void) override;
	virtual void Cleanup (void) override;

private:
	/// \brief Circle's cooperative scheduler.
	///
	/// Nothing calls it directly.  It is a member because *constructing* it is
	/// what makes a scheduler exist, and two things depend on one existing:
	///
	///   * libc++'s std::thread, which DOSBox-core uses inside retro_load_game()
	///     to run the emulator.  circle-stdlib's __libcpp_thread_create() builds
	///     a CLibCXXTask, i.e. a Circle CTask, and CTask's constructor ends with
	///     CScheduler::Get()->AddTask(this).
	///   * bare_input.cpp, which yields to pace USB polling.
	///
	/// CScheduler has no Initialize() - its constructor sets the static instance
	/// pointer and Get() returns it.  CStdlibAppStdio does not have a scheduler
	/// member (only CStdlibAppNetwork does), so before this member existed
	/// Get() returned null and CTask's constructor dereferenced it.  The emulator
	/// died inside std::thread without a single log line, which is exactly what
	/// the boot showed: "Setting pixel format to RETRO_PIXEL_FORMAT_XRGB8888."
	/// and then silence.
	CScheduler mScheduler;

	/// \brief How much of the boot is written to the screen once the port owns
	///	   it: 2 = everything, 1 = this port's lines only (boot.conf
	///	   boot_log=quiet), 0 = nothing (boot_log=off).  Decided once, from
	///	   boot.conf, before the display is created.
	unsigned m_nBootLogLevel;

	/// \brief Set while a message must be shown even with boot_log = off.
	///	   Used by the display-mismatch check: a boot that stops with nothing
	///	   on screen looks like a dead card.
	boolean m_bReportForced;

	/// \brief Decide what to boot: the .conf if present, otherwise the game
	///	   directory.  Returns a static string, or nullptr when there is
	///	   nothing to boot.
	const char *PickContent (void) const;

	/// \brief Frame hook: called by the core at every frame boundary.
	static void FrameHook (void *pParam);

	/// \brief Report progress.  Goes to the console *and* to the frame
	///\t   buffer once the frame buffer exists.
	///
	/// Two destinations on purpose, and the duplication is not laziness:
	///
	///   * Circle's logger goes to the serial port and to the firmware's
	///     console, which is what you read from a terminal when the display is
	///     the thing that is broken;
	///   * the frame buffer is what a Pi with an HDMI cable shows.
	///
	/// The second one only exists after BareVideoInit(), which is why the
	/// lines before it are console-only and why the frame buffer is taken as
	/// early as possible.
	///
	/// The screen logger is not enough on its own: Circle's logger writes
	/// through the firmware's console, and the moment CBcmFrameBuffer is
	/// initialised the display stops scanning that buffer out.  Everything
	/// logged after BareVideoInit would be invisible, which is exactly the
	/// window where the interesting failures are.
	void Report (const char *pFormat, ...);

	/// \brief A line of the boot screen's first stage, the BIOS-like summary
	///\t   (board, memory, video, sound, USB, network).  Into the boot log
	///\t   too, as "POST ...".
	void Post (const char *pFormat, ...);

	/// \brief From the summary to "Iniciando BM-DOS..." and the timed log.
	void BootStageTwo (void);

	/// \brief 0: the BIOS-like summary; 1: "Iniciando BM-DOS..." and the log.
	unsigned m_nBootStage;

	/// \brief Lines of status text written so far.
	unsigned m_nStatusLine;

	/// \brief Set once nothing more can be shown on screen.
	boolean m_bDisplayOwned;
};

#endif
