//
// bootconf_test.cpp
//
// BM-DOS - the boot.conf parser, on the host, with the files people actually
// write.  Not a unit test framework: a table, so that adding a case is one line
// and reading the output tells you what happened.
//
// Two reasons this exists:
//
//   * `make check-platform` cannot compile kernel.cpp - it pulls in
//     circle_stdlib_app.h and the whole kernel - and kernel.cpp is where the
//     port's only user-editable file is parsed.  Untestable code that runs
//     before the screen exists is how a card becomes unbootable from a text
//     editor.  So the parser lives in src/platform/boot_config.h, which needs
//     nothing from Circle, and this file compiles that header directly.
//
//   * The first version of this parser ignored what it could not read.  A
//     misspelt key, an out-of-range mode and a file that is not there all
//     produced the same result: the default, silently.  This table is the
//     counterweight - every row says what the file contains and what the port
//     must end up doing with it.
//
// It writes its fixture files next to itself (under build/ in the Makefile
// target) because BOOT_CONFIG is a fixed path and the parser has no parameter
// for it: the same fixed path the real code uses, which is the point.
//
//   make check-bootconf
//
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>

#include "../../src/platform/boot_config.h"

struct TCase
{
	const char *pName;
	const char *pContents;		// nullptr = do not create the file at all
	unsigned    nWantWidth;
	unsigned    nWantHeight;
	bool        bWantHave;
	unsigned    nWantMaxMHz;
	const char *pWantMode;
	const char *pWantBootLog;		// .73: auto | quiet | off
	const char *pWantWarningPart;	// nullptr = no warning expected
};

static const TCase Cases[] =
{
	// The file that ships with the port has to parse to the documented values.
	{ "el que se entrega",
	  "display = 1280x720     # 1920x1080 | 1280x720 | ...\n"
	  "cpu_max_mhz = 0        # techo del governor\n"
	  "mode = auto\n",
	  1280, 720, true, 0, "auto", "auto", nullptr },

	{ "resolucion menor y techo",
	  "display = 800x600\ncpu_max_mhz = 1500\n",
	  800, 600, true, 1500, "auto", "auto", nullptr },

	{ "sin espacios y con comentario",
	  "display=1024x768   # asi tambien\n",
	  1024, 768, true, 0, "auto", "auto", nullptr },

	{ "X mayuscula",
	  "display = 800X600\n",
	  800, 600, true, 0, "auto", "auto", nullptr },

	{ "CRLF de un editor de Windows",
	  "display = 640x480\r\n",
	  640, 480, true, 0, "auto", "auto", nullptr },

	// No file is not an error: defaults, and nothing to say about it.
	{ "fichero ausente", nullptr,
	  1280, 720, false, 0, "auto", "auto", nullptr },

	{ "solo comentarios y lineas vacias",
	  "# nada\n\n   \n",
	  1280, 720, false, 0, "auto", "auto", nullptr },

	// Everything below is a mistake, and every one of them has to be visible.
	{ "modo fuera de rango",
	  "display = 4000x2000\n",
	  1280, 720, false, 0, "auto", "auto", "4000x2000 out of range" },

	{ "modo ilegible",
	  "display = grande\n",
	  1280, 720, false, 0, "auto", "auto", "as WxH" },

	{ "clave mal escrita",
	  "resolucion = 800x600\n",
	  1280, 720, false, 0, "auto", "auto", "unknown key" },

	{ "frecuencia absurda, pero el modo se respeta",
	  "cpu_max_mhz = 9999\ndisplay = 640x480\n",
	  640, 480, true, 0, "auto", "auto", "out of range" },

	{ "mode que existe pero no se aplica",
	  "mode = performance\n",
	  1280, 720, false, 0, "performance", "auto", "not applied yet" },

	{ "mode inventado",
	  "mode = turbo\n",
	  1280, 720, false, 0, "auto", "auto", "unknown mode" },

	// vga_hz is no longer a key of this file (.65g removed it: it could not make
	// a slow machine faster, it only made the emulated machine slower, and it
	// carried a constant, a key and a panel star for that).  The promise that
	// remains is the one that made the parser safe in the first place: a card
	// with the old line in it still boots, and the log says why the line does
	// nothing instead of pretending it worked.
	{ "vga_hz = 60 ya no es una clave",
	  "vga_hz = 60\n",
	  1280, 720, false, 0, "auto", "auto", "unknown key" },

	{ "una linea vieja no rompe lo demas",
	  "vga_hz = 60\ndisplay = 640x480\ncpu_max_mhz = 1500\n",
	  640, 480, true, 1500, "auto", "auto", "unknown key" },


	// .73: boot_log.  "auto" is the default and the one a card being debugged
	// wants; "quiet" exists because the core's own config dump (forty lines of
	// "Variable dosbox::machine updated to ...") is what fills the screen at
	// boot, and it is worth nothing after the first time.
	{ "boot_log = quiet",
	  "boot_log = quiet\n",
	  1280, 720, false, 0, "auto", "quiet", nullptr },

	{ "boot_log = off con display",
	  "boot_log = off\ndisplay = 800x600\n",
	  800, 600, true, 0, "auto", "off", nullptr },

	{ "boot_log inventado",
	  "boot_log = silent\n",
	  1280, 720, false, 0, "auto", "auto", "unknown boot_log" },

	// The first complaint wins: the buffer is one line, and the first mistake
	// is the one to fix.
	{ "dos errores: manda el primero",
	  "display = 99999x99999\nresolucion = 800x600\n",
	  1280, 720, false, 0, "auto", "auto", "out of range" },};

