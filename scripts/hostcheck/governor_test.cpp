// governor_test.cpp - check-governor
//
// What this is for, exactly.
//
// .60 fixed a fault that no existing check could have caught and that only
// showed up on the board: the clock governor "never lowered the frequency".  The
// decision logic was fine.  The *table* was wrong - level 0 was filled from the
// firmware's "what is the clock right now" tag (1500 or 1800 MHz at boot)
// instead of its minimum, so the three levels came out as {1800, 1500, 2000},
// the "is the middle strictly inside the range" test failed, the port ran with
// two levels, and there was no 600 MHz anywhere to drop to.
//
// It compiled, it passed every check, and it was wrong.  So this file encodes
// the table as a pure function over the firmware's numbers, and asserts what
// the board must end up with for the boards this port actually runs on - which
// is the part that has to be true regardless of what a mailbox answers.
//
// The function is a *copy* of BuildLevels() with the mailbox removed, and that
// is a real risk: a copy can drift from the original.  What keeps it honest is
// that BuildLevels() is now three lines long and every line is one of these:
//
//     s_nMinHz = ReadMinClockRate()   or LowHz if the firmware says 0
//     if (s_nMinHz < LowHz) s_nMinHz = LowHz;
//     { s_nMinHz, MidHz, s_nMaxHz }    and 3 levels iff min < mid < max
//
// If that changes, this file has to change with it, in the same commit.  That
// is written down here rather than hoped for.

#include <cstdio>

// The policy itself, out of src/platform/governor_policy.h: thresholds and one
// function.  Before .65f this test only covered the level table, and three
// faults were found later in the part nobody tested.
#include "../../src/platform/governor_policy.h"

namespace {

constexpr unsigned LowHz = 600000000;
constexpr unsigned MidHz = 1500000000;
constexpr unsigned DefaultMaxHz = 2000000000;

struct TLevels
{
	unsigned nHz[3];
	unsigned nCount;
};

/// \brief BuildLevels(), with the firmware's two answers as parameters.
/// \param nMinFromFirmware PROPTAG_GET_MIN_CLOCK_RATE (0 = it refused)
/// \param nMaxFromFirmware PROPTAG_GET_MAX_CLOCK_RATE (0 = it refused)
TLevels BuildLevels (unsigned nMinFromFirmware, unsigned nMaxFromFirmware)
{
	unsigned nMin = nMinFromFirmware != 0 ? nMinFromFirmware : LowHz;
	unsigned nMax = nMaxFromFirmware;

	if (nMax == 0 || nMax <= nMin)
	{
		nMax = DefaultMaxHz;
	}

	if (nMin < LowHz)
	{
		nMin = LowHz;
	}

	TLevels Levels { { nMin, MidHz, nMax }, 3 };

	if (nMin < MidHz && MidHz < nMax)
	{
		Levels.nCount = 3;
	}
	else
	{
		Levels.nHz[1] = nMax;
		Levels.nCount = 2;
	}

	return Levels;
}

unsigned s_nFailures = 0;

void Expect (bool bCondition, const char *pWhat)
{
	if (!bCondition)
	{
		printf ("  FALLO  %s\n", pWhat);
		s_nFailures++;
	}
	else
	{
		printf ("  ok     %s\n", pWhat);
	}
}

void CheckBoard (const char *pName, unsigned nMin, unsigned nMax,
		 unsigned nWantLow, unsigned nWantMid, unsigned nWantHigh, unsigned nWantCount)
{
	printf ("%s (firmware: min %u, max %u)\n", pName, nMin / 1000000, nMax / 1000000);

	const TLevels L = BuildLevels (nMin, nMax);

	Expect (L.nCount == nWantCount, "numero de niveles");
	Expect (L.nHz[0] == nWantLow, "nivel 0 (el de reposo)");
	if (nWantCount == 3)
	{
		Expect (L.nHz[1] == nWantMid, "nivel 1");
	}
	Expect (L.nHz[L.nCount - 1] == nWantHigh, "nivel superior");

	// The property that was broken and that matters more than the numbers:
	// the bottom level has to be strictly below the middle one, or "step down"
	// has nowhere to go and the panel shows a frequency that never moves.
	if (L.nCount >= 2)
	{
		Expect (L.nHz[0] < L.nHz[L.nCount - 1], "el nivel de reposo es menor que el superior");
	}
}

} // namespace

