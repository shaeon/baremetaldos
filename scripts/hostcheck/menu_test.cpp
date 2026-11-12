//
// menu_test.cpp - host test for src/platform/menu_model.h (make check-menu)
//
// Shaeon (Carlos Palmero) 2026, for BM-DOS by @mmoroca.  GPL-3.0-or-later.
//
#include "../../src/platform/menu_model.h"

#include <cstdio>

using namespace baremenu;

static unsigned s_nFailures = 0;
static void Check (bool b, const char *p)
{
	std::printf ("  %s  %s\n", b ? "ok    " : "FAILED", p);
	if (!b) s_nFailures++;
}

static TItem Info (const char *l, const char *v) { TItem i; i.Kind = TKind::Info; i.Label = l; i.Value = v; return i; }
static TItem Section (const char *l) { TItem i; i.Kind = TKind::Section; i.Label = l; return i; }
static TItem Action (const char *l, int id) { TItem i; i.Kind = TKind::Action; i.Label = l; i.nId = id; i.Help = "Ayuda de la acción"; return i; }
static TItem Choice (const char *l, int id, std::vector<std::string> c, TApply a = TApply::Now)
{ TItem i; i.Kind = TKind::Choice; i.Label = l; i.nId = id; i.Choices = c; i.Apply = a; return i; }
static TItem Planned (const char *l) { TItem i; i.Kind = TKind::Planned; i.Label = l; i.Value = "próximamente"; return i; }

static bool Contains (const std::string &s, const char *p) { return s.find (p) != std::string::npos; }

