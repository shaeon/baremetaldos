//
// menu_render.h
//
// BM-DOS - the setup menu's 80x30 character grid (menu_model.h) turned into a
// 640x480 XRGB8888 picture with Circle's 8x16 font, in the colours of the VGA
// text palette.  The picture then goes through the normal video path, scaled
// like any DOS screen - which is also exactly what a real BIOS setup was: a
// VGA text screen.
//
// Only the font table is shared with Circle (struct TFont, circle/font.h), so
// scripts/hostcheck/menu_preview.cpp renders the same picture on the PC.
//
// Shaeon (Carlos Palmero) 2026, for BM-DOS by @mmoroca.  GPL-3.0-or-later.
//
#ifndef BARE_MENU_RENDER_H
#define BARE_MENU_RENDER_H

#include "menu_model.h"

#include <circle/font.h>
#include <cstdint>

namespace baremenu {

constexpr unsigned PixelWidth = Cols * 8;	// 640
constexpr unsigned PixelHeight = Rows * 16;	// 480

inline std::uint32_t VGAColour (unsigned n)
{
	static const std::uint32_t Palette[16] =
	{
		0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
		0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
	};
	return Palette[n & 15];
}

/// Draw the whole grid.  pOut is PixelWidth x PixelHeight, nPitch in pixels.
inline void RenderScreen (const CScreen &S, const TFont &Font, std::uint32_t *pOut, unsigned nPitch)
{
	const auto *pGlyphs = static_cast<const unsigned char *> (Font.data);

	for (unsigned r = 0; r < Rows; r++)
	{
		for (unsigned c = 0; c < Cols; c++)
		{
			const TCell &Cell = S.At (c, r);
			const std::uint32_t nFg = VGAColour (Cell.nFg);
			const std::uint32_t nBg = VGAColour (Cell.nBg);

			// Latin-1 is what the font has; anything else is drawn as '?'.
			std::uint32_t ch = Cell.nChar <= 0xFF ? Cell.nChar : '?';
			const unsigned char *pGlyph = nullptr;
			if (ch >= Font.first_char && ch <= Font.last_char && Font.width == 8 && Font.height >= 16)
			{
				pGlyph = pGlyphs + (ch - Font.first_char) * Font.height;
			}

			for (unsigned y = 0; y < 16; y++)
			{
				std::uint32_t *pRow = pOut + (r * 16 + y) * nPitch + c * 8;
				const unsigned char nBits = pGlyph != nullptr ? pGlyph[y] : 0;
				for (unsigned x = 0; x < 8; x++)
				{
					bool bOn = (nBits & (0x80 >> x)) != 0;
					if ((Cell.nLine & LineH) && y == 8) bOn = true;
					if ((Cell.nLine & LineV) && (x == 3 || x == 4)) bOn = true;
					pRow[x] = bOn ? nFg : nBg;
				}
			}
		}
	}
}

} // namespace baremenu

#endif
