//
// frame_exchange_test.cpp - host test for src/platform/frame_exchange.h
//
// Run with: make check-frames
//
// Two threads stand in for core 0 (publishing frames) and core 2 (taking
// them).  Every pixel of frame N holds N, so a frame the consumer sees half
// written by the producer shows up as a frame with two different values.
//
// Shaeon (Carlos Palmero) 2026, for BM-DOS by @mmoroca.  GPL-3.0-or-later.
//
#include "../../src/platform/frame_exchange.h"

#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

static unsigned s_nFailures = 0;

static void Check (bool bOk, const char *pWhat)
{
	std::printf ("  %s  %s\n", bOk ? "ok    " : "FAILED", pWhat);
	if (!bOk)
	{
		s_nFailures++;
	}
}

int main (void)
{
	using bareframe::CFrameExchange;
	using bareframe::TFrame;

	std::printf ("single thread\n");
	{
		CFrameExchange Exchange;
		Check (Exchange.Acquire () == nullptr, "nothing to acquire before the first publish");
		Check (Exchange.Current () == nullptr, "no current frame before the first acquire");

		// A pitched source: 4 wide, pitch 6; the two padding pixels must not
		// be copied.
		std::vector<std::uint32_t> Src (6 * 3, 0xDEAD);
		for (unsigned y = 0; y < 3; y++)
			for (unsigned x = 0; x < 4; x++)
				Src[y * 6 + x] = y * 10 + x;

		std::uint64_t nHash = 0;
		Check (Exchange.Copy (Src.data (), 4, 3, 6, &nHash), "copy publishes");
		const TFrame *pFrame = Exchange.Acquire ();
		bool bPacked = pFrame != nullptr && pFrame->nWidth == 4 && pFrame->nHeight == 3;
		for (unsigned y = 0; bPacked && y < 3; y++)
			for (unsigned x = 0; x < 4; x++)
				bPacked = bPacked && pFrame->pPixels[y * 4 + x] == y * 10 + x;
		Check (bPacked, "pitched rows arrive packed, padding dropped");
		Check (pFrame != nullptr && pFrame->nHash == nHash, "the consumer sees the producer's hash");
		Check (Exchange.Acquire () == nullptr, "no second acquire of the same frame");
		Check (Exchange.Current () == pFrame, "current is the last acquired frame");

		// Three publishes without a take: the newest wins, two overwritten.
		std::uint32_t a = 1, b = 2, c = 3;
		Exchange.Copy (&a, 1, 1, 1);
		Exchange.Copy (&b, 1, 1, 1);
		Exchange.Copy (&c, 1, 1, 1);
		pFrame = Exchange.Acquire ();
		Check (pFrame != nullptr && pFrame->pPixels[0] == 3, "the newest frame wins");
		Check (Exchange.Overwritten () == 2, "frames replaced before being taken are counted");

		// A bigger frame after small ones: the slot grows.
		std::vector<std::uint32_t> Big (800 * 600, 7);
		Check (Exchange.Copy (Big.data (), 800, 600, 800), "a larger frame grows its slot");
		pFrame = Exchange.Acquire ();
		Check (pFrame != nullptr && pFrame->nWidth == 800 && pFrame->pPixels[800 * 600 - 1] == 7,
		       "and arrives whole");
	}

	std::printf ("two threads\n");
	{
		constexpr unsigned W = 320, H = 200;
		constexpr std::uint32_t Frames = 20000;

		CFrameExchange Exchange;
		std::atomic<bool> bDone {false};
		std::atomic<unsigned> nTorn {0}, nBackwards {0}, nTaken {0};

		std::thread Consumer ([&] {
			std::uint64_t nLast = 0;
			for (;;)
			{
				const TFrame *pFrame = Exchange.Acquire ();
				if (pFrame == nullptr)
				{
					if (bDone.load (std::memory_order_acquire))
					{
						// One last look: the final publish may have
						// landed after the check above.
						pFrame = Exchange.Acquire ();
						if (pFrame == nullptr)
							break;
					}
					else
					{
						continue;
					}
				}

				const std::uint32_t nValue = pFrame->pPixels[0];
				for (unsigned i = 0; i < W * H; i++)
				{
					if (pFrame->pPixels[i] != nValue)
					{
						nTorn++;
						break;
					}
				}
				if (pFrame->nSequence <= nLast)
					nBackwards++;
				if (pFrame->nSequence != nValue)
					nTorn++;
				nLast = pFrame->nSequence;
				nTaken++;
			}
		});

		std::vector<std::uint32_t> Src (W * H);
		for (std::uint32_t n = 1; n <= Frames; n++)
		{
			for (auto &Pixel : Src)
				Pixel = n;
			Exchange.Copy (Src.data (), W, H, W);
		}
		bDone.store (true, std::memory_order_release);
		Consumer.join ();

		const TFrame *pLast = Exchange.Current ();
		Check (nTorn == 0, "no frame is ever seen half written");
		Check (nBackwards == 0, "frames are seen in publish order, never older");
		Check (pLast != nullptr && pLast->nSequence == Frames, "the last frame published is the last one seen");
		Check (nTaken + Exchange.Overwritten () == Frames,
		       "every frame is either taken or counted as overwritten");
		std::printf ("         (%u taken, %llu overwritten)\n", nTaken.load (),
			     static_cast<unsigned long long> (Exchange.Overwritten ()));
	}

	std::printf ("\n%u failures\n", s_nFailures);
	return s_nFailures == 0 ? 0 : 1;
}