int main (void)
{
	CMenu M;
	M.Texts.Version = "2026-11-11";
	M.Texts.KeysLine1 = "Flechas: elegir  Intro: cambiar  Esc: volver al juego";
	M.Texts.ApplyLegend = "R: al reiniciar DOSBox  B: al reiniciar la placa";

	TPage Sistema; Sistema.Title = "Sistema"; Sistema.Help = "Información de la placa.";
	Sistema.Items = { Section ("Placa"), Info ("Modelo", "Raspberry Pi 4B"), Info ("Reloj", "1500 MHz") };

	TPage Video; Video.Title = "Vídeo";
	Video.Items = { Section ("Pantalla"), Choice ("Núcleo de vídeo", 10, {"Núcleo 2", "Núcleo 0"}, TApply::RestartBoard),
			Info ("Resolución", "1280x720"), Choice ("Escalado", 11, {"Nítido", "Suave"}),
			Planned ("Corrección de aspecto") };

	TPage Salir; Salir.Title = "Salir";
	Salir.Items = { Action ("Volver al juego", 1), Action ("Reiniciar la placa", 2) };

	M.Pages = { Sistema, Video, Salir };
	M.Refresh ();

	std::printf ("navigation\n");
	Check (M.CurrentPage () == 0 && M.Selected () < 0, "a page of information has nothing selected");
	M.Key (TKey::Right);
	Check (M.CurrentPage () == 1 && M.Selected () == 1, "Right goes to the next tab and selects its first option");
	M.Key (TKey::Down);
	Check (M.Selected () == 3, "Down skips information lines");
	M.Key (TKey::Down);
	Check (M.Selected () == 4, "the cursor stops on a planned option, to show its help");
	M.Key (TKey::Down);
	Check (M.Selected () == 4, "Down at the last option stays there");
	M.Key (TKey::Home);
	Check (M.Selected () == 1, "Home goes to the first option");
	M.Key (TKey::Left); M.Key (TKey::Left);
	Check (M.CurrentPage () == 2, "Left from the first tab wraps to the last");

	std::printf ("events\n");
	TEvent E = M.Key (TKey::Enter);
	Check (E.Kind == TEventKind::Action && E.nId == 1, "Enter on an action reports it");
	E = M.Key (TKey::Escape);
	Check (E.Kind == TEventKind::Close, "Esc closes");
	M.Key (TKey::Right); M.Key (TKey::Right);		// Vídeo
	E = M.Key (TKey::Plus);
	Check (E.Kind == TEventKind::Changed && E.nId == 10 && E.nChoice == 1, "+ on a choice moves to the next value");
	E = M.Key (TKey::Plus);
	Check (E.nChoice == 0, "choices wrap around");
	E = M.Key (TKey::Minus);
	Check (E.nChoice == 1, "- goes the other way, wrapping too");
	M.Key (TKey::End);
	E = M.Key (TKey::Enter);
	Check (E.Kind == TEventKind::None, "a planned option cannot be changed");
	Check (M.Key (TKey::F10).Kind == TEventKind::SaveAndExit && M.Key (TKey::F9).Kind == TEventKind::Defaults,
	       "F10 saves and exits, F9 asks for the defaults");

	std::printf ("layout\n");
	M.Key (TKey::Home);
	CScreen S;
	M.Render (S);
	Check (Contains (S.RowText (0), "BM-DOS Setup Utility") && Contains (S.RowText (0), "2026-11-11"),
	       "title bar with the version on the right");
	Check (Contains (S.RowText (1), "Sistema") && Contains (S.RowText (1), "Vídeo") && Contains (S.RowText (1), "Salir"),
	       "all tabs on the second row, accents kept");
	{
		// The current tab is highlighted: find the 'V' of Vídeo.
		const std::string r = S.RowText (1);
		const unsigned x = CScreen::Length (r.substr (0, r.find ("Vídeo")));
		Check (S.At (x, 1).nBg == Blue && S.At (x, 1).nFg == White, "the current tab is white on blue");
	}
	const unsigned yChoice = CMenu::BodyTop + 1;
	Check (Contains (S.RowText (yChoice), "Núcleo de vídeo") && Contains (S.RowText (yChoice), "[Núcleo 0]"),
	       "a choice shows its value in brackets");
	Check (S.At (CMenu::LabelCol, yChoice).nBg == Blue, "the selected option is highlighted");
	Check (S.RowText (yChoice).find ('B') != std::string::npos && S.At (CMenu::ApplyCol, yChoice).nChar == 'B',
	       "a setting that needs a board restart is marked B");
	Check (S.At (CMenu::HelpCol - 1, CMenu::BodyTop + 5).nLine & LineV, "the help pane is separated by a line");
	Check (Contains (S.RowText (CMenu::BodyTop), "Núcleo de vídeo"), "the help pane titles the selected option");
	Check (Contains (S.RowText (Rows - 2), "Esc: volver al juego"), "the keys are on the bottom rows");

	{
		// The real set of tabs, longest titles included, must all fit.
		CMenu T;
		for (const char *p : { "Sistema", "Máquina", "Unidades", "Sonido", "Vídeo", "Entrada",
				       "Rendimiento", "Red", "Arranque", "Salir" })
		{
			TPage P; P.Title = p; T.Pages.push_back (P);
		}
		T.Refresh ();
		for (int i = 0; i < 9; i++) T.Key (TKey::Right);
		CScreen U; T.Render (U);
		const std::string r = U.RowText (1);
		const unsigned x = CScreen::Length (r.substr (0, r.find ("Salir")));
		Check (Contains (r, "Salir") && x + 5 < Cols && U.At (x + 5, 1).nBg == Blue,
		       "all ten tabs fit in 80 columns, the last one highlighted with its margin");
		Check (CScreen::Length (T.Texts.Title) < Cols, "title fits");
	}

	std::printf ("dialog\n");
	{
		CMenu D;
		TPage P; P.Title = "Salir"; P.Items = { Action ("Volver al juego", 1) };
		D.Pages = { P }; D.Refresh ();
		D.OpenDialog (7, "Cambios sin guardar", "¿Guardar los cambios antes de volver al juego?",
			      { "Guardar", "Descartar", "Seguir" });
		Check (D.DialogOpen (), "a dialog opens");
		Check (D.Key (TKey::Down).Kind == TEventKind::None, "arrow keys move inside the dialog, no event");
		TEvent Ev = D.Key (TKey::Enter);
		Check (Ev.Kind == TEventKind::Dialog && Ev.nId == 7 && Ev.nChoice == 1 && !Ev.bCancelled && !D.DialogOpen (),
		       "Enter answers with the option chosen and closes it");
		D.OpenDialog (8, "T", "x", { "Sí", "No" }, 1);
		Ev = D.Key (TKey::Escape);
		Check (Ev.Kind == TEventKind::Dialog && Ev.nId == 8 && Ev.bCancelled, "Esc cancels the dialog, not the menu");
		D.OpenDialog (9, "Cambios sin guardar", "¿Guardar?", { "Guardar", "Descartar" });
		CScreen T; D.Render (T);
		bool bTitle = false, bOpt = false;
		for (unsigned y = 0; y < Rows; y++)
		{
			bTitle = bTitle || Contains (T.RowText (y), " Cambios sin guardar ");
			bOpt = bOpt || (Contains (T.RowText (y), " Guardar ") && Contains (T.RowText (y), " Descartar "));
		}
		Check (bTitle && bOpt, "the dialog is drawn over the page with its title and options");
	}

	std::printf ("text input\n");
	{
		CMenu T;
		TPage P; P.Title = "Red"; P.Items = { Action ("Elegir red", 1) };
		T.Pages = { P }; T.Refresh ();
		T.OpenInput (5, "Contraseña", "Contraseña de \"Casa\":", "", true, 8, 63);
		Check (T.InputOpen () && T.DialogOpen (), "an input box opens and takes the keys");
		for (char c : std::string ("clave1")) T.Char (static_cast<unsigned char> (c));
		Check (T.Key (TKey::Enter).Kind == TEventKind::None && T.InputOpen (),
		       "Enter does nothing below the minimum length (8 for a WPA password)");
		T.Char (0xF1);			// ñ
		T.Char ('Z');
		T.Key (TKey::Backspace);	// takes 'Z' out
		T.Char ('9');
		CScreen U; T.Render (U);
		bool bStars = false;
		for (unsigned y = 0; y < Rows; y++) bStars = bStars || Contains (U.RowText (y), "********_");
		Check (bStars, "a hidden field shows one '*' per character");
		T.Key (TKey::Tab);
		CScreen V; T.Render (V);
		bool bClear = false;
		for (unsigned y = 0; y < Rows; y++) bClear = bClear || Contains (V.RowText (y), "clave1ñ9_");
		Check (bClear, "Tab shows it; Backspace removed one character, a two-byte one counts as one");
		TEvent Ev = T.Key (TKey::Enter);
		Check (Ev.Kind == TEventKind::Input && Ev.nId == 5 && Ev.Text == "clave1ñ9" && !T.InputOpen (),
		       "Enter returns the text");
		T.OpenInput (6, "Red", "Nombre:", "Ca", false, 1, 4);
		for (char c : std::string ("sa123")) T.Char (static_cast<unsigned char> (c));
		Ev = T.Key (TKey::Enter);
		Check (Ev.Text == "Casa", "no more than the maximum length goes in");
		T.OpenInput (7, "Red", "Nombre:", "x", false, 1, 32);
		Ev = T.Key (TKey::Escape);
		Check (Ev.Kind == TEventKind::Input && Ev.bCancelled, "Esc cancels the input");
	}

	std::printf ("long pages\n");
	{
		CMenu L;
		TPage P; P.Title = "Larga";
		P.Items.push_back (Choice ("Modo", 1, {"a", "b"}));
		for (int i = 0; i < 40; i++) P.Items.push_back (Info (("Dato " + std::to_string (i)).c_str (), "x"));
		L.Pages = { P }; L.Refresh ();
		for (int i = 0; i < 39; i++) L.Key (TKey::Down);
		CScreen T; L.Render (T);
		bool bLast = false;
		for (unsigned y = CMenu::BodyTop; y < CMenu::BodyTop + CMenu::BodyRows; y++)
			bLast = bLast || Contains (T.RowText (y), "Dato 38 ");
		Check (L.Selected () == 39 && bLast,
		       "on a page longer than the screen the cursor walks the information lines, so they scroll into view");
		TPage Short; Short.Title = "Corta"; Short.Items = { Choice ("Modo", 1, {"a"}), Info ("Dato", "x") };
		CMenu S2; S2.Pages = { Short }; S2.Refresh (); S2.Key (TKey::Down);
		Check (S2.Selected () == 0, "on a page that fits, information lines are still skipped");
	}

	std::printf ("long action names\n");
	{
		CMenu L;
		TPage P; P.Title = "Salir";
		P.Items = { Action ("Guardar y reiniciar la placa", 1), Planned ("Actualizar desde GitHub") };
		L.Pages = { P }; L.Refresh ();
		CScreen T; L.Render (T);
		Check (Contains (T.RowText (CMenu::BodyTop), "Guardar y reiniciar la placa"),
		       "an action with no value is DRAWN whole, across the value column");
		Check (Contains (T.RowText (CMenu::BodyTop + 1), "Actualizar desde GitHub")
		       && T.At (CMenu::ValueCol, CMenu::BodyTop + 1).nChar == 'p',
		       "a planned option keeps its name to its column, its value in its own");
	}

	std::printf ("scrolling and wrapping\n");
	{
		CMenu L;
		TPage P; P.Title = "Larga";
		for (int i = 0; i < 60; i++) P.Items.push_back (Choice (("Opción " + std::to_string (i)).c_str (), i, {"a", "b"}));
		L.Pages = { P }; L.Refresh ();
		for (int i = 0; i < 40; i++) L.Key (TKey::Down);
		CScreen T; L.Render (T);
		bool bVisible = false;
		for (unsigned y = CMenu::BodyTop; y < CMenu::BodyTop + CMenu::BodyRows; y++)
			bVisible = bVisible || Contains (T.RowText (y), "Opción 40 ");
		Check (L.Selected () == 40 && bVisible, "a long page scrolls to keep the selection visible");
	}
	{
		CScreen T;
		const unsigned n = CMenu::Wrap (T, 0, 0, 10, 5, "una frase bastante larga para partir");
		Check (n >= 3 && CScreen::Length (T.RowText (0).substr (0, 10)) == 10, "help text wraps by words inside its box");
	}

	std::printf ("\n%u failures\n", s_nFailures);
	return s_nFailures == 0 ? 0 : 1;
}
