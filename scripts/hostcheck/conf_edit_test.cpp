//
// conf_edit_test.cpp - host test for src/platform/conf_edit.h (make check-conf)
//
// Shaeon (Carlos Palmero) 2026, for BM-DOS by @mmoroca.  GPL-3.0-or-later.
//
#include "../../src/platform/conf_edit.h"

#include <cstdio>
#include <fstream>
#include <sstream>

using namespace bareconf;

static unsigned s_nFailures = 0;
static void Check (bool b, const char *p)
{
	std::printf ("  %s  %s\n", b ? "ok    " : "FAILED", p);
	if (!b) s_nFailures++;
}

static std::string ReadFile (const char *p)
{
	std::ifstream f (p, std::ios::binary);
	std::stringstream s; s << f.rdbuf ();
	return s.str ();
}

int main (int argc, char **argv)
{
	const std::string Dir = argc > 1 ? argv[1] : "sd";

	std::printf ("boot.conf\n");
	{
		const std::string Orig =
			"# a comment\n"
			"display = 1280x720\n"
			"\n"
			"vsync = on   # wait for it\n"
			"#video_core = 0\n"
			"video_core = 2\n";
		CConfText C (Orig, true);
		std::string v;
		Check (C.Get ("", "vsync", &v) && v == "on", "reads a value, inline comment cut");
		Check (C.Get ("", "video_core", &v) && v == "2", "a commented-out line is not a value");
		Check (C.Text () == Orig, "unchanged text comes back byte for byte");

		C.Set ("", "vsync", "off");
		C.Set ("", "video_core", "0");
		C.Set ("", "scaler", "smooth");
		const std::string New = C.Text ();
		Check (New.find ("vsync = off   # wait for it\n") != std::string::npos,
		       "a value changes in place, its comment and spacing kept");
		Check (New.find ("#video_core = 0\nvideo_core = 0\n") != std::string::npos,
		       "the live line changes, the commented one stays as it was");
		Check (New.size () > 4 && New.substr (New.size () - 16) == "scaler = smooth\n",
		       "a missing key is added at the end, in the file's spacing");
		Check (New.find ("# a comment\n") == 0, "comments survive");
	}

	std::printf ("dosbox.conf\n");
	{
		const std::string Orig =
			"[sdl]\r\n"
			"output=texture\r\n"
			"\r\n"
			"[dosbox]\r\n"
			"# machine: hercules, cga, ...\r\n"
			"machine=svga_s3\r\n"
			"memsize=16\r\n"
			"\r\n"
			"[autoexec]\r\n"
			"mount c /dos # not a comment for DOSBox\r\n";
		CConfText C (Orig, false);
		std::string v;
		Check (C.Get ("dosbox", "MemSize", &v) && v == "16", "keys and sections are case-insensitive");
		Check (C.Text () == Orig, "CRLF line ends come back as CRLF");

		C.Set ("dosbox", "memsize", "32");
		C.Set ("dosbox", "cputype", "pentium_slow");
		C.Set ("midi", "mididevice", "mt32");
		const std::string New = C.Text ();
		Check (New.find ("memsize=32\r\ncputype=pentium_slow\r\n") != std::string::npos,
		       "a new key goes after the last key of its section, in key=value style");
		Check (New.find ("\r\n[midi]\r\nmididevice=mt32\r\n") != std::string::npos,
		       "a missing section is created at the end");
		Check (New.find ("mount c /dos # not a comment for DOSBox\r\n") != std::string::npos,
		       "an autoexec line with '#' is untouched");
		CConfText D (New, false);
		Check (D.Get ("midi", "mididevice", &v) && v == "mt32" && D.Get ("dosbox", "machine", &v) && v == "svga_s3",
		       "the result reads back");
		Check (New.find ('\n') == New.find ("\r\n") + 1, "added lines use the file's CRLF");
	}

	std::printf ("cmdline.txt\n");
	{
		const std::string Orig = "loglevel=4 logdev=tty1 keymap=es width=1280 height=720 socmaxtemp=80\n";
		std::string v;
		Check (CmdlineGet (Orig, "width", &v) && v == "1280", "reads a word");
		Check (!CmdlineGet (Orig, "idth", &v), "matches whole keys only");
		std::string New = CmdlineSet (Orig, "width", "1920");
		New = CmdlineSet (New, "height", "1080");
		Check (New == "loglevel=4 logdev=tty1 keymap=es width=1920 height=1080 socmaxtemp=80\n",
		       "words change in place, one line, newline kept");
		New = CmdlineSet ("loglevel=4\r\n", "width", "800");
		Check (New == "loglevel=4 width=800\r\n", "a missing word is added before the line end");
	}

	std::printf ("the files on the card\n");
	{
		const std::string Boot = ReadFile ((Dir + "/boot.conf").c_str ());
		CConfText B (Boot, true);
		std::string v;
		Check (!Boot.empty () && B.Text () == Boot, "sd/boot.conf round-trips unchanged");
		Check (B.Get ("", "display", &v) && v == "1280x720", "sd/boot.conf: display read");
		const std::string Dos = ReadFile ((Dir + "/dosbox.conf").c_str ());
		CConfText D (Dos, false);
		Check (!Dos.empty () && D.Text () == Dos, "sd/dosbox.conf round-trips unchanged");
		Check (D.Get ("dosbox", "memsize", &v), "sd/dosbox.conf: [dosbox] memsize read");
	}

	std::printf ("\n%u failures\n", s_nFailures);
	return s_nFailures == 0 ? 0 : 1;
}
