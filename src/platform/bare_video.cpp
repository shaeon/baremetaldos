//
// bare_video.cpp
//
// by @mmoroca & arena.ai 2026  -  https://arena.ai
//
// BM-DOS - video backend.
//
// Takes the XRGB8888 frames DOSBox-core produces and puts them on the HDMI
// display through Circle's frame buffer.
//
// Design notes that matter for later:
//
//  * The frame buffer is allocated once and never resized.  Re-allocating it
//    from inside a running emulator is possible in Circle but slow (the
//    firmware has to renegotiate the mode) and it invalidates pointers the
//    GPU holds.  We allocate a fixed mode (see BareVideoInit) and scale every
//    DOSBox frame into it.  DOSBox's native modes are small (320x200 ...
//    800x600), so the scale is nearest-neighbour with letterboxing, which
//    costs a fraction of what the CPU emulation costs.
//
//  * 320x200 and 640x400 get the classic 5:6 vertical stretch so text does
//    not look squashed, the same correction DOSBox applies by default.
//
//  * Presentation runs on core 0 in this milestone.  Moving it to core 1
//    (as pc-286-bare-metal, pi-mame and ZXMetalCircle do) only touches this
//    file, so the interface is kept free of core assumptions.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
#include "bare_env.h"
#include "frame_exchange.h"

#include <circle/bcmframebuffer.h>
#include <circle/chargenerator.h>
#include <circle/logger.h>
#include <circle/synchronize.h>
#include <circle/timer.h>
#include <circle/util.h>

#include <atomic>
#include <cstdint>
#include <cstdio>

static const char FromVideo[] = "video";

namespace {

CBcmFrameBuffer *s_pFrameBuffer = nullptr;

unsigned s_nModeWidth = 0;
unsigned s_nModeHeight = 0;

// Destination geometry for the current source size.
unsigned s_nSrcWidth = 0;
unsigned s_nSrcHeight = 0;
unsigned s_nDstWidth = 0;
unsigned s_nDstHeight = 0;
unsigned s_nDstX = 0;
unsigned s_nDstY = 0;

// Presents left before the boot text is wiped from underneath the picture (see
// BareVideoPresent).  Zero means "nothing to do", which is the normal case.
unsigned s_nWipeAfterPresents = 0;

// How many frames have reached the screen since the port started.  Used only
// for the heartbeat above.
unsigned s_nPresentedSinceStart = 0;

u32 *s_pRowBuffer = nullptr;
u32 *s_pRowBuffer2 = nullptr;		// second row, for vertical interpolation
unsigned s_nRowBufferPixels = 0;

CCharGenerator s_Font;

// Status text geometry.  Top-left corner, one font height per line, with a
// one-line gap from the edge.  Fixed, so the position does not depend on the
// source mode DOSBox happens to be in.
const unsigned STATUS_X = 8;
const unsigned STATUS_TOP = 8;
const unsigned PanelMargin = 8;	// gap between the status line and the screen edge
// No cap: the display is 1080 lines at 16+2 pixels per line, so there is room
// for sixty.  The old cap of 24 was a guess that turned out to be exactly the
// number of lines the boot needs, which is why the interesting part - what
// happens after "core: load" - was being written and thrown away.
const unsigned STATUS_MAX_LINES = 60;

// True once the text on screen has scrolled: after that the top line is no
// longer "BM-DOS <snapshot>", so it is no longer safe to wipe the screen.
bool s_bStatusScrolled = false;

// Cleared as soon as the emulator has something of its own to show.  The status
// column lives in the top-left corner and DOSBox's 640x400 output is scaled and
// centred right on top of it, so from the first real frame onwards the status
// text would be covering the C: prompt it exists to explain.  Up to then it is
// the only thing on screen and worth every line.
bool s_bStatusEnabled = true;

// Frame accounting, so the frame loop can tell "the emulator is working" from
// "the emulator has stopped".  BareMicros() is the CPU's counter register: no
// device access, safe from any task.
u64 s_nFramesPresented = 0;
u64 s_nDuplicatePresents = 0;
u64 s_nLastFrameMicros = 0;

// .65g: how many of the presented frames were DIFFERENT from the one before.
//
// This is the number the governor needed and did not have.  "Nothing has been
// presented for a second" cannot mean "the machine is idle" on a DOS prompt,
// because a prompt presents seventy frames a second of the same picture - so
// the governor never saw a quiet machine, stayed busy, and walked to 1800 MHz
// on a screen doing nothing.  A frame that is identical to the last one is an
// emulator that is running and not producing; that is what idle looks like
// from here, and it is cheap to compute: a hash of every 64th word while the
// mirror is being written anyway, which is the copy this function already does.
u64 s_nFramesChanged = 0;
u64 s_nLastFrameHash = 0;
boolean s_bLastFrameHashValid = FALSE;

// Line cursor for BareVideoStatusPrint().  Owned here rather than by the
// caller so that the kernel's own progress lines and the core's log messages
// (which arrive through BareLog) share one column and cannot overlap.
unsigned s_nStatusCursor = 0;

// The status line.  Off by default: it is a diagnostic, and a diagnostic drawn
// over the picture is something you turn on when you want it.  The PetSis key
// toggles it (see bare_libretro.cpp for the key, kernel.cpp for the drawing).
boolean s_bFooterVisible = FALSE;

// One slot per line of the top-right corner, each remembering the rectangle it
// drew last time so that it can erase exactly that.  Two slots are in use:
//
//   0  the status panel (diagnostic, toggled with PetSis, persistent)
//   1  a message from the core ("CPU speed: fixed 23880 cycles", transient)
//
// They share the mechanism on purpose.  Before this, the panel was drawn
// bottom-left and the core's message was drawn bottom-left by another function
// that had no way to erase itself - so they landed on top of each other and the
// loser stayed on screen for ever.  DOSBox does not draw that message: it comes
// through RETRO_ENVIRONMENT_SET_MESSAGE, i.e. to this file, which is exactly
// why it can be moved and erased.
struct TLineSlot
{
	unsigned nX;
	unsigned nY;
	unsigned nWidth;
	unsigned nHeight;

	// The text as it was last given.  Kept here, not in the caller, because
	// the present path repaints the slots once per frame: the panel is one
	// line that changes every second, but the *repaint* has to happen every
	// frame or a game that redraws its screen would wipe it.
	char Text[128];
};

constexpr unsigned MaxSlots = 2;
TLineSlot s_Slots[MaxSlots] = {};

// ---------------------------------------------------------------------------
// The mirror: the source frame, kept exactly as the core handed it over
// ---------------------------------------------------------------------------
//
// Why a copy of the source is needed at all, and why this is the fix for two
// different problems at once:
//
//   * The status lines are drawn *on top of* the picture.  DOS never repaints
//     the corner they live in, so when a line moves, grows, shrinks or is
//     erased, the pixels underneath it are simply gone - the only thing that
//     could restore them is the next frame, and that frame does not include
//     the region the line covered.  That is the dirt the panel leaves behind.
//   * When the picture is scaled, only part of the frame buffer belongs to the
//     emulator: the letterbox bars around it do not.  Repainting the whole
//     bars for every frame is pure cost.
//
// With the source frame kept, the present path only has to recompute the
// regions that are *visible*: the destination rectangle and any line slot that
// is currently drawn.  Everything else in the frame buffer is either correct
// already or about to be overwritten.  A skipped present (the core says
// "nothing changed") still repaints those regions, which is what makes a line
// erase properly even when the emulator is not drawing.
//
// The buffer is a handful of MB at 640x400 and 2 MB at 1920x1080 - see the
// per-frame cost table in README.  It is allocated once, when the source
// geometry first changes, and never grows again.
// The aspect ratio the core reports (retro_system_av_info::geometry), and
// whether it has said anything yet.  0 = nothing heard, use the built-in rule.
// The rate the core says it is running at, in millihertz (16666 = 59.994 Hz,
// 14285 = 70.007 Hz).  Zero until the core says something; the frame loop then
// paces at 60, which is what it always did.
unsigned s_nCoreRefreshMHz = 0;		// what the rest of the port paces against

unsigned s_nCoreAspectX = 0;
unsigned s_nCoreAspectY = 0;
boolean s_bAspectFromCore = FALSE;

u32 *s_pMirror = nullptr;
unsigned s_nMirrorPixels = 0;		// allocated
unsigned s_nMirrorWidth = 0;		// valid
unsigned s_nMirrorHeight = 0;

// --- core 2 presenter (see "Presentation on core 2" at the end of the file) ---
bareframe::CFrameExchange s_Frames;		// core 0 -> core 2
std::atomic<bool> s_bPresenterOn {false};	// core 0 publishes instead of drawing

// Configured from boot.conf (BareVideoPresenterConfigure); the presenter
// starts by itself once the emulator's picture owns the screen.
bool s_bPresenterWanted = false;
bool s_bPresenterWantVsync = true;

// Requests from core 0 to core 2.
std::atomic<bool> s_bRepaintRequest {false};	// re-present the current frame
std::atomic<bool> s_bHoldRequest {false};	// stop flipping (screenshot)
std::atomic<bool> s_bHeld {false};		// core 2: holding, page stable
std::atomic<unsigned> s_nShownPage {0};		// page core 2 scans out now

// --- the line slots as shared state --------------------------------------
//
// With the presenter on, core 0 keeps updating s_Slots (text and rectangle)
// but does not draw them; core 2 draws them into every page it flips to.  A
// sequence lock keeps core 2 from copying a slot half written: core 0 makes
// the sequence odd while it writes, core 2 copies and retries if the
// sequence was odd or moved.  Core 0 never waits for core 2.
std::atomic<unsigned> s_nOverlaySeq {0};
unsigned s_nOverlayWriteDepth = 0;		// core 0 only: nested writers

struct TOverlayWrite
{
	TOverlayWrite (void)
	{
		if (s_nOverlayWriteDepth++ == 0)
		{
			s_nOverlaySeq.store (s_nOverlaySeq.load (std::memory_order_relaxed) + 1,
					     std::memory_order_relaxed);
			std::atomic_thread_fence (std::memory_order_release);
		}
	}

