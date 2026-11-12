//
// bare_menu.cpp
//
// BM-DOS - the setup menu, opened with Win+F12: a PC BIOS setup screen for the
// port.  This file connects menu_model.h (pages, cursor, layout) and
// menu_render.h (pixels) to the machine:
//
//   * keys come from the USB keyboard handler (interrupt context) through a
//     small ring, and are only acted on in BareMenuTick(), on core 0's frame
//     loop - the same place everything that draws or touches files runs;
//   * while the menu is open the frame loop does not run the core: DOS is
//     paused, and with no audio from DOSBox the output is silent;
//   * the menu is a 640x480 picture presented through the normal video path,
//     so it works with video_core = 0 and 2 alike, scaled like a VGA screen;
//   * on the way out the last DOS frame is presented again (DOSBox only
//     redraws when its picture changes).
//
// This first step has the frame, the keys, the Sistema page with live
// values, the Salir page's actions, and the other pages listing what each
// will hold, greyed.  Settings that change things come page by page next.
//
// Shaeon (Carlos Palmero) 2026, for BM-DOS by @mmoroca.  GPL-3.0-or-later.
//
#include "bare_env.h"
#include "menu_model.h"
#include "menu_render.h"
#include "conf_edit.h"
#include "menu_keymap.h"

#include <circle/font.h>
#include <circle/logger.h>
#include <circle/memory.h>
#include <circle/sched/scheduler.h>
#include <circle/timer.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <string>

#include "libretro.h"

// The commit this kernel was built from (Makefile, build/bmdos_version.h).
#if __has_include("../../build/bmdos_version.h")
#include "../../build/bmdos_version.h"
#endif
#ifndef BMDOS_COMMIT
#define BMDOS_COMMIT "sin-git"
#endif

#ifndef DOSBOX_SNAPSHOT
#define DOSBOX_SNAPSHOT "dev"
#endif

static const char FromMenu[] = "menu";

namespace {

using namespace baremenu;

// --- state (core 0's frame loop, except where marked) ----------------------

std::atomic<bool> s_bActive {false};		// set by the key handler too
bool s_bOpened = false;				// the first tick after opening ran
bool s_bClosing = false;

CMenu s_Menu;
CScreen s_Screen;
u32 *s_pPixels = nullptr;
bool s_bDirty = true;
u64 s_nLastRefresh = 0;

// Keys from the interrupt handler to the frame loop: single producer, single
// consumer, indices only ever advanced by their own side.
struct TKeyEvent { u16 nKey; bool bDown; };
constexpr unsigned KeyRingSize = 64;
TKeyEvent s_KeyRing[KeyRingSize];
std::atomic<unsigned> s_nKeyHead {0};		// written by the handler
std::atomic<unsigned> s_nKeyTail {0};		// written by the frame loop

// Key repeat for the navigation keys (USB keyboards do not repeat by
// themselves; the BIOS of a PC did).
bool s_bRepeatKey = false;
TKey s_RepeatKey = TKey::Down;
unsigned s_nRepeatRetroKey = 0;
u64 s_nRepeatNext = 0;
constexpr u64 RepeatDelayMicros = 400000;
constexpr u64 RepeatRateMicros = 60000;

enum TActionId
{
	ActResume = 1,
	ActShutdown,
	ActReboot,
	ActSave,
	ActSaveRestart,
	ActRestartDos,
	ActDiscard,
	ActDefaults,
	ActTestSound,
	ActTestMidi,
	ActDriveC,
	ActDriveE,
	ActDriveERemove,
	ActFloppy,
	ActFloppyEject,
	ActCD,
	ActCDEject,
	ActBootGame,
	ActBootPrompt,
	ActWebPassword,
	ActWiFiList,
	ActWiFiManual,
	ActWiFiRescan,
	ActWiFiEntry = 20000,		// + index into the list
	ActBrowseUp,
	ActBrowsePick,			// "elegir esta carpeta"
	ActBrowseEntry = 10000,		// + index into the listing
};

// --- drives --------------------------------------------------------------------
//
// C: and E: are folders on the card, kept as MOUNT lines in dosbox.conf's
// [autoexec] (R).  A: and D: are disk images swapped while DOS runs, through
// the core's disk control (BareDiskInsert): nothing is saved for them, as a
// disk changed by hand on a real PC is not either.

struct TFolderDrive
{
	char cLetter;
	std::string Default;
	std::string Saved;		// from [autoexec]; "" = not mounted
	std::string Pending;
};

TFolderDrive s_DriveC { 'C', "/dos", "", "" };
TFolderDrive s_DriveE { 'E', "", "", "" };

// What runs at boot: a program on C:, as its DOS path from C:'s root
// ("DOOM\\DOOM.EXE"), or "" for the C:\\> prompt.  Kept as the menu's own
// block at the end of [autoexec] (conf_edit.h GetBlock / SetBlock), R.
std::string s_BootSaved;
std::string s_BootPending;

// SUPR / F2 at boot: the window in which they open the menu (BareMenuBootWindow).
std::atomic<bool> s_bBootWindow {false};
bool s_bOpenedAtBoot = false;

// The menu is open at boot, before DOSBox exists (BareMenuBootWindow): an R
// setting needs no DOS restart then - DOSBox reads the files when it starts,
// a moment later.  Without this, saving one asked to restart DOS, and the
// restart ran right after the boot: a second DOSBox start for nothing.
bool s_bBeforeDos = false;

std::string s_FloppyImage;		// what this menu put in A: / D:, for display
std::string s_CDImage;

// --- the card browser ------------------------------------------------------------

enum class TBrowse { None, FolderC, FolderE, Floppy, CD, Game };

TBrowse s_Browse = TBrowse::None;
std::string s_BrowsePath = "/";
std::vector<std::pair<std::string, bool>> s_BrowseEntries;	// name, is a folder

// --- sound tests (core 0's frame loop) -------------------------------------
//
// While the menu is open DOSBox makes no sound, so the test feeds the output
// itself: a sine, or silence for the MIDI mixer to add the chord to.  Frames
// are generated as real time passes, a few tens of milliseconds ahead, and go
// through BareAudioQueue - DC blocker, MIDI, volume and all - like DOSBox's.
enum class TTest { None, Tone, Midi };
TTest s_Test = TTest::None;
u64 s_nTestStart = 0;
u64 s_nTestFrames = 0;			// frames queued since the start
bool s_bTestChordOff = false;
float s_fTestPhase = 0.0f;

enum TDialogId
{
	DlgUnsaved = 1,		// Esc with changes: Guardar / Descartar / Seguir
	DlgDefaults,		// F9: Sí / No
	DlgRestartNow,		// after saving B settings: Reiniciar la placa / Más tarde
	DlgRestartDos,		// after saving R settings: Reiniciar DOS ahora / Más tarde
	DlgSaveFailed,		// Aceptar
	DlgInfo,		// Aceptar, nothing to do after it
	InWiFiPassword = 50,	// text input: the password of s_WiFiSSID
	InWiFiSSID,		// text input: a network's name, typed
	InWebPassword,		// text input: the web server's password
};

// The web server's password (boot.conf web_password), a free string like the
// folder drives; "" = none.
std::string s_WebPassSaved;
std::string s_WebPassPending;

// --- choosing a WiFi network -------------------------------------------------
//
// The networks wpa_supplicant has seen (bare_net.cpp, BareNetWiFiNetworks), a
// password typed in an input box, and SD:/wpa_supplicant.conf written with
// them.  The file is written at once - it is the WiFi's own file, not a
// setting the menu keeps pending - and the connection is made at the next
// boot, by wpa_supplicant reading it.
bool s_bWiFiList = false;
std::vector<TBareWiFiNetwork> s_WiFiNetworks;
std::string s_WiFiSSID;			// the network being joined
bool s_bWiFiSecure = true;

// Modifier keys, for typing (menu_keymap.h): Shift and AltGr (right Alt).
// Each key on its own.  The keyboard handler delivers all eight modifiers
// whenever one changes (left ones first, then right ones), so one flag for
// "Shift" was set by "left Shift down" and cleared at once by the "right Shift
// up" that follows it: the left Shift never worked in a text box.
bool s_bLeftShift = false;
bool s_bRightShift = false;
bool s_bRightAlt = false;
bool s_bModeKey = false;

// The layout of the keyboard on the desk: cmdline.txt's keymap= (what Circle
// is told the keyboard is), read when the menu opens; without one, the DOS
// layout (KEYB) is the best guess.
baremenu::TLayout s_Layout = baremenu::TLayout::US;

// --- settings: what the menu can change, and where it is kept ------------------------
//
// Each setting is one value in one file.  The menu reads the files when it
// opens (so a file edited on the PC is what it shows), keeps the choice made
// in the menu apart from the one in the file, and writes only what changed,
// in place, through conf_edit.h.  A setting that applies at once has a
// function to apply it; leaving without saving puts the file's value back.

enum TFile { FileBoot, FileDOSBox, FileCmdline };

const char *const BootPath = "SD:/boot.conf";
const char *const DOSBoxPath = "SD:/dosbox/dosbox.conf";
const char *const CmdlinePath = "SD:/cmdline.txt";

struct TChoice
{
	std::string Label;		// what the menu shows
	std::string Value;		// what the file holds
};

struct TSetting
{
	int nId;
	TFile File;
	const char *pSection;		// "" for boot.conf
	const char *pKey;		// for the display mode: see SaveDisplay
	const char *pLabel;
	const char *pHelp;
	TApply Apply;
	std::vector<TChoice> Choices;
	unsigned nDefault;
	void (*pLive) (const std::string &Value);	// applied at once, or nullptr

	unsigned nSaved = 0;		// the value in the file
	unsigned nPending = 0;		// the value in the menu
};

enum TSettingId
{
	SetDisplay = 100,
	SetVideoCore,
	SetVsync,
	SetScaler,

	SetMachine = 200,
	SetMemSize,
	SetCPUCore,
	SetCPUType,
	SetCycles,
	SetCycleStep,

	SetAudioOut = 300,
	SetVolume,
	SetSBType,
	SetSBBase,
	SetSBIrq,
	SetSBDma,
	SetSBHdma,
	SetOPL,
	SetGUS,
	SetSpeaker,
	SetTandy,
	SetMidiDevice,
	SetMpu,
	SetSoundFont,		// [midi] midiconfig when mididevice = fluidsynth
	SetMT32Model,		// [midi] midiconfig when mididevice = mt32
	SetMidiVolume,

	SetKeyb = 400,			// [autoexec] KEYB line
	SetMouseSpeed,
	SetJoyType,
	SetJoyTimed,
	SetJoyDeadzone,

	SetCPUMax = 500,
	SetGovMode,
	SetSocMaxTemp,			// cmdline.txt socmaxtemp=

	SetBootLog = 600,
	SetSetupWait,

	SetNetwork = 700,
	SetNetIP,
	SetTimeZone,
	SetWiFiCountry,			// wpa_supplicant.conf country=
	SetWebServer,
	SetSerial1,
	SetSerial2,
	SetIPX,
};

// The address the network has now, if "Fijar la actual" was offered: saving
// that choice writes the gateway and the DNS of the moment too.
std::string s_NetCurrentGateway;
std::string s_NetCurrentDNS;
std::string s_NetCurrentChoice;		// the choice's value ("a.b.c.d/n"), "" if none

void LiveScaler (const std::string &Value)
{
	BareVideoSetSharpScaling (Value == "sharp" ? TRUE : FALSE);
}

} // namespace

// DOSBox's own globals (src/cpu/cpu.cpp): the cycle count the emulated CPU
// runs at, and whether it is a fixed number.  Ctrl+F11/F12 change the first;
// the menu does the same while the machine is paused.  Bit32s is int32_t.
extern int32_t CPU_CycleMax;
extern bool CPU_CycleAutoAdjust;

