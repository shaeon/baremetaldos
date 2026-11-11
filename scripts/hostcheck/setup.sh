#!/bin/sh
#
# Produce the minimal header set that lets the platform files compile outside
# Circle.  Sourced and driven by check-kernel.sh; not run by hand.
#
# Usage:
#   make check-kernel            # calls it
#
# The stand-in headers are written out by this script.  Their signatures are
# copied from Circle (include/circle/*.h) and circle-stdlib; that is the known
# cost of not having the real trees on the host, and check-kernel is written so
# that a signature that drifts is noticed rather than trusted.
set -e
cd "$(dirname "$0")"
mkdir -p circle/sound circle/sched circle/usb circle/input

# types.h - and the reason it does NOT include <cstdint> any more.
#
# The first version of this stand-in typedef'd u8..u64 out of <cstdint>, which
# quietly gave every file a working int16_t/uint16_t through Circle's header.
# The real Circle's include/circle/types.h includes <assert.h> and nothing else:
# it has u8..u64 and s8..s64, and NO stdint types at all.  So .60 wrote
# "extern \"C\" int16_t BareJoypadAxis(...)" in bare_env.h, every check here
# passed, and the board build stopped with:
#
#     src/platform/bare_env.h:496:12: error: 'int16_t' does not name a type
#
# A stand-in that is more generous than the real header does not test anything:
# it hides exactly the class of mistake it exists to catch.  The typedefs below
# are the same *types* the real ones are (signed char, signed short, ...), so
# the port still sees s16 where Circle defines s16.
cat > circle/types.h <<'EOF'
#pragma once
#include <assert.h>
typedef unsigned char u8; typedef unsigned short u16;
typedef unsigned int u32; typedef unsigned long u64;
typedef signed char s8; typedef signed short s16;
typedef signed int s32; typedef signed long s64;
typedef bool boolean;
// Circle's own names, and the real header spells them per architecture
// (int / unsigned int on 32-bit, long / unsigned long on 64-bit) - it never
// uses intptr_t, because types.h does not include <cstdint>.
typedef long intptr;
typedef unsigned long uintptr;
#define TRUE true
#define FALSE false
#define BIT(n) (1 << (n))
#define RASPPI 4
#define ARM_ALLOW_MULTI_CORE
EOF

cat > circle/util.h <<'EOF'
#pragma once
#include <cstring>
EOF

cat > circle/macros.h <<'EOF'
#pragma once
#define PACKED __attribute__((packed))
EOF

cat > circle/logger.h <<'EOF'
#pragma once
#include <circle/types.h>
enum TLogSeverity { LogPanic, LogError, LogWarning, LogNotice, LogDebug };
class CDevice;
class CLogger
{
public:
	static CLogger *Get (void);
	void Write (const char *pSource, TLogSeverity Severity, const char *pMessage, ...);
	// .68: the port re-points the logger at its own frame buffer, so both of
	// these exist in the contract now.  They are Circle's own functions.
	void SetNewTarget (CDevice *pTarget);
	CDevice *GetTarget (void) const;
};
EOF

cat > circle/timer.h <<'EOF'
#pragma once
#include <circle/types.h>
#define HZ 100
class CTimer
{
public:
	static unsigned GetClockTicks (void);
	static u64 GetClockTicks64 (void);
	static unsigned GetTicks (void);
	static void SimpleusDelay (unsigned nMicroSeconds);
	static void SimpleMsDelay (unsigned nMilliSeconds);
};
EOF

cat > circle/sched/scheduler.h <<'EOF'
#pragma once
#include <circle/types.h>

// Only what the port uses.  Get()/Yield()/GetCurrentTask() are what
// bare_thread.cpp needs, and they are the whole reason it can be compiled on
// the host at all: bare_thread.cpp is the file that decides *when* the emulator
// runs, and in .57 it grew real logic (metering frames the emulator drives by
// itself) after a round spent chasing a speed bug in the wrong file.  A file
// that is not compiled here is a file where that happens again.
class CTask
{
public:
	boolean IsSuspended (void);
};

class CScheduler
{
public:
	static CScheduler *Get (void);
	static boolean IsActive (void);
	void Yield (void);
	CTask *GetCurrentTask (void);
	void Sleep (unsigned nSeconds);
};
EOF