	~TOverlayWrite (void)
	{
		if (--s_nOverlayWriteDepth == 0)
		{
			s_nOverlaySeq.store (s_nOverlaySeq.load (std::memory_order_relaxed) + 1,
					     std::memory_order_release);
		}
	}
};

inline bool CoreZeroDraws (void)
{
	return !s_bPresenterOn.load (std::memory_order_acquire);
}


u32 *FrameBufferWords (void)
{
	return reinterpret_cast<u32 *> (
		static_cast<uintptr_t> (s_pFrameBuffer->GetBuffer ()));
}

// Integer scale that fits the current mode, regardless of source size.
unsigned MaxIntegerScale (unsigned nSrcWidth, unsigned nSrcHeight, unsigned nYStretchNum,
			  unsigned nYStretchDen)
{
	const unsigned nEffHeight = nSrcHeight * nYStretchNum / nYStretchDen;
	unsigned nScaleX = s_nModeWidth / nSrcWidth;
	unsigned nScaleY = nEffHeight ? s_nModeHeight / nEffHeight : 0;

	unsigned nScale = nScaleX < nScaleY ? nScaleX : nScaleY;

	return nScale ? nScale : 1;
}

void ComputeDestination (unsigned nSrcWidth, unsigned nSrcHeight)
{
	if (nSrcWidth == s_nSrcWidth && nSrcHeight == s_nSrcHeight)
	{
		return;
	}

	s_nSrcWidth = nSrcWidth;
	s_nSrcHeight = nSrcHeight;

	// Vertical stretch, in twentieths of a pixel so that the two values the
	// core uses come out exact: 320x200 is a 4:3 picture in square pixels and
	// needs 6/5 = 1.2x vertically; everything else is 1:1.  The core overrides
	// this through its own aspect_ratio, which wins when it says something
	// (BareVideoSetSourceAspect), because the core is the one that knows what
	// the VGA is doing; this is the fallback for when it says nothing.
	const boolean bStretch = (nSrcHeight == 200 || nSrcHeight == 400)
				 && !s_bAspectFromCore;
	const unsigned nStretchNum = bStretch ? 6 : 5;
	const unsigned nStretchDen = bStretch ? 5 : 5;

	// The destination rectangle: the largest 4:3 picture the mode can hold,
	// centred.  Computed as one decision instead of two independent clamps,
	// because the failure mode of two clamps is a picture squashed on one
	// axis - which is what 720x400 gave on a 16:9 mode: 1440x800, a ratio of
	// 1.80, i.e. a text screen stretched a third too wide.
	//
	// Aspect first, size second:
	//
	//   1. take the usable height (the whole mode height: the status panel is
	//      painted *over* the picture now, not in a strip reserved below it);
	//   2. work out the width that height implies for a 4:3 picture;
	//   3. if that width does not fit, derive the height from the width.
	//
	// Integer scaling is kept where it is exact, because a DOS picture at an
	// exact multiple has square pixels and no resampling at all; only when it
	// is not exact is the picture fitted fractionally, which costs one
	// interpolated copy but keeps the shape right.
	// -----------------------------------------------------------------
	// The strip at the bottom belongs to the port, not to the emulator
	// -----------------------------------------------------------------
	//
	// .57: this is what the flicker was.  The panel is drawn on top of the
	// picture once a second, but the picture is *rescanned over it on every
	// frame* - the destination rectangle reaches the bottom of the mode, so
	// the text was overwritten 60 times a second and repainted once, i.e. it
	// was visible for one frame in sixty.  Erasing it first and drawing the
	// picture on top is not a fix; it is the same flicker with more work.
	//
	// So the picture gets the mode *minus* one line slot, and the strip that
	// is left is never written by the emulator.  Three consequences worth
	// naming:
	//
	//   * the panel costs nothing per frame (it is not repainted at all any
	//     more - see BareVideoPresent), which also removes the "repaint the
	//     slot into the emulator's fresh frame" gamble;
	//   * a message from the core (slot 1) is stable for the same reason;
	//   * the picture is the same size whether the panel is shown or not, so
	//     toggling it does not resize the screen and does not leave dirt.
	//
	// The lost 18 pixels come out of the letterbox, not out of the picture:
	// 4:3 in a 16:9 mode already wastes 25 % of the width, and the rectangle
	// is recomputed from what is left.
	const unsigned nSlotHeight = s_Font.GetCharHeight () + 2;
	const unsigned nReserved = nSlotHeight * MaxSlots;
	const unsigned nAvailW = s_nModeWidth;
	const unsigned nAvailH = s_nModeHeight > nReserved
			       ? s_nModeHeight - nReserved : s_nModeHeight;

	// Aspect the picture should have on screen, from the source: the core's
	// own aspect_ratio when it has said anything (it knows what the VGA is
	// doing), otherwise 4:3 - which is what every mode that matters here
	// already declares (retro_system_av_info::geometry.aspect_ratio for
	// 320x200, 640x480 and the text modes is 4/3 in dosbox-core).
	//
	// Note what is deliberately NOT done after this: the 6/5 vertical stretch
	// of 320x200 is not applied to the destination size.  It is already part
	// of the aspect, and applying it twice is how a picture ends up a fifth
	// taller than it should be.  The stretch stays where it belongs, in the
	// scaler's source height (see the nStretchNum use below), so that the
	// sampling grid matches the pixels and the *shape* comes out 4:3.
	unsigned nAspectNum = 4;
	unsigned nAspectDen = 3;
	if (s_bAspectFromCore)
	{
		nAspectNum = s_nCoreAspectX;
		nAspectDen = s_nCoreAspectY;
	}

	// The largest rectangle with that aspect which fits the mode.
	unsigned nFitW = nAvailH * nAspectNum / nAspectDen;
	unsigned nFitH = nAvailH;
	if (nFitW > nAvailW)
	{
		nFitW = nAvailW;
		nFitH = nAvailW * nAspectDen / nAspectNum;
	}

	// Integer scale where one reaches close to that rectangle, fractional
	// otherwise.
	//
	// An exact multiple has square pixels and no resampling: a 320x200 picture
	// at 4x looks like a DOS monitor.  But "the largest
	// integer scale that fits" wastes the mode badly - 640x400 on 1280x720 is
	// 640x480, a third of the screen - so the rule is the largest integer
	// scale whose rectangle fills at least 85 % of the area the aspect allows.
	// 85 % is where the border stops being noticeable; below that, the picture
	// is fitted fractionally, which costs one interpolated copy and keeps the
	// shape exact.
	//
	// The candidate rectangles come from the source's *grid* - width times an
	// integer, height times the same integer - and the aspect of each one is
	// checked before it is accepted, so a source whose grid is not 4:3 cannot
	// sneak a wrong shape in through the integer path.
	const unsigned nAreaAllowed = nFitW * nFitH;

	unsigned nDstW = 0;
	unsigned nDstH = 0;
	for (unsigned nScale = MaxIntegerScale (nSrcWidth, nSrcHeight,
						nStretchNum, nStretchDen);
	     nScale >= 1; nScale--)
	{
		// The source is sampled over a grid nStretchNum/nStretchDen taller,
		// so the destination is that much taller as well: 320x200 at 3x is
		// 960x720, and 960/720 = 4/3 with no further stretching.
		const unsigned nW = nSrcWidth * nScale;
		const unsigned nH = nSrcHeight * nScale;

		if (nW > nFitW || nH > nFitH)
		{
			continue;
		}

		// Aspect of this candidate, against the one wanted.  1/40 of the
		// ratio is the tolerance: 960x720 (exact) passes, 1280x800 (1.6
		// against 1.333) does not.
		const unsigned nDiff = nW * nAspectDen > nH * nAspectNum
					 ? nW * nAspectDen - nH * nAspectNum
					 : nH * nAspectNum - nW * nAspectDen;
		if (nDiff * 40 > nH * nAspectNum)
		{
			continue;
		}

		if (nW * nH * 100 >= nAreaAllowed * 85)
		{
			nDstW = nW;
			nDstH = nH;
			break;
		}

		if (nScale == 1)
		{
			break;
		}
	}

	if (nDstW != 0)
	{
		s_nDstWidth = nDstW;
		s_nDstHeight = nDstH;
	}
	else
	{
		// Fractional fit: the largest rectangle the aspect allows.  Exact
		// shape, no wasted border, one interpolated copy per frame.
		s_nDstWidth = nFitW;
		s_nDstHeight = nFitH;
	}

	// Never zero, whatever the arithmetic did: zero would make the loops below
	// divide by it.
	if (s_nDstWidth == 0)
	{
		s_nDstWidth = 1;
	}
	if (s_nDstHeight == 0)
	{
		s_nDstHeight = 1;
	}

	// Centred in the area the picture is allowed to use, which is the mode
	// minus the reserved strip at the bottom (see above).
	s_nDstX = (s_nModeWidth - s_nDstWidth) / 2;
	s_nDstY = (nAvailH - s_nDstHeight) / 2;

	if (s_nDstWidth > s_nRowBufferPixels)
	{
		delete[] s_pRowBuffer;
		delete[] s_pRowBuffer2;
		s_pRowBuffer = new u32[s_nDstWidth];
		s_pRowBuffer2 = new u32[s_nDstWidth];
		s_nRowBufferPixels = (s_pRowBuffer != nullptr && s_pRowBuffer2 != nullptr)
				   ? s_nDstWidth : 0;

		if (s_pRowBuffer == nullptr || s_pRowBuffer2 == nullptr)
		{
			// One of the two failed: drop both, so the scaler takes the
			// replicate path instead of dereferencing half a pair.
			delete[] s_pRowBuffer;
			delete[] s_pRowBuffer2;
			s_pRowBuffer = nullptr;
			s_pRowBuffer2 = nullptr;
		}
	}
}

void FillRect (u32 *pDst, unsigned nPitch, unsigned nX, unsigned nY,
	       unsigned nWidth, unsigned nHeight, u32 nColor)
{
	for (unsigned y = 0; y < nHeight; y++)
	{
		if (nY + y >= s_nModeHeight)
		{
			break;
		}

		u32 *pRow = pDst + (nY + y) * nPitch + nX;
		for (unsigned x = 0; x < nWidth && nX + x < s_nModeWidth; x++)
		{
			pRow[x] = nColor;
		}
	}
}

// Circle's font puts the *leftmost* pixel of a glyph in the most significant
// bit of the line: CCharGenerator::GetPixel() tests (Line & (1 << (width-1-x))).
// Getting this backwards mirrors every character, which is exactly what the
// first boot showed ("DosBoxCircle" legible in a mirror - the old name, and the boot line says BM-DOS now).  Read the same bit
// Circle does.
void DrawText (u32 *pDst, unsigned nPitch, unsigned nX, unsigned nY,
	       const char *pText, u32 nColor)
{
	const unsigned nCharWidth = s_Font.GetCharWidth ();
	const unsigned nCharHeight = s_Font.GetCharHeight ();

	for (unsigned nLine = 0; nLine < nCharHeight; nLine++)
	{
		if (nY + nLine >= s_nModeHeight)
		{
			break;
		}

		unsigned nPosX = nX;
		for (const char *p = pText; *p != '\0'; p++, nPosX += nCharWidth)
		{
			const u32 nBits = s_Font.GetPixelLine (*p, nLine);

			for (unsigned nBit = 0; nBit < nCharWidth; nBit++)
			{
				if (nPosX + nBit >= s_nModeWidth)
				{
					break;
				}

				if ((nBits & (1U << (nCharWidth - 1 - nBit))) != 0)
				{
					pDst[(nY + nLine) * nPitch + nPosX + nBit] = nColor;
				}
			}
		}
	}
}


// ---------------------------------------------------------------------------
// Resampling: integer and fractional are not the same problem
// ---------------------------------------------------------------------------
//
// Until .58 there was only nearest-neighbour.  That is the right answer when the
// scale is an exact multiple (a 320x200 at 3x has square pixels and no
// resampling at all) and the *wrong* one when it is not - and that is what the
// user saw: "the characters in text mode have small defects".
//
// The two cases need different filters, and which one applies is already known
// because ComputeDestination() decides between an integer path and a
// fractional one:
//
//   * INTEGER (every source pixel becomes the same number of destination
//     pixels): replicate.  Blurring here would be a self-inflicted wound.
//
//   * FRACTIONAL (each destination pixel covers a different fraction of a
//     source pixel, e.g. 720x400 -> 912x684): a box filter - the exact area
//     average - removes the sampling beats (uneven pixel widths, strokes that
//     come out one pixel wide and then two) at the cost of being a little
//     softer.  That trade is what "que se vea suave" means in a DOS emulator,
//     and it is the same problem DOSBox solves with its normalNx scalers.
//
// The vertical pass is separable and cheap because a fractional scale repeats
// each source row once or twice: the row is resampled once and reused while the
// source row is the same.  Horizontally the filter runs per destination pixel
// with fixed-point weights, so there is no floating point per pixel.
//
// Cost: a handful of multiply-adds per pixel per axis.  At 912x684 and 60 Hz
// that is around 150 M ops/s, a few percent of one A72 core - and it lands on
// the side of the port that is *not* the interpreted CPU, so the panel's
// "pace" number is the check on whether the host is keeping up.

/// \brief Linear mix of two ARGB pixels, weight 0..256 on the second.
///
/// Vertical interpolation only, and only two rows: that is all a box filter
/// needs vertically when the step is between one and two rows, and it keeps the
/// inner loop to three multiply-adds per pixel.
static inline u32 MixPixel (u32 nA, u32 nB, unsigned nWeight)
{
	const unsigned nInv = 256 - nWeight;

	const unsigned nR = (((nA >> 16) & 0xFF) * nInv + ((nB >> 16) & 0xFF) * nWeight) >> 8;
	const unsigned nG = (((nA >> 8) & 0xFF) * nInv + ((nB >> 8) & 0xFF) * nWeight) >> 8;
	const unsigned nB_ = ((nA & 0xFF) * nInv + (nB & 0xFF) * nWeight) >> 8;

	return (nR << 16) | (nG << 8) | nB_;
}

/// \brief Resample one source row horizontally with a box filter.
static void ResampleRowBox (const u32 *pSrcRow, unsigned nSrcW,
			    u32 *pDstRow, unsigned nDstW)
{
	// 16 bits of fractional position.  nStep is the distance between two
	// destination pixels in source coordinates.
	//
	// The accumulators below are 64-bit on purpose.  With 32 bits they are
	// safe only while a destination pixel spans at most two source pixels
	// (255 x 2 x 65536 is just under 2^32), which is true of every scale this
	// port actually uses - and false the moment something reduces by more
	// than 2, where they wrap and produce colours with no relation to the
	// picture.  The generator's test drives the function at 640 -> 1 to keep
	// that case honest; 64-bit registers cost nothing on AArch64 and remove
	// the cliff instead of documenting where it is.
	constexpr unsigned FracBits = 16;
	constexpr unsigned FracOne = 1u << FracBits;

	unsigned nPos = 0;
	const unsigned nStep = (nSrcW << FracBits) / nDstW;

	// One reciprocal instead of a divide per pixel.
	//
	// Three 32-bit divides per output pixel is ~50 cycles each on a Cortex-A72
	// (there is no hardware integer divide), and at 1392x1044 that alone is
	// 200 ms per frame - more than the frame budget of a 70 Hz mode.  So the
	// reciprocal of the span is computed once per row and each pixel costs a
	// multiply and a shift.
	//
	// Two details that are not cosmetic, both found by the generator's test:
	//
	//   * the reciprocal is floored, so a multiply-shift alone always rounds
	//     *down*: a flat white row came out 254.  Half an ulp is added before
	//     the shift, which makes it round to nearest and gives 255 back.
	//   * 32 bits of reciprocal is not enough when the span is large: at
	//     640 -> 1 the reciprocal is 102.4 and the truncation alone loses a
	//     level off every channel.  48 bits costs nothing (the product still
	//     fits in 64: the accumulator is at most 255 x nStep, and nStep x
	//     2^48/nStep is 255 x 2^48 = 7.2e16) and the error becomes invisible
	//     even at the extreme.
	//   * nWeightSum is nStep for every pixel except possibly the last, where
	//     the clamp to nSrcW can cut the span short.  Those pixels take the
	//     true divide, which is at most one per row.
	constexpr unsigned InvBits = 48;
	constexpr u64 InvOne = 1ull << InvBits;
	constexpr u64 nHalfUlp = InvOne / 2;

	const u64 nInvSpan = InvOne / nStep;

	for (unsigned nX = 0; nX < nDstW; nX++)
	{
		const unsigned nEnd = nPos + nStep;

		const unsigned nFirst = nPos >> FracBits;
		unsigned nLast = (nEnd - 1) >> FracBits;
		if (nLast >= nSrcW)
		{
			nLast = nSrcW - 1;
		}

		unsigned nWeightSum = 0;
		u64 nR = 0, nG = 0, nB = 0;

		for (unsigned nI = nFirst; nI <= nLast; nI++)
		{
			const unsigned nPixStart = nI << FracBits;
			const unsigned nPixEnd = nPixStart + FracOne;

			const unsigned nFrom = nPos > nPixStart ? nPos : nPixStart;
			const unsigned nTo = nEnd < nPixEnd ? nEnd : nPixEnd;
			if (nTo <= nFrom)
			{
				continue;
			}

			const unsigned nWeight = nTo - nFrom;
			const u32 nPixel = pSrcRow[nI];

			nR += static_cast<u64> ((nPixel >> 16) & 0xFF) * nWeight;
			nG += static_cast<u64> ((nPixel >> 8) & 0xFF) * nWeight;
			nB += static_cast<u64> (nPixel & 0xFF) * nWeight;
			nWeightSum += nWeight;
		}

		if (nWeightSum == 0)
		{
			nWeightSum = 1;		// unreachable; keeps the divide defined
		}

		// nWeightSum == nStep for every pixel except the last, where the
		// clamp to nSrcW can cut it short; the reciprocal is therefore
		// exact enough everywhere and the one true divide stays for the
		// last pixel only when it differs.
		unsigned nR8, nG8, nB8;
		if (nWeightSum == nStep)
		{
			nR8 = static_cast<unsigned> ((nR * nInvSpan + nHalfUlp) >> InvBits);
			nG8 = static_cast<unsigned> ((nG * nInvSpan + nHalfUlp) >> InvBits);
			nB8 = static_cast<unsigned> ((nB * nInvSpan + nHalfUlp) >> InvBits);
		}
		else
		{
			nR8 = static_cast<unsigned> ((nR + nWeightSum / 2) / nWeightSum);
			nG8 = static_cast<unsigned> ((nG + nWeightSum / 2) / nWeightSum);
			nB8 = static_cast<unsigned> ((nB + nWeightSum / 2) / nWeightSum);
		}

		pDstRow[nX] = (nR8 << 16) | (nG8 << 8) | nB8;

		nPos = nEnd;
	}
}

} // namespace

