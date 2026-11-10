//
// midi_queue.h
//
// BM-DOS - the two pieces of the MIDI path that do not depend on Circle, kept
// in a header of their own so that scripts/hostcheck/midi_test.cpp can test
// them on the PC (make check-midi):
//
//   * TSPSCRing   a single-producer / single-consumer ring.  Core 0 produces
//                 MIDI bytes and consumes rendered samples; core 1 does the
//                 opposite.  Exactly one writer and one reader per ring, so
//                 two atomics with acquire/release are all the locking there
//                 is - no spin lock, no IRQ masking, nothing that can stall
//                 the emulator on core 0.
//
//   * TMidiParser turns the raw byte stream an MPU-401 sends (running status,
//                 SysEx, real-time bytes in the middle of anything) into
//                 whole messages.  Same rules as DOSBox's MIDI_RawOutByte()
//                 in src/gui/midi.cpp, which this port does not build.
//
// Shaeon (Carlos Palmero) 2026, for BM-DOS by @mmoroca.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
#ifndef BARE_MIDI_QUEUE_H
#define BARE_MIDI_QUEUE_H

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace baremidi {

// ---------------------------------------------------------------------------
// TSPSCRing
// ---------------------------------------------------------------------------
//
// N must be a power of two.  The indices run free and are masked on access,
// so "full" and "empty" are told apart without wasting a slot.  The two
// indices sit on separate cache lines: each core writes only its own, and
// sharing a line would make every write a cross-core invalidation.
template <typename T, std::size_t N>
class TSPSCRing
{
	static_assert (N >= 2 && (N & (N - 1)) == 0, "N must be a power of two");

public:
	/// \brief Producer side.  Copies as many items as fit.
	/// \return items written (0..nCount); the rest is the caller's to drop
	std::size_t Write (const T *pData, std::size_t nCount)
	{
		const std::size_t nHead = m_nHead.load (std::memory_order_relaxed);
		const std::size_t nTail = m_nTail.load (std::memory_order_acquire);
		const std::size_t nFree = N - (nHead - nTail);
		if (nCount > nFree)
		{
			nCount = nFree;
		}

		for (std::size_t i = 0; i < nCount; i++)
		{
			m_Data[(nHead + i) & (N - 1)] = pData[i];
		}

		m_nHead.store (nHead + nCount, std::memory_order_release);
		return nCount;
	}

	/// \brief Consumer side.  Copies out as many items as are there.
	/// \return items read (0..nCount)
	std::size_t Read (T *pData, std::size_t nCount)
	{
		const std::size_t nTail = m_nTail.load (std::memory_order_relaxed);
		const std::size_t nHead = m_nHead.load (std::memory_order_acquire);
		const std::size_t nUsed = nHead - nTail;
		if (nCount > nUsed)
		{
			nCount = nUsed;
		}

		for (std::size_t i = 0; i < nCount; i++)
		{
			pData[i] = m_Data[(nTail + i) & (N - 1)];
		}

		m_nTail.store (nTail + nCount, std::memory_order_release);
		return nCount;
	}

	/// \brief Items waiting.  Exact for the consumer, a lower bound for the
	///	   producer (the consumer may have taken more since).
	std::size_t Count (void) const
	{
		return m_nHead.load (std::memory_order_acquire)
		     - m_nTail.load (std::memory_order_acquire);
	}

	static constexpr std::size_t Capacity (void) { return N; }

	/// \brief Empty the ring.  ONLY when neither side is running - it writes
	///	   both indices, which is exactly what the two sides must not do.
	void Reset (void)
	{
		m_nHead.store (0, std::memory_order_relaxed);
		m_nTail.store (0, std::memory_order_relaxed);
	}

private:
	alignas (64) std::atomic<std::size_t> m_nHead {0};	// written by the producer
	alignas (64) std::atomic<std::size_t> m_nTail {0};	// written by the consumer
	alignas (64) T m_Data[N];
};

// ---------------------------------------------------------------------------
// TMidiParser
// ---------------------------------------------------------------------------
//
// Feed() takes one byte at a time and calls the sink when a message is
// complete:
//
//   Sink.ShortMsg (u32)               status | data1 << 8 | data2 << 16
//                                     (the packing Munt's playMsg() and the
//                                     Win32 MIDI API use)
//   Sink.SysEx (const u8 *, size_t)   F0 ... F7, both included
//
// The rules, the same as DOSBox's MIDI_RawOutByte():
//   * F8..FF (real time) are delivered at once, even inside a SysEx, and
//     do not touch running status.
//   * Any other status byte ends a SysEx in progress: the SysEx is closed
//     with F7 and delivered, then the new status is processed.  (F7 itself
//     is the normal way to end one.)
//   * Channel messages (80..EF) set running status; data bytes that arrive
//     after a complete message start a new one with the same status.
//   * System common (F1..F6) cancels running status.
//   * Data bytes with no status are dropped.
//   * A SysEx longer than SysExMax is truncated, not overflowed, and still
//     delivered with its F7.
class TMidiParser
{
public:
	static constexpr std::size_t SysExMax = 8192;	// DOSBox's SYSEX_SIZE

	template <typename TSink>
	void Feed (std::uint8_t nByte, TSink &Sink)
	{
		if (nByte >= 0xF8)			// real time
		{
			Sink.ShortMsg (nByte);
			return;
		}

		if (m_bInSysEx)
		{
			if (nByte < 0x80)
			{
				if (m_nSysExUsed < SysExMax - 1)	// keep room for F7
				{
					m_SysEx[m_nSysExUsed++] = nByte;
				}
				return;
			}

			// Any status byte ends it.
			m_SysEx[m_nSysExUsed++] = 0xF7;
			m_bInSysEx = false;
			Sink.SysEx (m_SysEx, m_nSysExUsed);

			if (nByte == 0xF7)
			{
				m_nStatus = 0;
				return;
			}
			// fall through: process the new status byte
		}

		if (nByte & 0x80)
		{
			m_nStatus = 0;
			m_nPos = 0;

			if (nByte == 0xF0)
			{
				m_SysEx[0] = 0xF0;
				m_nSysExUsed = 1;
				m_bInSysEx = true;
				return;
			}

			const unsigned nLen = MessageLength (nByte);
			if (nLen == 0)			// F7 alone, F4, F5: nothing to send
			{
				return;
			}

			m_Msg[0] = nByte;
			m_nPos = 1;
			m_nLen = nLen;
			if (nByte < 0xF0)
			{
				m_nStatus = nByte;	// running status for channel messages
			}
		}
		else
		{
			if (m_nPos == 0)
			{
				if (m_nStatus == 0)
				{
					return;		// data without status
				}

				m_Msg[0] = m_nStatus;	// running status
				m_nPos = 1;
				m_nLen = MessageLength (m_nStatus);
			}

			m_Msg[m_nPos++] = nByte;
		}

		if (m_nPos >= m_nLen)
		{
			std::uint32_t nMsg = m_Msg[0];
			if (m_nLen > 1) nMsg |= static_cast<std::uint32_t> (m_Msg[1]) << 8;
			if (m_nLen > 2) nMsg |= static_cast<std::uint32_t> (m_Msg[2]) << 16;
			Sink.ShortMsg (nMsg);
			m_nPos = 0;
		}
	}

	void Reset (void)
	{
		m_nStatus = 0;
		m_nPos = 0;
		m_nLen = 0;
		m_bInSysEx = false;
		m_nSysExUsed = 0;
	}

	/// \brief Bytes in a message with this status, status included.
	///	   0 for bytes that are not a message on their own.
	static constexpr unsigned MessageLength (std::uint8_t nStatus)
	{
		switch (nStatus & 0xF0)
		{
		case 0x80: case 0x90: case 0xA0: case 0xB0: case 0xE0:
			return 3;
		case 0xC0: case 0xD0:
			return 2;
		default:
			break;
		}

		switch (nStatus)
		{
		case 0xF1: case 0xF3:	return 2;	// MTC quarter frame, song select
		case 0xF2:		return 3;	// song position
		case 0xF6:		return 1;	// tune request
		default:		return 0;	// F0 handled apart; F4, F5, F7
		}
	}

private:
	std::uint8_t  m_nStatus = 0;		// running status, 0 = none
	std::uint8_t  m_Msg[3] = {};
	unsigned      m_nPos = 0;
	unsigned      m_nLen = 0;
	bool          m_bInSysEx = false;
	std::size_t   m_nSysExUsed = 0;
	std::uint8_t  m_SysEx[SysExMax];
};

} // namespace baremidi

#endif