namespace {

void LiveCycles (const std::string &Value)
{
	// Only a fixed count is applied at once, and only if the machine runs a
	// fixed count now; switching modes (auto, max) waits for the restart.
	if (Value.rfind ("fixed ", 0) == 0 && !CPU_CycleAutoAdjust)
	{
		const int nCycles = atoi (Value.c_str () + 6);
		if (nCycles > 0)
		{
			CPU_CycleMax = nCycles;
		}
	}
}

void LiveVolume (const std::string &Value)
{
	BareAudioSetVolume (static_cast<unsigned> (atoi (Value.c_str ())));
}

void LiveMidiVolume (const std::string &Value)
{
	BareMidiSetVolume (static_cast<unsigned> (atoi (Value.c_str ())));
}

void LiveGovMode (const std::string &Value)
{
	BareSysInfoSetMode (Value.c_str ());
}

void LiveMouseSpeed (const std::string &Value)
{
	BareInputSetMouseSpeed (static_cast<unsigned> (atoi (Value.c_str ())));
}

// "es  850" -> "es 850": KEYB arguments compared word by word.
std::string NormaliseWords (const std::string &In)
{
	std::string Out;
	for (const std::string &W : bareconf::Words (bareconf::Lower (In)))
	{
		if (!Out.empty ()) Out += ' ';
		Out += W;
	}
	return Out;
}

// The SoundFonts on the card, for the SoundFont setting: every .sf2 in
// /dosbox/system, as the path dosbox.conf's midiconfig takes.
std::vector<TChoice> ScanSoundFonts (void)
{
	std::vector<TChoice> Fonts;
	DIR *pDir = opendir ("SD:/dosbox/system");
	if (pDir != nullptr)
	{
		while (const dirent *pEntry = readdir (pDir))
		{
			const std::string Name = pEntry->d_name;
			if (Name.size () > 4 && bareconf::Lower (Name.substr (Name.size () - 4)) == ".sf2")
			{
				Fonts.push_back (TChoice { Name, "/dosbox/system/" + Name });
			}
		}
		closedir (pDir);
	}
	if (Fonts.empty ())
	{
		Fonts.push_back (TChoice { "(no hay .sf2)", "/dosbox/system/default.sf2" });
	}
	return Fonts;
}

// "24000", "fixed  24000", "Fixed 24000" -> "fixed 24000"; anything else as is.
std::string NormaliseCycles (const std::string &In)
{
	std::string s = bareconf::Lower (bareconf::Trim (In));
	if (!s.empty () && s.find_first_not_of ("0123456789") == std::string::npos)
	{
		return "fixed " + s;
	}
	if (s.rfind ("fixed", 0) == 0)
	{
		return "fixed " + bareconf::Trim (s.substr (5));
	}
	return s;
}

std::vector<TSetting> s_Settings;

std::string Fit (const std::string &Text, unsigned nWidth);

// The widths of the menu's columns (menu_model.h).
constexpr unsigned LabelWidth = CMenu::ValueCol - CMenu::LabelCol - 1;	// 23
constexpr unsigned ValueWidth = CMenu::ApplyCol - CMenu::ValueCol - 1;	// 24

TSetting *FindSetting (int nId);

void DefineSettings (void)
{
	s_Settings.clear ();

	s_Settings.push_back ({ SetDisplay, FileCmdline, "", "width/height", "Resolución",
		"El tamaño del framebuffer. Para la imagen más nítida, que coincida con el modo "
		"HDMI de config.txt (hdmi_mode=16 es 1920x1080, hdmi_mode=4 es 1280x720): si no, "
		"la GPU vuelve a escalar con suavizado. 800x600 alivia una Pi 3. Se guarda en "
		"cmdline.txt, que es el que manda, y en boot.conf.",
		TApply::RestartBoard,
		{ { "640x480", "640x480" }, { "800x600", "800x600" }, { "1024x768", "1024x768" },
		  { "1280x720", "1280x720" }, { "1280x1024", "1280x1024" }, { "1920x1080", "1920x1080" } },
		3, nullptr });

	s_Settings.push_back ({ SetVideoCore, FileBoot, "", "video_core", "Núcleo de vídeo",
		"Núcleo 2: escala la imagen en otro núcleo, en una página oculta, y la muestra "
		"con vsync; el emulador recupera ese tiempo. Núcleo 0: como al principio, en el "
		"mismo núcleo que DOSBox. Para comparar o si el 2 da problemas en alguna placa.",
		TApply::RestartBoard,
		{ { "Núcleo 2", "2" }, { "Núcleo 0", "0" } }, 0, nullptr });

	s_Settings.push_back ({ SetVsync, FileBoot, "", "vsync", "Vsync",
		"Con el núcleo 2: esperar al refresco de la pantalla tras cada cambio de página. "
		"Sí: sin tearing (un modo de 70 Hz en una pantalla de 60 Hz pierde un frame de "
		"cada siete). No: se muestra en cuanto está listo. La velocidad del juego no "
		"depende de esto.",
		TApply::RestartBoard,
		{ { "Sí", "on" }, { "No", "off" } }, 0, nullptr });

	s_Settings.push_back ({ SetScaler, FileBoot, "", "scaler", "Escalado",
		"Nítido: cada fila de pantalla es la fila del juego que tiene debajo, y solo "
		"mezclan las del borde entre dos; el texto conserva los bordes. Suave: cada fila "
		"se interpola de las dos más cercanas. Se ve al momento, en este mismo menú.",
		TApply::Now,
		{ { "Nítido", "sharp" }, { "Suave", "smooth" } }, 0, LiveScaler });

	// --- Máquina: dosbox.conf -----------------------------------------------

	s_Settings.push_back ({ SetMachine, FileDOSBox, "dosbox", "machine", "Tipo de máquina",
		"La tarjeta de vídeo del PC emulado. SVGA S3 sirve para casi todo, incluidos los "
		"juegos VESA. Los juegos de 8088 que esperan CGA o Tandy (gráficos y sonido de "
		"16 colores) piden esas máquinas; Hercules, para los de monocromo.",
		TApply::RestartDOSBox,
		{ { "SVGA S3 Trio64", "svga_s3" }, { "SVGA Tseng ET4000", "svga_et4000" },
		  { "SVGA Tseng ET3000", "svga_et3000" }, { "SVGA Paradise", "svga_paradise" },
		  { "VGA", "vgaonly" }, { "EGA", "ega" }, { "CGA", "cga" }, { "Tandy", "tandy" },
		  { "PCjr", "pcjr" }, { "Hercules", "hercules" },
		  { "VESA sin LFB", "vesa_nolfb" }, { "VESA antigua", "vesa_oldvbe" } },
		0, nullptr });

	s_Settings.push_back ({ SetMemSize, FileDOSBox, "dosbox", "memsize", "Memoria",
		"La RAM del PC emulado. 16 MB valen para casi todo; algún juego viejo falla con "
		"mucha memoria y alguno de Windows 9x o con DOS4GW agradece 32 o más (63 es el "
		"máximo de DOSBox).",
		TApply::RestartDOSBox,
		{ { "1 MB", "1" }, { "2 MB", "2" }, { "4 MB", "4" }, { "8 MB", "8" },
		  { "16 MB", "16" }, { "32 MB", "32" }, { "63 MB", "63" } },
		4, nullptr });

	s_Settings.push_back ({ SetCPUCore, FileDOSBox, "cpu", "core", "Núcleo de CPU",
		"Cómo emula DOSBox el x86. Dinámico traduce a código ARM y es el rápido; normal "
		"interpreta instrucción a instrucción (más lento, a veces más compatible); simple "
		"solo sirve para modo real; auto empieza en normal y pasa a dinámico en modo "
		"protegido.",
		TApply::RestartDOSBox,
		{ { "Dinámico", "dynamic" }, { "Normal", "normal" }, { "Simple", "simple" }, { "Auto", "auto" } },
		0, nullptr });

	s_Settings.push_back ({ SetCPUType, FileDOSBox, "cpu", "cputype", "Tipo de CPU",
		"El juego de instrucciones del procesador emulado. Auto vale para casi todo; "
		"386_prefetch lo necesitan algunos juegos con código automodificable; pentium_slow "
		"da instrucciones de Pentium a los que las comprueban.",
		TApply::RestartDOSBox,
		{ { "Auto", "auto" }, { "386", "386" }, { "386 lento", "386_slow" },
		  { "386 prefetch", "386_prefetch" }, { "486 lento", "486_slow" },
		  { "Pentium lento", "pentium_slow" } },
		0, nullptr });

	s_Settings.push_back ({ SetCycles, FileDOSBox, "cpu", "cycles", "Velocidad",
		"Cuántas instrucciones ejecuta el PC emulado. Cada valor imita a un procesador "
		"de la época (aproximado). Se aplica al momento si la máquina va a ciclos fijos; "
		"después afina con Ctrl+F11/F12 mirando \"pace\" en el panel: 35/70 es velocidad "
		"real. Más de lo que la placa puede dar se oye como cortes de sonido.",
		TApply::Now,
		{ { "8088 a 4,77 MHz", "fixed 300" }, { "286 a 12 MHz", "fixed 1500" },
		  { "386SX a 20 MHz", "fixed 3000" }, { "386DX a 33 MHz", "fixed 6000" },
		  { "486DX a 33 MHz", "fixed 12000" }, { "486DX2 a 66 MHz", "fixed 24000" },
		  { "Pentium a 60 MHz", "fixed 31000" }, { "Pentium a 75 MHz", "fixed 43500" },
		  { "Pentium a 100 MHz", "fixed 60000" } },
		5, LiveCycles });

	s_Settings.push_back ({ SetCycleStep, FileDOSBox, "cpu", "cycleup", "Paso de Ctrl+F11/F12",
		"Cuántos ciclos suben o bajan Ctrl+F12 y Ctrl+F11. Se guarda para los dos.",
		TApply::RestartDOSBox,
		{ { "500", "500" }, { "1000", "1000" }, { "2000", "2000" }, { "5000", "5000" } },
		2, nullptr });

	// --- Sonido: boot.conf (salida, volúmenes) y dosbox.conf (tarjetas) ------

	s_Settings.push_back ({ SetAudioOut, FileBoot, "", "audio_out", "Salida",
		"Por dónde sale el sonido. HDMI: por el cable de la pantalla. Jack: la salida "
		"de 3,5 mm de la placa (PWM). Si la elegida no arranca, se prueba la otra.",
		TApply::RestartBoard, { { "HDMI", "hdmi" }, { "Jack 3,5 mm", "jack" } }, 0, nullptr });

	s_Settings.push_back ({ SetVolume, FileBoot, "", "volume", "Volumen general",
		"El volumen de todo lo que sale, después del mezclador de DOSBox. Se oye al "
		"momento: prueba con \"Probar sonido\".",
		TApply::Now,
		{ { "100 %", "100" }, { "90 %", "90" }, { "80 %", "80" }, { "70 %", "70" }, { "60 %", "60" },
		  { "50 %", "50" }, { "40 %", "40" }, { "30 %", "30" }, { "20 %", "20" }, { "10 %", "10" },
		  { "Silencio", "0" } },
		0, LiveVolume });

	s_Settings.push_back ({ SetSBType, FileDOSBox, "sblaster", "sbtype", "Sound Blaster",
		"El modelo de Sound Blaster. SB16 es el más compatible; algunos juegos antiguos "
		"solo conocen la Pro o la 2.0. Game Blaster es la CMS de Creative.",
		TApply::RestartDOSBox,
		{ { "Sound Blaster 16", "sb16" }, { "Sound Blaster Pro 2", "sbpro2" },
		  { "Sound Blaster Pro", "sbpro1" }, { "Sound Blaster 2.0", "sb2" },
		  { "Sound Blaster 1.x", "sb1" }, { "Game Blaster", "gb" }, { "Ninguna", "none" } },
		0, nullptr });

	s_Settings.push_back ({ SetSBBase, FileDOSBox, "sblaster", "sbbase", "  Puerto",
		"La dirección de E/S de la Sound Blaster. 220 es lo que esperan los juegos. "
		"DOSBox la anuncia en la variable BLASTER.",
		TApply::RestartDOSBox,
		{ { "220", "220" }, { "240", "240" }, { "260", "260" }, { "280", "280" } }, 0, nullptr });

	s_Settings.push_back ({ SetSBIrq, FileDOSBox, "sblaster", "irq", "  IRQ",
		"La interrupción de la Sound Blaster. 7 o 5 son las habituales.",
		TApply::RestartDOSBox,
		{ { "7", "7" }, { "5", "5" }, { "3", "3" }, { "9", "9" }, { "10", "10" } }, 0, nullptr });

	s_Settings.push_back ({ SetSBDma, FileDOSBox, "sblaster", "dma", "  DMA",
		"El canal DMA de 8 bits. 1 es el habitual.",
		TApply::RestartDOSBox, { { "1", "1" }, { "3", "3" }, { "0", "0" } }, 0, nullptr });

	s_Settings.push_back ({ SetSBHdma, FileDOSBox, "sblaster", "hdma", "  DMA alto",
		"El canal DMA de 16 bits (solo SB16). 5 es el habitual.",
		TApply::RestartDOSBox, { { "5", "5" }, { "6", "6" }, { "7", "7" } }, 0, nullptr });

	s_Settings.push_back ({ SetOPL, FileDOSBox, "sblaster", "oplmode", "FM (OPL / AdLib)",
		"El chip FM de la música AdLib. Auto pone el que lleva la Sound Blaster elegida "
		"(OPL3 en la SB16 y la Pro 2). Dual OPL2 es el de la Pro original.",
		TApply::RestartDOSBox,
		{ { "Auto", "auto" }, { "OPL2", "opl2" }, { "Dual OPL2", "dualopl2" }, { "OPL3", "opl3" },
		  { "OPL3 Gold", "opl3gold" }, { "CMS", "cms" }, { "Ninguno", "none" } },
		0, nullptr });

	s_Settings.push_back ({ SetGUS, FileDOSBox, "gus", "gus", "Gravis Ultrasound",
		"Para su música hace falta la carpeta ULTRASND con los patches en C:\\ULTRASND "
		"(la de los discos de Gravis). Los juegos que suben sus propios sonidos, como "
		"Second Reality, no los necesitan.",
		TApply::RestartDOSBox, { { "No", "false" }, { "Sí", "true" } }, 0, nullptr });

	s_Settings.push_back ({ SetSpeaker, FileDOSBox, "speaker", "pcspeaker", "Altavoz del PC",
		"El pitido del PC. Algunos juegos lo usan para todo el sonido.",
		TApply::RestartDOSBox, { { "Sí", "true" }, { "No", "false" } }, 0, nullptr });

	s_Settings.push_back ({ SetTandy, FileDOSBox, "speaker", "tandy", "Sonido Tandy",
		"El chip de sonido de 3 voces del Tandy 1000. Auto lo activa con la máquina Tandy.",
		TApply::RestartDOSBox, { { "Auto", "auto" }, { "Sí", "on" }, { "No", "off" } }, 0, nullptr });

	s_Settings.push_back ({ SetMidiDevice, FileDOSBox, "midi", "mididevice", "Dispositivo MIDI",
		"General MIDI: FluidSynth con un SoundFont de /dosbox/system. MT-32: Munt, con las "
		"ROM de Roland en /dosbox/system (no se incluyen). Se generan en el núcleo 1. En "
		"el juego, elige la música General MIDI o Roland en el puerto 330.",
		TApply::RestartDOSBox,
		{ { "Ninguno", "none" }, { "General MIDI", "fluidsynth" }, { "Roland MT-32", "mt32" } },
		0, nullptr });

	s_Settings.push_back ({ SetMpu, FileDOSBox, "midi", "mpu401", "  MPU-401",
		"La interfaz MIDI que ven los juegos. Inteligente sirve para todos; UART solo para "
		"los que no piden el modo inteligente. Ninguna desconecta el MIDI aunque haya "
		"dispositivo.",
		TApply::RestartDOSBox,
		{ { "Inteligente", "intelligent" }, { "UART", "uart" }, { "Ninguna", "none" } }, 0, nullptr });

	s_Settings.push_back ({ SetSoundFont, FileDOSBox, "midi", "midiconfig", "  SoundFont",
		"Para General MIDI: el fichero .sf2 de /dosbox/system que usa FluidSynth. Copia "
		"más en esa carpeta y aparecerán aquí.",
		TApply::RestartDOSBox, ScanSoundFonts (), 0, nullptr });

	s_Settings.push_back ({ SetMT32Model, FileDOSBox, "midi", "midiconfig", "  Modelo Roland",
		"Para MT-32: qué pareja de ROM usar si hay las dos. MT-32: MT32_CONTROL.ROM y "
		"MT32_PCM.ROM; CM-32L: CM32L_CONTROL.ROM y CM32L_PCM.ROM.",
		TApply::RestartDOSBox, { { "MT-32", "" }, { "CM-32L", "cm32l" } }, 0, nullptr });

	s_Settings.push_back ({ SetMidiVolume, FileBoot, "", "midi_volume", "Volumen del MIDI",
		"El volumen de la música MIDI frente al resto del sonido de DOSBox. Se oye al "
		"momento: prueba con \"Probar MIDI\".",
		TApply::Now,
		{ { "25 %", "25" }, { "50 %", "50" }, { "75 %", "75" }, { "100 %", "100" },
		  { "125 %", "125" }, { "150 %", "150" }, { "200 %", "200" } },
		3, LiveMidiVolume });

	// --- Entrada ------------------------------------------------------------

	s_Settings.push_back ({ SetKeyb, FileDOSBox, "autoexec", "keyb", "Teclado",
		"La distribución de teclado de DOS (KEYB en el autoexec de dosbox.conf). El "
		"teclado USB envía posiciones físicas: esto decide qué carácter da cada tecla. "
		"Ninguna deja el teclado de EE. UU. de DOS.",
		TApply::RestartDOSBox,
		{ { "España", "es 850" }, { "Latinoamérica", "la 850" }, { "EE. UU.", "us" },
		  { "Reino Unido", "uk 850" }, { "Francia", "fr 850" }, { "Alemania", "gr 850" },
		  { "Italia", "it 850" }, { "Portugal", "po 860" }, { "Brasil", "br 850" },
		  { "Ninguna", "" } },
		0, nullptr });

	s_Settings.push_back ({ SetMouseSpeed, FileBoot, "", "mouse_speed", "Velocidad del ratón",
		"Cuánto se mueve el puntero de DOS por cada movimiento del ratón USB. Se nota al "
		"momento al volver al juego.",
		TApply::Now,
		{ { "25 %", "25" }, { "50 %", "50" }, { "75 %", "75" }, { "100 %", "100" },
		  { "125 %", "125" }, { "150 %", "150" }, { "200 %", "200" }, { "300 %", "300" },
		  { "400 %", "400" } },
		3, LiveMouseSpeed });

	s_Settings.push_back ({ SetJoyType, FileDOSBox, "joystick", "joysticktype", "Joystick",
		"El joystick que ve DOS, hecho con los mandos USB. 2 ejes: el de dos botones de "
		"siempre; 4 ejes: dos sticks; FCS y CH: los de los simuladores de vuelo. "
		"Ninguno: algunos juegos se cuelgan si buscan joystick y no hay.",
		TApply::RestartDOSBox,
		{ { "Ninguno", "none" }, { "Auto", "auto" }, { "2 ejes", "2axis" }, { "4 ejes", "4axis" },
		  { "4 ejes (2.º mando)", "4axis_2" }, { "Thrustmaster FCS", "fcs" }, { "CH Flightstick", "ch" } },
		0, nullptr });

	s_Settings.push_back ({ SetJoyTimed, FileDOSBox, "joystick", "timed", "  Ejes temporizados",
		"Cómo mide DOS los ejes. Si el joystick deriva hacia un lado, prueba a cambiarlo.",
		TApply::RestartDOSBox, { { "Sí", "true" }, { "No", "false" } }, 0, nullptr });

	s_Settings.push_back ({ SetJoyDeadzone, FileDOSBox, "joystick", "deadzone", "  Zona muerta",
		"Cuánto hay que mover el stick, en %, antes de que DOS lo note. Súbela si el "
		"mando se mueve solo.",
		TApply::RestartDOSBox,
		{ { "0 %", "0" }, { "5 %", "5" }, { "10 %", "10" }, { "15 %", "15" }, { "20 %", "20" },
		  { "25 %", "25" }, { "30 %", "30" } },
		2, nullptr });

	// --- Rendimiento ---------------------------------------------------------

	s_Settings.push_back ({ SetGovMode, FileBoot, "", "mode", "Modo",
		"Auto: el governor sube el reloj cuando el emulador no llega y lo baja con la "
		"pantalla quieta. Máximo: siempre en el nivel más alto. Ahorro: siempre en el más "
		"bajo. En los tres, si la temperatura llega al límite, el reloj baja.",
		TApply::Now,
		{ { "Auto", "auto" }, { "Máximo", "performance" }, { "Ahorro", "powersave" } },
		0, LiveGovMode });

	s_Settings.push_back ({ SetCPUMax, FileBoot, "", "cpu_max_mhz", "Techo del reloj",
		"El reloj más alto que usará el governor. Sin límite: el máximo del firmware "
		"(1500 o 1800 en una Pi 4, 2400 en una Pi 5). Útil en una caja sin ventilación.",
		TApply::RestartBoard,
		{ { "Sin límite", "0" }, { "600 MHz", "600" }, { "1000 MHz", "1000" },
		  { "1200 MHz", "1200" }, { "1500 MHz", "1500" }, { "1800 MHz", "1800" },
		  { "2000 MHz", "2000" }, { "2400 MHz", "2400" } },
		0, nullptr });

	s_Settings.push_back ({ SetSocMaxTemp, FileCmdline, "", "socmaxtemp", "Límite de temperatura",
		"A partir de esta temperatura el governor baja el reloj un nivel cada vez. Se "
		"guarda en cmdline.txt (socmaxtemp=).",
		TApply::RestartBoard,
		{ { "60 °C", "60" }, { "65 °C", "65" }, { "70 °C", "70" }, { "75 °C", "75" },
		  { "80 °C", "80" }, { "85 °C", "85" } },
		4, nullptr });

	// --- Red ------------------------------------------------------------------

	s_Settings.push_back ({ SetNetwork, FileBoot, "", "network", "Conexión",
		"La red de la placa. Ethernet: el puerto de red. WiFi: la red de "
		"SD:/wpa_supplicant.conf, con el firmware del chip en SD:/firmware/. Ninguna: "
		"el arranque no la toca. Viene en WiFi; quien no quiera red, Ninguna.",
		TApply::RestartBoard,
		{ { "Ninguna", "off" }, { "Ethernet", "ethernet" }, { "WiFi", "wifi" } }, 2, nullptr });

	s_Settings.push_back ({ SetWiFiCountry, FileBoot, "", "", "País del WiFi",
		"Decide qué canales puede usar el WiFi (en Europa, del 1 al 13). Se escribe en "
		"wpa_supplicant.conf al elegir una red.",
		TApply::RestartBoard,
		{ { "España", "ES" }, { "Portugal", "PT" }, { "México", "MX" }, { "Argentina", "AR" },
		  { "Colombia", "CO" }, { "Chile", "CL" }, { "Perú", "PE" }, { "EE. UU.", "US" },
		  { "Reino Unido", "GB" }, { "Francia", "FR" }, { "Alemania", "DE" }, { "Italia", "IT" } },
		0, nullptr });

	s_Settings.push_back ({ SetWebServer, FileBoot, "", "web_server", "Servidor web",
		"La tarjeta desde el navegador de cualquier ordenador de la misma red: ver "
		"carpetas, bajar, subir ficheros o carpetas enteras (arrastrándolas), crear y "
		"borrar. Apagado por defecto: puede escribir y borrar en la tarjeta. Mejor "
		"con contraseña.",
		TApply::RestartBoard, { { "Apagado", "off" }, { "Encendido", "on" } }, 0, nullptr });

	s_Settings.push_back ({ SetTimeZone, FileBoot, "", "timezone", "Zona horaria",
		"La hora de DOS: la de la red (NTP) en esta zona, con el cambio de verano. DOS "
		"la toma al arrancar, así que se aplica al reiniciar. Otra zona se escribe en "
		"boot.conf como regla POSIX (timezone = ...).",
		TApply::RestartDOSBox,
		{ { "España (península)", "CET-1CEST,M3.5.0,M10.5.0/3" },
		  { "Canarias, Portugal", "WET0WEST,M3.5.0/1,M10.5.0" },
		  { "Reino Unido", "GMT0BST,M3.5.0/1,M10.5.0" },
		  { "Europa central", "CET-1CEST,M3.5.0,M10.5.0/3" },
		  { "UTC", "UTC0" },
		  { "México (centro)", "CST6" },
		  { "Colombia, Perú", "<-05>5" },
		  { "Venezuela", "<-04>4" },
		  { "Argentina, Uruguay", "<-03>3" },
		  { "Chile", "<-04>4<-03>,M9.1.6/24,M4.1.6/24" },
		  { "EE. UU. este", "EST5EDT,M3.2.0,M11.1.0" },
		  { "EE. UU. oeste", "PST8PDT,M3.2.0,M11.1.0" } },
		4, nullptr });

	// Serial ports: what DOS finds on COM1 / COM2.  The modem dials a host
	// (ATDT servidor:puerto, or ATDT 192.168.1.20:23) over the board's
	// network; listening, it also answers incoming telnet calls.  Null-modem
	// is a serial cable to another DOSBox over TCP: one side waits, the other
	// connects (the connecting side needs the host, written in dosbox.conf).
	s_Settings.push_back ({ SetIPX, FileDOSBox, "ipx", "ipx", "IPX",
		"La red IPX de los juegos de los 90 (DOOM, Duke 3D, Warcraft II, Command & "
		"Conquer), por UDP como en cualquier DOSBox. Con IPX activado, en DOS: IPXNET "
		"STARTSERVER en una máquina, e IPXNET CONNECT <su IP> en las demás; luego el "
		"juego, en modo red IPX. Vale con otros DOSBox de PC, no solo con placas.",
		TApply::RestartDOSBox, { { "No", "false" }, { "Sí", "true" } }, 0, nullptr });

	for (int nPort = 1; nPort <= 2; nPort++)
	{
		s_Settings.push_back ({ nPort == 1 ? SetSerial1 : SetSerial2, FileDOSBox, "serial",
			nPort == 1 ? "serial1" : "serial2", nPort == 1 ? "COM1" : "COM2",
			"Módem: para BBS, ATDT servidor:puerto por la red de la placa. Que contesta: "
			"acepta también telnet entrante al puerto 23. Null-modem: cable serie con otro "
			"DOSBox por TCP; espera en el 2323 (el otro: nullmodem server:IP port:2323).",
			TApply::RestartDOSBox,
			{ { "Nada", "dummy" }, { "Módem", "modem" }, { "Módem que contesta", "modem listenport:23" },
			  { "Null-modem (espera)", "nullmodem port:2323" }, { "Desactivado", "disabled" } },
			0, nullptr });
	}

	{
		TSetting IP { SetNetIP, FileBoot, "", "ip", "Dirección IP",
			"DHCP: la da el router. Fija: siempre la misma, útil para entrar por la red "
			"desde el PC. \"Fijar\" guarda como fija la que tiene ahora, con su "
			"puerta de enlace y su DNS. Otra distinta se escribe en boot.conf (ip =, "
			"gateway =, dns =).",
			TApply::RestartBoard, { { "DHCP", "dhcp" } }, 0, nullptr };

		s_NetCurrentChoice.clear ();
		TBareNetInfo Net;
		BareNetInfo (&Net);
		if (BareNetReady () && Net.IP[0] != '\0')
		{
			unsigned m[4] = {}, nPrefix = 0;
			sscanf (Net.Mask, "%u.%u.%u.%u", &m[0], &m[1], &m[2], &m[3]);
			for (unsigned i = 0; i < 4; i++) nPrefix += static_cast<unsigned> (__builtin_popcount (m[i]));
			char Choice[32];
			snprintf (Choice, sizeof Choice, "%s/%u", Net.IP, nPrefix ? nPrefix : 24);
			s_NetCurrentChoice = Choice;
			s_NetCurrentGateway = Net.Gateway;
			s_NetCurrentDNS = Net.DNS;
			IP.Choices.push_back ({ "Fijar: " + std::string (Net.IP), s_NetCurrentChoice });
		}
		s_Settings.push_back (IP);
	}

	// --- Arranque -------------------------------------------------------------

	s_Settings.push_back ({ SetBootLog, FileBoot, "", "boot_log", "Mensajes de arranque",
		"Lo que se ve mientras arranca. Todo: cada paso, lo que hace falta para "
		"diagnosticar. Resumen: solo lo importante. Nada: pantalla negra hasta DOS.",
		TApply::RestartBoard,
		{ { "Todo", "auto" }, { "Resumen", "quiet" }, { "Nada", "off" } }, 0, nullptr });

	s_Settings.push_back ({ SetSetupWait, FileBoot, "", "setup_wait", "Espera de SUPR",
		"Los segundos que espera el arranque a que pulses SUPR o F2 para entrar aquí "
		"antes de que arranque DOSBox. Sin espera: Win+F12 sigue abriendo el menú "
		"después.",
		TApply::RestartBoard,
		{ { "Sin espera", "0" }, { "1 s", "1" }, { "2 s", "2" }, { "3 s", "3" }, { "5 s", "5" } },
		2, nullptr });

	// The default SoundFont: default.sf2 if the card has it.
	if (TSetting *pFont = FindSetting (SetSoundFont))
	{
		for (unsigned i = 0; i < pFont->Choices.size (); i++)
		{
			if (bareconf::Lower (pFont->Choices[i].Label) == "default.sf2") pFont->nDefault = i;
		}
	}
}

TSetting *FindSetting (int nId)
{
	for (TSetting &S : s_Settings)
	{
		if (S.nId == nId) return &S;
	}
	return nullptr;
}

bool ReadText (const char *pPath, std::string *pText)
{
	pText->clear ();
	FILE *pFile = fopen (pPath, "rb");
	if (pFile == nullptr)
	{
		return false;
	}
	char Buffer[512];
	size_t n;
	while ((n = fread (Buffer, 1, sizeof Buffer, pFile)) != 0)
	{
		pText->append (Buffer, n);
	}
	fclose (pFile);
	return true;
}

// Write next to the file and swap: a cut in the middle leaves either the old
// file or the new one, plus a stray ".new" - never half a file.
bool WriteText (const char *pPath, const std::string &Text)
{
	const std::string Tmp = std::string (pPath) + ".new";
	FILE *pFile = fopen (Tmp.c_str (), "wb");
	if (pFile == nullptr)
	{
		return false;
	}
	const bool bWritten = fwrite (Text.data (), 1, Text.size (), pFile) == Text.size ();
	const bool bClosed = fclose (pFile) == 0;	// f_close: f_sync to the card
	if (!bWritten || !bClosed)
	{
		remove (Tmp.c_str ());
		return false;
	}
	remove (pPath);
	return rename (Tmp.c_str (), pPath) == 0;
}

unsigned IndexOf (const TSetting &S, const std::string &Value)
{
	for (unsigned i = 0; i < S.Choices.size (); i++)
	{
		if (S.Choices[i].Value == Value) return i;
	}
	return S.nDefault;
}

void LoadSettings (void)
{
	std::string Boot, DOSBox, Cmdline;
	ReadText (BootPath, &Boot);
	ReadText (DOSBoxPath, &DOSBox);
	ReadText (CmdlinePath, &Cmdline);
	const bareconf::CConfText BootConf (Boot, true);
	const bareconf::CConfText DOSBoxConf (DOSBox, false);

	{
		std::string Pass;
		BootConf.Get ("", "web_password", &Pass);
		s_WebPassSaved = s_WebPassPending = bareconf::Trim (Pass);
	}

	s_DriveC.Saved = s_DriveC.Pending = bareconf::GetMount (DOSBoxConf, 'C');
	s_DriveE.Saved = s_DriveE.Pending = bareconf::GetMount (DOSBoxConf, 'E');

	// The boot program from the menu's block: "CD \DIR" + "PROG.EXE".
	{
		std::string Dir, Prog;
		for (const std::string &L : bareconf::GetBlock (DOSBoxConf))
		{
			const std::vector<std::string> W = bareconf::Words (L);
			if (W.size () >= 2 && bareconf::Lower (W[0]) == "cd")
			{
				Dir = W[1];
				while (!Dir.empty () && Dir[0] == '\\') Dir.erase (0, 1);
			}
			else if (W.size () == 1 && W[0].find (':') == std::string::npos)
			{
				Prog = W[0];
			}
		}
		s_BootSaved = s_BootPending = Prog.empty () ? "" : (Dir.empty () ? Prog : Dir + "\\" + Prog);
	}

	for (TSetting &S : s_Settings)
	{
		std::string Value;
		bool bFound = false;

		if (S.nId == SetDisplay)
		{
			// cmdline.txt's width= / height= is what the console and the port
			// use; a mode not in the list is added to it, so that the menu
			// shows what the card really has.
			std::string W, H;
			if (bareconf::CmdlineGet (Cmdline, "width", &W) && bareconf::CmdlineGet (Cmdline, "height", &H))
			{
				Value = W + "x" + H;
				bFound = true;
				bool bKnown = false;
				for (const TChoice &C : S.Choices) bKnown = bKnown || C.Value == Value;
				if (!bKnown)
				{
					S.Choices.insert (S.Choices.begin (), TChoice { Fit (Value, ValueWidth - 2), Value });
					S.nDefault++;		// the default stays the same mode
				}
			}
		}
		else if (S.nId == SetWiFiCountry)
		{
			// Before the boot.conf branch: it is declared with the boot.conf
			// settings but lives in wpa_supplicant.conf, and used to fall into
			// that branch - always showing the default, whatever the file said.
			// The active country= line, as wpa_supplicant reads it: not one in
			// a comment (the template's comments mention it).
			S.nSaved = S.nPending = S.nDefault;
			std::string WPA;
			ReadText ("SD:/wpa_supplicant.conf", &WPA);
			size_t n = 0;
			while (n < WPA.size ())
			{
				size_t e = WPA.find ('\n', n);
				if (e == std::string::npos) e = WPA.size ();
				const std::string Line = bareconf::Trim (WPA.substr (n, e - n));
				if (Line.compare (0, 8, "country=") == 0)
				{
					const std::string Code = bareconf::Trim (Line.substr (8));
					for (unsigned i = 0; i < S.Choices.size (); i++)
						if (S.Choices[i].Value == Code) { S.nSaved = S.nPending = i; }
					break;
				}
				n = e + 1;
			}
			continue;
		}
		else if (S.File == FileBoot)
		{
			bFound = BootConf.Get (S.pSection, S.pKey, &Value);
		}
		else if (S.File == FileCmdline)
		{
			bFound = bareconf::CmdlineGet (Cmdline, S.pKey, &Value);
		}
		else if (S.nId == SetKeyb)
		{
			Value = NormaliseWords (bareconf::GetCommand (DOSBoxConf, "keyb"));
			bFound = true;		// no KEYB line is the "Ninguna" choice
		}
		else if (S.nId == SetSoundFont || S.nId == SetMT32Model)
		{
			// One key, two settings: a path ending in .sf2 is the SoundFont,
			// "cm32l" is the model; whichever it is not keeps its default.
			std::string Config;
			DOSBoxConf.Get ("midi", "midiconfig", &Config);
			Config = bareconf::Trim (Config);
			const std::string Lower = bareconf::Lower (Config);
			if (S.nId == SetSoundFont && Lower.size () > 4 && Lower.substr (Lower.size () - 4) == ".sf2")
			{
				for (unsigned i = 0; i < S.Choices.size (); i++)
				{
					const std::string &Path = S.Choices[i].Value;
					const std::string Base = Lower.substr (Lower.find_last_of ("/\\") + 1);
					if (bareconf::Lower (Path.substr (Path.find_last_of ('/') + 1)) == Base)
					{
						S.nSaved = S.nPending = i;
					}
				}
			}
			else if (S.nId == SetSoundFont)
			{
				S.nSaved = S.nPending = S.nDefault;
			}
			else
			{
				S.nSaved = S.nPending = Lower == "cm32l" ? 1 : 0;
			}
			continue;
		}
		else if (S.File == FileDOSBox)
		{
			bFound = DOSBoxConf.Get (S.pSection, S.pKey, &Value);
		}

		// Values compare without regard to case - except a time zone, where
		// "CET" and "cet" are different zone names to newlib.
		Value = S.nId == SetTimeZone ? bareconf::Trim (Value) : bareconf::Lower (bareconf::Trim (Value));
		if (S.nId == SetCycles)
		{
			Value = NormaliseCycles (Value);
		}
		if (S.nId == SetSerial1 || S.nId == SetSerial2)
		{
			Value = NormaliseWords (Value);
		}

		// A value the list does not have is shown as it is, as the first
		// choice, so that the menu never shows something the card does not
		// say - and saving without touching it writes it back unchanged.
		if (bFound && S.nId != SetDisplay)
		{
			bool bKnown = false;
			for (const TChoice &C : S.Choices) bKnown = bKnown || C.Value == Value;
			if (!bKnown && !Value.empty ())
			{
				// Shown as it is in the file, cut to fit (the whole value is
				// written back unchanged: only the label is shortened).
				S.Choices.insert (S.Choices.begin (), TChoice { Fit (Value, ValueWidth - 2), Value });
				S.nDefault++;
			}
		}

		S.nSaved = bFound ? IndexOf (S, Value) : S.nDefault;
		S.nPending = S.nSaved;
	}
}

bool AnyModified (void)
{
	for (const TSetting &S : s_Settings)
	{
		if (S.nPending != S.nSaved) return true;
	}
	return s_DriveC.Pending != s_DriveC.Saved || s_DriveE.Pending != s_DriveE.Saved
	       || s_BootPending != s_BootSaved || s_WebPassPending != s_WebPassSaved;
}

// B: a change that needs the board restarted (the display mode, the network,
// the audio output...); the web password is one of them.
bool ModifiedNeedsBoard (void)
{
	for (const TSetting &S : s_Settings)
	{
		if (S.nPending != S.nSaved && S.Apply == TApply::RestartBoard) return true;
	}
	return s_WebPassPending != s_WebPassSaved;
}

// R: a change DOSBox reads when it starts - it applies by restarting DOS alone
// (BareCoreRestart: DOSBox unloaded and loaded again, the board keeps running).
// The drives and the boot program are R.
bool ModifiedNeedsDos (void)
{
	for (const TSetting &S : s_Settings)
	{
		if (S.nPending != S.nSaved && S.Apply == TApply::RestartDOSBox) return true;
	}
	return s_DriveC.Pending != s_DriveC.Saved || s_DriveE.Pending != s_DriveE.Saved
	       || s_BootPending != s_BootSaved;
}

void SetPending (TSetting &S, unsigned nChoice)
{
	S.nPending = nChoice;
	if (S.pLive != nullptr)
	{
		S.pLive (S.Choices[nChoice].Value);
	}
}

void Discard (void)
{
	s_DriveC.Pending = s_DriveC.Saved;
	s_DriveE.Pending = s_DriveE.Saved;
	s_BootPending = s_BootSaved;
	s_WebPassPending = s_WebPassSaved;

	for (TSetting &S : s_Settings)
	{
		if (S.nPending != S.nSaved)
		{
			SetPending (S, S.nSaved);
		}
	}
}

void LoadDefaults (void)
{
	s_DriveC.Pending = s_DriveC.Default;
	s_DriveE.Pending = s_DriveE.Default;
	s_BootPending.clear ();
	s_WebPassPending.clear ();

	for (TSetting &S : s_Settings)
	{
		if (S.nPending != S.nDefault && S.nDefault < S.Choices.size ())
		{
			SetPending (S, S.nDefault);
		}
	}
}

/// Write every changed setting to its file.  FALSE if a file could not be
/// written; the settings that were written count as saved either way.
bool SaveSettings (void)
{
	bool bOk = true;
	bool bBoot = false, bDOSBox = false, bCmdline = false;
	for (const TSetting &S : s_Settings)
	{
		if (S.nPending == S.nSaved) continue;
		if (S.nId == SetDisplay) { bCmdline = true; bBoot = true; }
		else if (S.nId == SetWiFiCountry) continue;	// wpa_supplicant.conf, below
		else if (S.File == FileCmdline) bCmdline = true;
		else if (S.File == FileBoot) bBoot = true;
		else if (S.File == FileDOSBox) bDOSBox = true;
	}
	if (s_DriveC.Pending != s_DriveC.Saved || s_DriveE.Pending != s_DriveE.Saved
	    || s_BootPending != s_BootSaved)
	{
		bDOSBox = true;
	}
	if (s_WebPassPending != s_WebPassSaved)
	{
		bBoot = true;
	}

	// The WiFi country lives in wpa_supplicant.conf, which is otherwise written
	// only when a network is chosen: changed on its own, it used to be marked
	// saved and written nowhere.  Its line is changed in place (or added at the
	// top); the rest of the file, network and password included, is untouched.
	if (const TSetting *pCountry = FindSetting (SetWiFiCountry);
	    pCountry != nullptr && pCountry->nPending != pCountry->nSaved)
	{
		std::string Text;
		if (ReadText ("SD:/wpa_supplicant.conf", &Text))
		{
			const std::string &Code = pCountry->Choices[pCountry->nPending].Value;
			size_t n = 0;
			bool bDone = false;
			while (n < Text.size () && !bDone)
			{
				size_t e = Text.find ('\n', n);
				if (e == std::string::npos) e = Text.size ();
				size_t a = n;
				while (a < e && (Text[a] == ' ' || Text[a] == '\t')) a++;
				if (Text.compare (a, 8, "country=") == 0)
				{
					size_t v = a + 8, w = v;
					while (w < e && Text[w] != '\r' && Text[w] != ' ' && Text[w] != '\t') w++;
					Text.replace (v, w - v, Code);
					bDone = true;
				}
				n = e + 1;
			}
			if (!bDone)
			{
				Text = "country=" + Code + "\n" + Text;
			}
			bOk = WriteText ("SD:/wpa_supplicant.conf", Text) && bOk;
		}
	}

	if (bCmdline)
	{
		std::string Text;
		ReadText (CmdlinePath, &Text);
		const TSetting *pDisplay = FindSetting (SetDisplay);
		if (pDisplay->nPending != pDisplay->nSaved)
		{
			const std::string &Mode = pDisplay->Choices[pDisplay->nPending].Value;
			const size_t x = Mode.find ('x');
			Text = bareconf::CmdlineSet (Text, "width", Mode.substr (0, x));
			Text = bareconf::CmdlineSet (Text, "height", Mode.substr (x + 1));
		}
		for (const TSetting &S : s_Settings)
		{
			if (S.File == FileCmdline && S.nId != SetDisplay && S.nPending != S.nSaved)
			{
				Text = bareconf::CmdlineSet (Text, S.pKey, S.Choices[S.nPending].Value);
			}
		}
		bOk = WriteText (CmdlinePath, Text) && bOk;
	}

	if (bBoot)
	{
		std::string Text;
		ReadText (BootPath, &Text);
		bareconf::CConfText Conf (Text, true);
		for (const TSetting &S : s_Settings)
		{
			if (S.nPending == S.nSaved) continue;
			if (S.nId == SetDisplay) Conf.Set ("", "display", S.Choices[S.nPending].Value);
			else if (S.nId == SetWiFiCountry) continue;	// lives in wpa_supplicant.conf

			else if (S.File == FileBoot) Conf.Set (S.pSection, S.pKey, S.Choices[S.nPending].Value);
			if (S.nId == SetNetIP && !s_NetCurrentChoice.empty ()
			    && S.Choices[S.nPending].Value == s_NetCurrentChoice)
			{
				if (!s_NetCurrentGateway.empty ()) Conf.Set ("", "gateway", s_NetCurrentGateway);
				if (!s_NetCurrentDNS.empty ()) Conf.Set ("", "dns", s_NetCurrentDNS);
			}
		}
		if (s_WebPassPending != s_WebPassSaved)
		{
			Conf.Set ("", "web_password", s_WebPassPending);
		}
		bOk = WriteText (BootPath, Conf.Text ()) && bOk;
	}

	if (bDOSBox)
	{
		std::string Text;
		ReadText (DOSBoxPath, &Text);
		bareconf::CConfText Conf (Text, false);
		for (const TSetting &S : s_Settings)
		{
			if (S.nId == SetSoundFont || S.nId == SetMT32Model)
			{
				continue;		// written below, for the device chosen
			}
			if (S.nId == SetKeyb)
			{
				if (S.nPending != S.nSaved)
				{
					bareconf::SetCommand (Conf, "KEYB", S.Choices[S.nPending].Value);
				}
				continue;
			}
			if (S.nPending != S.nSaved && S.File == FileDOSBox)
			{
				Conf.Set (S.pSection, S.pKey, S.Choices[S.nPending].Value);
				if (S.nId == SetCycleStep)
				{
					Conf.Set (S.pSection, "cycledown", S.Choices[S.nPending].Value);
				}
			}
		}
		// The folder drives: MOUNT lines in [autoexec].
		for (TFolderDrive *pDrive : { &s_DriveC, &s_DriveE })
		{
			if (pDrive->Pending != pDrive->Saved)
			{
				bareconf::SetMount (Conf, pDrive->cLetter, pDrive->Pending);
			}
		}

		// What runs at boot: the menu's block at the end of [autoexec].
		if (s_BootPending != s_BootSaved)
		{
			std::vector<std::string> Lines;
			if (!s_BootPending.empty ())
			{
				const size_t n = s_BootPending.find_last_of ('\\');
				Lines.push_back ("C:");
				Lines.push_back (n == std::string::npos ? "CD \\"
					       : "CD \\" + s_BootPending.substr (0, n));
				Lines.push_back (n == std::string::npos ? s_BootPending : s_BootPending.substr (n + 1));
			}
			bareconf::SetBlock (Conf, Lines);
		}

		// midiconfig: the SoundFont for General MIDI, the model for MT-32;
		// left alone with no device (it may hold something set by hand).
		{
			const TSetting *pDev = FindSetting (SetMidiDevice);
			const TSetting *pFont = FindSetting (SetSoundFont);
			const TSetting *pModel = FindSetting (SetMT32Model);
			const std::string &Dev = pDev->Choices[pDev->nPending].Value;
			const bool bTouched = pDev->nPending != pDev->nSaved || pFont->nPending != pFont->nSaved
					      || pModel->nPending != pModel->nSaved;
			if (bTouched && Dev == "fluidsynth")
			{
				Conf.Set ("midi", "midiconfig", pFont->Choices[pFont->nPending].Value);
			}
			else if (bTouched && Dev == "mt32")
			{
				Conf.Set ("midi", "midiconfig", pModel->Choices[pModel->nPending].Value);
			}
		}

		bOk = WriteText (DOSBoxPath, Conf.Text ()) && bOk;
	}

	for (TSetting &S : s_Settings)
	{
		S.nSaved = S.nPending;
	}
	s_DriveC.Saved = s_DriveC.Pending;
	s_DriveE.Saved = s_DriveE.Pending;
	s_BootSaved = s_BootPending;
	s_WebPassSaved = s_WebPassPending;

	CLogger::Get ()->Write (FromMenu, bOk ? LogNotice : LogError,
				bOk ? "settings saved" : "settings: a file could not be written");
	return bOk;
}

TItem SettingItem (const TSetting &S)
{
	TItem Item;
	Item.Kind = TKind::Choice;
	Item.Label = S.pLabel;
	Item.Help = S.pHelp;
	Item.Apply = S.Apply;
	Item.nId = S.nId;
	for (const TChoice &C : S.Choices) Item.Choices.push_back (C.Label);
	Item.nChoice = S.nPending;
	Item.bModified = S.nPending != S.nSaved;
	return Item;
}

// --- texts -----------------------------------------------------------------

std::string Format (const char *pFormat, ...)
{
	char Buffer[160];
	va_list Args;
	va_start (Args, pFormat);
	vsnprintf (Buffer, sizeof Buffer, pFormat, Args);
	va_end (Args);
	return Buffer;
}

// A name the menu does not choose (a WiFi network, a file) cut to a width the
// DOS way, "Monkey Island 2 - LeCh~"; the caller puts the whole name in the
// help pane, which has room for it.
std::string Fit (const std::string &Text, unsigned nWidth)
{
	if (CScreen::Length (Text) <= nWidth)
	{
		return Text;
	}
	std::string Out;
	size_t i = 0;
	for (unsigned n = 0; n + 1 < nWidth && i < Text.size (); n++)
	{
		CScreen::Encode (Out, CScreen::Decode (Text, i));
	}
	return Out + "~";
}


TItem Info (const std::string &Label, const std::string &Value, const std::string &Help = "")
{
	TItem Item;
	Item.Kind = TKind::Info;
	Item.Label = Label;
	Item.Value = Value;
	Item.Help = Help;
	return Item;
}

TItem Section (const std::string &Label)
{
	TItem Item;
	Item.Kind = TKind::Section;
	Item.Label = Label;
	return Item;
}

TItem Action (const std::string &Label, int nId, const std::string &Help)
{
	TItem Item;
	Item.Kind = TKind::Action;
	Item.Label = Label;
	Item.nId = nId;
	Item.Help = Help;
	return Item;
}

TItem Planned (const std::string &Label, const std::string &Help, TApply Apply = TApply::Now)
{
	TItem Item;
	Item.Kind = TKind::Planned;
	Item.Label = Label;
	Item.Value = "próximamente";
	Item.Help = Help;
	Item.Apply = Apply;
	return Item;
}

// --- the pages -------------------------------------------------------------

TPage PageSistema (void)
{
	TPage P;
	P.Title = "Sistema";
	P.Help = "Lo que la placa y el port están haciendo ahora mismo. Los valores se "
		 "actualizan cada segundo.";

	unsigned nArmMHz = 0, nCoreMHz = 0, nCelsius = 0;
	BareSysInfoGet (&nArmMHz, &nCoreMHz, &nCelsius);

	const unsigned nFirmware = BareSysInfoFirmwareMode ();
	std::string Power = "normal";
	if (nFirmware & 0x01) Power = "¡TENSIÓN BAJA!";
	else if (nFirmware & 0x08) Power = "límite de temperatura";
	else if (nFirmware & 0x06) Power = "reloj recortado";
	else if (nFirmware & 0x10000) Power = "bien; hubo tensión baja";

	const u64 nSeconds = BareMicros () / 1000000;

	unsigned nModeW = 0, nModeH = 0;
	BareVideoModeSize (&nModeW, &nModeH);

	TBareVideoPresenterStats Video;
	BareVideoPresenterStats (&Video);

	const char *pMidi = BareMidiActive () ? BareMidiName () : "ninguno";

	P.Items =
	{
		Section ("BM-DOS"),
		Info ("Versión", BMDOS_COMMIT,
		      "El commit de git del que salió este kernel: lo que distingue un kernel "
		      "de otro.  La instantánea es el sello de versión del proyecto, que se "
		      "cambia a mano."),
		Info ("Instantánea", DOSBOX_SNAPSHOT),
		Info ("Emulador", "DOSBox-core (libretro)"),
		Info ("Encendido desde hace", Format ("%llu:%02llu:%02llu",
			(unsigned long long) (nSeconds / 3600),
			(unsigned long long) (nSeconds / 60 % 60),
			(unsigned long long) (nSeconds % 60))),

		Section ("Placa"),
		Info ("Modelo", BareSysInfoBoardName ()),
		Info ("Memoria", Format ("%u MB", static_cast<unsigned> (
			CMemorySystem::Get ()->GetMemSize () / (1024 * 1024)))),
		Info ("Reloj ARM", Format ("%u MHz", nArmMHz)),
		Info ("Governor", BareSysInfoGovernorState ()),
		Info ("Temperatura", Format ("%u °C", nCelsius)),
		Info ("Alimentación", Power, "Lo que dice el firmware: con tensión baja la placa "
			"no sube el reloj aunque el governor lo pida. Usa una fuente oficial."),

		Section ("Núcleos"),
		Info ("Núcleo 0", "DOSBox, E/S y audio",
			"DOSBox no se puede repartir entre núcleos: corre entero en el 0, junto con el teclado, el ratón, los mandos y la mezcla de audio."),
		Info ("Núcleo 1", BareCoresIsRunning (1)
			? (BareMidiActive () ? Format ("MIDI (%s)", pMidi) : std::string ("MIDI (en espera)"))
			: std::string ("libre")),
		Info ("Núcleo 2", Video.bRunning
			? std::string (Video.bVsync ? "vídeo, con vsync" : "vídeo, sin vsync")
			: std::string ("libre"),
			Video.bRunning ? Format ("Escala y presenta la imagen. %llu fotogramas mostrados.",
						 (unsigned long long) Video.nShown) : std::string ()),
		Info ("Núcleo 3", "libre"),

		Section ("Pantalla y sonido"),
		Info ("Resolución", Format ("%ux%u", nModeW, nModeH)),
		Info ("Escalado", BareVideoSharpScaling () ? "nítido" : "suave"),
		Info ("Salida de audio", BareAudioDeviceName ()),
		Info ("MIDI", !BareMidiActive () ? std::string ("apagado")
			      : strcmp (BareMidiName (), "mt32") == 0 ? std::string ("Roland MT-32")
			      : std::string ("General MIDI"),
		      BareMidiStatus ()),
	};
	return P;
}


TPage PageSalir (void)
{
	TPage P;
	P.Title = "Salir";
	P.Help = "Volver al juego, o apagar y reiniciar con seguridad.";
	P.Items =
	{
		Action ("Volver al juego", ActResume,
			"Cierra el menú y DOS sigue donde estaba. También con Esc."),
		Action ("Apagado seguro", ActShutdown,
			"Vacía la tarjeta y detiene la placa, como Ctrl+F9. Espera al mensaje "
			"antes de quitar la corriente."),
		Action ("Reiniciar DOS", ActRestartDos,
			"Como Ctrl+Alt+Supr en MS-DOS: DOS empieza de nuevo (dosbox.conf y su "
			"autoexec otra vez) en unos segundos, sin reiniciar la placa. Los cambios "
			"sin guardar del menú se descartan."),
		Action ("Reiniciar la placa", ActReboot,
			"Reinicia la Raspberry Pi, como Ctrl+Alt+PetSis."),
		Section (""),
		Action ("Guardar y volver", ActSave,
			"Guarda los cambios en boot.conf, dosbox.conf y cmdline.txt (solo las líneas "
			"que cambian; los comentarios se conservan) y vuelve al juego. Igual que F10."),
		Action ("Guardar y reiniciar", ActSaveRestart,
			"Guarda y reinicia: DOS si los cambios son R, la placa si alguno es B."),
		Action ("Salir sin guardar", ActDiscard,
			"Descarta los cambios hechos en el menú, deshace los que se aplicaron al "
			"momento y vuelve al juego."),
		Action ("Valores por defecto", ActDefaults,
			"Pone los valores de fábrica en el menú (F9). No se guardan hasta que guardes."),
		Planned ("Actualizar desde GitHub", "Buscará una versión nueva en las releases."),
	};
	return P;
}

// --- WiFi --------------------------------------------------------------------------

void BuildPages (void);

constexpr unsigned PageIndexRed = 7;		// ..., Rendimiento, Red, Arranque, Salir

bool WiFiTextOk (const std::string &Text)
{
	// wpa_supplicant reads ssid="..." and psk="..." up to the LAST quote of the
	// line (config.c, wpa_config_parse_string: os_strrchr), with no escapes: a
	// quote or a backslash inside is taken as it is.  Only a line end cannot
	// be there.  (This refused quotes and backslashes; on a Spanish keyboard
	// Shift+2 is a quote.)
	for (unsigned char c : Text)
	{
		if (c < 0x20 || c == 0x7F) return false;
	}
	return true;
}

bool WriteWiFiConfig (const std::string &SSID, const std::string &Password, bool bSecure)
{
	const TSetting *pCountry = FindSetting (SetWiFiCountry);
	const std::string Country = pCountry->Choices[pCountry->nPending].Value;

	std::string Text =
		"# wpa_supplicant.conf - written by the BM-DOS setup menu (Win+F12, Red).\n"
		"# The WiFi network the board joins with boot.conf network = wifi.  It can\n"
		"# also be edited by hand on a PC; the menu rewrites it when a network is\n"
		"# chosen there.\n"
		"\n"
		"country=" + Country + "\n"
		"\n"
		"network={\n"
		"\tssid=\"" + SSID + "\"\n";
	if (bSecure)
	{
		Text += "\tpsk=\"" + Password + "\"\n"
			"\tproto=RSN WPA\n"
			"\tkey_mgmt=WPA-PSK\n";
	}
	else
	{
		Text += "\tkey_mgmt=NONE\n";
	}
	Text += "}\n";
	return WriteText ("SD:/wpa_supplicant.conf", Text);
}

void WiFiJoin (const std::string &Password)
{
	if (!WiFiTextOk (s_WiFiSSID) || !WiFiTextOk (Password))
	{
		s_Menu.OpenDialog (DlgInfo, "No se puede guardar",
				   "El nombre o la contraseña tienen un carácter de control, que "
				   "wpa_supplicant.conf no admite.", { "Aceptar" });
		return;
	}
	if (!WriteWiFiConfig (s_WiFiSSID, Password, s_bWiFiSecure))
	{
		s_Menu.OpenDialog (DlgSaveFailed, "No se pudo guardar",
				   "No se pudo escribir SD:/wpa_supplicant.conf en la tarjeta.", { "Aceptar" });
		return;
	}

	// The connection itself: WiFi on, and a restart.
	TSetting *pNet = FindSetting (SetNetwork);
	for (unsigned i = 0; i < pNet->Choices.size (); i++)
	{
		if (pNet->Choices[i].Value == "wifi") pNet->nPending = i;
	}
	s_bWiFiList = false;
	BuildPages ();
	s_Menu.GoToPage (PageIndexRed);
	s_Menu.OpenDialog (DlgInfo, "Red WiFi guardada",
			   "Guardada \"" + s_WiFiSSID + "\" en wpa_supplicant.conf. Guarda con F10 (la "
			   "conexión queda en WiFi) y reinicia: la placa se conectará al arrancar.",
			   { "Aceptar" });
}

TPage PageWiFi (void)
{
	TPage P;
	P.Title = "Elegir red WiFi";
	P.Help = "Las redes que ha encontrado el WiFi de la placa. Esc vuelve a Red.";

	s_WiFiNetworks.resize (24);
	s_WiFiNetworks.resize (BareNetWiFiNetworks (s_WiFiNetworks.data (), 24));

	P.Items.push_back (Section ("Redes encontradas"));
	if (BareNetMode () != BareNetWiFi)
	{
		P.Items.push_back (Info ("(el WiFi está apagado)", "",
			"Para buscar redes el WiFi tiene que estar encendido: pon Conexión en WiFi, "
			"guarda y reinicia; aunque no haya ninguna red configurada, la placa busca "
			"las que tiene alrededor. Mientras, puedes escribir el nombre a mano."));
	}
	else if (s_WiFiNetworks.empty ())
	{
		P.Items.push_back (Info ("(ninguna todavía)", "",
			"El WiFi busca cada pocos segundos. Usa \"Buscar de nuevo\" en un momento."));
	}
	for (size_t i = 0; i < s_WiFiNetworks.size (); i++)
	{
		const TBareWiFiNetwork &N = s_WiFiNetworks[i];
		const int nRSSI = N.nRSSI;
		const char *pBars = nRSSI >= -55 ? "||||" : nRSSI >= -67 ? "|||." : nRSSI >= -75 ? "||.." : "|...";
		TItem Item = Action (Fit (N.SSID, LabelWidth), ActWiFiEntry + static_cast<int> (i),
			std::string (N.SSID) + "\n\n"
			+ (N.bSecure ? "Intro: escribir la contraseña de esta red."
				     : "Red abierta, sin contraseña. Intro: elegirla."));
		Item.Value = Format ("%s %d dBm %s", pBars, nRSSI, N.bSecure ? "clave" : "abierta");
		P.Items.push_back (Item);
	}
	P.Items.push_back (Section (""));
	P.Items.push_back (Action ("Buscar de nuevo", ActWiFiRescan, "Actualiza la lista."));
	P.Items.push_back (Action ("Escribir el nombre", ActWiFiManual,
		"Escribir a mano el nombre de la red: para una red oculta, o si aún no "
		"aparece en la lista."));
	return P;
}

// --- browser ---------------------------------------------------------------------

bool BrowseWants (const std::string &Name, bool bFolder)
{
	if (bFolder) return true;
	const std::string L = bareconf::Lower (Name);
	auto Ends = [&] (const char *pExt) { const size_t n = strlen (pExt);
					      return L.size () > n && L.compare (L.size () - n, n, pExt) == 0; };
	switch (s_Browse)
	{
	case TBrowse::Floppy:	return Ends (".img");		// what the core's disk control mounts in A:
	case TBrowse::CD:	return Ends (".iso") || Ends (".cue");
	case TBrowse::Game:	return Ends (".exe") || Ends (".com") || Ends (".bat");
	default:		return false;			// folder modes list folders only
	}
}

void BrowseRead (void)
{
	s_BrowseEntries.clear ();
	DIR *pDir = opendir (("SD:" + s_BrowsePath).c_str ());
	if (pDir != nullptr)
	{
		while (const dirent *pEntry = readdir (pDir))
		{
			const std::string Name = pEntry->d_name;
			if (Name.empty () || Name[0] == '.') continue;
			// Circle's newlib has no d_type (FatFs's FILINFO is not passed
			// on), so the entry is asked about by name.
			struct stat Info;
			const std::string Full = "SD:" + (s_BrowsePath == "/" ? "/" + Name : s_BrowsePath + "/" + Name);
			const bool bFolder = stat (Full.c_str (), &Info) == 0 && S_ISDIR (Info.st_mode);
			if (BrowseWants (Name, bFolder) && s_BrowseEntries.size () < 400)
			{
				s_BrowseEntries.push_back ({ Name, bFolder });
			}
		}
		closedir (pDir);
	}

	// Folders first, then files, each by name without regard to case.
	std::sort (s_BrowseEntries.begin (), s_BrowseEntries.end (),
		   [] (const auto &a, const auto &b)
		   {
			   if (a.second != b.second) return a.second;
			   return bareconf::Lower (a.first) < bareconf::Lower (b.first);
		   });
}

void BrowseOpen (TBrowse Mode, const std::string &Start)
{
	s_Browse = Mode;
	s_BrowsePath = Start.empty () || Start[0] != '/' ? "/" : Start;

	// Start where the current value is: for an image, in its folder.
	if (Mode == TBrowse::Floppy || Mode == TBrowse::CD)
	{
		const size_t n = s_BrowsePath.find_last_of ('/');
		s_BrowsePath = n == 0 ? "/" : s_BrowsePath.substr (0, n);
	}

	BrowseRead ();
	if (s_BrowseEntries.empty () && s_BrowsePath != "/")
	{
		s_BrowsePath = "/";		// the folder is gone: start at the root
		BrowseRead ();
	}
}

std::string BrowseJoin (const std::string &Name)
{
	return s_BrowsePath == "/" ? "/" + Name : s_BrowsePath + "/" + Name;
}

TPage PageBrowser (void)
{
	TPage P;
	const bool bFolders = s_Browse == TBrowse::FolderC || s_Browse == TBrowse::FolderE;
	P.Title = s_Browse == TBrowse::FolderC ? "Elegir carpeta para C:"
		: s_Browse == TBrowse::FolderE ? "Elegir carpeta para E:"
		: s_Browse == TBrowse::Floppy ? "Elegir disquete (.img) para A:"
		: s_Browse == TBrowse::Game ? "Elegir el programa de arranque (en C:)"
		: "Elegir CD (.iso / .cue) para D:";
	P.Help = "Intro abre una carpeta. Esc vuelve sin elegir nada.";

	P.Items.push_back (Section (Fit (s_BrowsePath, CMenu::HelpCol - 2 - CMenu::LabelCol)));
	if (bFolders)
	{
		P.Items.push_back (Action ("[ Elegir esta carpeta ]", ActBrowsePick,
			Format ("La unidad apuntará a %s.", s_BrowsePath.c_str ())));
	}
	if (s_BrowsePath != "/")
	{
		P.Items.push_back (Action ("..", ActBrowseUp, "La carpeta de arriba."));
	}
	for (size_t i = 0; i < s_BrowseEntries.size (); i++)
	{
		const auto &E = s_BrowseEntries[i];
		const std::string Name = E.second ? E.first + "/" : E.first;
		P.Items.push_back (Action (Fit (Name, LabelWidth),
					   ActBrowseEntry + static_cast<int> (i),
					   Name + "\n\n"
					   + (E.second ? "Intro: entrar en la carpeta."
						       : s_Browse == TBrowse::Game
							 ? "Intro: arrancar este programa al encender."
							 : "Intro: meter esta imagen en la unidad, al momento.")));
	}
	if (s_BrowseEntries.empty ())
	{
		P.Items.push_back (Info (bFolders ? "(no hay carpetas)"
					 : s_Browse == TBrowse::Game ? "(no hay programas aquí)"
					 : "(no hay imágenes aquí)", ""));
	}
	return P;
}

TPage PageUnidades (void)
{
	TPage P;
	P.Title = "Unidades";
	P.Help = "Carpetas de la tarjeta como discos duros, y disquetes y CD como imágenes.";

	auto Folder = [] (const TFolderDrive &D) { return D.Pending.empty () ? std::string ("(ninguna)") : D.Pending; };

	TItem C = Action ("Unidad C:", ActDriveC,
		"La carpeta de la tarjeta que DOS ve como C:. Intro abre el explorador. Se guarda "
		"como MOUNT C en el [autoexec] de dosbox.conf, conservando sus opciones.");
	C.Value = Folder (s_DriveC);
	C.Apply = TApply::RestartDOSBox;
	C.bModified = s_DriveC.Pending != s_DriveC.Saved;

	TItem E = Action ("Unidad E:", ActDriveE,
		"Una segunda carpeta como E:, por ejemplo para tener juegos y utilidades por "
		"separado. Se guarda como MOUNT E, justo después de MOUNT C.");
	E.Value = Folder (s_DriveE);
	E.Apply = TApply::RestartDOSBox;
	E.bModified = s_DriveE.Pending != s_DriveE.Saved;

	P.Items = { Section ("Discos duros (carpetas)"), C, E };
	if (!s_DriveE.Pending.empty ())
	{
		TItem Remove = Action ("Quitar E:", ActDriveERemove, "Deja de montar E: al arrancar.");
		Remove.Apply = TApply::RestartDOSBox;
		P.Items.push_back (Remove);
	}

	P.Items.push_back (Section ("Disquete y CD-ROM (al momento)"));
	if (BareDiskAvailable ())
	{
		TItem A = Action ("Disquetera A:", ActFloppy,
			"Mete una imagen de disquete (.img) en A: sin reiniciar: como cambiar el "
			"disco en un PC de verdad. No se guarda: al reiniciar, A: vuelve a estar "
			"como diga el autoexec.");
		A.Value = s_FloppyImage.empty () ? "(vacía)" : s_FloppyImage.substr (s_FloppyImage.find_last_of ('/') + 1);
		TItem D = Action ("CD-ROM D:", ActCD,
			"Mete una imagen de CD (.iso o .cue con su .bin) en D: sin reiniciar. Para "
			"juegos de varios CD, vuelve aquí cuando pidan el siguiente.");
		D.Value = s_CDImage.empty () ? "(vacío)" : s_CDImage.substr (s_CDImage.find_last_of ('/') + 1);
		P.Items.push_back (A);
		P.Items.push_back (Action ("Expulsar disquete", ActFloppyEject, "Saca el disquete de A:."));
		P.Items.push_back (D);
		P.Items.push_back (Action ("Expulsar CD", ActCDEject, "Saca el CD de D:."));
	}
	else
	{
		P.Items.push_back (Info ("Disquetera A:", "no disponible",
			"El core no ofreció su control de discos; los disquetes y CD siguen pudiendo "
			"montarse con IMGMOUNT en el autoexec."));
	}
	return P;
}

void BuildPages (void)
{
	if (s_bWiFiList)
	{
		s_Menu.Pages = { PageWiFi () };
		s_Menu.Refresh ();
		return;
	}

	if (s_Browse != TBrowse::None)
	{
		s_Menu.Pages = { PageBrowser () };
		s_Menu.Refresh ();
		return;
	}

	std::vector<TPage> Pages;
	Pages.push_back (PageSistema ());

	{
		TPage P;
		P.Title = "Máquina";
		P.Help = "El PC que emula DOSBox. Se guarda en dosbox.conf.";
		P.Items =
		{
			Section ("PC emulado"),
			SettingItem (*FindSetting (SetMachine)),
			SettingItem (*FindSetting (SetMemSize)),
			Section ("Procesador"),
			SettingItem (*FindSetting (SetCPUCore)),
			SettingItem (*FindSetting (SetCPUType)),
			SettingItem (*FindSetting (SetCycles)),
			SettingItem (*FindSetting (SetCycleStep)),
			Info ("Ciclos ahora", CPU_CycleAutoAdjust ? std::string ("automático")
						: Format ("%d", static_cast<int> (CPU_CycleMax)),
			      "Los que usa la máquina en este momento, Ctrl+F11/F12 incluidos."),
		};
		Pages.push_back (P);
	}

	Pages.push_back (PageUnidades ());

	{
		TPage P;
		P.Title = "Sonido";
		P.Help = "Tarjetas de sonido, MIDI y volumen.";
		P.Items =
		{
			Section ("Salida"),
			SettingItem (*FindSetting (SetAudioOut)),
			SettingItem (*FindSetting (SetVolume)),
			Action ("Probar sonido", ActTestSound,
				"Un pitido por la izquierda y otro por la derecha, con el volumen "
				"general aplicado. Si no lo oyes, mira la salida y el volumen."),
			Section ("Tarjetas"),
			SettingItem (*FindSetting (SetSBType)),
			SettingItem (*FindSetting (SetSBBase)),
			SettingItem (*FindSetting (SetSBIrq)),
			SettingItem (*FindSetting (SetSBDma)),
			SettingItem (*FindSetting (SetSBHdma)),
			SettingItem (*FindSetting (SetOPL)),
			SettingItem (*FindSetting (SetGUS)),
			SettingItem (*FindSetting (SetSpeaker)),
			SettingItem (*FindSetting (SetTandy)),
			Section ("MIDI"),
			SettingItem (*FindSetting (SetMidiDevice)),
			SettingItem (*FindSetting (SetMpu)),
			SettingItem (*FindSetting (SetSoundFont)),
			SettingItem (*FindSetting (SetMT32Model)),
			SettingItem (*FindSetting (SetMidiVolume)),
			Action ("Probar MIDI", ActTestMidi,
				BareMidiActive ()
				? "Un acorde de piano por el sintetizador que está funcionando ahora, "
				  "con el volumen del MIDI aplicado."
				: "Ahora no hay sintetizador MIDI funcionando: elige un dispositivo, "
				  "guarda y reinicia."),
		};
		Pages.push_back (P);
	}

	{
		TPage P;
		P.Title = "Vídeo";
		P.Help = "Resolución, presentación y escalado.";
		P.Items =
		{
			Section ("Pantalla"),
			SettingItem (*FindSetting (SetDisplay)),
			Section ("Presentación"),
			SettingItem (*FindSetting (SetVideoCore)),
			SettingItem (*FindSetting (SetVsync)),
			SettingItem (*FindSetting (SetScaler)),
			Planned ("Corrección de aspecto", "Llegará en un paso próximo del menú."),
			Planned ("Panel de estado", "Llegará en un paso próximo del menú."),
		};
		Pages.push_back (P);
	}

	{
		TPage P;
		P.Title = "Entrada";
		P.Help = "Teclado, ratón, joystick y mandos.";
		const unsigned nPads = BareInputGamepads ();
		P.Items =
		{
			Section ("Teclado y ratón"),
			SettingItem (*FindSetting (SetKeyb)),
			SettingItem (*FindSetting (SetMouseSpeed)),
			Section ("Joystick"),
			SettingItem (*FindSetting (SetJoyType)),
			SettingItem (*FindSetting (SetJoyTimed)),
			SettingItem (*FindSetting (SetJoyDeadzone)),
			Info ("Mandos USB conectados", nPads == 0 ? std::string ("ninguno")
							   : Format ("%u", nPads),
			      "Los mandos que la placa ve ahora. Si conectas uno con el juego en marcha, "
			      "aparece aquí."),
			Planned ("Asignar botones", "Llegará en un paso próximo del menú: qué botón del "
				 "mando es cada botón del joystick, guardado por mando."),
		};
		Pages.push_back (P);
	}

	{
		TPage P;
		P.Title = "Rendimiento";
		P.Help = "Reloj de la placa y temperatura.";

		unsigned nArmMHz = 0, nCoreMHz = 0, nCelsius = 0;
		BareSysInfoGet (&nArmMHz, &nCoreMHz, &nCelsius);
		unsigned Levels[8] = {};
		const unsigned nLevels = BareSysInfoGovernorLevels (Levels);
		TBareGovernorStats Gov;
		BareSysInfoGovernorStats (&Gov);

		std::string LevelText;
		for (unsigned i = 0; i < nLevels && i < 8; i++)
		{
			LevelText += (i ? " / " : "") + Format ("%u", Levels[i]);
		}

		P.Items =
		{
			Section ("Governor"),
			SettingItem (*FindSetting (SetGovMode)),
			SettingItem (*FindSetting (SetCPUMax)),
			SettingItem (*FindSetting (SetSocMaxTemp)),
			Section ("Ahora"),
			Info ("Reloj ARM", Format ("%u MHz", nArmMHz)),
			Info ("Niveles (MHz)", LevelText.empty () ? std::string ("-") : LevelText,
			      "Los relojes entre los que se mueve el governor en esta placa."),
			Section ("Lo que hace el governor"),
			Info ("Carga", Gov.nLevels != 0 ? Format ("%u %% de cada fotograma", Gov.nLoadPercent)
							  : std::string ("-"),
			      "El trabajo de cada fotograma emulado frente al tiempo que tiene (14,3 ms "
			      "a 70 Hz), en media.  Por encima del 90 % el fotograma no llega y, si "
			      "sigue así, el governor sube el reloj; por debajo del 75 % va holgado y "
			      "lo baja al cabo de un rato."),
			Info ("Cambios", Format ("%u arriba, %u abajo", Gov.nUps, Gov.nDowns),
			      "Cuántas veces ha subido y bajado el reloj desde el arranque."),
			Info ("Último cambio", Gov.pLastReason != nullptr && Gov.pLastReason[0] != '\0'
					 ? std::string (Gov.pLastReason) : std::string ("ninguno todavía"),
			      "Por qué cambió el reloj la última vez: no llega, va holgado, pantalla "
			      "quieta, prompt de DOS, temperatura, el modo fijo, programa arrancado "
			      "(10 s en el máximo al empezar un programa) o huecos de audio (el "
			      "sonido se quedó sin datos: sube un nivel al momento)."),
			Info ("  hace", Gov.pLastReason != nullptr && Gov.pLastReason[0] != '\0'
					? Format ("%u:%02u:%02u", Gov.nSecondsSinceChange / 3600,
						  Gov.nSecondsSinceChange / 60 % 60, Gov.nSecondsSinceChange % 60)
					: std::string ("-")),
			Info ("Estado", BareSysInfoGovernorState (),
			      "Lo mismo que muestra el panel (PetSis): el nivel, y si el emulador va "
			      "holgado o justo."),
			Info ("Temperatura", Format ("%u °C", nCelsius)),
		};

		// The time spent at each clock, one line each (three levels did not
		// fit on one line).
		// Where core 0's time went in the last second before the menu was
		// opened (item 5: is the sound worth moving to a free core?).
		{
			TBareProfile Prof;
			BareProfileGet (&Prof);
			auto Pct = [] (unsigned nPermille)
				{ return Format ("%u,%u %%", nPermille / 10, nPermille % 10); };
			P.Items.push_back (Section ("Dónde se va el tiempo (núcleo 0)"));
			if (!Prof.bValid)
			{
				P.Items.push_back (Info ("  (aún sin medir)", "",
					"Hace falta un segundo entero con DOS en marcha. Vuelve a abrir el "
					"menú desde un juego."));
			}
			else
			{
				const char *pWhen = "El último segundo antes de abrir el menú, con DOS en marcha.";
				P.Items.push_back (Info ("  CPU x86", Pct (Prof.nCpu),
					std::string ("El procesador emulado y el resto de DOSBox. ") + pWhen));
				P.Items.push_back (Info ("  Sonido de DOSBox", Pct (Prof.nSound),
					std::string ("Lo que cuestan las tarjetas de sonido emuladas, sumadas: es lo "
						     "que se ganaría moviéndolas a un núcleo libre. ") + pWhen));
				for (unsigned i = 0; i < Prof.nChannels; i++)
				{
					P.Items.push_back (Info (Format ("    %s", Prof.pChannelName[i]), Pct (Prof.nChannel[i]),
						"Un canal del mezclador de DOSBox: FM es la música AdLib (OPL), SB "
						"los efectos digitales, GUS la Gravis, SPKR el altavoz."));
				}
				P.Items.push_back (Info ("  Vídeo", Pct (Prof.nVideo),
					"Entregar cada imagen (con el vídeo en el núcleo 2, solo copiarla)."));
				P.Items.push_back (Info ("  Audio y MIDI", Pct (Prof.nAudio),
					"La cola de salida del port, con la mezcla del MIDI del núcleo 1."));
				P.Items.push_back (Info ("  Teclado y ratón", Pct (Prof.nInput)));
				P.Items.push_back (Info ("  Esperando", Pct (Prof.nWait),
					"El metrónomo frenando la máquina para ir a tiempo real: tiempo libre."));
				P.Items.push_back (Info ("  Otros", Pct (Prof.nOther),
					"El bucle del port, el panel, la red y el servidor web."));
			}
		}

		P.Items.push_back (Section ("Tiempo en cada reloj"));
		for (unsigned i = 0; i < Gov.nLevels && i < 8; i++)
		{
			P.Items.push_back (Info (Format ("  %u MHz", Gov.nMHz[i]), Format ("%u %%", Gov.nPercentAt[i]),
						 "Qué parte del tiempo desde el arranque ha pasado en este reloj."));
		}
		Pages.push_back (P);
	}

	{
		TPage P;
		P.Title = "Red";
		P.Help = "La red de la placa, y lo que DOS verá de ella.";

		TBareNetInfo Net;
		BareNetInfo (&Net);
		auto Or = [] (const char *p) { return std::string (p[0] ? p : "-"); };
		char Now[48];
		BareNetLocalTime (Now, sizeof Now);

		// The WiFi network named in wpa_supplicant.conf, for reference.
		std::string SSID = "(falta el fichero)";
		{
			std::string Text;
			if (ReadText ("SD:/wpa_supplicant.conf", &Text))
			{
				SSID = "(sin red en el fichero)";
				const size_t a = Text.find ("ssid=\"");
				if (a != std::string::npos)
				{
					const size_t b = Text.find ('"', a + 6);
					if (b != std::string::npos) SSID = Text.substr (a + 6, b - a - 6);
				}
			}
		}

		P.Items =
		{
			Section ("La placa"),
			SettingItem (*FindSetting (SetNetwork)),
			SettingItem (*FindSetting (SetNetIP)),
			Info ("Red WiFi", Fit (SSID, ValueWidth), "La red de SD:/wpa_supplicant.conf:\n" + SSID),
			Action ("Elegir red WiFi", ActWiFiList,
				"Las redes que ve la placa, de la más fuerte a la más débil; eliges una y "
				"escribes su contraseña. Se guarda en wpa_supplicant.conf."),
			SettingItem (*FindSetting (SetWiFiCountry)),
			Section ("Estado"),
			Info ("Estado", BareNetReady () ? std::string ("conectada")
					: BareNetMode () == BareNetOff ? std::string ("apagada")
					: std::string ("sin conexión"),
			      BareNetState ()),
			Info ("Dirección", Or (Net.IP)),
			Info ("Máscara", Or (Net.Mask)),
			Info ("Puerta de enlace", Or (Net.Gateway)),
			Info ("DNS", Or (Net.DNS)),
			Info ("MAC", Or (Net.MAC)),
			Info ("Nombre en la red", Or (Net.Hostname)),
			Section ("Hora"),
			SettingItem (*FindSetting (SetTimeZone)),
			Info ("Hora ahora", Now, "La de la red (NTP, pool.ntp.org) en la zona elegida. "
			      "Sin red, la placa no sabe la hora: DOS empieza en 1970."),
			Section ("Para DOS: puertos serie y juego en red"),
			SettingItem (*FindSetting (SetSerial1)),
			SettingItem (*FindSetting (SetSerial2)),
			SettingItem (*FindSetting (SetIPX)),
			Section ("Servidor web"),
			SettingItem (*FindSetting (SetWebServer)),
			[] {
				TItem Pass = Action ("Contraseña", ActWebPassword,
					"La que pedirá el navegador (cualquier usuario). Vacía: sin "
					"contraseña. Hasta 32 caracteres, sin espacios ni #. Se guarda "
					"en boot.conf (web_password).");
				Pass.Value = s_WebPassPending.empty () ? "(ninguna)" : "********";
				Pass.Apply = TApply::RestartBoard;
				Pass.bModified = s_WebPassPending != s_WebPassSaved;
				return Pass;
			} (),
			Info ("Dirección", BareWebRunning () && BareNetReady () && Net.IP[0] != '\0'
				? Format ("http://%s/", Net.IP) : std::string ("(apagado o sin red)"),
				"Escríbela en el navegador de un ordenador de la misma red."),
		};
		Pages.push_back (P);
	}

	{
		TPage P;
		P.Title = "Arranque";
		P.Help = "Qué pasa al encender.";

		TItem Game = Action ("Al arrancar", ActBootGame,
			"Intro: elegir un programa de C: (.EXE, .COM o .BAT) que arranque solo al "
			"encender, en su carpeta. Se guarda al final del [autoexec] de dosbox.conf, "
			"entre dos comentarios del menú; lo demás del autoexec no se toca.");
		Game.Value = s_BootPending.empty () ? "Prompt de DOS (C:\\>)" : s_BootPending;
		Game.Apply = TApply::RestartDOSBox;
		Game.bModified = s_BootPending != s_BootSaved;

		P.Items = { Section ("DOS"), Game };
		if (!s_BootPending.empty ())
		{
			TItem Prompt = Action ("Volver al prompt", ActBootPrompt,
				"Que el arranque se quede en C:\\> sin lanzar nada.");
			Prompt.Apply = TApply::RestartDOSBox;
			P.Items.push_back (Prompt);
		}
		P.Items.push_back (Planned ("Lanzador de juegos", "Llegará en un paso próximo: una lista "
			"de los juegos de C: para elegir al encender."));
		P.Items.push_back (Section ("La placa"));
		P.Items.push_back (SettingItem (*FindSetting (SetBootLog)));
		P.Items.push_back (SettingItem (*FindSetting (SetSetupWait)));
		Pages.push_back (P);
	}

	Pages.push_back (PageSalir ());

	s_Menu.Pages = std::move (Pages);
	s_Menu.Refresh ();
}

void SetupTexts (void)
{
	s_Menu.Texts.Title = "BM-DOS Setup";
	s_Menu.Texts.Version = BMDOS_COMMIT;
	// ASCII and Latin-1 only: the font has no arrow glyphs (CP437's are
	// below 0x20 and Circle's table starts at 0x21).
	s_Menu.Texts.KeysLine1 = "Arriba/Abajo: elegir   Izq/Der: pestaña   Intro +/-: cambiar   Esc: al juego";
	s_Menu.Texts.KeysLine2 = "F9 Valores por defecto   F10 Guardar y salir";
	s_Menu.Texts.ApplyLegend = "Sin marca: al momento\nR: al reiniciar DOS\n   (Ctrl+Alt+Supr)\nB: al reiniciar la placa";
}

// --- keys --------------------------------------------------------------------

bool MapKey (unsigned nRetroKey, TKey *pKey)
{
	switch (nRetroKey)
	{
	case RETROK_UP:		*pKey = TKey::Up; return true;
	case RETROK_DOWN:	*pKey = TKey::Down; return true;
	case RETROK_LEFT:	*pKey = TKey::Left; return true;
	case RETROK_RIGHT:	*pKey = TKey::Right; return true;
	case RETROK_PAGEUP:	*pKey = TKey::PageUp; return true;
	case RETROK_PAGEDOWN:	*pKey = TKey::PageDown; return true;
	case RETROK_HOME:	*pKey = TKey::Home; return true;
	case RETROK_END:	*pKey = TKey::End; return true;
	case RETROK_RETURN:
	case RETROK_KP_ENTER:	*pKey = TKey::Enter; return true;
	case RETROK_ESCAPE:	*pKey = TKey::Escape; return true;
	case RETROK_F1:		*pKey = TKey::F1; return true;
	case RETROK_F9:		*pKey = TKey::F9; return true;
	case RETROK_F10:	*pKey = TKey::F10; return true;
	case RETROK_BACKSPACE:	*pKey = TKey::Backspace; return true;
	case RETROK_TAB:	*pKey = TKey::Tab; return true;

	// + and -: the keypad, the US positions, the Spanish positions (the keys
	// under ']' and '/' on a US map), and F5/F6 as on many BIOS setups.
	case RETROK_KP_PLUS:
	case RETROK_EQUALS:
	case RETROK_RIGHTBRACKET:
	case RETROK_F6:		*pKey = TKey::Plus; return true;
	case RETROK_KP_MINUS:
	case RETROK_MINUS:
	case RETROK_SLASH:
	case RETROK_F5:		*pKey = TKey::Minus; return true;
	default:		return false;
	}
}

bool Repeats (TKey Key)
{
	return Key == TKey::Up || Key == TKey::Down || Key == TKey::PageUp || Key == TKey::PageDown
	       || Key == TKey::Plus || Key == TKey::Minus || Key == TKey::Backspace;
}

void Close (void)
{
	s_bClosing = true;
}

void AskToLeave (void)
{
	if (!AnyModified ())
	{
		Close ();
		return;
	}
	s_Menu.OpenDialog (DlgUnsaved, "Cambios sin guardar",
			   "Has cambiado ajustes en el menú. ¿Qué hacemos con ellos?",
			   { "Guardar", "Descartar", "Seguir aquí" });
}

void SaveAndLeave (bool bRestart)
{
	const bool bBoard = ModifiedNeedsBoard ();
	const bool bDos = ModifiedNeedsDos () && !s_bBeforeDos;
	if (AnyModified () && !SaveSettings ())
	{
		s_Menu.OpenDialog (DlgSaveFailed, "No se pudo guardar",
				   "No se pudo escribir algún fichero en la tarjeta (¿protegida "
				   "contra escritura o llena?). Lo que sí se escribió queda guardado.",
				   { "Aceptar" });
		return;
	}

	// "Guardar y reiniciar": the board if a B setting changed, otherwise DOS.
	if (bRestart)
	{
		if (bBoard)
		{
			BareCoreRequestReboot ();
			return;
		}
		Close ();
		if (!s_bBeforeDos)
		{
			BareCoreRequestDosRestart ();
		}
		return;
	}

	if (bBoard)
	{
		s_Menu.OpenDialog (DlgRestartNow, "Reinicio necesario",
				   bDos ? "Guardado. Hay ajustes marcados B: se aplican, con los R, al "
					  "reiniciar la placa."
					: "Guardado. Los ajustes marcados B se aplican al reiniciar la placa.",
				   { "Reiniciar la placa", "Más tarde" });
		return;
	}

	if (bDos)
	{
		s_Menu.OpenDialog (DlgRestartDos, "Reiniciar DOS",
				   "Guardado. Los ajustes marcados R se aplican al reiniciar DOS (unos "
				   "segundos; la placa sigue encendida). Lo que haya en marcha en DOS se "
				   "pierde.",
				   { "Reiniciar DOS ahora", "Más tarde" });
		return;
	}

	Close ();
}

constexpr unsigned PageIndexUnidades = 2;	// Sistema, Máquina, Unidades, ...

void BrowseStart (TBrowse Mode, const std::string &Start)
{
	BrowseOpen (Mode, Start);
	BuildPages ();
	s_Menu.GoToPage (0);
}

constexpr unsigned PageIndexArranque = 8;	// ..., Red, Arranque, Salir

void BrowseClose (void)
{
	const bool bGame = s_Browse == TBrowse::Game;
	s_Browse = TBrowse::None;
	BuildPages ();
	s_Menu.GoToPage (bGame ? PageIndexArranque : PageIndexUnidades);
}

void Insert (const std::string &Path, bool bFloppy)
{
	BrowseClose ();
	if (BareDiskInsert (Path.c_str ()))
	{
		(bFloppy ? s_FloppyImage : s_CDImage) = Path;
		return;
	}
	s_Menu.OpenDialog (DlgSaveFailed, "No se pudo meter el disco",
			   Format ("DOSBox no pudo montar %s. ¿Es una imagen válida? Las .cue "
				   "necesitan su .bin al lado.", Path.c_str ()), { "Aceptar" });
}

void HandleBrowse (int nId)
{
	if (nId == ActBrowseUp)
	{
		const size_t n = s_BrowsePath.find_last_of ('/');
		s_BrowsePath = n == 0 ? "/" : s_BrowsePath.substr (0, n);
		BrowseRead ();
		BuildPages ();
		s_Menu.GoToPage (0);
		return;
	}

	if (nId == ActBrowsePick)
	{
		(s_Browse == TBrowse::FolderC ? s_DriveC : s_DriveE).Pending = s_BrowsePath;
		BrowseClose ();
		return;
	}

	const size_t i = static_cast<size_t> (nId - ActBrowseEntry);
	if (i >= s_BrowseEntries.size ()) return;
	const auto Entry = s_BrowseEntries[i];
	if (Entry.second)
	{
		s_BrowsePath = BrowseJoin (Entry.first);
		BrowseRead ();
		BuildPages ();
		s_Menu.GoToPage (0);
		return;
	}

	if (s_Browse == TBrowse::Game)
	{
		// From the card's path to the DOS path under C: ("/dos/DOOM/DOOM.EXE"
		// with C: at /dos -> "DOOM\\DOOM.EXE").  A program outside C:'s folder
		// is not on any drive at boot.
		const std::string Path = BrowseJoin (Entry.first);
		const std::string Root = s_DriveC.Pending.empty () ? "/dos" : s_DriveC.Pending;
		const std::string Prefix = Root == "/" ? "/" : Root + "/";
		BrowseClose ();
		if (Path.rfind (Prefix, 0) != 0)
		{
			s_Menu.OpenDialog (DlgSaveFailed, "Fuera de C:",
					   Format ("Ese programa no está dentro de %s, que es C:. Elige uno "
						   "de esa carpeta, o cambia C: primero.", Root.c_str ()),
					   { "Aceptar" });
			return;
		}
		std::string Dos = Path.substr (Prefix.size ());
		for (char &c : Dos) if (c == '/') c = '\\';
		s_BootPending = Dos;
		return;
	}

	Insert (BrowseJoin (Entry.first), s_Browse == TBrowse::Floppy);
}

// A text box: what is typed is SHOWN (Tab hides it).  Typing a password blind
// on a keyboard whose layout the menu has to be told is how wrong passwords
// get saved; this is a setup screen at home, not a login.  The line under the
// field says which layout the keys are read with.
void OpenTextInput (int nId, const std::string &Title, const std::string &Prompt,
		    const std::string &Initial, unsigned nMin, unsigned nMax)
{
	s_Menu.OpenInput (nId, Title, Prompt, Initial, false, nMin, nMax);
	s_Menu.SetInputNote (s_Layout == TLayout::Spanish
			     ? "Teclado: español (cmdline.txt keymap). AltGr+2 es @."
			     : "Teclado: EE. UU. Para otro: keymap=es en cmdline.txt.");
}

void HandleWiFi (const TEvent &Ev)
{
	if (Ev.Kind == TEventKind::Close)
	{
		s_bWiFiList = false;
		BuildPages ();
		s_Menu.GoToPage (PageIndexRed);
		return;
	}
	if (Ev.Kind != TEventKind::Action) return;

	if (Ev.nId == ActWiFiRescan)
	{
		BuildPages ();
		return;
	}
	if (Ev.nId == ActWiFiManual)
	{
		OpenTextInput (InWiFiSSID, "Red WiFi", "Nombre de la red (SSID):", "", 1, 32);
		return;
	}
	const size_t i = static_cast<size_t> (Ev.nId - ActWiFiEntry);
	if (i >= s_WiFiNetworks.size ()) return;
	s_WiFiSSID = s_WiFiNetworks[i].SSID;
	s_bWiFiSecure = s_WiFiNetworks[i].bSecure;
	if (s_bWiFiSecure)
	{
		OpenTextInput (InWiFiPassword, "Contraseña", "Contraseña de \"" + Fit (s_WiFiSSID, 40) + "\":",
			       "", 8, 63);
	}
	else
	{
		WiFiJoin ("");
	}
}

void HandleEvent (const TEvent &Ev)
{
	// Text typed in an input box: a WiFi name or password.
	if (Ev.Kind == TEventKind::Input)
	{
		if (Ev.bCancelled) return;
		if (Ev.nId == InWiFiSSID)
		{
			s_WiFiSSID = Ev.Text;
			s_bWiFiSecure = true;
			OpenTextInput (InWiFiPassword, "Contraseña",
				       "Contraseña (vacía si la red es abierta):",
				       "", 0, 63);
			return;
		}
		if (Ev.nId == InWebPassword)
		{
			// What boot.conf's reader accepts: printable ASCII, no spaces,
			// no '#' (a comment there).  Accents and ñ would be dropped at boot.
			bool bOk = true;
			for (unsigned char c : Ev.Text) bOk = bOk && c > ' ' && c < 0x7F && c != '#';
			if (!bOk)
			{
				s_Menu.OpenDialog (DlgInfo, "Contraseña no válida",
						   "Solo letras sin acento, números y símbolos, sin espacios ni #: "
						   "boot.conf no guarda otra cosa.", { "Aceptar" });
				return;
			}
			s_WebPassPending = Ev.Text;
			return;
		}
		if (Ev.nId == InWiFiPassword)
		{
			if (Ev.Text.empty ()) s_bWiFiSecure = false;
			else if (Ev.Text.size () < 8)
			{
				s_Menu.OpenDialog (DlgInfo, "Contraseña corta",
						   "Una contraseña WPA tiene entre 8 y 63 caracteres.", { "Aceptar" });
				return;
			}
			WiFiJoin (Ev.Text);
		}
		return;
	}

	if (s_bWiFiList)
	{
		HandleWiFi (Ev);
		return;
	}

	// The browser is a screen of its own: Esc leaves it, not the menu, and
	// its entries are actions of their own.
	if (s_Browse != TBrowse::None)
	{
		if (Ev.Kind == TEventKind::Close)
		{
			BrowseClose ();
		}
		else if (Ev.Kind == TEventKind::Action)
		{
			HandleBrowse (Ev.nId);
		}
		return;
	}

	switch (Ev.Kind)
	{
	case TEventKind::Close:
		AskToLeave ();
		break;

	case TEventKind::SaveAndExit:
		SaveAndLeave (false);
		break;

	case TEventKind::Defaults:
		s_Menu.OpenDialog (DlgDefaults, "Valores por defecto",
				   "¿Poner todos los ajustes del menú en sus valores de fábrica? "
				   "No se guardan hasta que guardes.", { "Sí", "No" }, 1);
		break;

	case TEventKind::Changed:
		if (TSetting *pSetting = FindSetting (Ev.nId))
		{
			SetPending (*pSetting, Ev.nChoice);
		}
		break;

	case TEventKind::Action:
		switch (Ev.nId)
		{
		case ActResume:		AskToLeave (); break;
		case ActSave:		SaveAndLeave (false); break;
		case ActSaveRestart:	SaveAndLeave (true); break;
		case ActDiscard:	Discard (); Close (); break;
		case ActDefaults:	{ TEvent Defaults; Defaults.Kind = TEventKind::Defaults; HandleEvent (Defaults); } break;
		case ActDriveC:
			BrowseStart (TBrowse::FolderC, s_DriveC.Pending.empty () ? "/dos" : s_DriveC.Pending);
			break;
		case ActDriveE:
			BrowseStart (TBrowse::FolderE, s_DriveE.Pending.empty () ? "/" : s_DriveE.Pending);
			break;
		case ActWebPassword:
			OpenTextInput (InWebPassword, "Servidor web", "Contraseña (vacía: ninguna):",
				       s_WebPassPending, 0, 32);
			break;
		case ActWiFiList:
			s_bWiFiList = true;
			BuildPages ();
			s_Menu.GoToPage (0);
			break;
		case ActBootGame:
		{
			std::string Start = s_DriveC.Pending.empty () ? "/dos" : s_DriveC.Pending;
			BrowseStart (TBrowse::Game, Start);
			break;
		}
		case ActBootPrompt:
			s_BootPending.clear ();
			break;
		case ActDriveERemove:
			s_DriveE.Pending.clear ();
			break;
		case ActFloppy:
			BrowseStart (TBrowse::Floppy, s_FloppyImage.empty () ? "/dos/x" : s_FloppyImage);
			break;
		case ActCD:
			BrowseStart (TBrowse::CD, s_CDImage.empty () ? "/dos/x" : s_CDImage);
			break;
		case ActFloppyEject:
			if (BareDiskEject ('A')) s_FloppyImage.clear ();
			break;
		case ActCDEject:
			if (BareDiskEject ('D')) s_CDImage.clear ();
			break;
		case ActTestSound:
			s_Test = TTest::Tone;
			s_nTestStart = BareMicros ();
			s_nTestFrames = 0;
			break;
		case ActTestMidi:
			if (BareMidiActive ())
			{
				BareMidiSendTestChord (TRUE);
				s_Test = TTest::Midi;
				s_bTestChordOff = false;
				s_nTestStart = BareMicros ();
				s_nTestFrames = 0;
			}
			break;
		case ActShutdown:
			// Changes not saved are dropped, as a power-off without saving
			// would; the shutdown itself is what was asked for.
			Discard ();
			Close ();
			BareCoreRequestShutdown ();
			break;
		case ActReboot:
			BareCoreRequestReboot ();
			break;
		case ActRestartDos:
			Discard ();
			Close ();
			if (!s_bBeforeDos)		// at boot DOS starts in a moment anyway
			{
				BareCoreRequestDosRestart ();
			}
			break;
		default:
			break;
		}
		break;

	case TEventKind::Dialog:
		if (Ev.nId == DlgUnsaved && !Ev.bCancelled)
		{
			if (Ev.nChoice == 0) SaveAndLeave (false);
			else if (Ev.nChoice == 1) { Discard (); Close (); }
		}
		else if (Ev.nId == DlgDefaults && !Ev.bCancelled && Ev.nChoice == 0)
		{
			LoadDefaults ();
		}
		else if (Ev.nId == DlgRestartNow)
		{
			if (!Ev.bCancelled && Ev.nChoice == 0) BareCoreRequestReboot ();
			else Close ();
		}
		else if (Ev.nId == DlgRestartDos)
		{
			Close ();
			if (!Ev.bCancelled && Ev.nChoice == 0) BareCoreRequestDosRestart ();
		}
		break;

	default:
		break;
	}
}

void UpdateTitle (void)
{
	s_Menu.Texts.Title = AnyModified () ? "BM-DOS Setup   * cambios sin guardar" : "BM-DOS Setup";
}

void Press (TKey Key)
{
	HandleEvent (s_Menu.Key (Key));
	BuildPages ();		// values and '*' marks follow the change
	UpdateTitle ();
	s_bDirty = true;
}

void PumpTest (u64 nNow)
{
	if (s_Test == TTest::None)
	{
		return;
	}

	const unsigned nRate = BareAudioSampleRate ();
	if (nRate == 0)
	{
		s_Test = TTest::None;
		return;
	}

	// Tone: 0.5 s at 440 Hz on the left, 0.5 s at 660 Hz on the right.
	// MIDI: the chord for 1.5 s, then 1 s more of silence for its release.
	const u64 nElapsed = nNow - s_nTestStart;
	const u64 nLength = s_Test == TTest::Tone ? 1000000 : 2500000;
	if (s_Test == TTest::Midi && !s_bTestChordOff && nElapsed >= 1500000)
	{
		BareMidiSendTestChord (FALSE);
		s_bTestChordOff = true;
	}
	if (nElapsed >= nLength)
	{
		s_Test = TTest::None;
		return;
	}

	// Keep about 60 ms queued ahead of real time.
	const u64 nDue = (nElapsed + 60000) * nRate / 1000000;
	static s16 Buffer[1024 * 2];
	while (s_nTestFrames < nDue)
	{
		const unsigned nFrames = static_cast<unsigned> (
			nDue - s_nTestFrames < 1024 ? nDue - s_nTestFrames : 1024);
		for (unsigned i = 0; i < nFrames; i++)
		{
			s16 nLeft = 0, nRight = 0;
			if (s_Test == TTest::Tone)
			{
				const bool bRight = (s_nTestFrames + i) * 2 >= nRate;	// second half
				const float fHz = bRight ? 660.0f : 440.0f;
				const s16 n = static_cast<s16> (9000.0f * __builtin_sinf (s_fTestPhase));
				s_fTestPhase += 2.0f * 3.14159265f * fHz / nRate;
				if (s_fTestPhase > 6.2831853f) s_fTestPhase -= 6.2831853f;
				(bRight ? nRight : nLeft) = n;
			}
			Buffer[2 * i] = nLeft;
			Buffer[2 * i + 1] = nRight;
		}
		BareAudioQueue (Buffer, nFrames);
		s_nTestFrames += nFrames;
	}
}

void Draw (void)
{
	if (s_pPixels == nullptr)
	{
		s_pPixels = new u32[PixelWidth * PixelHeight];
		if (s_pPixels == nullptr)
		{
			return;
		}
	}

	s_Menu.Render (s_Screen);
	RenderScreen (s_Screen, Font8x16, s_pPixels, PixelWidth);
	BareVideoPresent (s_pPixels, PixelWidth, PixelHeight, PixelWidth * sizeof (u32));
	s_bDirty = false;
}

} // namespace