// ---------------------------------------------------------------------------
// The display, and the two places where text can go
// ---------------------------------------------------------------------------
//
// There are exactly two ways anything can be written on the screen, and they
// exist because of a hard constraint: once the emulator draws, it owns the
// picture, and there is no console, no serial terminal and no window manager to
// fall back on.
//
//   BareVideoStatusPrint()   the boot overlay, top-left, in normal flow.  It
//                            carries the kernel's progress and the core's own
//                            log (bare_time.cpp routes every level here).  It
//                            steps aside on the first real frame, and comes
//                            back if the emulator stops (kernel.cpp's
//                            watchdog) - because a frozen picture explains
//                            nothing.
//
//   BareVideoFooter()        three lines in the bottom-left *on top of* the
//                            emulator, redrawn once a second.  DOS never fills
//                            the bottom of a 16:9 display, so this space is
//                            free; it is where keys, clocks, temperature and
//                            the sound device go.
//
// Both use Circle's built-in character generator (CCharGenerator), so there is
// no font data in this project and no library to link.
//
boolean BareVideoInit (unsigned nWidth, unsigned nHeight)
{
	// Virtual height is twice the physical one.  On the models where the
	// firmware grants two pages, SetVirtualOffset() then gives us flicker
	// free page flipping for free.  Where it does not, we simply write to
	// the visible page (set tearoff below if you see tearing).
	// If the mode asked for is not available, try the next one down instead of
	// failing: the whole point of a smaller mode is that the board can afford
	// it, and a monitor that refuses 1280x720 will usually take 1024x768.  The
	// list is ordered by how much work each mode is for the scaler.
	struct TMode { unsigned nW, nH; };
	static const TMode Fallbacks[] = {
		{ 1280,  720 }, { 1024,  768 }, {  800,  600 }, {  640,  480 },
	};
	const unsigned nFallbacks = sizeof Fallbacks / sizeof Fallbacks[0];

	TMode Try = { nWidth, nHeight };
	for (unsigned nAttempt = 0; ; nAttempt++)
	{
		s_pFrameBuffer = new CBcmFrameBuffer (Try.nW, Try.nH, 32,
						      Try.nW, Try.nH * 2);
		if (s_pFrameBuffer != nullptr && s_pFrameBuffer->Initialize ())
		{
			break;
		}

		delete s_pFrameBuffer;
		s_pFrameBuffer = nullptr;

		if (nAttempt >= nFallbacks)
		{
			CLogger::Get ()->Write (FromVideo, LogError,
						"Cannot initialise frame buffer %ux%u",
						nWidth, nHeight);
			return FALSE;
		}

		CLogger::Get ()->Write (FromVideo, LogWarning,
					"Display %ux%u refused, trying %ux%u",
					Try.nW, Try.nH,
					Fallbacks[nAttempt].nW, Fallbacks[nAttempt].nH);
		Try = Fallbacks[nAttempt];
	}

	s_nModeWidth  = s_pFrameBuffer->GetWidth ();
	s_nModeHeight = s_pFrameBuffer->GetHeight ();

	CLogger::Get ()->Write (FromVideo, LogNotice,
				"Display %ux%u (virtual %ux%u), pitch %u",
				s_nModeWidth, s_nModeHeight,
				s_pFrameBuffer->GetVirtWidth (),
				s_pFrameBuffer->GetVirtHeight (),
				s_pFrameBuffer->GetPitch ());

	// Clear the visible page only.  The firmware may have granted a virtual
	// height it could not actually back with memory (this is exactly what
	// "virtual 1920x2160" on a 1080p display is: twice the height, so page
	// flipping was *asked* for, not necessarily given).  Writing the second
	// page would be writing past the end of the buffer if it was not.
	BareVideoFill (0x00000000);

	// --- .72 probes: what the firmware actually gave us --------------------
	// .71 wrote these two with CLogger::Write() and nobody ever saw them: the
	// logger's target is Circle's console, which is a *different* frame buffer
	// from this one (that is the whole reason the boot log appears on top of the
	// picture).  A probe that goes to the log of the layer you are debugging is
	// not a probe.  They go through the status overlay now, which writes here:
	//
	//   "fb: virtual Wx2H"         -> the firmware granted the second page
	//   "fb: virtual WxH"          -> it granted one page only
	//   "fb: size < pitch*H"       -> the buffer cannot hold the mode asked for
	//
	// They are temporary and come out once the mode change is settled.
	{
		char Probe[96];

		snprintf (Probe, sizeof Probe, "fb: real %ux%u pitch %u size %u",
			  s_nModeWidth, s_nModeHeight,
			  s_pFrameBuffer->GetPitch (),
			  static_cast<unsigned> (s_pFrameBuffer->GetSize ()));
		BareVideoStatusPrint (Probe);

		snprintf (Probe, sizeof Probe, "fb: virtual %ux%u vb %u",
			  s_pFrameBuffer->GetVirtWidth (),
			  s_pFrameBuffer->GetVirtHeight (),
			  s_nModeHeight * 2);
		BareVideoStatusPrint (Probe);
	}

	return TRUE;
}

void BareVideoDeinit (void)
{
	delete[] s_pRowBuffer;
	s_pRowBuffer = nullptr;

	delete[] s_pRowBuffer2;
	s_pRowBuffer2 = nullptr;
	s_nRowBufferPixels = 0;

	delete s_pFrameBuffer;
	s_pFrameBuffer = nullptr;
}

static void PresentVisibleTo (u32 *pDst, const u32 *pPixels, unsigned nWidth,
			      unsigned nHeight, unsigned nPitchBytes);
static void PresentVisible (const u32 *pPixels, unsigned nWidth, unsigned nHeight,
			    unsigned nPitchBytes);

// Paints one line slot from the text it holds.  Defined further down, next to
// BareVideoLine() which fills that text in, but used by the present path - the
// slots are repainted on every frame, over the picture.
void DrawSlot (unsigned nSlot);

// Paints black everything the destination rectangle does not cover.  Used by
// the present path (see the wipe in BareVideoPresent), declared here because it
// is used above its definition.
static void BareVideoClearOutside (void);

// Erases one line slot: paints its remembered rectangle black and forgets its
// text.  Declared here for the same reason as DrawSlot() - it is used above its
// definition, by the panel switch.
static void ClearSlot (unsigned nSlot);

// Copies the source frame into the mirror; FALSE only if the mirror could not
// be allocated.
boolean MirrorStore (const u32 *pPixels, unsigned nWidth, unsigned nHeight,
		     unsigned nPitchBytes);

