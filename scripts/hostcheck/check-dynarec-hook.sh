#!/bin/sh
#
# Generate and compile the JIT hook as a wrapper translation unit against
# DOSBox-core's own sources.  Without those sources it reports OMITIDO with a
# non-zero status, on purpose: a check that cannot look is not a pass.
#
# Usage:
#   make check-dynarec-hook      # needs third_party/dosbox-core (make fetch)
#
#
# check-dynarec-hook.sh - check-dynarec-hook
#
# The JIT hook is the riskiest thing in this port, and every part of it is the
# kind of mistake that does not fail where it is made:
#
#   * the wrong -DC_TARGETCPU: no backend compiled, or a preprocessor error.
#     Nothing catches it without this check (see .63: the flag was quoted and
#     the number was compared against a string).
#   * a wrapper TU that stops including core_dynrec.cpp, or an #undef that goes
#     missing: redefinition, or a hook that never runs.
#   * page-table code that cannot see the pointer it must fix: a JIT that faults
#     on its first block, on the board, with no message.
#   * a check that compiles with flags the real build does not pass: it says ok
#     while the board build stops on line 20 of the generated file.  That one
#     happened, so the flags below are read from the Makefile itself.
#
# So this script compiles the REAL thing: the generated wrapper unit, against
# the pinned DOSBox sources, with the port's own flags.  Not a mock, not a
# stand-in - the same translation unit the board build will compile, minus the
# AArch64 target.  If it passes here, the only thing left that can go wrong on
# the board is the page tables themselves, which check-dynarec covers.
#
# It skips - loudly, and with a nonzero exit - when the DOSBox tree is not
# present, because "I could not look" must never be reported as "all good".
#
set -e

DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$DIR/../.." && pwd)"
CXX="${HOST_CXX:-g++}"
DOSBOX="${DOSBOX_DIR:-$ROOT/third_party/dosbox-core}"

echo "check-dynarec-hook: el hook del JIT, compilado contra las fuentes del nucleo"
echo

if [ ! -f "$DOSBOX/src/cpu/core_dynrec.cpp" ]; then
	echo "OMITIDO  no hay fuentes de DOSBox en $DOSBOX"
	echo "         (hace falta 'make fetch'; sin ellas este check no puede mirar,"
	echo "          y un check que no mira no es un check que pasa)"
	exit 1
fi

# --- 1  the target flag actually selects the AArch64 backend --------------
"$CXX" -std=gnu++23 -Wall -Wextra -Wpedantic \
	-DC_DYNREC -DC_TARGETCPU=ARMV8LE \
	-I "$ROOT/src/platform" \
	-o /tmp/check-dynarec-target.$$ \
	"$DIR/dynarec_target_test.cpp"
/tmp/check-dynarec-target.$$ > /dev/null
rm -f /tmp/check-dynarec-target.$$
echo "  ok     -DC_TARGETCPU=ARMV8LE selecciona risc_armv8le.h"

# --- 2  the generated unit, against the real engine -----------------------
UNIT=/tmp/bare_dynrec_unit.$$.cpp
python3 "$DIR/gen-dynarec-unit.py" "$UNIT" "$DOSBOX" > /dev/null

# The include paths come from the Makefile, not from this script.
#
# They have to be the real build's, or this check tests a different program than
# the one that ships.  That is not a theory: until .63 the flags here were
# written by hand, they included $(DOSBOX_DIR)/src/cpu, and the board build -
# whose flags do not - failed on the very first line of the generated unit,
# `#include "core_dynrec.cpp"`, with "No such file or directory".  The check was
# green because it was passing an -I the real build never passes.
#
# `make print-host-includes` expands DOSBOX_INCLUDES and escapes the spaces
# that Circle's directory names contain; they are decoded back here.
INCLUDES=$(cd "$ROOT" && make -s --no-print-directory print-host-includes \
	   DOSBOX_DIR="$DOSBOX") || {
	echo "FALLO  'make print-host-includes' no contesta: sin las rutas del build"
	echo "       real, compilar aqui no demuestra nada sobre la placa."
	exit 1
}
INCLUDES=$(printf '%s' "$INCLUDES" | sed 's/\\ / /g')