cat > circle/memory.h <<'EOF'
#pragma once
#include <circle/types.h>
class CMemorySystem
{
public:
	static CMemorySystem *Get (void);
	u64 GetMemSize (void) const;
};
EOF

# The setup menu draws with Circle's 8x16 font table (bare_menu.cpp).
cat > circle/font.h <<'EOF'
#pragma once
struct TFont
{
	unsigned width;
	unsigned height;
	unsigned extra_height;
	unsigned first_char;
	unsigned last_char;
	const void *data;
};
extern const TFont Font8x16;
EOF

# bare_midi.cpp subclasses CMultiCoreSupport to run the synthesizers on core 1,
# so the stub carries the constructor and the two virtuals it overrides.
cat > circle/multicore.h <<'EOF'
#pragma once
#include <circle/types.h>
class CMemorySystem;
class CMultiCoreSupport
{
public:
	CMultiCoreSupport (CMemorySystem *pMemorySystem);
	virtual ~CMultiCoreSupport (void);
	virtual boolean Initialize (void);
	virtual void Run (unsigned nCore) = 0;
	static unsigned ThisCore (void);
};
EOF

cat > circle/interrupt.h <<'EOF'
#pragma once
#include <circle/types.h>
class CInterruptSystem { };
EOF

cat > circle/startup.h <<'EOF'
#pragma once
void halt (void);
void reboot (void);
EOF

cat > circle/koptions.h <<'EOF'
#pragma once
#include <circle/types.h>
enum TCPUSpeed { CPUSpeedLow, CPUSpeedMaximum, CPUSpeedUnknown };
class CKernelOptions
{
public:
	static CKernelOptions *Get (void);
	TCPUSpeed GetCPUSpeed (void) const;
	unsigned GetSoCMaxTemp (void) const;
	unsigned GetGPIOFanPin (void) const;
};
EOF

cat > circle/cputhrottle.h <<'EOF'
#pragma once
#include <circle/types.h>
enum TCPUSpeed { CPUSpeedLow, CPUSpeedMaximum, CPUSpeedUnknown };
EOF

cat > circle/bcmpropertytags.h <<'EOF'
#pragma once
#include <circle/types.h>
#include <circle/macros.h>
struct TPropertyTag { u32 nTagId; u32 nValueBufSize; u32 nValueLength; } PACKED;
struct TPropertyTagSimple { TPropertyTag Tag; u32 nValue; } PACKED;
struct TPropertyTagClockRate { TPropertyTag Tag; u32 nClockId; u32 nRate; } PACKED;
struct TPropertyTagTemperature { TPropertyTag Tag; u32 nTemperatureId; u32 nValue; } PACKED;
struct TPropertyTagSetClockRate
{
	TPropertyTag Tag;
	u32 nClockId;
	u32 nRate;
	u32 nSkipSettingTurbo;
} PACKED;
#define PROPTAG_GET_CLOCK_RATE		0x00030002
#define PROPTAG_GET_MAX_CLOCK_RATE	0x00030004
#define PROPTAG_GET_TEMPERATURE		0x00030006
#define PROPTAG_GET_MIN_CLOCK_RATE	0x00030007
#define PROPTAG_GET_MAX_TEMPERATURE	0x0003000A
#define PROPTAG_GET_THROTTLED		0x00030046
#define PROPTAG_SET_CLOCK_RATE		0x00038002
#define CLOCK_ID_ARM			3
#define CLOCK_ID_CORE			4
#define TEMPERATURE_ID			0
#define VIDEOCORE_TEMPERATURE_ID	1
class CBcmPropertyTags
{
public:
	boolean GetTag (u32 nTagId, void *pTag, unsigned nTagSize, unsigned nRequestParmSize = 0);
	boolean GetTags (void *pTags, unsigned nTagsSize);
};
EOF