void BareVideoPresent (const u32 *pPixels, unsigned nWidth, unsigned nHeight,
		       unsigned nPitchBytes)
{
	// First real frame from the emulator: the screen is DOS's from now on.
	// Stop writing status lines - but do NOT wipe the screen here.
	//
	// It used to call BareVideoFill(0) at this point, "so the boot log does not
	// sit on top of the C: prompt".  That was wrong twice over, and .69 showed
	// it as a black screen with nothing on it:
	//
	//   * the line that provokes this is DOS's own log (see the [libretro
	//     DEBUG] ones in Circle's log), which is written through CLogger - and
	//     CLogger's target is Circle's console, a *different* frame buffer.  So
	//     this wipe removed the port's own status text and left the console's
	//     text exactly where it was.  What it cleaned was the one thing that was
	//     not in the way;
	//   * and it did it on the FIRST frame, which is not DOS's C: prompt but
	//     whatever the core had ready after retro_load_game - in practice a
	//     black screen.  The boot produced a wipe, and then a wait of seconds
	//     before anything was drawn into it.
	//
	// The order that does what the comment always wanted: let the first frame be
	// drawn, and wipe once *behind* it (the flag below), so the boot text goes
	// and the picture that replaced it stays.  Two presents of margin, because a
	// mode can arrive twice (one from retro_get_system_av_info, one from the
	// first render).
	if (s_bStatusEnabled && pPixels != nullptr && nWidth != 0 && nHeight != 0)
	{
		s_bStatusEnabled = false;
		s_bStatusScrolled = true;
		s_nWipeAfterPresents = 2;
	}

	if (pPixels == nullptr)
	{
		// "Same picture as last time": DOSBox-core passes a null buffer when
		// it has nothing new to upload, and the frontend is expected to show
		// what it already has.  Counted separately because the two cases mean
		// very different things: a growing duplicate count with a live
		// emulator is normal, and a frozen duplicate count means the emulator
		// is not even presenting.
		s_nDuplicatePresents++;
		return;
	}

	s_nFramesPresented++;
	s_nLastFrameMicros = BareMicros ();

	// Note for whoever reads this next: presenting a DOSBox frame does NOT
	// clear the status text, because the emulator's output is drawn inside the
	// letterboxed destination rectangle in the middle of the screen while the
	// status column lives in the top-left corner.  They only overlap when the
	// source is 1920 wide, which DOSBox never produces.
	if (s_pFrameBuffer == nullptr || pPixels == nullptr || nWidth == 0 || nHeight == 0)
	{
		return;
	}

	// Hand the screen to core 2 once the emulator's picture owns it: the boot
	// text is gone (status off) and the margins were wiped behind the first
	// frames (s_nWipeAfterPresents back to 0).  From here on core 0 draws
	// nothing while the presenter runs.
	if (s_bPresenterWanted && !s_bPresenterOn.load (std::memory_order_relaxed)
	    && !s_bStatusEnabled && s_nWipeAfterPresents == 0 && s_nPresentedSinceStart >= 2)
	{
		if (!BareVideoPresenterStart (s_bPresenterWantVsync ? TRUE : FALSE))
		{
			s_bPresenterWanted = false;	// do not retry every frame
		}
	}

	// With the core 2 presenter running, core 0 only hands the frame over:
	// one copy into the triple buffer (which replaces the mirror copy below,
	// so it costs no more than before) and the sampled hash for the
	// governor.  Scaling, page flipping and vsync happen on core 2.
	if (s_bPresenterOn.load (std::memory_order_acquire))
	{
		u64 nHash = 0;
		if (s_Frames.Copy (pPixels, nWidth, nHeight, nPitchBytes / 4, &nHash))
		{
			if (!s_bLastFrameHashValid || nHash != s_nLastFrameHash)
			{
				s_nFramesChanged++;
				s_nLastFrameHash = nHash;
				s_bLastFrameHashValid = TRUE;
			}
			BareCoresWake ();	// core 2 may be sleeping in WFE
		}
		return;
	}

	// Keep the source.  Everything below (and BareVideoRepaint, and the line
	// slots) works from this copy, so that the picture can be rebuilt without
	// the core, which is what lets a line erase itself properly.
	if (!MirrorStore (pPixels, nWidth, nHeight, nPitchBytes))
	{
		return;
	}

	PresentVisible (pPixels, nWidth, nHeight, nPitchBytes);

	// --- heartbeat ---------------------------------------------------------
	// One line per 120 presents, for the whole first second of the picture, and
	// nothing after that.  The .69 boot ended on a black screen and nothing on
	// it said whether the core was presenting black frames, presenting nothing,
	// or presenting into memory that is not on screen - three very different
	// faults that look identical from the sofa.  These numbers tell them apart:
	//
	//   "presents 120 640x400" repeating  -> the core is alive and drawing
	//   it stops after the first line      -> the core stopped after one frame
	//   it never appears at all            -> BareVideoPresent is not called
	//
	// Only during the first second, so a release build does not scribble on the
	// screen for ever.
	// Same lesson as the probes: written through the overlay, because the logger
	// goes to Circle's console and the console is not this frame buffer.
	s_nPresentedSinceStart++;
	if (s_nPresentedSinceStart <= 120 && s_nPresentedSinceStart % 40 == 0)
	{
		char Beat[64];
		snprintf (Beat, sizeof Beat, "picture: %u presents, source %ux%u",
			  s_nPresentedSinceStart, nWidth, nHeight);
		BareVideoStatusPrint (Beat);
	}

	// The wipe described above, now that the picture is on screen, and it touches
	// ONLY the margins - never a pixel of the picture.  .70 painted the mode
	// black and presented the frame again, which is a black frame on the wire
	// between two identical pictures: that is the extra flash the boot showed,
	// "welcome -> black -> welcome".
	if (s_nWipeAfterPresents > 0)
	{
		s_nWipeAfterPresents = 0;

		// Only the margins, and after the picture: no black frame, no second
		// present of the same frame, no flash.  .70 did "fill + present again"
		// and that is the black frame the boot was showing between two copies
		// of the welcome screen.
		BareVideoClearOutside ();

		// The slots live in the bottom margin that was just cleared.
		for (unsigned nSlot = 0; nSlot < MaxSlots; nSlot++)
		{
			DrawSlot (nSlot);
		}
	}

	// The line slots are NOT repainted here any more, and that is the .57 fix
	// for the flicker rather than an omission.
	//
	// Until .56 the picture was presented over the whole mode and the slots
	// were drawn on top of it afterwards, once per frame, because the next
	// frame would overwrite them.  Sixty presents and sixty repaints a second
	// is sixty chances for the panel to be caught half-drawn by the display,
	// and the visible result was a line that flickered whenever the emulator
	// was repainting the area under it - which is the flicker seen on screen.
	//
	// Now the picture is confined to the mode minus one slot per line
	// (ComputeDestination reserves it), so the emulator cannot touch the strip
	// the slots live in and there is nothing to repaint.  A slot is drawn when
	// its text changes - once a second for the panel - and it stays.
}

// Keep the source frame.  Returns FALSE only when there is nowhere to keep it,
// which is a malloc failure and therefore a real problem; the caller then skips
// the frame instead of drawing from a stale buffer.
boolean MirrorStore (const u32 *pPixels, unsigned nWidth, unsigned nHeight,
		     unsigned nPitchBytes)
{
	const unsigned nNeeded = nWidth * nHeight;
	if (nNeeded > s_nMirrorPixels)
	{
		// Grow only, never shrink: a game that switches from 640x480 to
		// 320x200 and back should not allocate twice per mode change.
		delete[] s_pMirror;
		s_pMirror = new u32[nNeeded];
		s_nMirrorPixels = s_pMirror != nullptr ? nNeeded : 0;

		if (s_pMirror == nullptr)
		{
			CLogger::Get ()->Write (FromVideo, LogError,
				"cannot keep a %ux%u source frame (%u KB)",
				nWidth, nHeight, nNeeded * (unsigned) sizeof (u32) / 1024);
			s_nMirrorWidth = 0;
			s_nMirrorHeight = 0;
			return FALSE;
		}
	}

	const unsigned nSrcPitch = nPitchBytes / 4;
	for (unsigned nY = 0; nY < nHeight; nY++)
	{
		memcpy (s_pMirror + nY * nWidth, pPixels + nY * nSrcPitch,
			nWidth * sizeof (u32));
	}

	s_nMirrorWidth = nWidth;
	s_nMirrorHeight = nHeight;

	// --- .65g: did this frame say anything? --------------------------------
	// Every 64th word, FNV-ish, over the same bytes that were just copied.
	// At 640x400 that is 4000 reads out of 256000 - nothing next to the copy
	// it rides on, and precise enough for one question: is this the same
	// picture as last time?  A prompt that is simply sitting there produces
	// an identical buffer 70 times a second; a game in motion changes it every
	// time; a scrolling text screen changes it when it scrolls.
	//
	// The alternative - comparing the whole buffer - would cost a memcmp over
	// 400 KB per frame to answer a question a hash of every 64th word answers
	// for the purposes of a governor that is making a decision per *second*.
	u64 nHash = 1469598103934665603ULL;
	const u32 *pFlat = s_pMirror;
	const unsigned nWords = nWidth * nHeight;
	for (unsigned nI = 0; nI < nWords; nI += 64)
	{
		nHash = (nHash ^ pFlat[nI]) * 1099511628211ULL;
	}
	nHash ^= (static_cast<u64> (nWidth) << 32) | nHeight;

	if (!s_bLastFrameHashValid || nHash != s_nLastFrameHash)
	{
		s_nFramesChanged++;
		s_nLastFrameHash = nHash;
		s_bLastFrameHashValid = TRUE;
	}

	return TRUE;
}

// The scaling and centring, with no knowledge of the mirror or of the status
// lines.  Split out so that BareVideoRepaint() can call it with the mirror as the
// source instead of duplicating the scaler.
static void PresentVisible (const u32 *pPixels, unsigned nWidth, unsigned nHeight,
			    unsigned nPitchBytes)
{
	PresentVisibleTo (FrameBufferWords (), pPixels, nWidth, nHeight, nPitchBytes);
}

// The scaler proper, into any page of the frame buffer: pDst is the first word
// of that page.  Core 0 draws into page 0 (PresentVisible above); the core 2
// presenter draws into whichever page is hidden.
static void PresentVisibleTo (u32 *pDst, const u32 *pPixels, unsigned nWidth,
			      unsigned nHeight, unsigned nPitchBytes)
{
	ComputeDestination (nWidth, nHeight);

	const u32 nDstPitch = s_pFrameBuffer->GetPitch () / 4;

	const unsigned nSrcPitch = nPitchBytes / 4;

	if (s_nDstWidth == nWidth && s_nDstHeight == nHeight)
	{
		// 1:1, straight rows with centring.
		for (unsigned nY = 0; nY < nHeight; nY++)
		{
			memcpy (pDst + (s_nDstY + nY) * nDstPitch + s_nDstX,
				pPixels + nY * nSrcPitch,
				nWidth * sizeof (u32));
		}
	}
	else if (s_pRowBuffer != nullptr)
	{
		// Fractional scale: box filter, both axes (see ResampleRowBox).
		//
		// Two source rows are resampled and mixed vertically, and both results
		// are kept because a fractional vertical scale needs the same two rows
		// for several destination rows in a row.  The mapping dst->src is the
		// same one the integer path uses (nY * nHeight / s_nDstHeight), so the
		// picture lands in exactly the same place; what changes is that the
		// *fractional* part of the position is used instead of being thrown
		// away.
		if (s_pRowBuffer2 == nullptr || s_nRowBufferPixels < s_nDstWidth)
		{
			// No second buffer (allocation failed): replicate rows and keep
			// the old behaviour rather than not drawing at all.
			for (unsigned nY = 0; nY < s_nDstHeight; nY++)
			{
				const unsigned nSrcY = nY * nHeight / s_nDstHeight;
				ResampleRowBox (pPixels + nSrcY * nSrcPitch, nWidth,
						s_pRowBuffer, s_nDstWidth);

				memcpy (pDst + (s_nDstY + nY) * nDstPitch + s_nDstX,
					s_pRowBuffer, s_nDstWidth * sizeof (u32));
			}
		}
		else
		{
			unsigned nRowA = ~0u;		// source row currently in s_pRowBuffer
			unsigned nRowB = ~0u;		// ... and in s_pRowBuffer2

			// Vertical step in fixed point, same 16 bits as the row filter.
			const unsigned nVStep = (nHeight << 16) / s_nDstHeight;

			for (unsigned nY = 0; nY < s_nDstHeight; nY++)
			{
				const unsigned nVPos = nY * nVStep;
				const unsigned nY0 = nVPos >> 16;
				unsigned nY1 = nY0 + 1;
				if (nY1 >= nHeight)
				{
					nY1 = nHeight - 1;
				}

				// Fraction of the way from nY0 to nY1, 0..256.
				const unsigned nMix = ((nVPos & 0xFFFF) * 257) >> 16;

				if (nRowA != nY0)
				{
					ResampleRowBox (pPixels + nY0 * nSrcPitch, nWidth,
							s_pRowBuffer, s_nDstWidth);
					nRowA = nY0;
				}

				if (nMix == 0 || nY1 == nY0)
				{
					memcpy (pDst + (s_nDstY + nY) * nDstPitch + s_nDstX,
						s_pRowBuffer, s_nDstWidth * sizeof (u32));
					continue;
				}

				if (nRowB != nY1)
				{
					ResampleRowBox (pPixels + nY1 * nSrcPitch, nWidth,
							s_pRowBuffer2, s_nDstWidth);
					nRowB = nY1;
				}

				u32 *pOut = pDst + (s_nDstY + nY) * nDstPitch + s_nDstX;
				for (unsigned nX = 0; nX < s_nDstWidth; nX++)
				{
					pOut[nX] = MixPixel (s_pRowBuffer[nX],
							     s_pRowBuffer2[nX], nMix);
				}
			}
		}
	}
	else
	{
		// No row buffer at all: the very first frame, before anything has been
		// allocated.  Replicate and let the next frame do it properly.
		for (unsigned nY = 0; nY < s_nDstHeight; nY++)
		{
			const unsigned nSrcY = nY * nHeight / s_nDstHeight;
			const u32 *pSrcRow = pPixels + nSrcY * nSrcPitch;
			u32 *pOut = pDst + (s_nDstY + nY) * nDstPitch + s_nDstX;

			for (unsigned nX = 0; nX < s_nDstWidth; nX++)
			{
				pOut[nX] = pSrcRow[nX * nWidth / s_nDstWidth];
			}
		}
	}
}