static unsigned Errors = 0;

// video_core / vsync (the core 2 presenter), in a table of their own so that
// the cases above do not all have to spell out two more defaults.
struct TVideoCase
{
	const char *pName;
	const char *pContents;
	unsigned    nWantCore;
	bool        bWantVsync;
	const char *pWantWarningPart;	// nullptr = no warning expected
};

static const TVideoCase VideoCases[] =
{
	{ "video: sin claves, por defecto 2 + vsync", "display = 1280x720\n", 2, true, nullptr },
	{ "video: core 0", "video_core = 0\n", 0, true, nullptr },
	{ "video: core 2 sin vsync", "video_core = 2\nvsync = off\n", 2, false, nullptr },
	{ "video: valor invalido conserva el defecto", "video_core = 1\n", 2, true, "video_core" },
	{ "video: vsync invalido conserva el defecto", "vsync = maybe\n", 2, true, "vsync" },
};

struct TScaleCase { const char *pName; const char *pContents; bool bWantSharp; const char *pWantWarningPart; };
static const TScaleCase ScaleCases[] =
{
	{ "scaler: por defecto sharp", "display = 1280x720\n", true, nullptr },
	{ "scaler: smooth", "scaler = smooth\n", false, nullptr },
	{ "scaler: sharp", "scaler = sharp\n", true, nullptr },
	{ "scaler: invalido conserva sharp", "scaler = blurry\n", true, "scaler" },
};

static void Check (bool bOk, const char *pName, const char *pWhat,
		   const char *pGot = nullptr, const char *pWant = nullptr)
{
	if (bOk)
	{
		printf ("  ok   %-38s %s\n", pName, pWhat);
		return;
	}

	Errors++;
	printf ("  FALLA %-38s %s", pName, pWhat);
	if (pGot != nullptr)
	{
		printf (" (leido \"%s\", esperado \"%s\")", pGot,
			pWant != nullptr ? pWant : "");
	}
	printf ("\n");
}