# machineinfo.h - only what the port uses.
#
# It exists because .60 added BareSysInfoBoardName() and BareSysInfoCores():
# the log from a real board said "Raspberry Pi 400" while two rounds of
# clock work had been done against "a Pi 4", and a Pi 400 runs its ARM domain
# at 1800 MHz where a 4B runs at 1500.  The panel now names the board, which
# means the platform layer must be able to ask Circle - so the stand-in has to
# describe the call, or check-platform stops being able to compile the file
# that uses it.
#
# The enum is copied from Circle's include/circle/machineinfo.h (Step51.1.1).
# Only the models this port can plausibly run on are worth naming here; the
# numbers are what matters, because a mismatch would map a Pi 400 to a Pi 5.
# devicenameservice.h, input/mouse.h, usb/usbkeyboard.h - the three Circle
# headers bare_input.cpp includes and this environment never had.
#
# They were missing for a reason worth writing down: check-platform compiled
# only bare_sysinfo/bare_audio/bare_video/bare_time/bare_fs/bare_thread/
# bare_dynarec, and check-kernel compiled kernel/main/bare_libretro/
# bare_thread.  bare_input.cpp was in NEITHER list, so the one file that talks
# to Circle's USB stack - and that .60 rewrote to add gamepads - was compiled by
# nothing except the cross build.  Adding the stand-ins is
# what lets it be compiled here.
cat > circle/devicenameservice.h <<'EOF'
#pragma once
#include <circle/types.h>
class CDevice
{
public:
	virtual ~CDevice (void) {}
};
class CDeviceNameService
{
public:
	static CDeviceNameService *Get (void);
	// Circle has two GetDevice overloads and two AddDevice overloads.  The port
	// uses GetDevice("tty1") and nothing else today; the rest are here because a
	// stand-in that only has the one the port happens to call stops being a
	// stand-in for the header the board compiles against.
	CDevice *GetDevice (const char *pName, boolean bBlocking);
	CDevice *GetDevice (const char *pPrefix, unsigned nIndex, boolean bBlocking);
	void AddDevice (const char *pName, CDevice *pDevice, boolean bBlockDevice);
	void AddDevice (const char *pPrefix, unsigned nIndex, CDevice *pDevice, boolean bBlockDevice);
	void RemoveDevice (const char *pName, boolean bBlocking);
};
EOF

cat > circle/input/mouse.h <<'EOF'
#pragma once
#include <circle/devicenameservice.h>
typedef void TMouseStatusHandler (unsigned nButtons, int nDisplacementX,
				  int nDisplacementY, int nWheelMove);
class CMouseDevice : public CDevice
{
public:
	void RegisterStatusHandler (TMouseStatusHandler *pStatusHandler);
	unsigned GetButtonCount (void) const;
	boolean HasWheel (void) const;
};
EOF

cat > circle/usb/usbkeyboard.h <<'EOF'
#pragma once
#include <circle/devicenameservice.h>
typedef void TKeyStatusHandlerRaw (unsigned char ucModifiers, const unsigned char RawKeys[6]);
class CUSBKeyboardDevice : public CDevice
{
public:
	void RegisterKeyStatusHandlerRaw (TKeyStatusHandlerRaw *pStatusHandler,
					  boolean bCallOnChange = true);
	void UnregisterKeyStatusHandlerRaw (void);
	boolean SetLEDs (unsigned nLEDs);
};
EOF

# usbgamepad.h - only what bare_input.cpp uses.
#
# The enum values are copied from Circle's include/circle/usb/usbgamepad.h
# (Step51.1.1) because they are *bit masks* that reach the DOS joystick: a
# wrong value here would compile, pass every check, and map the wrong button
# on the board.  The names of the axes matter for the same reason.
cat > circle/usb/usbgamepad.h <<'EOF'
#pragma once
#include <circle/types.h>
#include <circle/devicenameservice.h>
enum TGamePadButton
{
	GamePadButtonGuide = BIT(0),
	GamePadButtonLT = BIT(3), GamePadButtonRT = BIT(4),
	GamePadButtonLB = BIT(5), GamePadButtonRB = BIT(6),
	GamePadButtonY = BIT(7), GamePadButtonB = BIT(8),
	GamePadButtonA = BIT(9), GamePadButtonX = BIT(10),
	GamePadButtonSelect = BIT(11), GamePadButtonL3 = BIT(12),
	GamePadButtonR3 = BIT(13), GamePadButtonStart = BIT(14),
	GamePadButtonUp = BIT(15), GamePadButtonRight = BIT(16),
	GamePadButtonDown = BIT(17), GamePadButtonLeft = BIT(18),
	GamePadButtonPlus = BIT(19), GamePadButtonMinus = BIT(20),
	GamePadButtonTouchpad = BIT(21)
};
enum TGamePadAxis
{
	GamePadAxisLeftX, GamePadAxisLeftY, GamePadAxisRightX, GamePadAxisRightY,
	GamePadAxisButtonLT, GamePadAxisButtonRT, GamePadAxisButtonUp,
	GamePadAxisButtonRight, GamePadAxisButtonDown, GamePadAxisButtonLeft,
	GamePadAxisButtonL1, GamePadAxisButtonR1, GamePadAxisButtonTriangle,
	GamePadAxisButtonCircle, GamePadAxisButtonCross, GamePadAxisButtonSquare,
	GamePadAxisUnknown
};
#define MAX_AXIS 16
#define MAX_HATS 6
struct TGamePadState
{
	int naxes;
	struct { int value; int minimum; int maximum; } axes[MAX_AXIS];
	int nhats;
	int hats[MAX_HATS];
	int nbuttons;
	unsigned buttons;
	int acceleration[3];
	int gyroscope[3];
};
typedef void TGamePadStatusHandler (unsigned nDeviceIndex, const TGamePadState *pGamePadState);
class CUSBGamePadDevice : public CDevice
{
public:
	boolean Configure (void);
	virtual unsigned GetProperties (void) { return 0; }
	virtual const TGamePadState *GetInitialState (void);
	virtual void RegisterStatusHandler (TGamePadStatusHandler *pStatusHandler);
};
EOF

