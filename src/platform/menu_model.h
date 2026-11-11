//
// menu_model.h
//
// BM-DOS - the setup menu (Win+F12), without Circle: pages of items, the
// cursor, the keys, and the layout drawn into an 80x30 grid of character
// cells.  bare_menu.cpp fills the pages with the machine's real values and
// turns the grid into pixels; scripts/hostcheck/menu_test.cpp drives this file
// on the PC (make check-menu).
//
// The look is a PC BIOS setup of the early nineties:
//
//   row 0      title bar
//   row 1      the tabs (Sistema, Máquina, ...), the current one highlighted
//   rows 2-26  items on the left, help for the selected item on the right
//   rows 27-29 the keys
//
// Every page is a list of items declared by the caller, so a page grows by
// adding entries, never by drawing a new screen.  Each item says when a
// change takes effect, shown in its own column:
//
//   (blank)  at once
//   R        when DOSBox restarts (a few seconds, the board keeps running)
//   B        when the board restarts
//
// Texts are UTF-8 in the source; the screen keeps them as UTF-8 too and the
// renderer converts to Latin-1, which is what Circle's 8x16 font covers.
//
// Shaeon (Carlos Palmero) 2026, for BM-DOS by @mmoroca.  GPL-3.0-or-later.
//
#ifndef BARE_MENU_MODEL_H
#define BARE_MENU_MODEL_H

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace baremenu {

// --- the character grid ------------------------------------------------------

constexpr unsigned Cols = 80;
constexpr unsigned Rows = 30;

// The sixteen colours of the VGA text palette, by their usual index.
enum TColour : std::uint8_t
{
	Black, Blue, Green, Cyan, Red, Magenta, Brown, LightGrey,
	DarkGrey, LightBlue, LightGreen, LightCyan, LightRed, LightMagenta, Yellow, White
};

// Lines drawn in pixels across a cell (the font has no box characters).
enum TLine : std::uint8_t
{
	LineNone = 0,
	LineH = 1,	// horizontal, through the middle of the cell
	LineV = 2,	// vertical, through the middle of the cell
};

struct TCell
{
	std::uint32_t nChar = ' ';	// a Unicode code point (Latin-1 range is drawn)
	std::uint8_t nFg = Black;
	std::uint8_t nBg = LightGrey;
	std::uint8_t nLine = LineNone;
};

class CScreen
{
public:
	void Fill (unsigned x, unsigned y, unsigned w, unsigned h, std::uint8_t nFg, std::uint8_t nBg)
	{
		for (unsigned r = y; r < y + h && r < Rows; r++)
			for (unsigned c = x; c < x + w && c < Cols; c++)
				m_Cells[r][c] = TCell { ' ', nFg, nBg, LineNone };
	}

	/// Write UTF-8 text from (x, y), at most nMax cells.  Returns cells used.
	unsigned Put (unsigned x, unsigned y, const std::string &Text, std::uint8_t nFg,
		      std::uint8_t nBg, unsigned nMax = Cols)
	{
		unsigned n = 0;
		for (size_t i = 0; i < Text.size () && n < nMax && x + n < Cols && y < Rows; )
		{
			const std::uint32_t c = Decode (Text, i);
			m_Cells[y][x + n] = TCell { c, nFg, nBg, LineNone };
			n++;
		}
		return n;
	}

	void HLine (unsigned x, unsigned y, unsigned w)
	{
		for (unsigned c = x; c < x + w && c < Cols && y < Rows; c++)
			m_Cells[y][c].nLine |= LineH;
	}

	void VLine (unsigned x, unsigned y, unsigned h)
	{
		for (unsigned r = y; r < y + h && r < Rows && x < Cols; r++)
			m_Cells[r][x].nLine |= LineV;
	}

	const TCell &At (unsigned x, unsigned y) const { return m_Cells[y][x]; }

	/// One row as UTF-8, for the tests.
	std::string RowText (unsigned y) const
	{
		std::string s;
		for (unsigned c = 0; c < Cols; c++)
			Encode (s, m_Cells[y][c].nChar);
		return s;
	}

	/// Code points of a UTF-8 string (how many cells it takes).
	static unsigned Length (const std::string &Text)
	{
		unsigned n = 0;
		for (size_t i = 0; i < Text.size (); ) { Decode (Text, i); n++; }
		return n;
	}

	static std::uint32_t Decode (const std::string &s, size_t &i)
	{
		const unsigned char c = static_cast<unsigned char> (s[i++]);
		if (c < 0x80) return c;
		unsigned nMore = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
		std::uint32_t cp = c & (0x3F >> nMore);
		while (nMore-- != 0 && i < s.size ())
			cp = (cp << 6) | (static_cast<unsigned char> (s[i++]) & 0x3F);
		return cp;
	}

	static void Encode (std::string &s, std::uint32_t cp)
	{
		if (cp < 0x80) { s += static_cast<char> (cp); return; }
		if (cp < 0x800)
		{
			s += static_cast<char> (0xC0 | (cp >> 6));
			s += static_cast<char> (0x80 | (cp & 0x3F));
			return;
		}
		s += static_cast<char> (0xE0 | (cp >> 12));
		s += static_cast<char> (0x80 | ((cp >> 6) & 0x3F));
		s += static_cast<char> (0x80 | (cp & 0x3F));
	}

private:
	TCell m_Cells[Rows][Cols];
};

// --- the model -----------------------------------------------------------------

enum class TApply { Now, RestartDOSBox, RestartBoard };

enum class TKind
{
	Info,		// label and value, not selectable
	Section,	// a heading inside a page, not selectable
	Action,		// Enter runs it
	Choice,		// +/- (or Enter) cycles through Choices
	Planned,	// greyed: an option that is coming; the cursor can stop on it
			// to read its help, but it cannot be changed
};

struct TItem
{
	TKind Kind = TKind::Info;
	std::string Label;
	std::string Value;			// Info; Choice uses Choices[nChoice]
	std::string Help;
	TApply Apply = TApply::Now;
	int nId = 0;				// for the caller: which action / setting
	std::vector<std::string> Choices;
	unsigned nChoice = 0;
	bool bModified = false;			// differs from the file: shown with '*'
};

struct TPage
{
	std::string Title;
	std::string Help;			// shown when nothing is selected
	std::vector<TItem> Items;
};

enum class TKey
{
	Up, Down, Left, Right, PageUp, PageDown, Home, End,
	Enter, Escape, Plus, Minus, F1, F9, F10
};

enum class TEventKind { None, Close, Action, Changed, SaveAndExit, Defaults, Dialog };

struct TEvent
{
	TEventKind Kind = TEventKind::None;
	int nId = 0;			// Action / Changed / Dialog: the item's or dialog's id
	unsigned nChoice = 0;		// Changed: the new choice; Dialog: the option
	bool bCancelled = false;	// Dialog: closed with Esc
};

// A question in a box over the page, as a BIOS asks "Save to CMOS and EXIT
// (Y/N)?".  While one is open it gets every key.
struct TDialog
{
	int nId = 0;
	std::string Title;
	std::string Text;
	std::vector<std::string> Options;
	unsigned nSel = 0;
};

struct TTexts
{
	std::string Title = "BM-DOS Setup Utility";
	std::string Version;
	std::string KeysLine1;
	std::string KeysLine2;
	std::string ApplyLegend;	// at the foot of the help pane
};

class CMenu
{
public:
	static constexpr unsigned BodyTop = 2;
	static constexpr unsigned BodyRows = 25;	// rows 2..26
	static constexpr unsigned HelpCol = 54;		// help pane starts here (25 columns)
	static constexpr unsigned LabelCol = 2;
	static constexpr unsigned ValueCol = 26;
	static constexpr unsigned ApplyCol = 51;

	std::vector<TPage> Pages;
	TTexts Texts;

	unsigned CurrentPage (void) const { return m_nPage; }

	void OpenDialog (int nId, const std::string &Title, const std::string &Text,
			 const std::vector<std::string> &Options, unsigned nDefault = 0)
	{
		m_Dialog = TDialog { nId, Title, Text, Options, nDefault < Options.size () ? nDefault : 0 };
		m_bDialog = !Options.empty ();
	}

	bool DialogOpen (void) const { return m_bDialog; }

	/// Selected item of the current page, or -1 when it has none selectable.
	int Selected (void) const
	{
		if (m_nPage >= Pages.size ()) return -1;
		const auto &Items = Pages[m_nPage].Items;
		if (m_nSel < Items.size () && Selectable (Items[m_nSel])) return static_cast<int> (m_nSel);
		return -1;
	}

	/// Pages were rebuilt (new values): keep the page and the selection where
	/// they still make sense.
	void Refresh (void)
	{
		if (m_nPage >= Pages.size ()) m_nPage = 0;
		FixSelection (+1);
	}

	TEvent Key (TKey Key)
	{
		TEvent Ev;

		if (m_bDialog)
		{
			const unsigned n = static_cast<unsigned> (m_Dialog.Options.size ());
			switch (Key)
			{
			case TKey::Left: case TKey::Up: case TKey::Minus:
				m_Dialog.nSel = (m_Dialog.nSel + n - 1) % n; break;
			case TKey::Right: case TKey::Down: case TKey::Plus:
				m_Dialog.nSel = (m_Dialog.nSel + 1) % n; break;
			case TKey::Enter:
				m_bDialog = false;
				Ev.Kind = TEventKind::Dialog; Ev.nId = m_Dialog.nId; Ev.nChoice = m_Dialog.nSel;
				break;
			case TKey::Escape:
				m_bDialog = false;
				Ev.Kind = TEventKind::Dialog; Ev.nId = m_Dialog.nId; Ev.bCancelled = true;
				break;
			default:
				break;
			}
			return Ev;
		}

		if (Pages.empty ()) return Ev;

		switch (Key)
		{
		case TKey::Left:	SetPage ((m_nPage + Pages.size () - 1) % Pages.size ()); break;
		case TKey::Right:	SetPage ((m_nPage + 1) % Pages.size ()); break;
		case TKey::Up:		Move (-1); break;
		case TKey::Down:	Move (+1); break;
		case TKey::PageUp:	for (unsigned i = 0; i < BodyRows / 2; i++) Move (-1); break;
		case TKey::PageDown:	for (unsigned i = 0; i < BodyRows / 2; i++) Move (+1); break;
		case TKey::Home:	m_nSel = 0; FixSelection (+1); break;
		case TKey::End:		m_nSel = Count () ? Count () - 1 : 0; FixSelection (-1); break;
		case TKey::Escape:	Ev.Kind = TEventKind::Close; break;
		case TKey::F10:		Ev.Kind = TEventKind::SaveAndExit; break;
		case TKey::F9:		Ev.Kind = TEventKind::Defaults; break;
		case TKey::F1:		break;		// the help pane is always shown

		case TKey::Enter:
		case TKey::Plus:
		case TKey::Minus:
		{
			const int nSel = Selected ();
			if (nSel < 0) break;
			TItem &Item = Pages[m_nPage].Items[nSel];
			if (Item.Kind == TKind::Action && Key == TKey::Enter)
			{
				Ev.Kind = TEventKind::Action;
				Ev.nId = Item.nId;
			}
			else if (Item.Kind == TKind::Choice && !Item.Choices.empty ())
			{
				const unsigned n = static_cast<unsigned> (Item.Choices.size ());
				Item.nChoice = Key == TKey::Minus ? (Item.nChoice + n - 1) % n
								  : (Item.nChoice + 1) % n;
				Ev.Kind = TEventKind::Changed;
				Ev.nId = Item.nId;
				Ev.nChoice = Item.nChoice;
			}
			break;
		}
		}
		return Ev;
	}

	void Render (CScreen &S) const
	{
		// Title bar.
		S.Fill (0, 0, Cols, 1, White, Blue);
		S.Put (1, 0, Texts.Title, White, Blue);
		if (!Texts.Version.empty ())
		{
			const unsigned n = CScreen::Length (Texts.Version);
			S.Put (Cols - 1 - n, 0, Texts.Version, LightCyan, Blue);
		}

		// Tabs, one space apart; the current one is highlighted including
		// the space on each side.  Ten titles fit in 80 columns this way,
		// not with a space on both sides of every tab.
		S.Fill (0, 1, Cols, 1, Black, Cyan);
		unsigned x = 1;
		for (unsigned p = 0; p < Pages.size () && x < Cols; p++)
		{
			const bool bCur = p == m_nPage;
			const unsigned n = CScreen::Length (Pages[p].Title);
			if (bCur)
			{
				S.Fill (x - 1, 1, n + 2, 1, White, Blue);
			}
			S.Put (x, 1, Pages[p].Title, bCur ? White : Black, bCur ? Blue : Cyan, Cols - x);
			x += n + 1;
		}

		// Body and help pane.
		S.Fill (0, BodyTop, Cols, BodyRows, Blue, LightGrey);
		S.VLine (HelpCol - 1, BodyTop, BodyRows);

		if (m_nPage < Pages.size ())
		{
			const auto &Items = Pages[m_nPage].Items;
			const unsigned nFirst = FirstVisible ();
			for (unsigned r = 0; r < BodyRows && nFirst + r < Items.size (); r++)
				RenderItem (S, BodyTop + r, Items[nFirst + r], nFirst + r == m_nSel && Selected () >= 0);

			// Scroll marks when the list is longer than the window.
			if (nFirst > 0) S.Put (HelpCol - 3, BodyTop, "^", DarkGrey, LightGrey);
			if (nFirst + BodyRows < Items.size ()) S.Put (HelpCol - 3, BodyTop + BodyRows - 1, "v", DarkGrey, LightGrey);

			const int nSel = Selected ();
			const std::string &Help = nSel >= 0 && !Items[nSel].Help.empty ()
						  ? Items[nSel].Help : Pages[m_nPage].Help;
			S.Put (HelpCol + 1, BodyTop, nSel >= 0 ? Items[nSel].Label : Pages[m_nPage].Title, Blue, LightGrey, Cols - HelpCol - 2);
			S.HLine (HelpCol, BodyTop + 1, Cols - HelpCol);
			Wrap (S, HelpCol + 1, BodyTop + 2, Cols - HelpCol - 2, BodyRows - 6, Help);

			S.HLine (HelpCol, BodyTop + BodyRows - 4, Cols - HelpCol);
			Wrap (S, HelpCol + 1, BodyTop + BodyRows - 3, Cols - HelpCol - 2, 3, Texts.ApplyLegend);
		}

		// Keys.
		S.Fill (0, BodyTop + BodyRows, Cols, Rows - BodyTop - BodyRows, White, Blue);
		S.Put (1, BodyTop + BodyRows + 1, Texts.KeysLine1, White, Blue);
		S.Put (1, BodyTop + BodyRows + 2, Texts.KeysLine2, White, Blue);

		if (m_bDialog)
		{
			RenderDialog (S);
		}
	}

	/// Word wrap into a box; returns rows used.
	static unsigned Wrap (CScreen &S, unsigned x, unsigned y, unsigned w, unsigned h, const std::string &Text)
	{
		unsigned nRow = 0;
		std::string Line;
		auto Flush = [&] { if (nRow < h) S.Put (x, y + nRow, Line, Black, LightGrey, w); nRow++; Line.clear (); };

		size_t i = 0;
		while (i <= Text.size ())
		{
			if (i == Text.size () || Text[i] == '\n')
			{
				Flush ();
				i++;
				if (i > Text.size ()) break;
				continue;
			}
			// Next word.
			size_t j = i;
			while (j < Text.size () && Text[j] != ' ' && Text[j] != '\n') j++;
			const std::string Word = Text.substr (i, j - i);
			const unsigned nNeed = CScreen::Length (Line) + (Line.empty () ? 0 : 1) + CScreen::Length (Word);
			if (nNeed > w && !Line.empty ()) Flush ();
			if (!Line.empty ()) Line += ' ';
			Line += Word;
			i = j;
			while (i < Text.size () && Text[i] == ' ') i++;
		}
		return nRow;
	}

private:
	unsigned m_nPage = 0;
	unsigned m_nSel = 0;
	TDialog m_Dialog;
	bool m_bDialog = false;

	void RenderDialog (CScreen &S) const
	{
		// Options on one line: "  Guardar    Descartar    Seguir  ".
		std::string OptLine;
		std::vector<unsigned> OptX;
		for (const std::string &O : m_Dialog.Options)
		{
			OptLine += "  ";
			OptX.push_back (CScreen::Length (OptLine));
			OptLine += " " + O + " ";
		}
		const unsigned nOptW = CScreen::Length (OptLine) + 2;

		const unsigned w = std::max<unsigned> (44, std::min<unsigned> (64, nOptW + 4));
		CScreen Probe;
		const unsigned nTextRows = Wrap (Probe, 0, 0, w - 4, Rows, m_Dialog.Text);
		const unsigned h = nTextRows + 6;
		const unsigned x = (Cols - w) / 2;
		const unsigned y = (Rows - h) / 2;

		S.Fill (x + 1, y + 1, w, h, Black, Black);		// shadow
		S.Fill (x, y, w, h, White, Red);
		S.HLine (x, y, w);
		S.HLine (x, y + h - 1, w);
		S.VLine (x, y, h);
		S.VLine (x + w - 1, y, h);

		const std::string Title = " " + m_Dialog.Title + " ";
		S.Put (x + (w - CScreen::Length (Title)) / 2, y, Title, Yellow, Red);

		// The text, wrapped, in the box's colours.
		CScreen T;
		Wrap (T, 0, 0, w - 4, nTextRows, m_Dialog.Text);
		for (unsigned r = 0; r < nTextRows; r++)
			for (unsigned c = 0; c < w - 4; c++)
				if (T.At (c, r).nChar != ' ')
				{
					std::string One;
					CScreen::Encode (One, T.At (c, r).nChar);
					S.Put (x + 2 + c, y + 2 + r, One, White, Red);
				}

		const unsigned ox = x + (w - CScreen::Length (OptLine)) / 2;
		const unsigned oy = y + h - 3;
		for (size_t i = 0; i < m_Dialog.Options.size (); i++)
		{
			const bool bSel = i == m_Dialog.nSel;
			S.Put (ox + OptX[i], oy, " " + m_Dialog.Options[i] + " ",
			       bSel ? Red : White, bSel ? White : Red);
		}
	}

	static bool Selectable (const TItem &Item)
	{
		return Item.Kind == TKind::Action || Item.Kind == TKind::Choice
		       || Item.Kind == TKind::Planned;
	}

	unsigned Count (void) const
	{
		return m_nPage < Pages.size () ? static_cast<unsigned> (Pages[m_nPage].Items.size ()) : 0;
	}

	void SetPage (unsigned nPage)
	{
		m_nPage = nPage;
		m_nSel = 0;
		FixSelection (+1);
	}

	// Move to the next selectable item in a direction; stay if there is none.
	void Move (int nDir)
	{
		const unsigned n = Count ();
		for (unsigned i = m_nSel; ; )
		{
			if (nDir < 0 ? i == 0 : i + 1 >= n) return;
			i = nDir < 0 ? i - 1 : i + 1;
			if (Selectable (Pages[m_nPage].Items[i])) { m_nSel = i; return; }
		}
	}

	// From m_nSel, find a selectable item in nDir, else the other way.
	void FixSelection (int nDir)
	{
		const unsigned n = Count ();
		if (n == 0) { m_nSel = 0; return; }
		if (m_nSel >= n) m_nSel = n - 1;
		if (Selectable (Pages[m_nPage].Items[m_nSel])) return;
		const unsigned nStart = m_nSel;
		Move (nDir);
		if (m_nSel == nStart) Move (-nDir);
	}

	unsigned FirstVisible (void) const
	{
		const unsigned n = Count ();
		if (n <= BodyRows || m_nSel < BodyRows / 2) return 0;
		const unsigned nFirst = m_nSel - BodyRows / 2;
		return nFirst + BodyRows > n ? n - BodyRows : nFirst;
	}

	static const char *ApplyMark (TApply Apply)
	{
		return Apply == TApply::RestartDOSBox ? "R" : Apply == TApply::RestartBoard ? "B" : "";
	}

	void RenderItem (CScreen &S, unsigned y, const TItem &Item, bool bSelected) const
	{
		const unsigned nLabelW = ValueCol - LabelCol - 1;
		switch (Item.Kind)
		{
		case TKind::Section:
			S.Put (LabelCol, y, Item.Label, Blue, LightGrey, HelpCol - 2 - LabelCol);
			return;

		case TKind::Info:
			S.Put (LabelCol, y, Item.Label, Black, LightGrey, nLabelW);
			S.Put (ValueCol, y, Item.Value, Blue, LightGrey, ApplyCol - ValueCol - 1);
			return;

		case TKind::Planned:
		{
			const std::uint8_t nBg = bSelected ? DarkGrey : LightGrey;
			const std::uint8_t nFg = bSelected ? LightGrey : DarkGrey;
			if (bSelected) S.Fill (LabelCol - 1, y, ApplyCol - LabelCol + 2, 1, nFg, nBg);
			S.Put (LabelCol, y, Item.Label, nFg, nBg, nLabelW);
			S.Put (ValueCol, y, Item.Value, nFg, nBg, ApplyCol - ValueCol - 1);
			S.Put (ApplyCol, y, ApplyMark (Item.Apply), nFg, nBg);
			return;
		}

		case TKind::Action:
		case TKind::Choice:
		{
			const std::uint8_t nFg = bSelected ? White : Black;
			const std::uint8_t nBg = bSelected ? Blue : LightGrey;
			if (bSelected) S.Fill (LabelCol - 1, y, ApplyCol - LabelCol + 2, 1, nFg, nBg);
			S.Put (LabelCol, y, Item.Label, nFg, nBg, nLabelW);

			std::string Value = Item.Kind == TKind::Choice && Item.nChoice < Item.Choices.size ()
					    ? "[" + Item.Choices[Item.nChoice] + "]" : Item.Value;
			S.Put (ValueCol, y, Value, bSelected ? Yellow : Blue, nBg, ApplyCol - ValueCol - 1);

			S.Put (ApplyCol, y, ApplyMark (Item.Apply), bSelected ? LightCyan : DarkGrey, nBg);
			if (Item.bModified)
			{
				S.Put (ApplyCol + 1, y, "*", bSelected ? Yellow : Red, nBg);
			}
			return;
		}
		}
	}
};

} // namespace baremenu

#endif