# The paths that belong to the ARM build are not here on purpose (the Makefile
# filters them out): pointing a host compiler at newlib's headers for the board
# gives two libc's at once and a wall of "conflicting declaration" inside
# <stdlib.h> - which reads like a broken toolchain and is not one.  The stand-in
# circle/ headers below play their part.
#
# The guard is not redundant with the filter: it catches the case where the list
# comes from somewhere else - an older Makefile, a hand-edited variable - and
# turns two hundred lines of libc errors into one sentence that says what
# happened.  A check that fails inexplicably gets ignored; one that fails and
# explains gets fixed.
# The pattern is `circle-stdlib/install`, not `circle-stdlib`: the plain
# `third_party/circle-stdlib/include` IS wanted (that is Circle's own headers -
# circle/logger.h lives there) and is on the list on the board's build too.
# Everything Circle is out, not just newlib's libc: circle-stdlib/install for the
# board's libc, and circle-stdlib/libs for Circle's own `linux/` stand-in
# headers, which collide with the real <linux/stat.h> and <linux/sched.h> on any
# Linux host.  Both are right for AArch64 and both are unbuildable here.
case "$INCLUDES" in
*circle-stdlib*)
	echo "FALLO  la lista de rutas trae circle-stdlib, que son cosas de la placa:"
	echo "       newlib para aarch64 (dos libc a la vez) y/o los linux/*.h de"
	echo "       Circle (chocan con los de verdad).  Ninguna de las dos compila"
	echo "       en el host.  Falta 'make print-host-includes' en el Makefile,"
	echo "       o el Makefile es viejo."
	exit 1
	;;
