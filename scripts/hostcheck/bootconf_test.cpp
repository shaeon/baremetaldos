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
struct TAudioCase { const char *pName; const char *pContents; bool bJack; unsigned nVol; unsigned nMidi; const char *pWarn; };
static const TAudioCase AudioCases[] =
{
	{ "audio: por defecto", "display = 1280x720\n", false, 100, 100, nullptr },
	{ "audio: jack y volumenes", "audio_out = jack\nvolume = 70\nmidi_volume = 150\n", true, 70, 150, nullptr },
	{ "audio: volumen fuera de rango", "volume = 120\n", false, 100, 100, "volume" },
	{ "audio: midi_volume bajo el minimo", "midi_volume = 10\n", false, 100, 100, "midi_volume" },
	{ "audio: salida desconocida", "audio_out = usb\n", false, 100, 100, "audio_out" },
	{ "audio: volumen no numerico", "volume = alto\n", false, 100, 100, "volume" },
	{ "raton: velocidad 200", "mouse_speed = 200\n", false, 100, 100, nullptr },
	{ "raton: velocidad fuera de rango", "mouse_speed = 1000\n", false, 100, 100, "mouse_speed" },
	{ "setup: sin espera", "setup_wait = 0\n", false, 100, 100, nullptr },
	{ "panel: visible", "panel = on\n", false, 100, 100, nullptr },
	{ "panel: valor raro", "panel = si\n", false, 100, 100, "panel" },
	{ "setup: espera fuera de rango", "setup_wait = 9\n", false, 100, 100, "setup_wait" },
	{ "hora: zona de Espana", "timezone = CET-1CEST,M3.5.0,M10.5.0/3\n", false, 100, 100, nullptr },
	{ "hora: zona con nombre entre <>", "timezone = <-03>3\n", false, 100, 100, nullptr },
	{ "hora: zona invalida", "timezone = Europe/Madrid hora\n", false, 100, 100, "timezone" },
	{ "web: encendido con clave", "web_server = on\nweb_password = s3cr3t!\n", false, 100, 100, nullptr },
	{ "web: valor desconocido", "web_server = si\n", false, 100, 100, "web_server" },
};

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

	for (const TAudioCase &Case : AudioCases)
	{
		FILE *pFile = fopen (BOOT_CONFIG, "w");
		if (pFile == nullptr) { Errors++; continue; }
		fputs (Case.pContents, pFile);
		fclose (pFile);

		TBootConfig Config;
		ReadBootConfig (&Config);
		Check (Config.bAudioJack == Case.bJack, Case.pName, "audio_out");
		Check (Config.nVolume == Case.nVol, Case.pName, "volume");
		Check (Config.nMidiVolume == Case.nMidi, Case.pName, "midi_volume");
		Check (Config.nMouseSpeed == (strstr (Case.pContents, "mouse_speed = 200") ? 200u : 100u), Case.pName, "mouse_speed");
		Check (Config.nSetupWait == (strstr (Case.pContents, "setup_wait = 0") ? 0u : 2u), Case.pName, "setup_wait");
		Check (Config.bPanel == (strstr (Case.pContents, "panel = on") != nullptr), Case.pName, "panel");
		Check (Config.bWebServer == (strstr (Case.pContents, "web_server = on") != nullptr), Case.pName, "web_server");
		Check (strcmp (Config.WebPassword, strstr (Case.pContents, "web_password = s3cr3t!") ? "s3cr3t!" : "") == 0,
		       Case.pName, "web_password");
		{
			const char *pTZ = strstr (Case.pContents, "timezone = ");
			const bool bValid = pTZ != nullptr && Case.pWarn == nullptr;
			std::string Want = bValid ? std::string (pTZ + 11, strcspn (pTZ + 11, "\n")) : std::string ("UTC0");
			Check (Want == Config.TimeZone, Case.pName, "timezone", Config.TimeZone, Want.c_str ());
		}
		if (Case.pWarn != nullptr)
			Check (strstr (Config.Warning, Case.pWarn) != nullptr, Case.pName, "aviso", Config.Warning, Case.pWarn);
		else
			Check (Config.Warning[0] == '\0', Case.pName, "sin aviso", Config.Warning, "");
	}

	// The board's network.
	{
		struct TNetCase { const char *pName; const char *pContents; int nNet; bool bDHCP; const char *pIP;
				  unsigned nPrefix; const char *pHost; const char *pWarn; };
		static const TNetCase NetCases[] =
		{
			{ "red: por defecto wifi", "display = 1280x720\n", 2, true, "0.0.0.0", 24, "bmdos", nullptr },
			{ "red: apagada a mano", "network = off\n", 0, true, "0.0.0.0", 24, "bmdos", nullptr },
			{ "red: wifi con dhcp", "network = wifi\nip = dhcp\n", 2, true, "0.0.0.0", 24, "bmdos", nullptr },
			{ "red: ethernet con ip fija", "network = ethernet\nip = 192.168.1.50/24\ngateway = 192.168.1.1\ndns = 8.8.8.8\nhostname = salon-pc\n",
			  1, false, "192.168.1.50", 24, "salon-pc", nullptr },
			{ "red: ip sin prefijo", "ip = 10.0.0.7\n", 2, false, "10.0.0.7", 24, "bmdos", nullptr },
			{ "red: ip invalida", "ip = 300.1.1.1/24\n", 2, true, "0.0.0.0", 24, "bmdos", "ip" },
			{ "red: prefijo invalido", "ip = 10.0.0.7/40\n", 2, true, "0.0.0.0", 24, "bmdos", "ip" },
			{ "red: tipo desconocido", "network = 5g\n", 2, true, "0.0.0.0", 24, "bmdos", "network" },
			{ "red: hostname invalido", "hostname = mi-pc!\n", 2, true, "0.0.0.0", 24, "bmdos", "hostname" },
		};
		for (const TNetCase &Case : NetCases)
		{
			FILE *pFile = fopen (BOOT_CONFIG, "w");
			if (pFile == nullptr) { Errors++; continue; }
			fputs (Case.pContents, pFile);
			fclose (pFile);
			TBootConfig Config;
			ReadBootConfig (&Config);
			char IP[20];
			snprintf (IP, sizeof IP, "%u.%u.%u.%u", Config.IP[0], Config.IP[1], Config.IP[2], Config.IP[3]);
			Check (static_cast<int> (Config.Network) == Case.nNet, Case.pName, "network");
			Check (Config.bDHCP == Case.bDHCP, Case.pName, "dhcp");
			Check (strcmp (IP, Case.pIP) == 0 && Config.nPrefix == Case.nPrefix, Case.pName, "ip", IP, Case.pIP);
			Check (strcmp (Config.Hostname, Case.pHost) == 0, Case.pName, "hostname", Config.Hostname, Case.pHost);
			if (Case.pWarn != nullptr)
				Check (strstr (Config.Warning, Case.pWarn) != nullptr, Case.pName, "aviso", Config.Warning, Case.pWarn);
			else
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