void BareVideoSetRefreshHz (double fHz)
{
	// The core reports the emulated rate as a double (59.923076..., 70.086...).
	// Stored in millihertz as an integer so that nothing downstream has to do
	// floating point in a per-frame path.
	if (fHz < 20.0 || fHz > 200.0)
	{
		return;			// nonsense: keep whatever was there
	}

	const unsigned nMilliHz = static_cast<unsigned> (fHz * 1000.0 + 0.5);

	if (nMilliHz == s_nCoreRefreshMHz)
	{
		return;
	}

	s_nCoreRefreshMHz = nMilliHz;

	// Tell the thread layer what a frame costs, so that it can hold *emulator
	// driven* frames to the same budget the frame loop holds the frontend to.
	// This is the .57 fix for the second speed bug: when a game draws faster
	// than the announced rate, DOSBox-core never returns to retro_run() and
	// loops on its own, so kernel.cpp's pacer never runs.  See the long note
	// on s_nFrameDrives in bare_thread.cpp.
	const unsigned nHz = (nMilliHz + 500) / 1000;
	if (nHz != 0)
	{
		BareThreadSetFrameBudget ((1000000 + nHz / 2) / nHz);
	}

	// One number, one line, and it is the mode's own rate because that is all
	// this port ever asks the core for.  .65 had a second number here (a pin
	// that lowered the rate) and with it came a star in the panel, a banner at
	// boot and a rule to keep the three in step.  All of it is gone: the rate
	// the core announces IS the rate the port paces at, and the panel's
	// denominator is that number.  See bare_coreopts.h for why.
	CLogger::Get ()->Write (FromVideo, LogNotice,
		"core refresh rate: %u.%03u Hz", nMilliHz / 1000, nMilliHz % 1000);
}

unsigned BareVideoRefreshMilliHz (void)
{
	// Zero means "the core has not said, or said something silly".  The caller
	// decides what to do about that; this layer does not guess.
	return s_nCoreRefreshMHz;
}

void BareVideoSetSourceAspect (unsigned nAspectX, unsigned nAspectY)
{
	// The core reports geometry.aspect_ratio as a double, and the caller turns
	// it into a fraction with a small denominator (960/720 comes out as 4/3).
	// Anything silly is ignored rather than trusted: this runs on a bare metal
	// board where a divide by zero is a hang, not an exception.
	if (nAspectX == 0 || nAspectY == 0)
	{
		s_bAspectFromCore = FALSE;
		return;
	}

	// A ratio far outside 4:3/16:9 is a bug in the caller, not a picture.
	if (nAspectX * 3 > nAspectY * 8)
	{
		return;
	}

	s_nCoreAspectX = nAspectX;
	s_nCoreAspectY = nAspectY;
	s_bAspectFromCore = TRUE;

	// Force the destination rectangle to be recomputed with the new ratio:
	// ComputeDestination() returns early when the source geometry has not
	// changed, and here the geometry is exactly the same.
	s_nSrcWidth = 0;
	s_nSrcHeight = 0;
}

void BareVideoRepaint (void)
{
	if (!CoreZeroDraws ())
	{
		s_bRepaintRequest.store (true, std::memory_order_release);
		BareCoresWake ();
		return;
	}

	// The present path only recomputes what is visible; see the mirror above.
	// This is what makes a skipped present still erase a line, and what makes
	// a line change not leave the old one behind: the source frame is still
	// here, so the rectangle under the line can be rebuilt without waiting for
	// the emulator to draw anything.
	if (s_pMirror == nullptr || s_nMirrorWidth == 0 || s_nMirrorHeight == 0)
	{
		return;
	}

	if (s_pFrameBuffer == nullptr)
	{
		return;
	}

	// Reuse the normal present path: it is the one place that knows about the
	// scale, the centring and the row buffer.  Passing the mirror as the
	// source costs one extra copy of the visible rectangle and saves having a
	// second implementation of the scaler that could disagree with the first.
	const unsigned nPitchBytes = s_nMirrorWidth * static_cast<unsigned> (sizeof (u32));

	// The picture only.  The slots live in the reserved strip below it and
	// were not covered by anything this repaint is undoing (see the note in
	// BareVideoPresent), so redrawing them here would be work with no effect.
	PresentVisible (s_pMirror, s_nMirrorWidth, s_nMirrorHeight, nPitchBytes);

	// And the line slots, after the picture and not before: DrawSlot() paints
	// in the reserved strip, and anything drawn before PresentVisible() is a
	// black rectangle by the time the frame is on screen.  BareVideoPresent()
	// deliberately does not repaint them (they only change once a second), so
	// this is the path that puts them back when something else has scribbled
	// over the strip - the wipe at the start of a mode, and the log lines that
	// the fuzzing probe below writes there.
	for (unsigned nSlot = 0; nSlot < MaxSlots; nSlot++)
	{
		DrawSlot (nSlot);
	}
}

// ---------------------------------------------------------------------------
// THE FUZZ
// ---------------------------------------------------------------------------
//
// Evidence first, because this one is not a theory.  Two frame-buffer captures
// on a Pi 3 at 800x600, and one at 1024x768 (SHOT0007.png), measured:
//
//   * 800x600: 35 "text" bands in the left margin, pitch 14.3 px (median 16).
//   * 800x600, other boot: 41 bands, pitch 13.0 px.
//   * 1024x768: NOTHING anywhere for 335 rows, then a sprinkle from row 384,
//     and from row 656 to 767 the whole width (x 0..1023) full of dots and
//     dashes - inside and outside the picture.
//
// Three things follow from those numbers and not from an opinion:
//
//   1. The pattern is not Circle's text.  8x16 with 2 px of gap gives a pitch
//      of 18, and 8x8 is the DOS font, which the panel does not use.  Neither
//      capture has 18.
//   2. It does not scale with the mode: 30 KB of "content" in the bottom of a
//      1024x768 against 6 KB at 800x600, in a *different* place.  Text would
//      land in the same rows of the same layout.
//   3. It is a repeating, dense, dotted pattern with the span of a few rows -
//      "unos 10 frames que se repiten" was the description that started this,
//      and it is the description of a small pattern tiled over and over.
//
// The only writer in this port that can produce that is the status column when
// its line cursor runs away: BareVideoStatus(nLine, …) computes
// y = STATUS_TOP + nLine * nLineHeight and clamps the *drawing* by nY, so a
// cursor that grows past the last line still writes rows.  Nothing bounded
// the cursor, and the core,
// through BareLog → BareVideoStatusPrint, is the one feeding it.
//
// So: bound it.  A status line beyond the last line the mode can show replaces
// the last one instead of being written past it, which is what the scrolling
// column is supposed to do anyway.  An emulator that logs forty lines a second
// for ever then costs forty overwrites of the same row, not forty new rows.
unsigned BareVideoStatusLines (void)
{
	if (s_pFrameBuffer == nullptr)
	{
		return 0;
	}

	const unsigned nLineHeight = s_Font.GetCharHeight () + 2;
	const unsigned nAvailable = s_nModeHeight > STATUS_TOP ? s_nModeHeight - STATUS_TOP : 0;
	const unsigned nLines = nAvailable / nLineHeight;

	return nLines < STATUS_MAX_LINES ? nLines : STATUS_MAX_LINES;
}

/// \brief Let the status overlay draw again.
///
/// The screen belongs to the emulator once it presents frames, and the overlay
/// steps aside (see BareVideoPresent).  If the emulator stops presenting - a
/// frame loop that is blocked, or one that has not produced anything yet - then
/// the status lines are worth more than a frozen picture: they are the only
/// account of what is happening.  The kernel re-arms the overlay from the frame
/// loop when a frame is overdue.
void BareVideoStatusEnable (void)
{
	if (!s_bStatusEnabled)
	{
		BareVideoFill (0x00000000);
		s_bStatusEnabled = true;
		s_bStatusScrolled = false;
		s_nStatusCursor = 0;
	}
}

boolean BareVideoStatusIsEnabled (void)
{
	return s_bStatusEnabled ? TRUE : FALSE;
}

void BareVideoStatusReset (void)
{
	// Deliberately does NOT clear the screen.  It is called once, from the
	// start of the kernel, where the display has just been filled black; and
	// the tempting "wipe it again when the first frame arrives" is what erased
	// the boot text in the last two boots.  The status column is the only map
	// of what happened before the hang, so nothing here is allowed to throw it
	// away.  If a future caller genuinely needs a blank screen, it wants
	// BareVideoFill() and it should say why.
	s_nStatusCursor = 0;
	s_bStatusScrolled = false;
}

void BareVideoStatusPrint (const char *pText)
{
	if (s_pFrameBuffer == nullptr || pText == nullptr || !s_bStatusEnabled)
	{
		return;
	}

	// One line of history, and no more than the screen has rows for.
	//
	// The failure this closes, measured and not guessed: see the long note above
	// ("THE FUZZ").  The column used to scroll one line per message, so a source
	// that logs faster than a human reads made the column
	// churn - and the pointer to what is happening on screen is gone the moment
	// the message that matters arrives.  Here, a message that arrives when the
	// column is already full simply replaces the last line: the newest thing is
	// always the one at the bottom, and the total amount of drawing per second
	// stops depending on how noisy the core is.
	const unsigned nLines = BareVideoStatusLines ();
	if (nLines == 0)
	{
		return;
	}

	if (s_nStatusCursor >= nLines)
	{
		s_nStatusCursor = nLines - 1;
	}

	BareVideoStatus (s_nStatusCursor, pText);

	if (s_nStatusCursor + 1 < nLines)
	{
		s_nStatusCursor++;
	}
	else
	{
		// The column is full: the next line replaces this one instead of
		// scrolling.  s_bStatusScrolled says the top line is no longer the
		// first message, which is what other code asks before trusting the
		// column as a history.
		s_bStatusScrolled = true;
	}
}

void BareVideoStatus (unsigned nLine, const char *pText)
{
	if (s_pFrameBuffer == nullptr || pText == nullptr || !s_bStatusEnabled)
	{
		return;
	}

	const unsigned nLineHeight = s_Font.GetCharHeight () + 2;
	const unsigned nY = STATUS_TOP + nLine * nLineHeight;

	if (nY + s_Font.GetCharHeight () > s_nModeHeight)
	{
		return;
	}

	u32 *pDst = FrameBufferWords ();
	const u32 nPitch = s_pFrameBuffer->GetPitch () / 4;

	// Erase the line first, right edge of the mode included.
	//
	// Without this the new text is drawn on top of the old one and the two
	// overlap character by character - "Var±ablerGbla&ka±uA±uuuuuu&" in the
	// photo of the boot, which is "Variable dosbox::machine ..." and the next
	// message occupying the same pixels.  It never showed before because the
	// column scrolled: every message moved the text up, so a line was only ever
	// written once.  Since .72 the newest message REPLACES the last one (that is
	// what stops a noisy core from churning the screen), so the same line is now
	// written over and over, and "over and over" needs an erase in between.
	FillRect (pDst, nPitch, STATUS_X, nY, s_nModeWidth - STATUS_X,
		  s_Font.GetCharHeight (), 0x00000000);

	DrawText (pDst, nPitch, STATUS_X, nY, pText, 0x00FFFFFF);
}

void BareVideoFill (u32 nColor)
{
	if (s_pFrameBuffer == nullptr)
	{
		return;
	}

	// A full-screen paint from core 0 (the stall report, a new boot log) is
	// core 0 taking the screen back: the presenter stops first and leaves
	// page 0 scanned out.
	BareVideoPresenterStop ();

	// The *whole* buffer, virtual size included: both pages.
	//
	// This used to say "Both pages" and then fill s_nModeWidth x s_nModeHeight at
	// offset 0, which is one page.  BareVideoInit() asks the firmware for a
	// virtual height of twice the physical one, so when the firmware grants it
	// the second page is real memory that nothing was clearing - and the moment
	// anything offsets the scanout into it (the firmware doing its own output
	// switch after a mode change is enough; this port never calls
	// SetVirtualOffset()), what is on screen is whatever was in that memory.
	//
	// GetSize() is the buffer the firmware actually allocated, which is the only
	// honest bound here: the virtual geometry is what was *asked for*, and this
	// comment block in BareVideoInit already records that the firmware sometimes
	// cannot back it.
	const u32 nPitchBytes = s_pFrameBuffer->GetPitch ();
	const u32 nSize = s_pFrameBuffer->GetSize ();
	if (nPitchBytes == 0 || nSize == 0)
	{
		return;
	}

	u32 *pWords = FrameBufferWords ();
	const u32 nWords = nSize / static_cast<u32> (sizeof (u32));
	for (u32 nI = 0; nI < nWords; nI++)
	{
		pWords[nI] = nColor;
	}

	// Circle maps normal RAM as cacheable, and this is the one write in the
	// port that fills megabytes at once, right in front of a scanout that does
	// not read through the CPU cache.  The old single-page fill was small
	// enough to wash out on its own; a 7 MB one is not, and a "cleared" screen
	// that is only cleared in the cache is a garbage screen with extra steps.
	// Per present this costs a few hundred microseconds, once per mode change.
	CleanAndInvalidateDataCacheRange (s_pFrameBuffer->GetBuffer (), nSize);
}

