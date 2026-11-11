#!/bin/sh
#
# Compare the symbols bare_env.h promises against the ones the platform files
# define, in both directions.  A function declared and never defined links
# nowhere; a function defined and never declared is dead code.
#
# Usage:
#   make check-symbols           # part of make check-host
#
# check-symbols.sh - los simbolos que el contrato (bare_env.h) promete frente a
# los que los ficheros de plataforma definen de verdad.
#
# Por que existe: en la .52 reescribi un bloque de bare_video.cpp por indices y
# me lleve por delante BareVideoFramesPresented().  Todo compilaba -el fichero
# no la usaba- y el fallo salio en el enlace, en la maquina del usuario, con
# tres "undefined reference".  Un -fsyntax-only no lo ve; nm si.
#
# Lo que cuenta como problema:
#   * una funcion del contrato que ningun fichero de plataforma define, y que
#     ademas se usa en kernel.cpp o bare_libretro.cpp  -> rompe el enlace;
#   * una funcion definida y declarada pero que no la llama nadie -> codigo
#     muerto (aviso, no error: puede ser parte del contrato).
#
# Uso:  make check-platform
#       (o a mano: sh scripts/hostcheck/check-symbols.sh [ruta-al-proyecto])
#
# Necesita g++ y nm del sistema - el del host, no el toolchain - y nada mas:
# las cabeceras de Circle que usa estan simuladas en este mismo directorio
# (setup.sh las escribe).  Es una comprobacion de *estructura*, no del build
# real: no sustituye a compilar, pero encuentra en dos segundos lo que si no se
# encuentra en la placa.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"
# Ruta absoluta: el script cambia de directorio para compilar con las cabeceras
# simuladas de aqui al lado, y una ruta relativa se rompe al hacerlo.
PROJ=${1:-$(cd "$HERE/../.." && pwd)}
PROJ=$(cd "$PROJ" && pwd)

if [ ! -d circle ]; then
	echo "falta el entorno simulado: ejecuta primero sh $HERE/setup.sh" >&2
	exit 2
fi

# bare_thread is here as of .57.  It was the one platform file this check could
# not compile - it needs CTask and CScheduler - and that is exactly where a
# whole round of timing logic went in without ever being compiled by anything
# but the cross build.  The stand-in scheduler in setup.sh is enough.
# bare_input joined in .60.  It was in neither this list nor check-kernel's,
# so the one file that talks to Circle's USB stack - and that .60 rewrote to
# add gamepads - was compiled by nothing except the cross build
# machine.  Two of the four faults that round fixed were in this file.
FILES="bare_sysinfo bare_audio bare_video bare_time bare_fs bare_thread bare_dynarec bare_input bare_cores bare_midi bare_menu"
OBJ=/tmp/check-symbols.$$
mkdir -p $OBJ

for f in $FILES; do
	g++ -std=gnu++23 -w -I. -c "$PROJ/src/platform/$f.cpp" -o "$OBJ/$f.o"
done

nm -C $OBJ/*.o | grep " T Bare" | sed 's/.* T //; s/(.*//' | sort -u > $OBJ/defs
# Solo nombres seguidos de parentesis, y nunca dentro de un typedef:
# "typedef void TBareFrameHook (void *pParam);" parece una funcion y no lo es.
sed 's|//.*||' "$PROJ/src/platform/bare_env.h" \
	| grep -v '^[[:space:]]*typedef' \
	| grep -oE "Bare[A-Z][A-Za-z]*[[:space:]]*\\(" \
	| tr -d ' \t(' | sort -u > $OBJ/decl

