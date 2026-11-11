//
// conf_edit.h
//
// BM-DOS - changing one value in a configuration file without touching
// anything else in it.  The setup menu writes boot.conf, dosbox.conf and
// cmdline.txt, and all three are also edited by hand on a PC: the comments,
// the order, the spacing, the CRLF line ends a Windows editor leaves, and the
// lines this port does not know about must all survive a save from the menu.
// No Circle dependency: scripts/hostcheck/conf_edit_test.cpp (make check-conf).
//
//   CConfText  "key = value" / "key=value" lines, optionally under [sections]
//              (dosbox.conf has sections; boot.conf has none - section "").
//              Set() changes the value in place on the line that has it, or
//              adds a line at the end of its section (creating the section at
//              the end of the file if needed), in the file's own style.
//   Cmdline*   cmdline.txt: one line of space-separated key=value words.
//
// Comments: '#' at the start of a line (after spaces) always; inline as well
// when the file allows it - boot.conf does (its reader cuts at any '#'),
// dosbox.conf does not (DOSBox keeps everything after '=').
//
// Shaeon (Carlos Palmero) 2026, for BM-DOS by @mmoroca.  GPL-3.0-or-later.
//
#ifndef BARE_CONF_EDIT_H
#define BARE_CONF_EDIT_H

#include <cctype>
#include <string>
#include <vector>

namespace bareconf {

inline std::string Lower (std::string s)
{
	for (char &c : s) c = static_cast<char> (std::tolower (static_cast<unsigned char> (c)));
	return s;
}

inline std::string Trim (const std::string &s)
{
	size_t a = 0, b = s.size ();
	while (a < b && std::isspace (static_cast<unsigned char> (s[a]))) a++;
	while (b > a && std::isspace (static_cast<unsigned char> (s[b - 1]))) b--;
	return s.substr (a, b - a);
}

class CConfText
{
public:
	CConfText (const std::string &Text, bool bInlineComments)
	: m_bInline (bInlineComments)
	{
		m_bCRLF = Text.find ("\r\n") != std::string::npos;
		m_bFinalNewline = !Text.empty () && Text.back () == '\n';

		size_t i = 0;
		while (i < Text.size ())
		{
			size_t j = Text.find ('\n', i);
			if (j == std::string::npos) j = Text.size ();
			std::string Line = Text.substr (i, j - i);
			if (!Line.empty () && Line.back () == '\r') Line.pop_back ();
			m_Lines.push_back (Line);
			i = j + 1;
		}

		// The style new lines are written in: "key = value" if the file
		// already writes it that way, "key=value" otherwise.
		for (const std::string &L : m_Lines)
		{
			TParsed P;
			if (Parse (L, &P) == Kind::Key)
			{
				m_bSpaced = L.find (" = ") != std::string::npos;
				break;
			}
		}
	}

	std::string Text (void) const
	{
		const char *pEol = m_bCRLF ? "\r\n" : "\n";
		std::string s;
		for (size_t i = 0; i < m_Lines.size (); i++)
		{
			s += m_Lines[i];
			if (i + 1 < m_Lines.size () || m_bFinalNewline) s += pEol;
		}
		return s;
	}

	/// The value of key in section ("" for lines before any [section]).
	bool Get (const std::string &Section, const std::string &Key, std::string *pValue) const
	{
		std::string Current;
		bool bFound = false;
		for (const std::string &L : m_Lines)
		{
			TParsed P;
			switch (Parse (L, &P))
			{
			case Kind::Section: Current = P.Name; break;
			case Kind::Key:
				if (Current == Lower (Section) && P.Name == Lower (Key))
				{
					*pValue = L.substr (P.nValueStart, P.nValueEnd - P.nValueStart);
					bFound = true;		// the last one wins, as in both readers
				}
				break;
			default: break;
			}
		}
		return bFound;
	}

