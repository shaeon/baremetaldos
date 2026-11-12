//
// key_report_test.cpp - host test for src/platform/key_report.h (make check-keys)
// Shaeon (Carlos Palmero) 2026, for BM-DOS by @mmoroca.  GPL-3.0-or-later.
//
#include "../../src/platform/key_report.h"

#include <cstdio>
#include <string>

static unsigned s_nFailures = 0;
static void Check (bool b, const char *p)
{
	std::printf ("  %s  %s\n", b ? "ok    " : "FAILED", p);
	if (!b) s_nFailures++;
}

// Feed reports, collect events as text: "+04 +16 -04".
static std::string Run (std::initializer_list<std::initializer_list<unsigned char>> Reports)
{
	unsigned char Old[6] = {};
	std::string Out;
	for (const auto &R : Reports)
	{
		unsigned char New[6] = {};
		unsigned i = 0;
		for (unsigned char k : R) New[i++] = k;
		barekeys::Diff (Old, New,
			[&] (unsigned char u) { char b[8]; std::snprintf (b, sizeof b, "-%02X ", u); Out += b; },
			[&] (unsigned char u) { char b[8]; std::snprintf (b, sizeof b, "+%02X ", u); Out += b; });
		if (!(New[0] >= 1 && New[0] <= 3))
			for (unsigned j = 0; j < 6; j++) Old[j] = New[j];
	}
	return Out;
}

int main (void)
{
	// 0x04 = a, 0x16 = s, 0x07 = d
	Check (Run ({ {0x04}, {} }) == "+04 -04 ", "a key down, then up");
	Check (Run ({ {0x04}, {0x04, 0x16}, {0x16}, {} }) == "+04 +16 -04 -16 ",
	       "fast typing, 'a' still held when 's' goes down: 'a' is pressed ONCE");
	Check (Run ({ {0x04}, {0x04}, {0x04}, {} }) == "+04 -04 ",
	       "a report repeated with the key held (a modifier changed, an idle report) presses nothing");
	Check (Run ({ {0x04, 0x16}, {0x16, 0x04}, {} }) == "+04 +16 -16 -04 ",
	       "the same keys in another order are the same keys");
	Check (Run ({ {0x04}, {0x01, 0x01, 0x01, 0x01, 0x01, 0x01}, {0x04}, {} }) == "+04 -04 ",
	       "a rollover error report changes nothing");
	Check (Run ({ {0x04, 0x16, 0x07}, {0x16} }) == "+04 +16 +07 -04 -07 ",
	       "several released at once");
	std::printf ("\n%u failures\n", s_nFailures);
	return s_nFailures == 0 ? 0 : 1;
}
