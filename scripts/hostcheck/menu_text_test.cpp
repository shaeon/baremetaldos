//
// menu_text_test.cpp - every text of the setup menu fits where it is drawn
// (make check-menu-text).
//
// Builds every page with the REAL bare_menu.cpp - the normal tabs, the WiFi
// list, the card browser with long names - over the board-side functions in
// menu_text_stubs.cpp, which return the longest plausible values (a Pi 400, a
// 32-character WiFi name, long MIDI and network states).  Then measures each
// label, value, choice and help text against its place on the 80x30 screen:
// labels 23, values 24 (choices with their brackets), sections 50, help
// within its pane.  Written because texts were being cut on the board.
//
// Shaeon (Carlos Palmero) 2026, for BM-DOS by @mmoroca.  GPL-3.0-or-later.
//
#include "../../src/platform/bare_menu.cpp"
#include <cstdio>
#include <map>
#include <sys/stat.h>
#include <unistd.h>

int main ()
{
	using namespace baremenu;
	unsigned nBad = 0;

	// The settings are read from "SD:/...", which on the PC is a folder named
	// "SD:" in the current one: run in build/, with the files a card has.
	if (chdir ("build") != 0) { std::printf ("no build/\n"); return 1; }
	mkdir ("SD:", 0755);
	if (FILE *f = fopen ("SD:/wpa_supplicant.conf", "w"))
	{
		fputs ("# Template: write your country as country=US, for example.\n"
		       "country=PT\n\nnetwork={\n\tssid=\"Casa\"\n\tpsk=\"x\"\n}\n", f);
		fclose (f);
	}
	SetupTexts ();
	DefineSettings ();
	LoadSettings ();
	{
		const TSetting *pC = FindSetting (SetWiFiCountry);
		const bool bOk = pC->Choices[pC->nSaved].Value == "PT";
		std::printf ("  %s  the WiFi country is read from wpa_supplicant.conf's active line (PT), not a comment\n",
			     bOk ? "ok    " : "FAILED");
		if (!bOk) nBad++;
	}
	auto Report = [&] (const std::string &Where, const std::string &What, unsigned n, unsigned nMax) {
		std::printf ("  NO CABE  %-40s %3u/%-3u  %s\n", Where.c_str (), n, nMax, What.c_str ());
		nBad++;
	};
	const unsigned nLabelW = CMenu::ValueCol - CMenu::LabelCol - 1;
	const unsigned nValueW = CMenu::ApplyCol - CMenu::ValueCol - 1;
	const unsigned nSectionW = CMenu::HelpCol - 2 - CMenu::LabelCol;
	const unsigned nHelpW = Cols - CMenu::HelpCol - 2;
	const unsigned nHelpRows = CMenu::BodyRows - 7;
	auto CheckAll = [&] () {
	CScreen Probe;
	for (const TPage &P : s_Menu.Pages)
	{
		if (s_Menu.Pages.size () > 1 && CScreen::Length (P.Title) > 20) Report (P.Title, "title", CScreen::Length (P.Title), 20);
		const unsigned nPH = CMenu::Wrap (Probe, 0, 0, nHelpW, 100, P.Help);
		if (nPH > nHelpRows) Report (P.Title + " / ayuda de la página", "", nPH, nHelpRows);
		if (P.Items.size () > CMenu::BodyRows) std::printf ("  (desplaza) %s: %zu líneas\n", P.Title.c_str (), P.Items.size ());
		for (const TItem &I : P.Items)
		{
			const std::string W = P.Title + " / " + I.Label;
			if (I.Kind == TKind::Section)
			{
				if (CScreen::Length (I.Label) > nSectionW) Report (W, "sección", CScreen::Length (I.Label), nSectionW);
				continue;
			}
			if (CScreen::Length (I.Label) > nLabelW) Report (W, "nombre", CScreen::Length (I.Label), nLabelW);
			if (I.Kind == TKind::Choice)
				for (const std::string &C : I.Choices)
				{
					const unsigned n = CScreen::Length (C) + 2;
					if (n > nValueW) Report (W, "[" + C + "]", n, nValueW);
				}
			else if (CScreen::Length (I.Value) > nValueW) Report (W, "valor: " + I.Value, CScreen::Length (I.Value), nValueW);
			const unsigned nH = CMenu::Wrap (Probe, 0, 0, nHelpW, 100, I.Help);
			if (nH > nHelpRows) Report (W, "ayuda", nH, nHelpRows);
		}
	}
	};
	BuildPages (); CheckAll ();
	s_bWiFiList = true; BuildPages (); CheckAll (); s_bWiFiList = false;
	s_Browse = TBrowse::CD; s_BrowsePath = "/dos/JUEGOS_DE_AVENTURA_GRAFICA"; s_BrowseEntries = { {"Monkey Island 2 - LeChuck's Revenge", true}, {"MONKEY2.CUE", false} }; BuildPages (); CheckAll ();
	if (CScreen::Length (s_Menu.Texts.KeysLine1) > Cols - 2) Report ("teclas 1", s_Menu.Texts.KeysLine1, CScreen::Length (s_Menu.Texts.KeysLine1), Cols - 2);
	std::printf ("\n%u fallos (textos que no caben o lecturas mal)\n", nBad);
	return nBad == 0 ? 0 : 1;
}