	void Set (const std::string &Section, const std::string &Key, const std::string &Value)
	{
		const std::string Sec = Lower (Section), K = Lower (Key);
		std::string Current;
		int nLastKeyLine = -1;		// in the section, the line to append after
		int nSectionLine = Sec.empty () ? -2 : -1;	// -2: the unnamed section
		int nHit = -1;

		for (size_t i = 0; i < m_Lines.size (); i++)
		{
			TParsed P;
			switch (Parse (m_Lines[i], &P))
			{
			case Kind::Section:
				Current = P.Name;
				if (Current == Sec) nSectionLine = static_cast<int> (i);
				break;
			case Kind::Key:
				if (Current == Sec)
				{
					nLastKeyLine = static_cast<int> (i);
					if (P.Name == K) nHit = static_cast<int> (i);
				}
				break;
			default:
				break;
			}
		}

		if (nHit >= 0)
		{
			TParsed P;
			Parse (m_Lines[nHit], &P);
			std::string &L = m_Lines[nHit];
			L = L.substr (0, P.nValueStart) + Value + L.substr (P.nValueEnd);
			return;
		}

		const std::string NewLine = Key + (m_bSpaced ? " = " : "=") + Value;

		if (nLastKeyLine >= 0)
		{
			m_Lines.insert (m_Lines.begin () + nLastKeyLine + 1, NewLine);
		}
		else if (nSectionLine >= 0)
		{
			m_Lines.insert (m_Lines.begin () + nSectionLine + 1, NewLine);
		}
		else if (nSectionLine == -2)
		{
			// boot.conf style: no sections; at the end of the file.
			m_Lines.push_back (NewLine);
			m_bFinalNewline = true;
		}
		else
		{
			if (!m_Lines.empty () && !Trim (m_Lines.back ()).empty ()) m_Lines.push_back ("");
			m_Lines.push_back ("[" + Section + "]");
			m_Lines.push_back (NewLine);
			m_bFinalNewline = true;
		}
	}

	// --- whole lines, for [autoexec] (commands, not key=value) ----------

	/// Lines of a section, as (index into the file, text), comments included.
	std::vector<std::pair<size_t, std::string>> SectionLines (const std::string &Section) const
	{
		std::vector<std::pair<size_t, std::string>> Out;
		std::string Current;
		for (size_t i = 0; i < m_Lines.size (); i++)
		{
			const std::string T = Trim (m_Lines[i]);
			if (!T.empty () && T[0] == '[' && T.find (']') != std::string::npos)
			{
				Current = Lower (Trim (T.substr (1, T.find (']') - 1)));
				continue;
			}
			if (Current == Lower (Section)) Out.push_back ({ i, m_Lines[i] });
		}
		return Out;
	}

	/// Index of the [section] header line, or -1.
	long SectionHeader (const std::string &Section) const
	{
		for (size_t i = 0; i < m_Lines.size (); i++)
		{
			const std::string T = Trim (m_Lines[i]);
			if (!T.empty () && T[0] == '[' && T.find (']') != std::string::npos
			    && Lower (Trim (T.substr (1, T.find (']') - 1))) == Lower (Section))
				return static_cast<long> (i);
		}
		return -1;
	}

	size_t LineCount (void) const { return m_Lines.size (); }

	void ReplaceLine (size_t nIndex, const std::string &Text)
	{
		if (nIndex < m_Lines.size ()) m_Lines[nIndex] = Text;
	}

	void InsertLine (size_t nIndex, const std::string &Text)
	{
		if (nIndex > m_Lines.size ()) nIndex = m_Lines.size ();
		m_Lines.insert (m_Lines.begin () + nIndex, Text);
		if (nIndex == m_Lines.size () - 1) m_bFinalNewline = true;
	}

	void RemoveLine (size_t nIndex)
	{
		if (nIndex < m_Lines.size ()) m_Lines.erase (m_Lines.begin () + nIndex);
	}

private:
	enum class Kind { Other, Section, Key };

	struct TParsed
	{
		std::string Name;
		size_t nValueStart = 0;
		size_t nValueEnd = 0;
	};

