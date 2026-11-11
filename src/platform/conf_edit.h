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

} // namespace bareconf

#endif
