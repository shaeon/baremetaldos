//
// key_report.h
//
// BM-DOS - what changed between two USB keyboard reports.  A boot-protocol
// report lists up to six keys that are DOWN NOW; a key event is a difference
// between two of them: in the old one and not in the new - released; in the
// new one and not in the old - pressed.  A key in both is still held, and is
// NOT pressed again.
//
// That last line is why this is its own file with a test.  The handler used to
// deliver "down" for every key in every report: type "as" with the 'a' still
// held when the 's' goes down - ordinary fast typing - and the report holding
// both delivered 'a' a second time ("aas"), in DOS and in the setup menu's
// text boxes alike; and Caps Lock held while another key changed toggled its
// LED again.  No Circle: scripts/hostcheck/key_report_test.cpp.
//
// Shaeon (Carlos Palmero) 2026, for BM-DOS by @mmoroca.  GPL-3.0-or-later.
//
#ifndef BARE_KEY_REPORT_H
#define BARE_KEY_REPORT_H

namespace barekeys {

constexpr unsigned ReportKeys = 6;

inline bool Holds (const unsigned char Report[ReportKeys], unsigned char ucUsage)
{
	for (unsigned i = 0; i < ReportKeys; i++)
	{
		if (Report[i] == ucUsage) return true;
	}
	return false;
}

/// Calls Released (usage) for each key of Old that is not in New, then
/// Pressed (usage) for each key of New that is not in Old.  0 is "no key";
/// 1..3 are the keyboard's error codes (rollover) and are not keys either.
template <typename TReleased, typename TPressed>
void Diff (const unsigned char Old[ReportKeys], const unsigned char New[ReportKeys],
	   TReleased Released, TPressed Pressed)
{
	// A rollover error report (all keys 0x01) says "too many keys", not
	// "everything was released": keep the state as it was.
	if (New[0] >= 1 && New[0] <= 3)
	{
		return;
	}

	for (unsigned i = 0; i < ReportKeys; i++)
	{
		if (Old[i] > 3 && !Holds (New, Old[i])) Released (Old[i]);
	}
	for (unsigned i = 0; i < ReportKeys; i++)
	{
		if (New[i] > 3 && !Holds (Old, New[i])) Pressed (New[i]);
	}
}

} // namespace barekeys

#endif
