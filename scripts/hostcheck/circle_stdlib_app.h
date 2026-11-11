//
// circle_stdlib_app.h - a stand-in for circle-stdlib, so that kernel.cpp can be
// compiled on the host.
//
// `make check-platform` covers src/platform/, which needs little from Circle.
// kernel.cpp is the other half, and it is the file where a mistake costs a
// five-minute cross build to find - the .54 round lost an hour to calling
// CKernel::Report() from a free function, which this catches in two seconds.
//
// It declares ONLY what kernel.cpp uses today: the base class, its logger, the
// scheduler, the interrupt system and the shutdown enum.  It is not a faithful
// copy and it is not meant to be - the values do not even have to be right,
// because nothing here is executed.  If kernel.cpp starts using something else
// from circle-stdlib, this file grows one line and `make check-kernel` says so
// by failing to compile.
//
// See scripts/hostcheck/check-kernel.sh.
//
#pragma once
#include <circle/types.h>
#include <circle/bcmframebuffer.h>
#include <circle/logger.h>
#include <circle/devicenameservice.h>
#include <circle/sched/scheduler.h>
#include <cstring>
static const char *const KernelName = "dosbox";
class CInterruptSystem {};
// main.cpp returns these to Circle's startup code, which turns them into
// halt()/reboot().  Values do not matter here - nothing runs.
#define EXIT_HALT		0
#define EXIT_REBOOT		1

class CStdlibApp {
public:
	enum TShutdownMode { ShutdownNone, ShutdownHalt, ShutdownReboot };
public:
	CStdlibApp (const char *pName) : mName (pName) {}
	virtual ~CStdlibApp (void) {}
	virtual bool Initialize (void) { return true; }
	virtual TShutdownMode Run (void) { return ShutdownHalt; }
	virtual void Cleanup (void) {}
	const char *GetKernelName (void) const { return mName; }
protected:
	const char *mName;
	CInterruptSystem mInterrupt;
};
// .68: the real CStdlibAppStdio inherits from CStdlibAppScreen, which owns the
// CScreenDevice the logger is pointed at.  The port now reaches for that member
// (CKernel::TakeOverDisplay resizes it and re-points the logger away from it),
// so the stand-in has to have it too - with the same name and the same shape,
// or a typo in kernel.cpp would be caught by the cross build instead of here.
class CScreenDevice {
public:
	CScreenDevice (unsigned nWidth, unsigned nHeight) : m_nWidth (nWidth), m_nHeight (nHeight) {}
	// .72: CKernel::Initialize() compares the console's physical mode against
	// boot.conf before creating a second frame buffer - a runtime mode change is
	// what Circle documents as unreliable, and it is the suspected cause of the
	// garbage.  The stand-in has to offer the same accessor, or a typo in
	// kernel.cpp would only be caught by the cross build.
	CBcmFrameBuffer *GetFrameBuffer (void) { return nullptr; }
	boolean Initialize (void) { return true; }
	boolean Resize (unsigned nWidth, unsigned nHeight) { m_nWidth = nWidth; m_nHeight = nHeight; return true; }
	unsigned GetWidth (void) const { return m_nWidth; }
	unsigned GetHeight (void) const { return m_nHeight; }
private:
	unsigned m_nWidth;
	unsigned m_nHeight;
};

// The serial port and the logger are members of the real CStdlibAppScreen too:
// once the port owns the display, CKernel::Initialize() points the logger at
// the serial port so that Circle's console stops drawing into memory the
// firmware has handed to the port's second page.
class CSerialDevice : public CDevice {};

class CStdlibAppStdio : public CStdlibApp {
public:
	CStdlibAppStdio (const char *pName) : CStdlibApp (pName), mScreen (0, 0) {}
	CScreenDevice mScreen;
	CSerialDevice mSerial;
	CLogger       mLogger;
};