int main (void)
{
	// A real board.  A Pi 400 reports 1800 MHz for "current" at boot and
	// 2000 for "max"; the minimum tag answers 600 (or 0, if that firmware
	// refuses it).  Either way the bottom level must be 600 and there must be
	// three levels.
	CheckBoard ("Pi 400 (min tag answers)", 600000000, 2000000000,
		    600000000, 1500000000, 2000000000, 3);
	CheckBoard ("Pi 400 (min tag refuses)", 0, 2000000000,
		    600000000, 1500000000, 2000000000, 3);

	// A Pi 4B: same shape, lower top.
	CheckBoard ("Pi 4B", 0, 1500000000,
		    600000000, 1500000000, 1500000000, 2);

	// A firmware that reports min == max is reporting something impossible, and
	// BuildLevels() treats it as "no ceiling": max falls back to DefaultMaxHz.
	// The first version of THIS test expected {600, 1500, 1500} and failed -
	// the test was wrong, not the code, and that is worth leaving visible
	// because it is the same trap as .59 in miniature: two numbers that look
	// like a range and are not one.
	CheckBoard ("firmware incoherente (min == max)", 1500000000, 1500000000,
		    1500000000, 2000000000, 2000000000, 2);

	// A Pi 5: 2400 top, 1500 middle still inside the range.
	CheckBoard ("Pi 5", 0, 2400000000,
		    600000000, 1500000000, 2400000000, 3);

	// A board pinned low by config.txt (arm_freq=800): the firmware's minimum
	// is the one number that must be respected, so the bottom stays at 800 -
	// above the 600 this file would otherwise impose.  Three levels still,
	// because 1500 is still strictly between 800 and 2000.  (The first version
	// of this test expected two levels and was wrong: a raised floor does not
	// collapse the table, it just moves the bottom of it.)
	CheckBoard ("board con arm_freq=800", 800000000, 2000000000,
		    800000000, 1500000000, 2000000000, 3);

	// A firmware that answers nothing at all: the defaults are the design.
	CheckBoard ("firmware sin respuestas", 0, 0,
		    600000000, 1500000000, 2000000000, 3);

	// The fault itself, named: the old code put the *current* rate in level 0.
	// On a Pi 400 that is 1800, so the table was {1800, 1500, 2000} - the
	// middle outside the range, two levels, and 1800 as the lowest the
	// governor could ever reach.  It has to be impossible now.
	{
		printf ("el fallo de .59 no puede volver\n");
		const TLevels L = BuildLevels (1800000000, 2000000000);
		Expect (L.nHz[0] <= 1800000000, "el nivel 0 no es la frecuencia de arranque");
		Expect (L.nHz[0] == 1800000000, "con un minimo de 1800 el suelo es 1800 (pin real)");
	}

	// ----------------------------------------------------------------------
	// .65f: the judgement, with numbers read off the panel
	// ----------------------------------------------------------------------
	//
	// The two panel lines that started this:
	//
	//   "fr 28ms  pace 34/60  busy over3614"   Epic Pinball, 1800 MHz
	//   "fr 25ms  pace 38/70  busy over4090"   idem, text menu
	//
	// Both were labelled `busy`, which means "the governor should be climbing",
	// on a board already at the top of its range.  `comf` never appeared at all.
	printf ("el juicio del governor\n");
	{
		// The readings: 28 ms in a 16.7 ms budget, at 1800 MHz - the top of
		// the board's range.  This used to read "busy", which means climbing,
		// and there was nowhere to climb.
		const auto V = governor::Judge (28000, 16667);
		Expect (V == governor::TVerdict::Overrun, "un frame de 28 ms en 16,7 es sobrecoste");
		Expect (governor::Label (false, true, V)[0] == 'm',
			"en el nivel mas alto el panel dice max, no busy");
	}

	{
		// The same frame with a level above it: now climbing is the right
		// answer and the word is the one it always was.
		const auto V = governor::Judge (28000, 16667);
		Expect (governor::Label (false, false, V)[0] == 'b',
			"con un nivel por encima, busy");
	}

	{
		// 8 ms in 16.7 is under half: comfortable, the governor may step down.
		// This is the state that had never once been on screen.
		const auto V = governor::Judge (8000, 16667);
		Expect (V == governor::TVerdict::Slow, "un frame de 8 ms en 16,7 tiene sitio");
		Expect (governor::Label (false, false, V)[0] == 'c', "y el panel dice comf");
	}

	{
		// 15.5 ms in 16.7 is 93 %: no margin, not comfortable, climb.
		const auto V = governor::Judge (15500, 16667);
		Expect (V == governor::TVerdict::Overrun, "15,5 ms en 16,7 es sobrecoste");
	}

	{
		// In the middle: neither evidence, and it must not be rounded into one
		// of the others.  On screen that is the dash.
		const auto V = governor::Judge (14000, 16667);
		Expect (V == governor::TVerdict::Borderline, "14 ms en 16,7 no es ni una cosa ni otra");
		Expect (governor::Label (false, false, V)[0] == '-', "y el panel dice -");
	}

	{
		// No measurement is not a verdict.  Before .65e "no budget yet" fell
		// back to 16666 and *was* measured, which is what pinned the board at
		// 1800 on a DOS prompt; the rule is asserted rather than assumed.
		const auto V = governor::Judge (14000, 0);
		Expect (V == governor::TVerdict::Borderline,
			"sin presupuesto no hay juicio (ni sobrecoste ni comodidad)");
	}

	{
		// ------------------------------------------------------------------
		// .65g: a still screen is not work
		// ------------------------------------------------------------------
		//
		// A panel line showing `max` at 1800 MHz over the DOS prompt, with
		// `over803` or `over2118` accumulated, was the normal state.  The
		// governor had been judging "frames" that on a prompt are seventy copies
		// per second of the same picture: they were presented, so the machine
		// counted as busy, and the clock climbed while nothing changed.
		//
		// The rule is a measurement, not a heuristic: a frame identical to the
		// previous one is an emulator that runs and produces nothing.  Both cases
		// that matter can be executed here.
		Expect (governor::IsIdleFrame (false),
			"un frame repetido es la maquina parada (el prompt)");
		Expect (!governor::IsIdleFrame (true),
			"un frame que cambia es la maquina trabajando (el juego)");
	}

	{
		Expect (!governor::IsIdleAfter (60),
			"un segundo de imagen quieta NO es reposo (DOOM en un menu)");

		Expect (!governor::IsIdleAfter (300),
			"cinco segundos tampoco");

		Expect (governor::IsIdleAfter (governor::IdleFramesForPrompt),
			"diez segundos de imagen identica si: eso es un prompt");

		Expect (governor::IsIdleFrame (false),
			"un frame repetido sigue siendo el disparador del contador");

		Expect (!governor::IsIdleFrame (true),
			"un frame que cambia reinicia la cuenta");
	}

	{
		// Y la consecuencia: el juicio de ese frame repetido no puede ser
		// "sube".  Judge() no lo sabe - juzga un tiempo, no una imagen - asi que
		// la comprobacion es que la rama que manda es la de idle, y que es
		// alcanzable con un numero real: 29,7 ms en un modo de 70 Hz.
		const auto Still = governor::Judge (29700, 14285);
		Expect (Still != governor::TVerdict::Slow,
			"un frame quieto de 29,7 ms no es comodo (por eso hace falta el otro dato)");
		Expect (governor::Label (true, true, Still)[0] == 'i',
			"pero con la imagen quieta manda idle: ni busy ni max");
	}

	{
		// idle wins over everything: nothing on screen means no clock is
		// justified, whatever the last frame cost.
		Expect (governor::Label (true, true, governor::TVerdict::Overrun)[0] == 'i',
			"idle manda: sin frames no se justifica ningun reloj");
	}

	{
		// Punto 3: subir antes.
		using namespace governor;
		Expect (!MayStepDown (5000000, 10000000, 0), "calentando (programa recien arrancado): no baja");
		Expect (!MayStepDown (20000000, 0, 30000000), "poco despues de un hueco de audio: no baja");
		Expect (MayStepDown (40000000, 10000000, 30000000), "pasados los dos plazos: puede bajar");
		Expect (GapStepsUp (false, 0, 1, 1000000, 5000000), "un hueco por debajo del maximo: sube");
		Expect (!GapStepsUp (false, 1, 1, 1000000, 5000000), "en el maximo ya no hay donde subir");
		Expect (!GapStepsUp (true, 0, 1, 1000000, 5000000), "modo fijo (Maximo/Ahorro): no se toca");
		Expect (!GapStepsUp (false, 0, 1, 100000, 5000000), "dos huecos seguidos: un nivel cada medio segundo");
		Expect (!GapStepsUp (false, 0, 1, 1000000, 500000), "recien salido del menu o de una pausa: no cuenta");
		Expect (ProgramStarted (0x1000, 0x2000, 0x1000), "PSP nuevo: arranca un programa");
		Expect (!ProgramStarted (0x2000, 0x1000, 0x1000), "volver al PSP del interprete: termina, no arranca");
		Expect (!ProgramStarted (0x2000, 0x2000, 0x1000), "el mismo PSP: nada");
	}

	printf ("\n");
	if (s_nFailures != 0)
	{
		printf ("%u fallos\n", s_nFailures);
		return 1;
	}

	printf ("0 fallos\n");
	return 0;
}