/// \brief Paint black everything the picture does not cover.
///
/// Four rectangles around the destination rectangle - top, bottom, left, right
/// - and never a pixel inside it.  That is the whole point: the alternative
/// (fill the mode black and draw the picture again) puts a black frame on the
/// screen for one refresh, which is exactly the "welcome -> black -> welcome"
/// the boot was showing.  This removes the boot text that is in the margins
/// without ever showing an empty screen.
static void BareVideoClearOutside (void)
{
	if (s_pFrameBuffer == nullptr || s_nDstWidth == 0 || s_nDstHeight == 0)
	{
		return;
	}

	const u32 nPitch = s_pFrameBuffer->GetPitch () / 4;
	u32 *pDst = FrameBufferWords ();

	// Above and below the picture, the full width of the mode.
	FillRect (pDst, nPitch, 0, 0, s_nModeWidth, s_nDstY, 0x00000000);
	FillRect (pDst, nPitch, 0, s_nDstY + s_nDstHeight, s_nModeWidth,
		  s_nModeHeight > s_nDstY + s_nDstHeight
			  ? s_nModeHeight - (s_nDstY + s_nDstHeight) : 0,
		  0x00000000);

	// And the two side margins, only as tall as the picture.
	FillRect (pDst, nPitch, 0, s_nDstY, s_nDstX, s_nDstHeight, 0x00000000);
	FillRect (pDst, nPitch, s_nDstX + s_nDstWidth, s_nDstY,
		  s_nModeWidth > s_nDstX + s_nDstWidth
			  ? s_nModeWidth - (s_nDstX + s_nDstWidth) : 0,
		  s_nDstHeight, 0x00000000);
}

void BareVideoShowMessage (const char *pText)
{
	// A core message goes in the line under the panel, same corner, same
	// erase-on-expiry - see BareVideoLine().  Erasing is the caller's job
	// (BareCoreRunFrame does it when the message's deadline passes), which is
	// why this is a one-liner and no longer a box with a border: a box has to be
	// erased too, and the old one never was.
	BareVideoLine (1, pText != nullptr && *pText != '\0' ? pText : nullptr);
}

// Little-endian writers for the BMP header.  Written by hand because there is
// no libc endian helper here and the header has to be exact: a BMP with the
// wrong field offset opens as garbage in every viewer.
static void Put16 (u8 *p, unsigned nOffset, unsigned nValue)
{
	p[nOffset + 0] = static_cast<u8> (nValue & 0xFF);
	p[nOffset + 1] = static_cast<u8> ((nValue >> 8) & 0xFF);
}

static void Put32 (u8 *p, unsigned nOffset, u32 nValue)
{
	p[nOffset + 0] = static_cast<u8> (nValue & 0xFF);
	p[nOffset + 1] = static_cast<u8> ((nValue >> 8) & 0xFF);
	p[nOffset + 2] = static_cast<u8> ((nValue >> 16) & 0xFF);
	p[nOffset + 3] = static_cast<u8> ((nValue >> 24) & 0xFF);
}

boolean BareVideoScreenshot (void)
{
	if (s_pFrameBuffer == nullptr || s_nModeWidth == 0 || s_nModeHeight == 0)
	{
		return FALSE;
	}

	// Numbered, and NEVER overwritten - which is what this comment has always
	// claimed and the code did not do.  The counter lives in RAM, so it
	// starts at 0 on every boot, and fopen(…, "wb") truncates: the second boot
	// destroyed SHOT0000 of the first, and a boot that takes two shots ended up
	// with one file.  .70: the name is only accepted if the file does not exist
	// yet, and the check is done by trying to *read* it - on FatFs there is no
	// O_EXCL, and fopen("rb") is the only honest way to ask "is this name free?"
	// without creating something.
	static unsigned s_nShot = 0;
	char Path[40];
	boolean bOpened = FALSE;
	FILE *pFile = nullptr;
	for (unsigned nTry = 0; nTry < 100 && !bOpened; nTry++)
	{
		snprintf (Path, sizeof Path, "SD:/DOSBOX/SHOT%04u.BMP", s_nShot++);

		FILE *pExists = fopen (Path, "rb");
		if (pExists != nullptr)
		{
			fclose (pExists);
			continue;		// that number is taken: next one
		}

		pFile = fopen (Path, "wb");
		bOpened = pFile != nullptr ? TRUE : FALSE;
	}

	if (!bOpened)
	{
		CLogger::Get ()->Write (FromVideo, LogWarning,
					"screenshot: no free SHOTnnnn.BMP name (100 tried)");
		return FALSE;
	}

	{
		const unsigned nRowBytes = (s_nModeWidth * 3 + 3) & ~3u;
		const u32 nImageBytes = nRowBytes * s_nModeHeight;
		u8 Header[54] = { 0 };

		Header[0] = 'B';
		Header[1] = 'M';
		Put32 (Header, 2, 54 + nImageBytes);
		Put32 (Header, 10, 54);
		Put32 (Header, 14, 40);
		Put32 (Header, 18, s_nModeWidth);
		Put32 (Header, 22, s_nModeHeight);
		Put16 (Header, 26, 1);
		Put16 (Header, 28, 24);
		Put32 (Header, 34, nImageBytes);

		boolean bOk = fwrite (Header, 1, sizeof Header, pFile) == sizeof Header;

		u32 nPitch = s_pFrameBuffer->GetPitch () / 4;
		const u32 *pDst = FrameBufferWords ();

		// With the presenter on, the page on screen changes under us 60
		// times a second.  Ask core 2 to hold (it stops flipping and acks),
		// copy the page it shows into RAM, and let it go again before the
		// slow part - the card write - so that the picture freezes for the
		// few milliseconds of the copy only.
		u32 *pCopy = nullptr;
		if (!CoreZeroDraws ())
		{
			s_bHoldRequest.store (true, std::memory_order_release);
			BareCoresWake ();
			for (unsigned nWait = 0; nWait < 200 && !s_bHeld.load (std::memory_order_acquire); nWait++)
			{
				CTimer::SimpleMsDelay (1);
			}

			pCopy = new u32[static_cast<size_t> (s_nModeWidth) * s_nModeHeight];
			if (pCopy != nullptr && s_bHeld.load (std::memory_order_acquire))
			{
				const u32 *pPage = FrameBufferWords ()
					+ static_cast<uintptr_t> (s_nShownPage.load (std::memory_order_acquire))
					  * s_nModeHeight * nPitch;
				for (unsigned nY = 0; nY < s_nModeHeight; nY++)
				{
					memcpy (pCopy + static_cast<size_t> (nY) * s_nModeWidth,
						pPage + static_cast<size_t> (nY) * nPitch,
						s_nModeWidth * sizeof (u32));
				}
				pDst = pCopy;
				nPitch = s_nModeWidth;
			}

			s_bHoldRequest.store (false, std::memory_order_release);
			BareCoresWake ();
		}

		u8 *pRow = new u8[nRowBytes];
		if (pRow == nullptr)
		{
			delete[] pCopy;
			fclose (pFile);
			return FALSE;
		}

		// BMP rows run bottom to top.
		for (unsigned nY = 0; bOk && nY < s_nModeHeight; nY++)
		{
			const u32 *pSrc = pDst + (s_nModeHeight - 1 - nY) * nPitch;
			for (unsigned nX = 0; nX < s_nModeWidth; nX++)
			{
				const u32 nPixel = pSrc[nX];
				pRow[nX * 3 + 0] = static_cast<u8> (nPixel & 0xFF);	  // B
				pRow[nX * 3 + 1] = static_cast<u8> ((nPixel >> 8) & 0xFF);  // G
				pRow[nX * 3 + 2] = static_cast<u8> ((nPixel >> 16) & 0xFF); // R
			}

			for (unsigned nX = s_nModeWidth * 3; nX < nRowBytes; nX++)
			{
				pRow[nX] = 0;
			}

			bOk = fwrite (pRow, 1, nRowBytes, pFile) == nRowBytes;
		}

		delete[] pRow;
		delete[] pCopy;
		fclose (pFile);

		if (bOk)
		{
			CLogger::Get ()->Write (FromVideo, LogNotice, "screenshot: %s", Path);
			return TRUE;
		}
	}

	return FALSE;
}

void BareVideoFooterSetVisible (boolean bVisible)
{
	TOverlayWrite Guard;
	const boolean bWas = s_bFooterVisible;

	if (bWas == bVisible)
	{
		return;
	}

	if (bWas && !bVisible)
	{
		// Turning it OFF.  The slot holds the last text it was given (see
		// BareVideoNoteHiddenPanelText), so there is a rectangle to hand back
		// and a text to forget, and ClearSlot() does both.
		//
		// This is done *before* the switch, and that ordering is the whole
		// fix: BareVideoLine() and DrawSlot() both used to consult
		// s_bFooterVisible, so with the flag already off there was no way to
		// reach the rectangle - the line stayed on screen for ever, which is
		// exactly "no se borra su zona y se queda la información congelada".
		//
		// No BareVideoRepaint() any more: the slot lives in the reserved strip
		// at the bottom, which the emulator never draws to, so there is no
		// picture underneath to restore.  Filling the rectangle black *is* the
		// erase.
		ClearSlot (0);
	}

	s_bFooterVisible = bVisible;

	if (bVisible)
	{
		// Turning it ON: draw once immediately, so the first thing visible
		// sees is the panel and not a second of nothing.
		DrawSlot (0);
	}
}

boolean BareVideoFooterVisible (void)
{
	return s_bFooterVisible;
}

// Where the text of a hidden panel goes.
//
// A hidden panel still has to *remember* what it was told, because that is what
// its rectangle is erased from when it is switched off: if the text is dropped,
// the slot looks empty and its pixels stay on screen (the "información
// congelada" of .58).  Storing it without drawing is exactly what a slot that
// is not visible needs.
static void BareVideoNoteHiddenPanelText (const char *pText)
{
	if (pText == nullptr)
	{
		return;
	}

	TLineSlot &Slot = s_Slots[0];

	size_t nChars = strlen (pText);
	if (nChars >= sizeof Slot.Text)
	{
		nChars = sizeof Slot.Text - 1;
	}

	memcpy (Slot.Text, pText, nChars);
	Slot.Text[nChars] = '\0';
}

// Erase one slot: paint its remembered rectangle black and empty its text.
//
// One function, because there are two callers - hiding the panel and a slot
// being replaced by nothing - and .58 shipped with the rule "the erase is the
// empty slot" written in two places that disagreed.  The rectangle is kept
// (nWidth/nHeight/nX/nY stay) so that the next line drawn there erases the same
// area; only the text goes.
static void ClearSlot (unsigned nSlot)
{
	if (s_pFrameBuffer == nullptr || nSlot >= MaxSlots)
	{
		return;
	}

	TLineSlot &Slot = s_Slots[nSlot];

	// Nothing was ever drawn here: there is no rectangle to hand back, and
	// painting one would mean painting a rectangle we never painted text in.
	if (Slot.nWidth == 0)
	{
		return;
	}

	TOverlayWrite Guard;

	if (CoreZeroDraws ())
	{
		const u32 nPitch = s_pFrameBuffer->GetPitch () / 4;
		FillRect (FrameBufferWords (), nPitch, Slot.nX, Slot.nY,
			  Slot.nWidth, Slot.nHeight, 0x00000000);
	}

	Slot.Text[0] = '\0';
}

