//
// menu_keymap.h
//
// BM-DOS - the character a key makes, for the setup menu's text boxes (a WiFi
// network's name and password).  The USB keyboard reports positions, not
// characters (libretro RETROK_* codes are the US layout's names for those
// positions), and DOS gets its layout from KEYB; the menu runs outside DOS, so
// it needs its own table.  Two layouts: US, and Spanish (Spain and Latin
// America share the keys a password uses), with Shift and AltGr.  Dead keys
// (´ ` ^ ¨ on a Spanish keyboard) give the accent itself - a WPA password is
// ASCII in practice, and a name with an accent can be typed as its letter.
//
// Only libretro.h: scripts/hostcheck/menu_keymap_test.cpp runs it on the PC.
//
// Shaeon (Carlos Palmero) 2026, for BM-DOS by @mmoroca.  GPL-3.0-or-later.
//
#ifndef BARE_MENU_KEYMAP_H
#define BARE_MENU_KEYMAP_H

#include "libretro.h"

#include <cstdint>

namespace baremenu {

enum class TLayout { US, Spanish };

/// The character for a key, or 0 if it makes none (arrows, F-keys...).
/// bCaps: Caps Lock is on.  It turns the letters round (and Shift turns them
/// back), ñ and ç included; it does nothing to digits and symbols.
inline std::uint32_t KeyToChar (unsigned nKey, bool bShift, bool bAltGr, TLayout Layout,
				bool bCaps = false)
{
	// Letters: the same position in both layouts.
	if (nKey >= RETROK_a && nKey <= RETROK_z)
	{
		if (bAltGr)
		{
			return Layout == TLayout::Spanish && nKey == RETROK_e ? 0x20AC : 0;	// €
		}
		return (bShift != bCaps) ? nKey - RETROK_a + 'A' : nKey;
	}

	// The keypad: digits and operators whatever the layout (Num Lock assumed).
	if (nKey >= RETROK_KP0 && nKey <= RETROK_KP0 + 9) return '0' + (nKey - RETROK_KP0);
	switch (nKey)
	{
	case RETROK_KP_PERIOD:		return '.';
	case RETROK_KP_DIVIDE:		return '/';
	case RETROK_KP_MULTIPLY:	return '*';
	case RETROK_KP_MINUS:		return '-';
	case RETROK_KP_PLUS:		return '+';
	case RETROK_SPACE:		return ' ';
	default:			break;
	}

	if (Layout == TLayout::US)
	{
		if (bAltGr) return 0;
		static const char Digits[] = "0123456789", Shifted[] = ")!@#$%^&*(";
		if (nKey >= RETROK_0 && nKey <= RETROK_9)
		{
			return static_cast<unsigned char> ((bShift ? Shifted : Digits)[nKey - RETROK_0]);
		}
		switch (nKey)
		{
		case RETROK_BACKQUOTE:	  return bShift ? '~' : '`';
		case RETROK_MINUS:	  return bShift ? '_' : '-';
		case RETROK_EQUALS:	  return bShift ? '+' : '=';
		case RETROK_LEFTBRACKET:  return bShift ? '{' : '[';
		case RETROK_RIGHTBRACKET: return bShift ? '}' : ']';
		case RETROK_BACKSLASH:	  return bShift ? '|' : '\\';
		case RETROK_SEMICOLON:	  return bShift ? ':' : ';';
		case RETROK_QUOTE:	  return bShift ? '"' : '\'';
		case RETROK_COMMA:	  return bShift ? '<' : ',';
		case RETROK_PERIOD:	  return bShift ? '>' : '.';
		case RETROK_SLASH:	  return bShift ? '?' : '/';
		case RETROK_OEM_102:	  return bShift ? '|' : '\\';
		default:		  return 0;
		}
	}

	// Spanish (es / la): the positions of a Spanish keyboard.
	if (bAltGr)
	{
		switch (nKey)
		{
		case RETROK_1:		  return '|';
		case RETROK_2:		  return '@';
		case RETROK_3:		  return '#';
		case RETROK_4:		  return '~';
		case RETROK_6:		  return 0xAC;		// ¬
		case RETROK_BACKQUOTE:	  return '\\';
		case RETROK_LEFTBRACKET:  return '[';
		case RETROK_RIGHTBRACKET: return ']';
		case RETROK_QUOTE:	  return '{';
		case RETROK_BACKSLASH:	  return '}';
		default:		  return 0;
		}
	}
	if (nKey >= RETROK_0 && nKey <= RETROK_9)
	{
		static const char Shifted[] = "=!\"\xB7$%&/()";	// 0..9 with Shift; 3 is '·'
		if (!bShift) return '0' + (nKey - RETROK_0);
		return nKey == RETROK_3 ? 0xB7 : static_cast<unsigned char> (Shifted[nKey - RETROK_0]);
	}
	switch (nKey)
	{
	case RETROK_BACKQUOTE:	  return bShift ? 0xAA : 0xBA;		// ª º
	case RETROK_MINUS:	  return bShift ? '?' : '\'';
	case RETROK_EQUALS:	  return bShift ? 0xBF : 0xA1;		// ¿ ¡
	case RETROK_LEFTBRACKET:  return bShift ? '^' : '`';
	case RETROK_RIGHTBRACKET: return bShift ? '*' : '+';
	case RETROK_BACKSLASH:	  return (bShift != bCaps) ? 0xC7 : 0xE7;	// Ç ç
	case RETROK_SEMICOLON:	  return (bShift != bCaps) ? 0xD1 : 0xF1;	// Ñ ñ
	case RETROK_QUOTE:	  return bShift ? 0xA8 : 0xB4;		// ¨ ´
	case RETROK_COMMA:	  return bShift ? ';' : ',';
	case RETROK_PERIOD:	  return bShift ? ':' : '.';
	case RETROK_SLASH:	  return bShift ? '_' : '-';
	case RETROK_OEM_102:	  return bShift ? '>' : '<';
	default:		  return 0;
	}
}

} // namespace baremenu

#endif