# Las que viven en ficheros que este entorno no puede compilar (necesitan
# libretro.h, CTask de Circle, etc.).  La lista es explicita a proposito: si
# aparece una nueva sin definir, no esta aqui y se avisa.
cat > $OBJ/elsewhere <<'EOF'
BareCoreDeinit
BareCoreInit
BareCoreLoadGame
# .65e: asks the core what rate its mode refreshes at (retro_get_system_av_info).
# Lives in bare_libretro.cpp, which this script does not compile because it needs
# libretro.h and the whole API - so it is declared but defined elsewhere, and the
# list is where that is said rather than being inferred.
BareCorePollRefreshMilliHz
# The setup menu's way into the core (bare_libretro.cpp): release the keys DOS
# holds, and the shutdown / reboot that Ctrl+F9 and Ctrl+Alt+PetSis also use.
BareCoreReleaseAllKeys
BareCoreRequestShutdown
BareCoreRequestReboot
# Restarting DOS without restarting the board (bare_libretro.cpp).
BareCoreRequestDosRestart
BareCoreRestart
BareCoreRestartWanted
# Disk images through the core's disk control (bare_libretro.cpp).
BareDiskInsert
BareDiskEject
BareDiskAvailable
# The board's network (bare_net.cpp): built for the board only, against
# Circle's net and WLAN headers, which the host stand-ins do not carry.
BareNetInit
BareNetMode
BareNetReady
BareNetState
BareNetInfo
BareNetSetTimeZone
BareNetTimeValid
BareNetWaitForTime
BareNetSocketsReady
BareNetLocalTime
BareNetWiFiNetworks
BareNetWiFiConfigured
# The card's web server (bare_web.cpp): built for the board only, like bare_net.
BareWebStart
BareWebRunning
BareCardFreeBytes
BareCorePaused
BareCoreRunFrame
BareCoreSetFrameHook
BareCoreWantsExit
BareDeliverKey
BareInputCoreCallbackInstalled
BareInputDeinit
BareInputInit
BareInputKeyEvents
BareInputLastKey
BareInputPoll
BarePushKey
BarePushMouse
BareTakeScreenshotRequest
BareThreadEmulatorDriving
BareThreadEmulatorYields
BareThreadHandBacksSinceReport
BareThreadMicrosSinceHandBack
# .60: these live in bare_input.cpp, which is compiled here now, but they are
# declared extern "C" - so nm shows them without the C++ decoration and the
# 'T Bare' filter does not see them.  Listed so that a missing one is a
# finding and not a false alarm.
BareJoypadAxis
BareJoypadBits
EOF

echo "=== definidas aqui: $(wc -l < $OBJ/defs)"
echo "=== declaradas en bare_env.h: $(wc -l < $OBJ/decl)"
echo "=== declaradas que NO define nadie de los compilables:"
# Both sides sorted before comm: the lists are built by different passes now
# (defs comes out of nm, decl out of a sed over the header) and comm fails with
# "file 2 is not in sorted order" - which is a warning that reads like a
# failure and is not one.  LC_ALL=C so that the sort order is the one comm
# expects regardless of the caller's locale.
sort -u $OBJ/defs -o $OBJ/defs
sort -u $OBJ/decl -o $OBJ/decl
sort -u $OBJ/elsewhere -o $OBJ/elsewhere
comm -23 $OBJ/decl $OBJ/defs | comm -23 - $OBJ/elsewhere > $OBJ/missing || true
if [ -s $OBJ/missing ]; then
	echo "  FALLO - estas romperian el enlace:"
	sed 's/^/    /' $OBJ/missing
	RC=1
else
	echo "  ninguna que no este justificada"
	RC=0
fi

# La lista de abajo es informativa: funciones declaradas en el contrato,
# definidas en un .cpp de src/ y de las que no aparece ninguna llamada en src/.
# Normalmente es codigo muerto.  Pero desde la .63 hay una familia que no lo es:
# las llama la unidad generada del JIT (build/bare_dynrec_unit.cpp), que es un
# .cpp, solo que no esta en src/.  Se dice cual y por que, en vez de dejar un
# nombre suelto que parece un problema en cada ejecucion.
cat > $OBJ/called_generated <<'EOF'
BareDynarecPrepareCodeCache:la llama build/bare_dynrec_unit.cpp (check-dynarec-hook)
EOF
cut -d: -f1 $OBJ/called_generated > $OBJ/called_generated_names

echo "=== declaradas, definidas y que no llama nadie (revisar a mano):"
for s in $(comm -12 $OBJ/decl $OBJ/defs); do
	n=$(grep -rho "\b$s[[:space:]]*(" "$PROJ/src" --include=*.cpp 2>/dev/null | wc -l)
	if [ "$n" -le 1 ]; then
		why=$(grep "^$s:" $OBJ/called_generated | cut -d: -f2-)
		if [ -n "$why" ]; then
			echo "    $s  ($why)"
		else
			echo "    $s"
		fi
	fi
done

rm -rf $OBJ
exit $RC