int main (void)
{
	// The parser opens BOOT_CONFIG, which is the path the card uses.  Here that
	// is a relative path, so the working directory has to be the fixture
	// directory: the Makefile target does exactly that.
	remove (BOOT_CONFIG);

	printf ("boot.conf: %u casos\n", (unsigned) (sizeof Cases / sizeof Cases[0]));

	for (unsigned i = 0; i < sizeof Cases / sizeof Cases[0]; i++)
	{
		const TCase *pCase = &Cases[i];

		remove (BOOT_CONFIG);
		if (pCase->pContents != nullptr)
		{
			FILE *pFile = fopen (BOOT_CONFIG, "w");
			if (pFile == nullptr)
			{
				printf ("  FALLA %-38s no puedo escribir %s\n",
					pCase->pName, BOOT_CONFIG);
				Errors++;
				continue;
			}
			fputs (pCase->pContents, pFile);
			fclose (pFile);
		}

		TBootConfig Config;
		ReadBootConfig (&Config);

		char Buffer[64];
		snprintf (Buffer, sizeof Buffer, "%ux%u", Config.nWidth, Config.nHeight);
		Check (   Config.nWidth == pCase->nWantWidth
		       && Config.nHeight == pCase->nWantHeight,
			pCase->pName, "modo", Buffer, "");

		// .73: boot_log, and this one is checked rather than printed.  The first
		// version of this addition labelled the field and asserted nothing, which
		// a deliberate sabotage proved: replacing the assignment in the parser
		// with (void) 0 left the whole table green.  A test that names a value
		// without comparing it is a comment.
		Check (strcmp (Config.BootLog, pCase->pWantBootLog) == 0, pCase->pName,
			"boot_log", Config.BootLog, pCase->pWantBootLog);

		Check (Config.bHaveDisplay == pCase->bWantHave, pCase->pName,
			pCase->bWantHave ? "boot.conf manda" : "valores por defecto",
			Config.bHaveDisplay ? "si" : "no", pCase->bWantHave ? "si" : "no");

		Check (Config.nCPUMaxMHz == pCase->nWantMaxMHz, pCase->pName,
			"techo del governor");
		Check (strcmp (Config.Mode, pCase->pWantMode) == 0, pCase->pName,
			"mode", Config.Mode, pCase->pWantMode);

		if (pCase->pWantWarningPart != nullptr)
		{
			Check (strstr (Config.Warning, pCase->pWantWarningPart) != nullptr,
				pCase->pName, "aviso", Config.Warning,
				pCase->pWantWarningPart);
		}
		else
		{
			Check (Config.Warning[0] == '\0', pCase->pName,
				"sin aviso", Config.Warning, "");
		}
	}

	for (const TVideoCase &Case : VideoCases)
	{
		FILE *pFile = fopen (BOOT_CONFIG, "w");
		if (pFile == nullptr)
		{
			Errors++;
			continue;
		}
		fputs (Case.pContents, pFile);
		fclose (pFile);

		TBootConfig Config;
		ReadBootConfig (&Config);

		Check (Config.nVideoCore == Case.nWantCore, Case.pName, "video_core");
		Check (Config.bVsync == Case.bWantVsync, Case.pName, "vsync");
		if (Case.pWantWarningPart != nullptr)
		{
			Check (strstr (Config.Warning, Case.pWantWarningPart) != nullptr,
			       Case.pName, "aviso", Config.Warning, Case.pWantWarningPart);
		}
		else
		{
			Check (Config.Warning[0] == '\0', Case.pName, "sin aviso", Config.Warning, "");
		}
	}

	for (const TScaleCase &Case : ScaleCases)
	{
		FILE *pFile = fopen (BOOT_CONFIG, "w");
		if (pFile == nullptr) { Errors++; continue; }
		fputs (Case.pContents, pFile);
		fclose (pFile);

		TBootConfig Config;
		ReadBootConfig (&Config);
		Check (Config.bSharpScale == Case.bWantSharp, Case.pName, "scaler");
		if (Case.pWantWarningPart != nullptr)
			Check (strstr (Config.Warning, Case.pWantWarningPart) != nullptr,
			       Case.pName, "aviso", Config.Warning, Case.pWantWarningPart);
		else
			Check (Config.Warning[0] == '\0', Case.pName, "sin aviso", Config.Warning, "");
	}

	remove (BOOT_CONFIG);

	printf ("\n%s (%u fallos)\n", Errors == 0 ? "TODO BIEN" : "HAY FALLOS", Errors);

	return Errors == 0 ? 0 : 1;
}
