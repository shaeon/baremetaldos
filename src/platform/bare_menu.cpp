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

#include <circle/font.h>
#include <circle/logger.h>
#include <circle/memory.h>
#include <circle/sched/scheduler.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <string>

#include "libretro.h"

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
};

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
	else if (nFirmware & 0x10000) Power = "normal (hubo tensión baja)";

	const u64 nSeconds = BareMicros () / 1000000;

	unsigned nModeW = 0, nModeH = 0;
	BareVideoModeSize (&nModeW, &nModeH);

	TBareVideoPresenterStats Video;
	BareVideoPresenterStats (&Video);

	const char *pMidi = BareMidiActive () ? BareMidiName () : "ninguno";

	P.Items =
	{
		Section ("BM-DOS"),
		Info ("Versión", DOSBOX_SNAPSHOT),
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
			? Format ("vídeo, %llu frames, vsync %s", (unsigned long long) Video.nShown,
				  Video.bVsync ? "sí" : "no")
			: std::string ("libre")),
		Info ("Núcleo 3", "libre"),

		Section ("Pantalla y sonido"),
		Info ("Resolución", Format ("%ux%u", nModeW, nModeH)),
		Info ("Escalado", BareVideoSharpScaling () ? "nítido" : "suave"),
		Info ("Salida de audio", BareAudioDeviceName ()),
		Info ("MIDI", BareMidiStatus ()),
	};
	return P;
}

TPage PagePlanned (const char *pTitle, const char *pHelp,
		   std::initializer_list<std::pair<const char *, TApply>> Items)
{
	TPage P;
	P.Title = pTitle;
	P.Help = pHelp;
	for (const auto &I : Items)
	{
		P.Items.push_back (Planned (I.first, "Llegará en un paso próximo del menú.", I.second));
	}
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
		Action ("Reiniciar la placa", ActReboot,
			"Reinicia la Raspberry Pi, como Ctrl+Alt+PetSis."),
		Section (""),
		Planned ("Guardar y volver", "Guardará los cambios en boot.conf y dosbox.conf."),
		Planned ("Guardar y reiniciar", "Guarda y reinicia DOSBox, para los ajustes marcados R.", TApply::RestartDOSBox),
		Planned ("Salir sin guardar", "Descarta los cambios hechos en el menú."),
		Planned ("Valores por defecto", "Vuelve a los valores de fábrica (F9)."),
		Planned ("Actualizar desde GitHub", "Buscará una versión nueva en las releases."),
	};
	return P;
}

