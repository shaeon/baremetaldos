#!/bin/sh
#
# Check when retro_set_controller_port_device() may be called, which is the
# half of controller support that is not the HID mapping.
#
# Usage:
#   make check-portorder         # part of make check-host
#
#
# check-portorder.sh - check-portorder
#
# Two halves, because the .61 failure had two faces:
#
#   * the RULES - when may this port talk to the core about its game port.
#     portorder_test.cpp runs them on the host (bare_portorder.h is pure
#     bookkeeping: no core, no Circle, no libretro.h).
#
#   * the CALL ORDER in the real file.  A correct state machine that
#     bare_libretro.cpp forgets to use is worth nothing, and that is exactly
#     how .61 shipped: the rules did not exist, the call sat before the load,
#     and every check in this tree passed.  So this script also reads
#     src/platform/bare_libretro.cpp and insists that the first call to
#     retro_set_controller_port_device() comes *after* retro_load_game().
#
# A note on why a line-order check is legitimate here and not a smell: the two
# things being ordered are two calls in one function of one file, and the
# ordering IS the bug.  The alternative - trusting a comment that says "do not
# move this" - has already been tried for one round.
#
set -e

DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$DIR/../.." && pwd)"
CXX="${HOST_CXX:-g++}"
SRC="$ROOT/src/platform/bare_libretro.cpp"

# --- the rules ------------------------------------------------------------
"$CXX" -std=gnu++23 -Wall -Wextra -Wpedantic -Wconversion \
	-I "$ROOT/src/platform" \
	-o /tmp/check-portorder.$$ "$DIR/portorder_test.cpp"
/tmp/check-portorder.$$
rm -f /tmp/check-portorder.$$

# --- the call order in the real file --------------------------------------
# The declaration at the top of the file is not a call; skip it.  The calls are
# written across two lines, so match the function name alone.
nFirstAnnounce=$(grep -n 'retro_set_controller_port_device (' "$SRC" \
	| grep -v 'void retro_set_controller_port_device' \
	| head -1 | cut -d: -f1)
nLoad=$(grep -n 'if (!retro_load_game (&game))' "$SRC" | head -1 | cut -d: -f1)

if [ -z "$nFirstAnnounce" ] || [ -z "$nLoad" ]; then
	echo "FALLO  no encuentro la llamada ni la carga en $SRC"
	exit 1
fi

if [ "$nFirstAnnounce" -lt "$nLoad" ]; then
	echo "FALLO  retro_set_controller_port_device() en la linea $nFirstAnnounce,"
	echo "       retro_load_game() en la $nLoad: se le habla al core ANTES de cargar."
	echo "       Dentro del core eso es control->GetSection() con control == null"
	echo "       (lo pone el task del emulador).  Ver bare_portorder.h."
	exit 1
fi

echo "  ok     el primer anuncio (linea $nFirstAnnounce) va despues de la carga ($nLoad)"
echo
echo "0 fallos"
