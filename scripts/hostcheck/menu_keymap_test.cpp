//
// menu_keymap_test.cpp - host test for src/platform/menu_keymap.h (make check-menu)
// Shaeon (Carlos Palmero) 2026, for BM-DOS by @mmoroca.  GPL-3.0-or-later.
//
#include "../../src/platform/menu_keymap.h"

#include <cstdio>

using namespace baremenu;

static unsigned s_nFailures = 0;
static void Check (bool b, const char *p)
{
	std::printf ("  %s  %s\n", b ? "ok    " : "FAILED", p);
	if (!b) s_nFailures++;
}

int main (void)
{
	const TLayout US = TLayout::US, ES = TLayout::Spanish;
	Check (KeyToChar (RETROK_a, false, false, ES) == 'a' && KeyToChar (RETROK_a, true, false, ES) == 'A',
	       "letters, with Shift");
	Check (KeyToChar (RETROK_2, true, false, US) == '@' && KeyToChar (RETROK_2, false, true, ES) == '@'
	       && KeyToChar (RETROK_2, true, false, ES) == '"', "@: Shift+2 in US, AltGr+2 in Spanish (Shift+2 is \")");
	Check (KeyToChar (RETROK_SEMICOLON, false, false, ES) == 0xF1 && KeyToChar (RETROK_SEMICOLON, false, false, US) == ';',
	       "the key right of L: ñ in Spanish, ; in US");
	Check (KeyToChar (RETROK_SLASH, false, false, ES) == '-' && KeyToChar (RETROK_SLASH, true, false, ES) == '_',
	       "Spanish - and _ where US has / and ?");
	Check (KeyToChar (RETROK_3, false, true, ES) == '#' && KeyToChar (RETROK_1, false, true, ES) == '|'
	       && KeyToChar (RETROK_e, false, true, ES) == 0x20AC, "AltGr: # | and the euro sign");
	Check (KeyToChar (RETROK_7, true, false, ES) == '/' && KeyToChar (RETROK_0, true, false, ES) == '=',
	       "Spanish Shift+7 is /, Shift+0 is =");
	Check (KeyToChar (RETROK_KP5, false, false, ES) == '5' && KeyToChar (RETROK_KP_PLUS, false, false, US) == '+',
	       "the keypad types digits and operators");
	Check (KeyToChar (RETROK_UP, false, false, ES) == 0 && KeyToChar (RETROK_F1, true, false, US) == 0,
	       "arrows and F-keys make no character");
	Check (KeyToChar (RETROK_a, false, false, ES, true) == 'A' && KeyToChar (RETROK_a, true, false, ES, true) == 'a'
	       && KeyToChar (RETROK_SEMICOLON, false, false, ES, true) == 0xD1
	       && KeyToChar (RETROK_1, false, false, ES, true) == '1',
	       "Caps Lock turns letters round (Shift turns them back), ñ included, not digits");
	Check (KeyToChar (RETROK_BACKSLASH, false, false, ES) == 0xE7 && KeyToChar (RETROK_BACKSLASH, false, true, ES) == '}',
	       "the ç key (ISO key next to Enter, delivered as backslash): ç, and } with AltGr");
	std::printf ("\n%u failures\n", s_nFailures);
	return s_nFailures == 0 ? 0 : 1;
}