cat > circle/machineinfo.h <<'EOF'
#pragma once
#include <circle/types.h>
enum TMachineModel
{
	MachineModelA, MachineModelBRelease1MB256, MachineModelBRelease2MB256,
	MachineModelBRelease2MB512, MachineModelAPlus, MachineModelBPlus,
	MachineModelZero, MachineModelZeroW, MachineModelZero2W,
	MachineModel2B, MachineModel3B, MachineModel3APlus, MachineModel3BPlus,
	MachineModelCM, MachineModelCM3, MachineModelCM3Plus,
	MachineModel4B, MachineModel400, MachineModelCM4, MachineModelCM4S,
	MachineModel5, MachineModel500, MachineModelCM5, MachineModelCM5Lite,
	MachineModelUnknown
};
class CMachineInfo
{
public:
	TMachineModel GetMachineModel (void) const;
	const char *GetMachineName (void) const;
	static CMachineInfo *Get (void);
};
EOF

cat > circle/synchronize.h <<'EOF'
#pragma once
#include <circle/types.h>
void CleanAndInvalidateDataCacheRange (u32 nAddress, u32 nLength);
void SyncDataAndInstructionCache (void);
// bare_cores.cpp parks cores with WFE and wakes them with DSB + SEV.
#define DataSyncBarrier()	((void) 0)
#define WaitForEvent()		((void) 0)
#define SendEvent()		((void) 0)
EOF

cat > circle/terminal.h <<'EOF'
#pragma once
#include <circle/chargenerator.h>
#include <circle/devicenameservice.h>
#include <circle/types.h>

class CDisplay;				// real Circle: display.h; a frame buffer is one

class CTerminalDevice : public CDevice
{
public:
	CTerminalDevice (CDisplay *pDisplay, unsigned nDeviceIndex = 0,
			 const TFont &rFont = DEFAULT_FONT,
			 CCharGenerator::TFontFlags FontFlags = CCharGenerator::FontFlagsNone);
	boolean Initialize (void);
	unsigned GetWidth (void) const;
	unsigned GetHeight (void) const;
	unsigned GetColumns (void) const;
	unsigned GetRows (void) const;
	int Write (const void *pBuffer, size_t nCount);
};
EOF

cat > circle/bcmframebuffer.h <<'EOF'
#pragma once
#include <circle/types.h>
class CBcmFrameBuffer
{
public:
	CBcmFrameBuffer (unsigned nWidth, unsigned nHeight, unsigned nDepth,
			 unsigned nVirtualWidth = 0, unsigned nVirtualHeight = 0, unsigned nDisplay = 0);
	boolean Initialize (void);
	u32 GetWidth (void) const;
	u32 GetHeight (void) const;
	u32 GetVirtWidth (void) const;
	u32 GetVirtHeight (void) const;
	u32 GetPitch (void) const;
	u32 GetSize (void) const;
	u32 GetDepth (void) const;
	uintptr GetBuffer (void) const;
	boolean SetVirtualOffset (u32 nOffsetX, u32 nOffsetY);
	boolean WaitForVerticalSync (void);
};
EOF