// --- public interface ------------------------------------------------------------

boolean BareMenuKey (unsigned nRetroKey, boolean bDown, boolean bWinDown)
{
	if (!s_bActive.load (std::memory_order_acquire))
	{
		// Win+F12 opens it - and, while the boot waits for it, SUPR or F2.
		// Everything else goes to DOS as before.
		const bool bBootKey = s_bBootWindow.load (std::memory_order_acquire) && bDown
				      && (nRetroKey == RETROK_DELETE || nRetroKey == RETROK_F2);
		if (bBootKey || (nRetroKey == RETROK_F12 && bDown && bWinDown))
		{
			s_bActive.store (true, std::memory_order_release);
			return TRUE;
		}
		return FALSE;
	}

	// Open: queue the key for the frame loop.  A full ring drops the key,
	// which at 64 entries means somebody is leaning on the keyboard.
	const unsigned nHead = s_nKeyHead.load (std::memory_order_relaxed);
	if (nHead - s_nKeyTail.load (std::memory_order_acquire) < KeyRingSize)
	{
		s_KeyRing[nHead % KeyRingSize] = TKeyEvent { static_cast<u16> (nRetroKey), bDown != FALSE };
		s_nKeyHead.store (nHead + 1, std::memory_order_release);
	}
	return TRUE;
}

