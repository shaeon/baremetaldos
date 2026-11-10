//
// scale_weights.h
//
// BM-DOS - which source rows an output row is made of, and in what proportion.
// No Circle dependency: scripts/hostcheck/scale_weights_test.cpp runs it on the
// PC (make check-scale).
//
// Two ways, chosen in boot.conf (scaler =):
//
//   sharp  (default) area sampling, the same rule the horizontal scaler has
//          always used (ResampleRowBox): an output row covers an interval of
//          source rows; if that interval lies inside one source row, the
//          output row IS that row; only a row that straddles the boundary
//          between two mixes them, in proportion to how much of each it
//          covers.  Only the rows on a boundary are mixed - one per source
//          row at most - and each keeps the weight it really has, so a
//          one-pixel line stays one row high plus at most a faint edge.
//   smooth linear interpolation by the position of the row's top edge, which
//          is what the port did before: nearly every output row mixes two
//          source rows, so a one-pixel line becomes a soft band.
//
// When the picture is shrunk (more source rows than output rows) both fall
// back to the linear rule: an output row then covers more than one source
// row, and two-row area sampling would only drop rows.
//
// Shaeon (Carlos Palmero) 2026, for BM-DOS by @mmoroca.  GPL-3.0-or-later.
//
#ifndef BARE_SCALE_WEIGHTS_H
#define BARE_SCALE_WEIGHTS_H

namespace barescale {

struct TRowMix
{
	unsigned nRow0;		// first source row
	unsigned nRow1;		// second source row (== nRow0 when not mixed)
	unsigned nMix;		// weight of nRow1, 0..256 (0 = nRow0 alone)
};

/// \param nY       output row
/// \param nSrcRows source height
/// \param nDstRows output height
/// \param bSharp   area sampling (true) or linear (false)
///
/// Positions are computed per row from the two heights, in 64 bits, rather
/// than by adding a rounded 16.16 step: the rounding error of the step grows
/// with every row, and at x3 exactly it put row 3 back on source row 0.
inline TRowMix RowMix (unsigned nY, unsigned nSrcRows, unsigned nDstRows, bool bSharp)
{
	constexpr unsigned long long One = 1ull << 16;

	const unsigned long long nPos = (static_cast<unsigned long long> (nY) * nSrcRows << 16) / nDstRows;
	const unsigned long long nEnd = (static_cast<unsigned long long> (nY + 1) * nSrcRows << 16) / nDstRows;

	TRowMix Mix;
	Mix.nRow0 = static_cast<unsigned> (nPos >> 16);
	if (Mix.nRow0 >= nSrcRows)
	{
		Mix.nRow0 = nSrcRows - 1;
	}
	Mix.nRow1 = Mix.nRow0 + 1 < nSrcRows ? Mix.nRow0 + 1 : Mix.nRow0;

	if (bSharp && nSrcRows <= nDstRows)
	{
		// The row covers [nPos, nEnd).  The part past the next boundary,
		// if any, belongs to nRow1.
		const unsigned long long nSpan = nEnd - nPos;
		const unsigned long long nBoundary = static_cast<unsigned long long> (Mix.nRow0 + 1) << 16;
		Mix.nMix = nEnd > nBoundary && Mix.nRow1 != Mix.nRow0 && nSpan != 0
			   ? static_cast<unsigned> (((nEnd - nBoundary) * 256 + nSpan / 2) / nSpan)
			   : 0;
	}
	else
	{
		Mix.nMix = Mix.nRow1 != Mix.nRow0
			   ? static_cast<unsigned> (((nPos & (One - 1)) * 257) >> 16) : 0;
	}

	if (Mix.nMix == 0)
	{
		Mix.nRow1 = Mix.nRow0;
	}
	return Mix;
}

} // namespace barescale

#endif