void BuildPages (void)
{
	std::vector<TPage> Pages;
	Pages.push_back (PageSistema ());

	Pages.push_back (PagePlanned ("Máquina", "El PC que emula DOSBox.",
	{
		{ "Tipo de máquina", TApply::RestartDOSBox },
		{ "Memoria", TApply::RestartDOSBox },
		{ "Núcleo de CPU", TApply::RestartDOSBox },
		{ "Tipo de CPU", TApply::RestartDOSBox },
		{ "Velocidad (CPU real)", TApply::Now },
		{ "Ciclos", TApply::Now },
		{ "Paso de Ctrl+F11/F12", TApply::Now },
	}));

	Pages.push_back (PagePlanned ("Unidades", "Carpetas, imágenes de disco y CD-ROM.",
	{
		{ "Unidad C:", TApply::RestartDOSBox },
		{ "Unidad D:", TApply::RestartDOSBox },
		{ "Disquetera A:", TApply::Now },
		{ "CD-ROM (ISO/CUE)", TApply::Now },
		{ "Cambiar de disco", TApply::Now },
		{ "Explorar la tarjeta", TApply::Now },
	}));

	Pages.push_back (PagePlanned ("Sonido", "Tarjetas de sonido, MIDI y volumen.",
	{
		{ "Salida (HDMI/jack)", TApply::RestartBoard },
		{ "Volumen general", TApply::Now },
		{ "Sound Blaster", TApply::RestartDOSBox },
		{ "OPL / AdLib", TApply::RestartDOSBox },
		{ "Gravis Ultrasound", TApply::RestartDOSBox },
		{ "Altavoz del PC", TApply::RestartDOSBox },
		{ "Dispositivo MIDI", TApply::RestartDOSBox },
		{ "SoundFont", TApply::RestartDOSBox },
		{ "Modelo de MT-32", TApply::RestartDOSBox },
		{ "Volumen del MIDI", TApply::Now },
		{ "Probar sonido", TApply::Now },
	}));

	Pages.push_back (PagePlanned ("Vídeo", "Resolución, presentación y escalado.",
	{
		{ "Resolución", TApply::RestartBoard },
		{ "Núcleo de vídeo", TApply::RestartBoard },
		{ "Vsync", TApply::RestartBoard },
		{ "Escalado", TApply::Now },
		{ "Corrección de aspecto", TApply::Now },
		{ "Panel de estado", TApply::Now },
	}));

	Pages.push_back (PagePlanned ("Entrada", "Teclado, ratón, joystick y mandos.",
	{
		{ "Distribución del teclado", TApply::RestartDOSBox },
		{ "Sensibilidad del ratón", TApply::Now },
		{ "Tipo de joystick", TApply::RestartDOSBox },
		{ "Mandos", TApply::Now },
	}));

	Pages.push_back (PagePlanned ("Rendimiento", "Reloj de la placa y temperatura.",
	{
		{ "Techo del reloj", TApply::Now },
		{ "Modo del governor", TApply::Now },
		{ "Límite de temperatura", TApply::RestartBoard },
	}));

	Pages.push_back (PagePlanned ("Red", "Conexión de la placa y red para DOS.",
	{
		{ "WiFi", TApply::RestartBoard },
		{ "Ethernet", TApply::RestartBoard },
		{ "Dirección IP", TApply::RestartBoard },
		{ "NE2000 para DOS", TApply::RestartDOSBox },
		{ "Módem en COM1 (telnet)", TApply::RestartDOSBox },
		{ "IPX", TApply::RestartDOSBox },
		{ "Hora por NTP", TApply::RestartBoard },
		{ "Servidor web de la tarjeta", TApply::RestartBoard },
	}));

	Pages.push_back (PagePlanned ("Arranque", "Qué pasa al encender.",
	{
		{ "Al arrancar", TApply::RestartDOSBox },
		{ "Mensajes de arranque", TApply::RestartBoard },
		{ "Espera de SUPR", TApply::RestartBoard },
	}));

	Pages.push_back (PageSalir ());

	s_Menu.Pages = std::move (Pages);
	s_Menu.Refresh ();
}

void SetupTexts (void)
{
	s_Menu.Texts.Title = "BM-DOS Setup";
	s_Menu.Texts.Version = DOSBOX_SNAPSHOT;
	// ASCII and Latin-1 only: the font has no arrow glyphs (CP437's are
	// below 0x20 and Circle's table starts at 0x21).
	s_Menu.Texts.KeysLine1 = "Arriba/Abajo: elegir   Izq/Der: pestaña   Intro +/-: cambiar   Esc: al juego";
	s_Menu.Texts.KeysLine2 = "F9 Valores por defecto   F10 Guardar y salir";
	s_Menu.Texts.ApplyLegend = "R: al reiniciar DOSBox\nB: al reiniciar la placa\nSin marca: al momento";
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
	       || Key == TKey::Plus || Key == TKey::Minus;
}

void HandleEvent (const TEvent &Ev)
{
	switch (Ev.Kind)
	{
	case TEventKind::Close:
	case TEventKind::SaveAndExit:		// nothing to save yet
		s_bClosing = true;
		break;

	case TEventKind::Action:
		if (Ev.nId == ActResume)
		{
			s_bClosing = true;
		}
		else if (Ev.nId == ActShutdown)
		{
			s_bClosing = true;
			BareCoreRequestShutdown ();
		}
		else if (Ev.nId == ActReboot)
		{
			BareCoreRequestReboot ();
		}
		break;

	default:
		break;
	}
}

void Press (TKey Key)
{
	HandleEvent (s_Menu.Key (Key));
	s_bDirty = true;
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
		// Win+F12 opens it.  Everything else goes to DOS as before.
		if (nRetroKey == RETROK_F12 && bDown && bWinDown)
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
		SetupTexts ();
		BuildPages ();
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

	// Live values once a second.
	if (nNow - s_nLastRefresh >= 1000000)
	{
		s_nLastRefresh = nNow;
		BuildPages ();		// the cursor stays where it was (CMenu::Refresh)
		s_bDirty = true;
	}

	if (s_bDirty)
	{
		Draw ();
	}
}