boolean BareMenuActive (void)
{
	return s_bActive.load (std::memory_order_acquire) ? TRUE : FALSE;
}

void BareMenuTick (void)
{
	if (!s_bActive.load (std::memory_order_acquire))
	{
		return;
	}

	// USB completions may run in scheduler tasks: keep them moving while the
	// core is not polling input.
	CScheduler::Get ()->Yield ();

	const u64 nNow = BareMicros ();

	if (!s_bOpened)
	{
		s_bOpened = true;
		s_bClosing = false;
		s_bRepeatKey = false;
		BareCoreReleaseAllKeys ();
		BareVideoSaveScreen ();
		BareSysInfoRefresh ();
		SetupTexts ();
		DefineSettings ();
		LoadSettings ();

		// The keyboard for the text boxes: no key held, and its layout.
		s_bLeftShift = s_bRightShift = s_bRightAlt = s_bModeKey = false;
		{
			std::string Cmdline, Map;
			ReadText (CmdlinePath, &Cmdline);
			if (bareconf::CmdlineGet (Cmdline, "keymap", &Map))
			{
				s_Layout = bareconf::Lower (Map) == "es" ? TLayout::Spanish : TLayout::US;
			}
			else
			{
				const TSetting *pKeyb = FindSetting (SetKeyb);
				const std::string &Dos = pKeyb != nullptr ? pKeyb->Choices[pKeyb->nPending].Value : "";
				s_Layout = Dos.rfind ("es", 0) == 0 || Dos.rfind ("la", 0) == 0
					   ? TLayout::Spanish : TLayout::US;
			}
		}
		BuildPages ();
		UpdateTitle ();
		s_nLastRefresh = nNow;
		s_bDirty = true;
		CLogger::Get ()->Write (FromMenu, LogNotice, "setup menu open");
	}

	// Keys from the handler.
	for (;;)
	{
		const unsigned nTail = s_nKeyTail.load (std::memory_order_relaxed);
		if (nTail == s_nKeyHead.load (std::memory_order_acquire))
		{
			break;
		}
		const TKeyEvent Ev = s_KeyRing[nTail % KeyRingSize];
		s_nKeyTail.store (nTail + 1, std::memory_order_release);

		// Modifiers, for typing in an input box: each key keeps its own state.
		switch (Ev.nKey)
		{
		case RETROK_LSHIFT:	s_bLeftShift = Ev.bDown; continue;
		case RETROK_RSHIFT:	s_bRightShift = Ev.bDown; continue;
		case RETROK_RALT:	s_bRightAlt = Ev.bDown; continue;
		case RETROK_MODE:	s_bModeKey = Ev.bDown; continue;
		default:		break;
		}

		// An input box takes characters first; the keys that edit it
		// (Backspace, Tab, Enter, Esc) go through the usual mapping below.
		if (s_Menu.InputOpen () && Ev.bDown)
		{
			const std::uint32_t c = KeyToChar (Ev.nKey, s_bLeftShift || s_bRightShift,
							   s_bRightAlt || s_bModeKey, s_Layout,
							   BareInputCapsLock () != FALSE);
			if (c != 0)
			{
				s_Menu.Char (c);
				s_bDirty = true;
				continue;
			}
		}

		TKey Key;
		if (!MapKey (Ev.nKey, &Key))
		{
			continue;
		}

		if (Ev.bDown)
		{
			Press (Key);
			s_bRepeatKey = Repeats (Key);
			s_RepeatKey = Key;
			s_nRepeatRetroKey = Ev.nKey;
			s_nRepeatNext = nNow + RepeatDelayMicros;
		}
		else if (s_bRepeatKey && Ev.nKey == s_nRepeatRetroKey)
		{
			s_bRepeatKey = false;
		}
	}

	PumpTest (nNow);

	if (s_bRepeatKey && !s_bClosing && nNow >= s_nRepeatNext)
	{
		Press (s_RepeatKey);
		s_nRepeatNext = nNow + RepeatRateMicros;
	}

	if (s_bClosing)
	{
		s_bClosing = false;
		s_bOpened = false;
		s_bRepeatKey = false;
		s_bActive.store (false, std::memory_order_release);
		BareVideoRestoreScreen ();
		CLogger::Get ()->Write (FromMenu, LogNotice, "setup menu closed");
		return;
	}

	// Live values once a second.  The frame loop does not run while the menu
	// is open, so the clock and the temperature are refreshed from here.
	if (nNow - s_nLastRefresh >= 1000000)
	{
		s_nLastRefresh = nNow;
		BareSysInfoRefresh ();
		BuildPages ();		// the cursor stays where it was (CMenu::Refresh)
		s_bDirty = true;
	}

	if (s_bDirty)
	{
		Draw ();
	}
}

