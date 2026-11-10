// midi_test.cpp - check-midi
//
// What src/platform/midi_queue.h promises, checked on the PC: the byte stream
// rules of the parser (the ones a game's MPU-401 driver actually exercises -
// running status, SysEx, real-time bytes in the middle of both) and the ring
// that carries bytes to core 1 and samples back.
//
// The ring's cross-core behaviour cannot be proved here - it needs two cores
// and a weakly ordered memory system - but a two-thread run on the host does
// catch the classic mistakes (wrong index on wrap, full/empty confusion, a
// relaxed store where a release was needed shows up as torn data under
// -fsanitize=thread when it is available).
//
// Every check compares against a value.  A check that only prints passes
// any sabotage (README, "Lessons learned").

#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "../../src/platform/midi_queue.h"

using namespace baremidi;

namespace {

unsigned s_nFail = 0;

void Check (bool bOk, const char *pWhat)
{
	std::printf ("  %-6s %s\n", bOk ? "ok" : "FAIL", pWhat);
	if (!bOk)
	{
		s_nFail++;
	}
}

struct TRecorder
{
	std::vector<std::uint32_t> Short;
	std::vector<std::vector<std::uint8_t>> SysExMsgs;

	void ShortMsg (std::uint32_t nMsg) { Short.push_back (nMsg); }
	void SysEx (const std::uint8_t *p, std::size_t n) { SysExMsgs.emplace_back (p, p + n); }
};

void FeedAll (TMidiParser &P, TRecorder &R, std::initializer_list<int> Bytes)
{
	for (int b : Bytes)
	{
		P.Feed (static_cast<std::uint8_t> (b), R);
	}
}

void TestParser (void)
{
	std::printf ("parser\n");

	{
		TMidiParser P; TRecorder R;
		FeedAll (P, R, {0x90, 0x3C, 0x64});
		Check (R.Short.size () == 1 && R.Short[0] == 0x643C90, "note on packs as status|d1<<8|d2<<16");
	}
	{
		TMidiParser P; TRecorder R;
		FeedAll (P, R, {0x90, 0x3C, 0x64, 0x40, 0x50, 0x3C, 0x00});
		Check (R.Short.size () == 3 && R.Short[1] == 0x504090 && R.Short[2] == 0x003C90,
		       "running status repeats the last channel status");
	}
	{
		TMidiParser P; TRecorder R;
		FeedAll (P, R, {0xC5, 0x10, 0x20});
		Check (R.Short.size () == 2 && R.Short[0] == 0x10C5 && R.Short[1] == 0x20C5,
		       "two-byte messages (program change) with running status");
	}
	{
		TMidiParser P; TRecorder R;
		FeedAll (P, R, {0x3C, 0x64});
		Check (R.Short.empty (), "data bytes with no status are dropped");
	}
	{
		TMidiParser P; TRecorder R;
		FeedAll (P, R, {0x90, 0x3C, 0xF8, 0x64});
		Check (R.Short.size () == 2 && R.Short[0] == 0xF8 && R.Short[1] == 0x643C90,
		       "real time inside a message is delivered and does not break it");
	}
	{
		TMidiParser P; TRecorder R;
		FeedAll (P, R, {0xF0, 0x41, 0x10, 0x16, 0x12, 0xF7});
		const std::vector<std::uint8_t> Want {0xF0, 0x41, 0x10, 0x16, 0x12, 0xF7};
		Check (R.SysExMsgs.size () == 1 && R.SysExMsgs[0] == Want, "SysEx is delivered F0..F7 inclusive");
	}
	{
		TMidiParser P; TRecorder R;
		FeedAll (P, R, {0xF0, 0x41, 0xFE, 0x10, 0xF7});
		const std::vector<std::uint8_t> Want {0xF0, 0x41, 0x10, 0xF7};
		Check (R.SysExMsgs.size () == 1 && R.SysExMsgs[0] == Want && R.Short.size () == 1 && R.Short[0] == 0xFE,
		       "active sensing inside SysEx goes out on its own");
	}
	{
		TMidiParser P; TRecorder R;
		FeedAll (P, R, {0xF0, 0x41, 0x10, 0x90, 0x3C, 0x64});
		const std::vector<std::uint8_t> Want {0xF0, 0x41, 0x10, 0xF7};
		Check (R.SysExMsgs.size () == 1 && R.SysExMsgs[0] == Want && R.Short.size () == 1 && R.Short[0] == 0x643C90,
		       "a status byte closes an unterminated SysEx and is then processed");
	}
	{
		TMidiParser P; TRecorder R;
		FeedAll (P, R, {0x90, 0x3C, 0x64, 0xF0, 0x01, 0xF7, 0x3C, 0x00});
		Check (R.Short.size () == 1, "SysEx cancels running status");
	}
	{
		TMidiParser P; TRecorder R;
		FeedAll (P, R, {0xF6, 0xF2, 0x01, 0x02});
		Check (R.Short.size () == 2 && R.Short[0] == 0xF6 && R.Short[1] == 0x0201F2,
		       "system common lengths (tune request, song position)");
	}
	{
		TMidiParser P; TRecorder R;
		P.Feed (0xF0, R);
		for (std::size_t i = 0; i < TMidiParser::SysExMax + 100; i++)
		{
			P.Feed (0x55, R);
		}
		P.Feed (0xF7, R);
		Check (R.SysExMsgs.size () == 1 && R.SysExMsgs[0].size () == TMidiParser::SysExMax
		       && R.SysExMsgs[0].back () == 0xF7,
		       "an oversized SysEx is truncated to SysExMax and still ends in F7");
	}
}

void TestRing (void)
{
	std::printf ("ring\n");

	{
		TSPSCRing<int, 8> Ring;
		int In[10] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
		Check (Ring.Write (In, 10) == 8, "write stops at capacity");
		Check (Ring.Count () == 8, "count after a full write");
		int Out[10] = {};
		Check (Ring.Read (Out, 10) == 8 && Out[0] == 0 && Out[7] == 7, "read returns what was written, in order");
		Check (Ring.Read (Out, 1) == 0, "read on empty returns 0");
	}
	{
		TSPSCRing<int, 8> Ring;
		int In[6] = {1, 2, 3, 4, 5, 6};
		int Out[6] = {};
		bool bOk = true;
		for (int nRound = 0; nRound < 100 && bOk; nRound++)	// indices wrap many times
		{
			bOk = Ring.Write (In, 6) == 6 && Ring.Read (Out, 6) == 6
			   && std::memcmp (In, Out, sizeof In) == 0;
		}
		Check (bOk, "data survives the index wrapping around the buffer");
	}
	{
		// Two threads, a long stream, and every value checked on arrival.
		static TSPSCRing<std::uint32_t, 256> Ring;
		constexpr std::uint32_t Total = 2000000;
		bool bOrdered = true;

		std::thread Producer ([] {
			std::uint32_t n = 0;
			while (n < Total)
			{
				std::uint32_t Block[37];
				std::size_t nCount = 0;
				while (nCount < 37 && n + nCount < Total)
				{
					Block[nCount] = n + nCount;
					nCount++;
				}
				n += Ring.Write (Block, nCount);
			}
		});

		std::uint32_t nExpect = 0;
		while (nExpect < Total)
		{
			std::uint32_t Block[53];
			const std::size_t nGot = Ring.Read (Block, 53);
			for (std::size_t i = 0; i < nGot; i++)
			{
				if (Block[i] != nExpect++)
				{
					bOrdered = false;
				}
			}
		}
		Producer.join ();
		Check (bOrdered && Ring.Count () == 0, "two threads: 2M values arrive complete and in order");
	}
}

} // namespace

int main (void)
{
	TestParser ();
	TestRing ();
	std::printf ("\n%u failures\n", s_nFail);
	return s_nFail == 0 ? 0 : 1;
}