// Draw one line, from the text it was last given.  Kept apart from
// BareVideoLine() so that the present path can repaint a slot without having
// the text passed in again: the text lives in the slot, and repainting is
// something that happens once per frame, not once per line change.
//
// An empty slot is drawn as *nothing*, not as an erasure: use ClearSlot() for
// that.  This function used to try to do both - `pText[0] == 0` returned
// early, with a comment claiming the rectangle was cleared first - and it was
// not, so BareVideoFooterSetVisible() emptied the slot, called this, and left
// the frozen line exactly where it was.  Which is half of the symptom
// ("no se borra su zona") surviving its own fix.
void DrawSlot (unsigned nSlot)
{
	if (s_pFrameBuffer == nullptr || nSlot >= MaxSlots)
	{
		return;
	}

	// NOTE: slot 0 is NOT skipped when the panel is switched off, and that is
	// deliberate.  BareVideoLine() returns early while the panel is hidden -
	// nothing should be *written* to a hidden panel - but this function is the
	// repaint path, which runs once per frame, and skipping slot 0 here would
	// mean the strip under a visible panel is never refreshed.  Erasing goes
	// through ClearSlot(); drawing an empty slot here is a no-op.
	TLineSlot &Slot = s_Slots[nSlot];

	// Nothing was ever drawn here, or it was erased: nothing to draw.
	if (Slot.nWidth == 0 || Slot.Text[0] == '\0' || !CoreZeroDraws ())
	{
		return;
	}

	const char *pText = Slot.Text;

	const u32 nPitch = s_pFrameBuffer->GetPitch () / 4;
	u32 *pDst = FrameBufferWords ();

	const unsigned nLineHeight = s_Font.GetCharHeight () + 2;

	// Wipe the rectangle first: the text changes width every second (the
	// counters grow), and the tail of a longer previous line would stay.
	FillRect (pDst, nPitch, Slot.nX, Slot.nY, Slot.nWidth, Slot.nHeight,
		  0x00000000);

	DrawText (pDst, nPitch, Slot.nX + PanelMargin, Slot.nY + 1, pText,
		  0x00FFFFFF);

	(void) nLineHeight;
}

void BareVideoLine (unsigned nSlot, const char *pText)
{
	if (s_pFrameBuffer == nullptr || nSlot >= MaxSlots)
	{
		return;
	}

	// Writing to a hidden panel makes no sense - it is invisible - so this one
	// *does* return early.  Erasing it goes through
	// DrawSlot(), which is where the note above explains why that one must not
	// return early.
	TOverlayWrite Guard;
	const bool bDraw = CoreZeroDraws ();

	if (nSlot == 0 && !s_bFooterVisible)
	{
		BareVideoNoteHiddenPanelText (pText);
		return;
	}

	TLineSlot &Slot = s_Slots[nSlot];
	const u32 nPitch = s_pFrameBuffer->GetPitch () / 4;
	u32 *pDst = FrameBufferWords ();

	// nullptr means "erase this slot", and it erases the rectangle that slot
	// actually drew last time.  Nothing else can do it: these lines are painted
	// *after* the emulator's frame, and a DOS text screen does not repaint
	// itself, so a line nobody erases stays for ever - which is what happens
	// to the "CPU speed" message after Ctrl+F11.
	if (pText == nullptr)
	{
		// Where the pixels come back from: the mirror.  Repainting here, and
		// not in the caller, means every erase - the panel switch, a message
		// timing out, a message replaced by a shorter one - ends with the
		// emulator's picture visible again instead of a black rectangle.
		// The rectangle is kept: it is the region that has to be erased
		// the next time something is drawn there.  Only the pixels and the
		// text go.  Same function as the panel switch, on purpose.
		ClearSlot (nSlot);

		// No BareVideoRepaint() here any more.  The slot is in the reserved
		// strip, which the emulator does not draw to, so there is nothing
		// underneath it to put back: painting it black *is* the erase.  Until
		// .57 this called a full repaint of the picture per erase, which is
		// both unnecessary and - being a partial redraw next to a live scanout
		// - part of what made toggling the panel look dirty.
		return;
	}

	// One line per slot, stacked downwards, in the strip at the *bottom* of the
	// display that ComputeDestination() keeps the picture out of.
	//
	// Why the bottom and not the top-right corner it used to be: the strip is
	// the one region of the frame buffer the emulator never writes to, which
	// is what makes the text stable instead of being repainted over sixty
	// times a second (see the note in BareVideoPresent).  A 4:3 picture in a
	// 16:9 mode leaves those rows empty anyway, so nothing is taken from the
	// game - and if the monitor's own OSD uses the bottom, it draws on the
	// panel, not the other way round.
	const unsigned nLineHeight = s_Font.GetCharHeight () + 2;

	// Clamped twice, and both clamps matter.  strlen() is a size_t->unsigned
	// narrowing (a line of text is never 4 GB, but the compiler cannot know
	// that and -Wconversion is right to say so), and a text longer than the
	// screen would make nX unsigned-wrap and draw off the left edge.
	size_t nChars = strlen (pText);
	if (nChars > s_nModeWidth)
	{
		nChars = s_nModeWidth;
	}

	// Left-aligned and *fixed*, which is two changes in one.  It used to be
	// right-aligned, so every digit that disappeared (the queue count, the drop
	// counter) moved the whole line sideways and made it impossible to read
	// while watching it.  Now the line starts at the left edge and grows to the
	// right, so a changing number changes one end and nothing else moves.
	const unsigned nTextWidth = static_cast<unsigned> (nChars) * s_Font.GetCharWidth ();
	const unsigned nWidth = nTextWidth + 2 * PanelMargin;
	const unsigned nX = 0;
	// Stacked upwards from the last row of the mode: slot 0 (the panel) on the
	// bottom line, slot 1 (a message from the core) above it.  Both inside the
	// strip reserved by ComputeDestination.
	const unsigned nY = s_nModeHeight - (nSlot + 1) * nLineHeight;

	// Erase before drawing: the text changes width every second (the drop
	// counter grows), and without this the tail of the previous longer line
	// would stay behind.
	if (Slot.nWidth != 0 && bDraw)
	{
		FillRect (pDst, nPitch, Slot.nX, Slot.nY,
			  Slot.nWidth, Slot.nHeight, 0x00000000);
	}

	// A copy clamped to the same length as the box, so that the erase rectangle
	// and the text always agree: strlen() bounds the copy, and the longest line
	// this port draws is under 100 characters.  The copy also goes into the
	// slot, which is what lets the present path repaint it without the caller.
	if (nChars >= sizeof Slot.Text)
	{
		nChars = sizeof Slot.Text - 1;
	}
	memcpy (Slot.Text, pText, nChars);
	Slot.Text[nChars] = '\0';

	if (bDraw)
	{
		FillRect (pDst, nPitch, nX, nY, nWidth, nLineHeight, 0x00000000);
		DrawText (pDst, nPitch, nX + PanelMargin, nY + 1, Slot.Text, 0x00FFFFFF);
	}

	Slot.nX = nX;
	Slot.nY = nY;
	Slot.nWidth = nWidth;
	Slot.nHeight = nLineHeight;
}

void BareVideoFooter (unsigned nRow, const char *pText)
{
	// The name the frame loop has always used for the panel.  Row 0 is the
	// panel; the parameter is kept so that the caller does not have to learn a
	// new name for the same thing.
	BareVideoLine (nRow, pText);
}

u64 BareVideoFramesPresented (void)
{
	// Counted in BareVideoPresent(), i.e. once per picture that reached the
	// frame buffer.  The frame loop uses it for two things: waiting until the
	// core has produced something before drawing the panel, and detecting a
	// DOS prompt (a text screen that is not changing presents nothing, and a
	// prompt is where the clock governor wants to be at 600 MHz).
	return s_nFramesPresented;
}

u64 BareVideoFramesChanged (void)
{
	// Frames that were not identical to the one before them.  A machine that
	// presents 70 frames a second and changes none of them is idle, and that is
	// the difference the clock governor needs and the present count cannot give
	// it.  See the note on s_nFramesChanged.
	return s_nFramesChanged;
}

u64 BareVideoDuplicatePresents (void)
{
	return s_nDuplicatePresents;
}

u64 BareVideoMicrosSinceLastFrame (void)
{
	if (s_nLastFrameMicros == 0)
	{
		return 0;
	}

	const u64 nNow = BareMicros ();

	return nNow > s_nLastFrameMicros ? nNow - s_nLastFrameMicros : 0;
}

// ===========================================================================
// Presentation on core 2
// ===========================================================================
//
// While the emulator runs, core 2 does everything that puts a frame on the
// screen, and core 0 only publishes frames into s_Frames:
//
//   core 0                                  core 2
//   BareVideoPresent()                      PresenterLoop()
//     s_Frames.Copy() + hash  --newest-->     s_Frames.Acquire()
//     SEV                                     scale into the HIDDEN page
//                                             SetVirtualOffset(that page)
//                                             WaitForVerticalSync()
//                                             BareSysInfoServiceTick()
//
// Two pages: BareVideoInit() asks the firmware for a virtual height of twice
// the mode, and the presenter only ever draws into the page that is not being
// scanned out, so there is no tearing.  If the firmware granted one page only,
// the presenter draws into it and flips nothing (same picture as before, minus
// the work on core 0).
//
// Vertical sync: after a flip the presenter waits for vsync, so that it never
// starts the next frame in the page the display has not finished leaving.  The
// emulator is NOT paced by this - core 0 keeps its own clock, and a frame that
// arrives while core 2 waits simply replaces the previous one in the triple
// buffer (counted as overwritten).  DOS at 70 Hz on a 60 Hz display therefore
// drops about one frame in seven, instead of tearing.
//
// The wait goes through the firmware mailbox and holds its spin lock until the
// vsync, which is why the presenter is also the mailbox owner while it runs
// (BareSysInfoSetMailboxProxy): core 0 never has to take that lock.
//
// When there is nothing new, core 2 sleeps in WFE.  Core 0 wakes it with SEV
// when it publishes, and the generic timer's event stream (enabled below, on
// core 2 only) wakes it about every millisecond so that the mailbox service
// keeps running while the picture is still.
//
// Not yet here (next step): the status slots, the footer and the boot text,
// which core 0 still draws into page 0.  Until they move, the presenter is
// not started by default.

