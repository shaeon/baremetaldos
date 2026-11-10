//
// frame_exchange.h
//
// BM-DOS - the hand-over of emulated frames from core 0 (DOSBox) to core 2
// (the presenter).  No Circle dependency, so scripts/hostcheck/
// frame_exchange_test.cpp runs it on the PC with two threads (make
// check-frames).
//
// A classic lock-free triple buffer:
//
//   slot W   owned by the producer (core 0): the frame being copied in
//   slot R   owned by the consumer (core 2): the frame being scaled out
//   slot P   the latest published frame, owned by neither
//
// Publish() swaps W and P; Acquire() swaps P and R when P holds something
// newer than R.  Each swap is ONE atomic exchange of a small word holding the
// slot index of P and a "new" bit, so neither side ever waits for the other:
//
//   * if core 2 is slower than the emulator, frames are overwritten in P
//     before core 2 takes them - counted as Overwritten(), and that is the
//     right behaviour: the newest picture wins, nothing queues up;
//   * if core 2 is faster, Acquire() returns nullptr and it shows what it
//     already has (or waits for the next vsync).
//
// The producer is the only side that allocates: a slot grows (never shrinks)
// when it is the producer's slot W, which the consumer cannot be reading.
//
// Shaeon (Carlos Palmero) 2026, for BM-DOS by @mmoroca.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
#ifndef BARE_FRAME_EXCHANGE_H
#define BARE_FRAME_EXCHANGE_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

namespace bareframe {

struct TFrame
{
	std::uint32_t *pPixels = nullptr;	// XRGB8888, rows packed (pitch = width)
	std::size_t nCapacity = 0;		// pixels allocated
	unsigned nWidth = 0;
	unsigned nHeight = 0;
	std::uint64_t nSequence = 0;		// 1, 2, 3 ... in publish order
	std::uint64_t nHash = 0;		// sampled hash, see Copy()
};

class CFrameExchange
{
public:
	CFrameExchange (void) = default;
	CFrameExchange (const CFrameExchange &) = delete;
	CFrameExchange &operator= (const CFrameExchange &) = delete;

	~CFrameExchange (void)
	{
		for (TFrame &Frame : m_Slot)
		{
			delete[] Frame.pPixels;
		}
	}

	// --- producer (core 0) ----------------------------------------------

	/// \brief Copy a frame in from a pitched source and publish it.
	/// \return false (and nothing published) when the slot could not grow,
	///	    which is out of memory; *pHash receives the frame's hash.
	///
	/// The hash samples every 64th pixel (plus the size): it is what the
	/// governor uses to tell a still screen from a moving one, and computing
	/// it here, over words that are being copied anyway, costs next to
	/// nothing.  Same function as bare_video.cpp's MirrorStore used.
	bool Copy (const std::uint32_t *pSrc, unsigned nWidth, unsigned nHeight,
		   unsigned nPitchPixels, std::uint64_t *pHash = nullptr)
	{
		TFrame &Frame = m_Slot[m_nWrite];

		const std::size_t nNeeded = static_cast<std::size_t> (nWidth) * nHeight;
		if (nNeeded > Frame.nCapacity)
		{
			delete[] Frame.pPixels;
			Frame.pPixels = new (std::nothrow) std::uint32_t[nNeeded];
			Frame.nCapacity = Frame.pPixels != nullptr ? nNeeded : 0;
			if (Frame.pPixels == nullptr)
			{
				return false;
			}
		}

		for (unsigned nY = 0; nY < nHeight; nY++)
		{
			std::memcpy (Frame.pPixels + static_cast<std::size_t> (nY) * nWidth,
				     pSrc + static_cast<std::size_t> (nY) * nPitchPixels,
				     nWidth * sizeof (std::uint32_t));
		}

		std::uint64_t nHash = 1469598103934665603ULL;
		for (std::size_t i = 0; i < nNeeded; i += 64)
		{
			nHash = (nHash ^ Frame.pPixels[i]) * 1099511628211ULL;
		}
		nHash ^= (static_cast<std::uint64_t> (nWidth) << 32) | nHeight;

		Frame.nWidth = nWidth;
		Frame.nHeight = nHeight;
		Frame.nHash = nHash;
		Frame.nSequence = ++m_nPublished;

		Publish ();

		if (pHash != nullptr)
		{
			*pHash = nHash;
		}
		return true;
	}

	/// Frames published so far (producer side).
	std::uint64_t Published (void) const { return m_nPublished; }

	/// Frames that were replaced in P before the consumer took them.
	std::uint64_t Overwritten (void) const
	{
		return m_nOverwritten.load (std::memory_order_relaxed);
	}

	// --- consumer (core 2) ----------------------------------------------

	/// \brief Take the newest published frame, if there is one newer than
	///	   the one the consumer holds.
	/// \return the frame, valid until the next Acquire(); nullptr if
	///	    nothing new was published.
	const TFrame *Acquire (void)
	{
		if ((m_nShared.load (std::memory_order_acquire) & NewBit) == 0)
		{
			return nullptr;
		}

		const unsigned nOld = m_nShared.exchange (m_nRead, std::memory_order_acq_rel);
		m_nRead = nOld & IndexMask;
		return &m_Slot[m_nRead];
	}

	/// \brief The frame the consumer holds now (the last one acquired), or
	///	   nullptr before the first Acquire().  For repaints: re-present it
	///	   without waiting for the emulator.
	const TFrame *Current (void) const
	{
		const TFrame &Frame = m_Slot[m_nRead];
		return Frame.nSequence != 0 ? &Frame : nullptr;
	}

	/// \brief Back to the initial state.  ONLY when neither side is running.
	void Reset (void)
	{
		m_nWrite = 0;
		m_nShared.store (1, std::memory_order_relaxed);
		m_nRead = 2;
		m_nPublished = 0;
		m_nOverwritten.store (0, std::memory_order_relaxed);
		for (TFrame &Frame : m_Slot)
		{
			Frame.nSequence = 0;
			Frame.nWidth = Frame.nHeight = 0;
		}
	}

private:
	static constexpr unsigned IndexMask = 0x3;
	static constexpr unsigned NewBit = 0x4;

	void Publish (void)
	{
		const unsigned nOld = m_nShared.exchange (m_nWrite | NewBit,
							  std::memory_order_acq_rel);
		if (nOld & NewBit)
		{
			m_nOverwritten.fetch_add (1, std::memory_order_relaxed);
		}
		m_nWrite = nOld & IndexMask;
	}

	TFrame m_Slot[3];

	// Producer-private.
	alignas (64) unsigned m_nWrite = 0;
	std::uint64_t m_nPublished = 0;

	// Shared: slot index of P, plus NewBit while P has not been taken.
	alignas (64) std::atomic<unsigned> m_nShared {1};
	std::atomic<std::uint64_t> m_nOverwritten {0};

	// Consumer-private.
	alignas (64) unsigned m_nRead = 2;
};

} // namespace bareframe

#endif