	Kind Parse (const std::string &L, TParsed *pP) const
	{
		const std::string T = Trim (L);
		if (T.empty () || T[0] == '#' || T[0] == ';') return Kind::Other;

		if (T[0] == '[')
		{
			const size_t e = T.find (']');
			if (e == std::string::npos) return Kind::Other;
			pP->Name = Lower (Trim (T.substr (1, e - 1)));
			return Kind::Section;
		}

		const size_t nEq = L.find ('=');
		if (nEq == std::string::npos) return Kind::Other;
		if (m_bInline)
		{
			const size_t nHash = L.find ('#');
			if (nHash != std::string::npos && nHash < nEq) return Kind::Other;
		}

		pP->Name = Lower (Trim (L.substr (0, nEq)));
		if (pP->Name.empty ()) return Kind::Other;

		size_t a = nEq + 1;
		while (a < L.size () && (L[a] == ' ' || L[a] == '\t')) a++;
		size_t b = L.size ();
		if (m_bInline)
		{
			const size_t nHash = L.find ('#', a);
			if (nHash != std::string::npos) b = nHash;
		}
		while (b > a && (L[b - 1] == ' ' || L[b - 1] == '\t')) b--;
		pP->nValueStart = a;
		pP->nValueEnd = b;
		return Kind::Key;
	}