namespace {

std::atomic<bool> s_bPresenterStop {false};
std::atomic<bool> s_bPresenterRunning {false};
std::atomic<bool> s_bPresenterVsync {true};

std::atomic<u64> s_nPresenterShown {0};		// frames flipped onto the screen
std::atomic<unsigned> s_nPresenterLastMicros {0};	// scale time of the last frame
std::atomic<unsigned> s_nPresenterPeakMicros {0};	// worst since the last stats read
std::atomic<bool> s_bPresenterTwoPages {false};
std::atomic<bool> s_bPresenterVsyncWorks {false};

unsigned s_nFrontPage = 0;		// core 2 only while it runs

// The generic timer's event stream: one event every 2^(EVNTI+1) counter
// ticks.  EVNTI = 14 is 0.6 ms with the 54 MHz counter of a Pi 4 / Pi 5 and
// 1.7 ms with the 19.2 MHz counter of a Pi 3 - short enough for the mailbox
// service and for not missing a published frame, long enough to keep the
// core asleep almost all of the time it has nothing to do.
void EnableEventStream (void)
{
#if AARCH == 64
	u64 nCntKCtl;
	asm volatile ("mrs %0, cntkctl_el1" : "=r" (nCntKCtl));
	nCntKCtl &= ~(u64) 0xFC;		// EVNTI [7:4], EVNTDIR [3], EVNTEN [2]
	nCntKCtl |= (14u << 4) | (1u << 2);
	asm volatile ("msr cntkctl_el1, %0" :: "r" (nCntKCtl));
	asm volatile ("isb");
#endif
}

u32 *PageWords (unsigned nPage)
{
	return FrameBufferWords () + static_cast<uintptr_t> (nPage) * s_nModeHeight
				     * (s_pFrameBuffer->GetPitch () / 4);
}

// Black outside the picture rectangle, in one page.  Same rectangles as
// BareVideoClearOutside(), for any page.
void ClearOutsidePage (unsigned nPage)
{
	const u32 nPitch = s_pFrameBuffer->GetPitch () / 4;
	u32 *pDst = PageWords (nPage);

	FillRect (pDst, nPitch, 0, 0, s_nModeWidth, s_nDstY, 0x00000000);
	FillRect (pDst, nPitch, 0, s_nDstY + s_nDstHeight, s_nModeWidth,
		  s_nModeHeight > s_nDstY + s_nDstHeight
			  ? s_nModeHeight - (s_nDstY + s_nDstHeight) : 0,
		  0x00000000);
	FillRect (pDst, nPitch, 0, s_nDstY, s_nDstX, s_nDstHeight, 0x00000000);
	FillRect (pDst, nPitch, s_nDstX + s_nDstWidth, s_nDstY,
		  s_nModeWidth > s_nDstX + s_nDstWidth
			  ? s_nModeWidth - (s_nDstX + s_nDstWidth) : 0,
		  s_nDstHeight, 0x00000000);
}

// The slots as core 2 last copied them (sequence lock, see TOverlayWrite).
struct TOverlaySnapshot
{
	TLineSlot Slots[MaxSlots];
	boolean bFooterVisible;
};

bool TakeOverlay (TOverlaySnapshot &Snap, unsigned &nSeq)
{
	for (unsigned nTry = 0; nTry < 8; nTry++)
	{
		const unsigned nBefore = s_nOverlaySeq.load (std::memory_order_acquire);
		if (nBefore & 1)
		{
			continue;		// core 0 is writing; it takes microseconds
		}

		memcpy (Snap.Slots, s_Slots, sizeof Snap.Slots);
		Snap.bFooterVisible = s_bFooterVisible;
		std::atomic_thread_fence (std::memory_order_acquire);

		if (s_nOverlaySeq.load (std::memory_order_relaxed) == nBefore)
		{
			nSeq = nBefore;
			return true;
		}
	}

	return false;			// try again on the next pass
}

// The slot strip at the bottom of one page: cleared, then every slot with
// text drawn the way DrawSlot() draws it.  The picture never reaches this
// strip (ComputeDestination reserves it), so what is drawn here stays until
// the next overlay change or a margin clear of that page.
void DrawOverlayPage (unsigned nPage, const TOverlaySnapshot &Snap)
{
	const u32 nPitch = s_pFrameBuffer->GetPitch () / 4;
	u32 *pDst = PageWords (nPage);

	const unsigned nLineHeight = s_Font.GetCharHeight () + 2;
	const unsigned nStrip = nLineHeight * MaxSlots;
	if (nStrip < s_nModeHeight)
	{
		FillRect (pDst, nPitch, 0, s_nModeHeight - nStrip, s_nModeWidth, nStrip, 0x00000000);
	}

	for (unsigned nSlot = 0; nSlot < MaxSlots; nSlot++)
	{
		const TLineSlot &Slot = Snap.Slots[nSlot];
		if (Slot.nWidth == 0 || Slot.Text[0] == '\0' || (nSlot == 0 && !Snap.bFooterVisible))
		{
			continue;
		}

		FillRect (pDst, nPitch, Slot.nX, Slot.nY, Slot.nWidth, Slot.nHeight, 0x00000000);
		DrawText (pDst, nPitch, Slot.nX + PanelMargin, Slot.nY + 1, Slot.Text, 0x00FFFFFF);
	}
}

void PresenterLoop (void)
{
	EnableEventStream ();

	const boolean bTwoPages = s_pFrameBuffer->GetVirtHeight () >= 2 * s_nModeHeight;
	s_bPresenterTwoPages.store (bTwoPages, std::memory_order_relaxed);

	// Start on page 0, which is what the display shows now.
	s_nFrontPage = 0;
	s_nShownPage.store (0, std::memory_order_release);
	if (bTwoPages)
	{
		(void) s_pFrameBuffer->SetVirtualOffset (0, 0);
	}

	// Per page: the picture rectangle its margins were cleared for, and the
	// overlay sequence drawn into it (~0 = nothing valid drawn).
	unsigned nClearedFor[2][4] = {};
	unsigned nOverlayDrawn[2] = { ~0u, ~0u };

	TOverlaySnapshot Overlay;
	unsigned nOverlaySeq = ~0u;

	bool bVsyncWorks = true;
	s_bPresenterRunning.store (true, std::memory_order_release);

	while (!s_bPresenterStop.load (std::memory_order_acquire))
	{
		BareSysInfoServiceTick ();

		// --- hold (screenshot): keep the page on screen stable ----------
		if (s_bHoldRequest.load (std::memory_order_acquire))
		{
			s_bHeld.store (true, std::memory_order_release);
			while (s_bHoldRequest.load (std::memory_order_acquire)
			       && !s_bPresenterStop.load (std::memory_order_acquire))
			{
				BareSysInfoServiceTick ();
				WaitForEvent ();
			}
			s_bHeld.store (false, std::memory_order_release);
			continue;
		}

		// --- overlay: take a new copy of the slots if core 0 changed them --
		const unsigned nSeqNow = s_nOverlaySeq.load (std::memory_order_acquire);
		if (nSeqNow != nOverlaySeq && (nSeqNow & 1) == 0)
		{
			unsigned nTaken;
			if (TakeOverlay (Overlay, nTaken))
			{
				nOverlaySeq = nTaken;
			}
		}

		// --- the frame: a new one, or the current one on a repaint request -
		const bareframe::TFrame *pFrame = s_Frames.Acquire ();
		if (pFrame == nullptr && s_bRepaintRequest.exchange (false, std::memory_order_acq_rel))
		{
			pFrame = s_Frames.Current ();
		}

		if (pFrame == nullptr || pFrame->pPixels == nullptr)
		{
			// Nothing to flip.  A panel change still has to reach the
			// screen: draw it straight into the page on show - it is the
			// strip below the picture, which the display reads as text
			// appearing, not as a torn picture.
			if (nOverlaySeq != ~0u && nOverlayDrawn[s_nFrontPage] != nOverlaySeq)
			{
				DrawOverlayPage (s_nFrontPage, Overlay);
				nOverlayDrawn[s_nFrontPage] = nOverlaySeq;
			}

			WaitForEvent ();
			continue;
		}

		const unsigned nBack = bTwoPages ? 1 - s_nFrontPage : 0;
		const u64 nStart = BareMicros ();

		ComputeDestination (pFrame->nWidth, pFrame->nHeight);
		const unsigned Rect[4] = { s_nDstX, s_nDstY, s_nDstWidth, s_nDstHeight };
		if (memcmp (nClearedFor[nBack], Rect, sizeof Rect) != 0)
		{
			ClearOutsidePage (nBack);	// also wipes the slot strip
			memcpy (nClearedFor[nBack], Rect, sizeof Rect);
			nOverlayDrawn[nBack] = ~0u;
		}

		PresentVisibleTo (PageWords (nBack), pFrame->pPixels, pFrame->nWidth,
				  pFrame->nHeight, pFrame->nWidth * sizeof (u32));

		if (nOverlaySeq != ~0u && nOverlayDrawn[nBack] != nOverlaySeq)
		{
			DrawOverlayPage (nBack, Overlay);
			nOverlayDrawn[nBack] = nOverlaySeq;
		}

		const unsigned nMicros = static_cast<unsigned> (BareMicros () - nStart);
		s_nPresenterLastMicros.store (nMicros, std::memory_order_relaxed);
		if (nMicros > s_nPresenterPeakMicros.load (std::memory_order_relaxed))
		{
			s_nPresenterPeakMicros.store (nMicros, std::memory_order_relaxed);
		}

		if (bTwoPages)
		{
			// The frame buffer is device memory (Circle maps GPU memory
			// above the ARM's share as DEVICE): the writes are not cached,
			// and the mailbox call below orders them before the flip.
			if (s_pFrameBuffer->SetVirtualOffset (0, nBack * s_nModeHeight))
			{
				s_nFrontPage = nBack;
				s_nShownPage.store (nBack, std::memory_order_release);
			}
		}

		if (bVsyncWorks && s_bPresenterVsync.load (std::memory_order_relaxed))
		{
			// A firmware that does not know the tag answers FALSE at once;
			// stop asking instead of calling it 60 times a second.
			bVsyncWorks = s_pFrameBuffer->WaitForVerticalSync ();
			s_bPresenterVsyncWorks.store (bVsyncWorks, std::memory_order_relaxed);
		}

		s_nPresenterShown.fetch_add (1, std::memory_order_relaxed);
	}

	// Leave the screen the way core 0 expects to find it: the latest picture
	// and the panel on page 0, page 0 scanned out.  Core 0 draws into page 0
	// only.
	const bareframe::TFrame *pLast = s_Frames.Current ();
	if (s_nFrontPage != 0)
	{
		if (pLast != nullptr && pLast->pPixels != nullptr)
		{
			ComputeDestination (pLast->nWidth, pLast->nHeight);
			ClearOutsidePage (0);
			PresentVisibleTo (PageWords (0), pLast->pPixels, pLast->nWidth,
					  pLast->nHeight, pLast->nWidth * sizeof (u32));
		}
		if (nOverlaySeq != ~0u)
		{
			DrawOverlayPage (0, Overlay);
		}
	}
	if (bTwoPages)
	{
		(void) s_pFrameBuffer->SetVirtualOffset (0, 0);
	}
	s_nFrontPage = 0;
	s_nShownPage.store (0, std::memory_order_release);

	s_bPresenterRunning.store (false, std::memory_order_release);
	// Returning parks core 2 again (bare_cores.cpp); it can be restarted.
}

} // namespace

boolean BareVideoPresenterStart (boolean bVsync)
{
	if (s_bPresenterOn.load (std::memory_order_acquire))
	{
		return TRUE;
	}

	if (s_pFrameBuffer == nullptr || !BareCoresAlive (2) || BareCoresIsRunning (2))
	{
		return FALSE;
	}

	s_Frames.Reset ();
	s_bPresenterStop.store (false, std::memory_order_relaxed);
	s_bPresenterVsync.store (bVsync != FALSE, std::memory_order_relaxed);
	s_nPresenterShown.store (0, std::memory_order_relaxed);
	s_nPresenterPeakMicros.store (0, std::memory_order_relaxed);

	// Core 2 owns the mailbox from now on (it waits for vsync through it).
	BareSysInfoSetMailboxProxy (TRUE);

	if (!BareCoresRun (2, PresenterLoop))
	{
		BareSysInfoSetMailboxProxy (FALSE);
		CLogger::Get ()->Write (FromVideo, LogWarning,
					"core 2 not available: presenting on core 0");
		return FALSE;
	}

	s_bPresenterOn.store (true, std::memory_order_release);
	CLogger::Get ()->Write (FromVideo, LogNotice, "presenting on core 2 (vsync %s)",
				bVsync ? "on" : "off");
	return TRUE;
}

void BareVideoPresenterConfigure (boolean bEnable, boolean bVsync)
{
	s_bPresenterWanted = bEnable != FALSE;
	s_bPresenterWantVsync = bVsync != FALSE;
}

void BareVideoPresenterStop (void)
{
	if (!s_bPresenterOn.load (std::memory_order_acquire))
	{
		return;
	}

	// Once core 0 has taken the screen back from a running presenter (stall
	// report, shutdown), it keeps it: no automatic restart on the next frame.
	s_bPresenterWanted = false;

	// Core 0 stops publishing first, so that nothing new arrives while core
	// 2 winds down; then core 2 is asked to stop and waited for.
	s_bPresenterOn.store (false, std::memory_order_release);
	s_bPresenterStop.store (true, std::memory_order_release);
	BareCoresWake ();

	// A vsync wait is at most one refresh (17 ms at 60 Hz, 33 ms at 30 Hz);
	// half a second is a hung firmware call, and then the screen is not
	// worth waiting for.
	for (unsigned nWait = 0; nWait < 500 && BareCoresIsRunning (2); nWait++)
	{
		CTimer::SimpleMsDelay (1);
	}

	BareSysInfoSetMailboxProxy (FALSE);

	// Core 0 is the drawer again: give it the last picture as its mirror, so
	// that BareVideoRepaint() and the screenshot keep working.
	const bareframe::TFrame *pLast = s_Frames.Current ();
	if (pLast != nullptr && pLast->pPixels != nullptr)
	{
		(void) MirrorStore (pLast->pPixels, pLast->nWidth, pLast->nHeight,
				    pLast->nWidth * static_cast<unsigned> (sizeof (u32)));
	}
}

boolean BareVideoPresenterActive (void)
{
	return s_bPresenterOn.load (std::memory_order_acquire) ? TRUE : FALSE;
}

void BareVideoPresenterStats (TBareVideoPresenterStats *pStats)
{
	if (pStats == nullptr)
	{
		return;
	}

	pStats->bRunning = s_bPresenterRunning.load (std::memory_order_acquire) ? TRUE : FALSE;
	pStats->bTwoPages = s_bPresenterTwoPages.load (std::memory_order_relaxed) ? TRUE : FALSE;
	pStats->bVsync = s_bPresenterVsync.load (std::memory_order_relaxed)
			 && s_bPresenterVsyncWorks.load (std::memory_order_relaxed) ? TRUE : FALSE;
	pStats->nShown = s_nPresenterShown.load (std::memory_order_relaxed);
	pStats->nOverwritten = s_Frames.Overwritten ();
	pStats->nLastMicros = s_nPresenterLastMicros.load (std::memory_order_relaxed);
	pStats->nPeakMicros = s_nPresenterPeakMicros.exchange (0, std::memory_order_relaxed);
}