boolean BareMenuBootWindow (unsigned nSeconds)
{
	if (nSeconds == 0)
	{
		return FALSE;
	}

	s_bOpenedAtBoot = false;
	s_bBeforeDos = true;
	s_bBootWindow.store (true, std::memory_order_release);

	const u64 nEnd = BareMicros () + static_cast<u64> (nSeconds) * 1000000;
	while (BareMicros () < nEnd || BareMenuActive ())
	{
		if (BareMenuActive ())
		{
			s_bOpenedAtBoot = true;
			s_bBootWindow.store (false, std::memory_order_release);
			BareMenuTick ();
		}
		else
		{
			CScheduler::Get ()->Yield ();
		}
		CScheduler::Get ()->MsSleep (15);	// the WiFi and DHCP keep going
	}

	s_bBootWindow.store (false, std::memory_order_release);
	s_bBeforeDos = false;
	return s_bOpenedAtBoot ? TRUE : FALSE;
}

void BareMenuShowMessage (const char *pText)
{
	if (s_pPixels == nullptr)
	{
		s_pPixels = new u32[PixelWidth * PixelHeight];
		if (s_pPixels == nullptr)
		{
			return;
		}
	}
	CScreen Message;
	Message.Fill (0, 0, Cols, Rows, White, Blue);
	const std::string Text = pText != nullptr ? pText : "";
	const unsigned n = CScreen::Length (Text);
	Message.Put (n < Cols ? (Cols - n) / 2 : 0, Rows / 2, Text, White, Blue);
	RenderScreen (Message, Font8x16, s_pPixels, PixelWidth);
	BareVideoPresent (s_pPixels, PixelWidth, PixelHeight, PixelWidth * sizeof (u32));
}

void BareMenuOnDosRestart (void)
{
	// The images the menu put in A: and D: are gone with the old machine: the
	// new one has what its autoexec mounts.  The Unidades page must not keep
	// showing their names.
	s_FloppyImage.clear ();
	s_CDImage.clear ();
}