*lib/circle/*)
	echo "FALLO  la lista trae las cabeceras de Circle para la placa."
	echo "       En el host las pone scripts/hostcheck/circle.  Ver"
	echo "       'make print-host-includes'."
	exit 1
	;;
esac

# The two -idirafter are the ones the Makefile cannot supply: on the board
# circle/logger.h and circle/types.h come from circle-stdlib, and on a host
# without a configured toolchain they do not exist.  -idirafter means "search
# this last", so when the real headers ARE there they win - which is what keeps
# the stand-in libretro.h in scripts/hostcheck from shadowing the real one
# (the bug that cost most of a round: it must never be a plain -I).
#
# -Wno-deprecated-literal-operator: fmt's `operator"" _format` is upstream's
# code and C++23 deprecates the spelling; it is not our warning and it is not
# news.  One suppression, here, rather than a blanket -w: everything else the
# compiler has to say about this file is still worth reading.
# shellcheck disable=SC2086  # the list is meant to be split
"$CXX" -std=gnu++23 -fsyntax-only -Wno-deprecated-literal-operator \
	-DC_DYNREC -DC_TARGETCPU=ARMV8LE \
	-DCPU_Core_Dynrec_Cache_Init=BareDynrecCacheInitOriginal \
	-D__LIBRETRO__ -DWITH_FAKE_SDL -DC_IPX=0 -DC_MODEM=0 \
	-DFMT_HEADER_ONLY -DPAGESIZE=4096 -D__QNX__ -DNO_PWD_H \
	-DGIT_VERSION='"check"' -DSVN_VERSION='""' \
	$INCLUDES \
	-idirafter "$ROOT/scripts/hostcheck" \
	-idirafter "$ROOT/scripts/hostcheck/fatfs" \
	"$UNIT"
rm -f "$UNIT"
echo "  ok     la unidad generada compila contra core_dynrec.cpp sin editar"

# --- 3  the hook is wired: one definition, and it is ours ------------------
if ! grep -q '#undef CPU_Core_Dynrec_Cache_Init' "$DIR/gen-dynarec-unit.py"; then
	echo "FALLO  la unidad generada ya no hace #undef del nombre: el -D del"
	echo "       Makefile renombraria tambien NUESTRA definicion (redefinition)"
	exit 1
fi
echo "  ok     el generador deshace el renombrado antes de definir el hook"

# --- 4  the page-table code is the one that gets called -------------------
if ! grep -q 'BareDynarecPrepareCodeCache' "$DIR/gen-dynarec-unit.py"; then
	echo "FALLO  la unidad generada ya no llama a BareDynarecPrepareCodeCache:"
	echo "       el JIT ejecutaria desde paginas con PXN = 1 y fallaria en el"
	echo "       primer bloque, con la placa en negro y sin mensaje."
	exit 1
fi
echo "  ok     la unidad generada llama a BareDynarecPrepareCodeCache"

# --- 5  the range handed over is the allocation, not more ------------------
#
# The hook makes executable whatever range it is given, and a page past the end
# of malloc()'s buffer belongs to somebody else.  So the two expressions are
# evaluated and compared as numbers - not as text, because they are spelled
# differently on purpose and only the value has to match:
#
#     dyn_cache.h:  malloc (CACHE_TOTAL + CACHE_MAXSIZE + PAGESIZE_TEMP - 1
#                          + PAGESIZE_TEMP)
#     generator:    CACHE_TOTAL + CACHE_MAXSIZE + (2 * PAGESIZE_TEMP) - 2
#
# The constants are read from the pinned sources, so this keeps working if
# upstream changes CACHE_TOTAL.
if ! python3 - "$DOSBOX" "$DIR/gen-dynarec-unit.py" <<'PYEOF'
import pathlib, re, sys

dosbox = pathlib.Path(sys.argv[1])
generator = pathlib.Path(sys.argv[2])

cache_h = (dosbox / "src" / "cpu" / "dyn_cache.h").read_text()
engine = (dosbox / "src" / "cpu" / "core_dynrec.cpp").read_text()

env = {}
for name in ("CACHE_TOTAL", "CACHE_MAXSIZE"):
    m = re.search(r"#define\s+" + name + r"\s+\(([^)]*)\)", engine)
    if not m:
        sys.exit("no encuentro #define %s en core_dynrec.cpp" % name)
    env[name] = int(eval(m.group(1), {}))

# PAGESIZE_TEMP is 4096 unless C_HAVE_MPROTECT, which this port does not set
# (dyn_cache.h:600-604; the comment below explains why it should not).
env["PAGESIZE_TEMP"] = 4096

# The first malloc() in that file is the 4 KB invalidation map, not the cache.
# Searching by name is the only way to be sure which one is being compared.
m = re.search(r"cache_code_start_ptr\s*=\s*\(Bit8u\*\)\s*malloc\s*\(([^;]*)\)\s*;",
              cache_h)
if not m:
    sys.exit("no encuentro el malloc() de cache_code_start_ptr en dyn_cache.h")
allocation = m.group(1)

m = re.search(r"nBytes\s*=\s*([^;]*);", generator.read_text())
if not m:
    sys.exit("no encuentro la linea 'nBytes =' en gen-dynarec-unit.py")
hook = m.group(1)

def evaluate(expression, what):
    cleaned = expression.replace("\\\n", " ")
    if not re.fullmatch(r"[0-9A-Za-z_+*/() \t-]+", cleaned):
        sys.exit("la expresion de %s tiene algo que no se evaluar: %s"
                 % (what, expression.strip()))
    for name, value in env.items():
        cleaned = re.sub(r"\b" + name + r"\b", str(value), cleaned)
    if re.search(r"[A-Za-z_]", cleaned):
        sys.exit("la expresion de %s usa un nombre que no conozco: %s"
                 % (what, expression.strip()))
    return eval(cleaned, {"__builtins__": {}})

want = evaluate(allocation, "dyn_cache.h")
got = evaluate(hook, "el hook")

if want != got:
    print("FALLO  el rango del hook no es el malloc() del nucleo:")
    print("         nucleo: %d bytes (%s)" % (want, allocation.strip()))
    print("         hook:   %d bytes (%s)" % (got, hook.strip()))
    print("       %d bytes de mas es memoria de otro; de menos deja codigo" %
          (got - want))
    print("       en la cache sin permiso de ejecucion.")
    sys.exit(1)

print("  ok     el rango es el malloc() del nucleo: %d bytes" % want)
PYEOF
then
	exit 1
fi

echo
echo "0 fallos"
