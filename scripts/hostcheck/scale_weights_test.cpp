//
// scale_weights_test.cpp - host test for src/platform/scale_weights.h
// (make check-scale).  Shaeon (Carlos Palmero) 2026, for BM-DOS by @mmoroca.
// GPL-3.0-or-later.
//
#include "../../src/platform/scale_weights.h"

#include <cstdio>

static unsigned s_nFailures = 0;
static void Check (bool b, const char *p)
{
	std::printf ("  %s  %s\n", b ? "ok    " : "FAILED", p);
	if (!b) s_nFailures++;
}

static unsigned PureRows (unsigned nSrc, unsigned nDst, bool bSharp)
{
	unsigned nPure = 0;
	for (unsigned y = 0; y < nDst; y++)
		nPure += barescale::RowMix (y, nSrc, nDst, bSharp).nMix == 0;
	return nPure;
}

int main (void)
{
	using barescale::RowMix;

	// 400 -> 720 (text mode and 640x400 at 1280x720): x1.8.
	const unsigned nSharp = PureRows (400, 720, true);
	const unsigned nSmooth = PureRows (400, 720, false);
	std::printf ("         400 -> 720: pure rows sharp %u, smooth %u (of 720)\n", nSharp, nSmooth);
	// Enlarging, only rows on a source boundary mix: at most one per source
	// boundary (399 here), so at least 720 - 399 rows are pure.
	Check (nSharp >= 720 - 399, "sharp: only rows on a source boundary are mixed");
	Check (nSmooth < nSharp, "smooth mixes more rows than sharp");

	// An exact multiple is all pure in sharp mode.
	Check (PureRows (200, 600, true) == 600, "sharp: x3 exactly is all pure rows");

	// Every source row is used, in order, and the weights add up: the mixed
	// row's weight is the share of the output row past the boundary.
	{
		bool bOrdered = true, bAll = true;
		unsigned nLast = 0;
		bool bSeen[400] = {};
		for (unsigned y = 0; y < 720; y++)
		{
			const auto m = RowMix (y, 400, 720, true);
			bOrdered = bOrdered && m.nRow0 >= nLast && m.nMix <= 256;
			nLast = m.nRow0;
			bSeen[m.nRow0] = true;
		}
		for (bool b : bSeen) bAll = bAll && b;
		Check (bOrdered, "sharp: rows advance in order, weights in 0..256");
		Check (bAll, "sharp: no source row is skipped when enlarging");
	}

	// The last row never reads past the picture.
	{
		const auto m = RowMix (719, 400, 720, true);
		Check (m.nRow0 < 400 && m.nRow1 < 400, "the last output row stays inside the source");
	}

	// Shrinking falls back to linear in both modes.
	{
		bool bSame = true;
		for (unsigned y = 0; y < 540; y++)
		{
			const auto a = RowMix (y, 600, 540, true), b = RowMix (y, 600, 540, false);
			bSame = bSame && a.nRow0 == b.nRow0 && a.nMix == b.nMix;
		}
		Check (bSame, "shrinking: sharp and smooth are the same (linear)");
	}

	std::printf ("\n%u failures\n", s_nFailures);
	return s_nFailures == 0 ? 0 : 1;
}