	std::vector<std::string> m_Lines;
	bool m_bInline;
	bool m_bCRLF = false;
	bool m_bFinalNewline = true;
	bool m_bSpaced = true;
};

// --- cmdline.txt: one line of words ---------------------------------------------

inline bool CmdlineGet (const std::string &Text, const std::string &Key, std::string *pValue)
{
	size_t i = 0;
	while (i < Text.size ())
	{
		while (i < Text.size () && std::isspace (static_cast<unsigned char> (Text[i]))) i++;
		size_t j = i;
		while (j < Text.size () && !std::isspace (static_cast<unsigned char> (Text[j]))) j++;
		const std::string Word = Text.substr (i, j - i);
		const size_t nEq = Word.find ('=');
		if (nEq != std::string::npos && Word.substr (0, nEq) == Key)
		{
			*pValue = Word.substr (nEq + 1);
			return true;
		}
		i = j;
	}
	return false;
}

/// Replace key's value in place, or add "key=value" at the end of the line.
/// The file stays one line: the firmware reads only the first.
inline std::string CmdlineSet (const std::string &Text, const std::string &Key, const std::string &Value)
{
	size_t i = 0;
	while (i < Text.size ())
	{
		while (i < Text.size () && std::isspace (static_cast<unsigned char> (Text[i]))) i++;
		size_t j = i;
		while (j < Text.size () && !std::isspace (static_cast<unsigned char> (Text[j]))) j++;
		const std::string Word = Text.substr (i, j - i);
		const size_t nEq = Word.find ('=');
		if (nEq != std::string::npos && Word.substr (0, nEq) == Key)
		{
			return Text.substr (0, i) + Key + "=" + Value + Text.substr (j);
		}
		i = j;
	}

	// Not there: before the line end, if any.
	size_t nEnd = Text.size ();
	while (nEnd > 0 && (Text[nEnd - 1] == '\n' || Text[nEnd - 1] == '\r')) nEnd--;
	std::string Head = Text.substr (0, nEnd);
	if (!Head.empty () && Head.back () != ' ') Head += ' ';
	return Head + Key + "=" + Value + Text.substr (nEnd);
}

// --- [autoexec] command lines ----------------------------------------------------
//
// "KEYB es 850": a command found by its first word.  Get returns the rest of
// the line; Set replaces the whole line (the first active one), adds it at the
// end of [autoexec] when there is none, or removes it when Args is empty.

inline long FindCommand (const CConfText &C, const std::string &Command)
{
	for (const auto &L : C.SectionLines ("autoexec"))
	{
		const std::string T = Trim (L.second);
		if (T.empty () || T[0] == '#') continue;
		const size_t n = T.find_first_of (" \t");
		if (Lower (T.substr (0, n)) == Lower (Command)) return static_cast<long> (L.first);
	}
	return -1;
}

inline std::string GetCommand (const CConfText &C, const std::string &Command)
{
	const long n = FindCommand (C, Command);
	if (n < 0) return "";
	for (const auto &L : C.SectionLines ("autoexec"))
	{
		if (static_cast<long> (L.first) != n) continue;
		const std::string T = Trim (L.second);
		const size_t s = T.find_first_of (" \t");
		return s == std::string::npos ? "" : Trim (T.substr (s));
	}
	return "";
}

inline void SetCommand (CConfText &C, const std::string &Command, const std::string &Args)
{
	const long n = FindCommand (C, Command);
	const std::string Line = Command + " " + Args;
	if (n >= 0)
	{
		if (Args.empty ()) C.RemoveLine (static_cast<size_t> (n));
		else C.ReplaceLine (static_cast<size_t> (n), Line);
		return;
	}
	if (Args.empty ()) return;

	// At the end of [autoexec]: after its last line that is not blank.
	const auto Lines = C.SectionLines ("autoexec");
	size_t nAt = 0;
	bool bFound = false;
	for (const auto &L : Lines)
	{
		if (!Trim (L.second).empty ()) { nAt = L.first + 1; bFound = true; }
	}
	if (!bFound)
	{
		const long nHeader = C.SectionHeader ("autoexec");
		if (nHeader < 0)
		{
			C.InsertLine (C.LineCount (), "[autoexec]");
			nAt = C.LineCount ();
		}
		else
		{
			nAt = static_cast<size_t> (nHeader) + 1;
		}
	}
	C.InsertLine (nAt, Line);
}

// --- [autoexec] block owned by the setup menu -------------------------------------
//
// What runs at boot (a game) is a few lines - change drive, change folder, run
// - so the menu keeps them between two marker comments at the end of
// [autoexec], and only ever touches what is between its own markers.  DOSBox
// ignores '#' lines in [autoexec].

constexpr const char *BlockBegin = "# BM-DOS: arranque (lo escribe el menu Win+F12)";
constexpr const char *BlockEnd = "# BM-DOS: fin del arranque";

inline std::vector<std::string> GetBlock (const CConfText &C)
{
	std::vector<std::string> Out;
	bool bIn = false;
	for (const auto &L : C.SectionLines ("autoexec"))
	{
		const std::string T = Trim (L.second);
		if (T == BlockBegin) { bIn = true; continue; }
		if (T == BlockEnd) break;
		if (bIn && !T.empty ()) Out.push_back (T);
	}
	return Out;
}

/// Replace the block's lines; an empty list removes the block, markers included.
inline void SetBlock (CConfText &C, const std::vector<std::string> &Lines)
{
	// Out with the old block.
	long nBegin = -1, nEnd = -1;
	for (const auto &L : C.SectionLines ("autoexec"))
	{
		const std::string T = Trim (L.second);
		if (T == BlockBegin && nBegin < 0) nBegin = static_cast<long> (L.first);
		else if (T == BlockEnd && nBegin >= 0) { nEnd = static_cast<long> (L.first); break; }
	}
	size_t nAt = 0;
	if (nBegin >= 0)
	{
		const long nLast = nEnd >= 0 ? nEnd : nBegin;
		for (long i = nLast; i >= nBegin; i--) C.RemoveLine (static_cast<size_t> (i));
		nAt = static_cast<size_t> (nBegin);
	}
	if (Lines.empty ()) return;

	if (nBegin < 0)
	{
		// At the end of [autoexec], after its last non-blank line.
		bool bFound = false;
		for (const auto &L : C.SectionLines ("autoexec"))
			if (!Trim (L.second).empty ()) { nAt = L.first + 1; bFound = true; }
		if (!bFound)
		{
			const long nHeader = C.SectionHeader ("autoexec");
			if (nHeader < 0) { C.InsertLine (C.LineCount (), "[autoexec]"); nAt = C.LineCount (); }
			else nAt = static_cast<size_t> (nHeader) + 1;
		}
	}

	C.InsertLine (nAt++, BlockBegin);
	for (const std::string &L : Lines) C.InsertLine (nAt++, L);
	C.InsertLine (nAt, BlockEnd);
}

// --- [autoexec] MOUNT lines --------------------------------------------------
//
// "MOUNT C /dos -freesize 1024": the drive letter is the second word and the
// host folder the third; whatever follows (options) is kept.  Matched without
// regard to case, comments excluded.

inline std::vector<std::string> Words (const std::string &Line)
{
	std::vector<std::string> W;
	size_t i = 0;
	while (i < Line.size ())
	{
		while (i < Line.size () && std::isspace (static_cast<unsigned char> (Line[i]))) i++;
		size_t j = i;
		while (j < Line.size () && !std::isspace (static_cast<unsigned char> (Line[j]))) j++;
		if (j > i) W.push_back (Line.substr (i, j - i));
		i = j;
	}
	return W;
}

/// Index in the file of the active "MOUNT <letter> ..." line in [autoexec], or -1.
inline long FindMount (const CConfText &C, char cLetter)
{
	for (const auto &L : C.SectionLines ("autoexec"))
	{
		const std::vector<std::string> W = Words (L.second);
		if (W.size () >= 3 && W[0][0] != '#' && Lower (W[0]) == "mount"
		    && Lower (W[1]) == std::string (1, static_cast<char> (std::tolower (cLetter))))
		{
			return static_cast<long> (L.first);
		}
	}
	return -1;
}

/// The folder MOUNT <letter> points at, or "" when there is no such line.
inline std::string GetMount (const CConfText &C, char cLetter)
{
	const long n = FindMount (C, cLetter);
	if (n < 0) return "";
	for (const auto &L : C.SectionLines ("autoexec"))
		if (static_cast<long> (L.first) == n) return Words (L.second)[2];
	return "";
}

/// Point MOUNT <letter> at Folder (options on the line kept), add the line
/// after "MOUNT C" (or at the top of [autoexec]) if there is none, or remove
/// it when Folder is empty.
inline void SetMount (CConfText &C, char cLetter, const std::string &Folder)
{
	const long n = FindMount (C, cLetter);
	if (n >= 0)
	{
		if (Folder.empty ())
		{
			C.RemoveLine (static_cast<size_t> (n));
			return;
		}
		std::string Line;
		for (const auto &L : C.SectionLines ("autoexec"))
			if (static_cast<long> (L.first) == n) Line = L.second;
		// Replace the third word in place, keeping everything around it.
		size_t i = 0;
		for (int w = 0; w < 2; w++)
		{
			while (i < Line.size () && std::isspace (static_cast<unsigned char> (Line[i]))) i++;
			while (i < Line.size () && !std::isspace (static_cast<unsigned char> (Line[i]))) i++;
		}
		while (i < Line.size () && std::isspace (static_cast<unsigned char> (Line[i]))) i++;
		size_t j = i;
		while (j < Line.size () && !std::isspace (static_cast<unsigned char> (Line[j]))) j++;
		C.ReplaceLine (static_cast<size_t> (n), Line.substr (0, i) + Folder + Line.substr (j));
		return;
	}

	if (Folder.empty ()) return;

	const std::string NewLine = std::string ("MOUNT ") + static_cast<char> (std::toupper (cLetter)) + " " + Folder;
	const long nC = FindMount (C, 'c');
	if (nC >= 0)
	{
		C.InsertLine (static_cast<size_t> (nC) + 1, NewLine);
		return;
	}
	const long nHeader = C.SectionHeader ("autoexec");
	if (nHeader >= 0)
	{
		C.InsertLine (static_cast<size_t> (nHeader) + 1, NewLine);
	}
	else
	{
		C.InsertLine (C.LineCount (), "[autoexec]");
		C.InsertLine (C.LineCount (), NewLine);
	}
}

} // namespace bareconf

#endif