cat > circle/chargenerator.h <<'EOF'
#pragma once
#include <circle/types.h>
struct TFont;				// real Circle: the font table itself
extern const TFont Font8x16;		// DEFAULT_FONT in Circle
#define DEFAULT_FONT Font8x16
class CCharGenerator
{
public:
	// The only flag the port uses; the real enum has more.
	enum TFontFlags
	{
		FontFlagsNone = 0,
	};
	unsigned GetCharWidth (void) const;
	unsigned GetCharHeight (void) const;
	const u8 *GetCharPointer (char chChar) const;
	u32 GetPixelLine (char chChar, unsigned nLine) const;
};
EOF

# Circle's sound headers take size_t, and the real ones get it from Circle's own
# types.h (which defines it per architecture, see include/circle/types.h).  The
# stand-in stopped providing it on purpose - see the note above types.h - so each
# header that needs it says so, which is also what the real tree does.
cat > circle/sound/soundbasedevice.h <<'EOF'
#pragma once
#include <cstddef>
#include <circle/types.h>
enum TSoundFormat
{
	SoundFormatUnsigned8,
	SoundFormatSigned16,
	SoundFormatSigned24,
	SoundFormatSigned24_32,
	SoundFormatUnsigned32,
	SoundFormatIEC958,
	SoundFormatFloat32,
	SoundFormatUnknown
};
class CSoundBaseDevice
{
public:
	CSoundBaseDevice (unsigned nSampleRate, unsigned nChunkSize = 0, unsigned nQueueSizeMsecs = 0);
	virtual ~CSoundBaseDevice (void);
	boolean AllocateQueue (unsigned nSizeFrames);
	void SetWriteFormat (TSoundFormat Format, unsigned nChannels = 2);
	int Write (const void *pBuffer, size_t nCount);
	unsigned GetQueueSizeFrames (void);
	unsigned GetQueueFramesAvail (void);
	boolean Start (void);
	boolean Cancel (void);
	boolean IsActive (void);
};
EOF

cat > circle/sound/hdmisoundbasedevice.h <<'EOF'
#pragma once
#include <circle/sound/soundbasedevice.h>
class CHDMISoundBaseDevice : public CSoundBaseDevice
{
public:
	CHDMISoundBaseDevice (CInterruptSystem *pInterrupt,
			      unsigned nSampleRate = 44100, unsigned nChunkSize = 4000);
};
EOF

cat > circle/sound/pwmsoundbasedevice.h <<'EOF'
#pragma once
#include <circle/sound/soundbasedevice.h>
class CPWMSoundBaseDevice : public CSoundBaseDevice
{
public:
	CPWMSoundBaseDevice (CInterruptSystem *pInterrupt,
			     unsigned nSampleRate = 44100, unsigned nChunkSize = 4000);
};
EOF

cat > circle/sound/i2ssoundbasedevice.h <<'EOF'
#pragma once
#include <circle/sound/soundbasedevice.h>
class CI2SSoundBaseDevice : public CSoundBaseDevice
{
public:
	CI2SSoundBaseDevice (CInterruptSystem *pInterrupt, unsigned nSampleRate = 44100,
			     unsigned nChunkSize = 4000, boolean bInI2S = TRUE);
};
EOF

cat > bare_env.h <<'EOF'
#pragma once
#include <circle/types.h>
u64 BareMicros (void);
boolean BareAudioInit (CInterruptSystem *pInterrupt);
void BareAudioDeinit (void);
void BareAudioQueue (const s16 *pFrames, unsigned nFrames);
void BareAudioDrain (void);
void BareAudioStats (u64 *, u64 *, u64 *);
const char *BareAudioDeviceName (void);
unsigned BareAudioQueueFrames (unsigned *, unsigned *);
void BareSysInfoSetPerformance (void);
void BareSysInfoSetCLIActivity (boolean);
void BareSysInfoGovernorTick (unsigned);
void BareSysInfoRefresh (void);
void BareSysInfoGet (unsigned *, unsigned *, unsigned *);
unsigned BareSysInfoGovernorLevel (void);
unsigned BareSysInfoFirmwareMode (void);
void BareVideoInit (void);
void BareVideoDeinit (void);
void BareVideoPresent (const void *, unsigned, unsigned, unsigned);
unsigned BareVideoFramesPresented (void);
boolean BareVideoScreenshot (void);
void BareVideoFooterSetVisible (boolean);
boolean BareVideoFooterVisible (void);
void BareVideoFooter (unsigned, const char *);
EOF
echo "entorno simulado listo en $(pwd)"
