#!/usr/bin/env bash
#
# Build DOSBox-core the *upstream* way, on the host.  Not part of the bare
# metal build: it answers "does the DOSBox tree I have still build on its own?"
# before blaming the port's build for a failure.
#
# Usage:
#   ./scripts/build-core.sh      # after scripts/setup.sh; needs third_party/dosbox-core
#
# This is not part of the bare metal build.  It exists because it answers a
# question you will ask at some point: "is the DOSBox tree I have actually
# good?"  It builds the very same sources the port compiles into a libretro .so
# for your Linux machine, which you can then run in RetroArch.
#
# If this build works and the Circle build does not, the problem is in the port
# (the platform layer), not in DOSBox-core or in the revision you checked out.
# That is a very useful thing to be able to rule out.
#
#   ./scripts/build-core.sh
#   ls third_party/dosbox-core/libretro/dosbox_core_libretro.so
#
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

DOSBOX_DIR="${DOSBOX_DIR:-$REPO_ROOT/third_party/dosbox-core}"

if [ ! -d "$DOSBOX_DIR" ]; then
	echo "error: $DOSBOX_DIR not found - run 'make fetch' first" >&2
	exit 1
fi

echo "=== dosbox-core revision"
git -C "$DOSBOX_DIR" log -1 --oneline

echo
echo "=== building the upstream libretro core (unix, no extra MIDI backends)"
# Keep it minimal on purpose: no fluidsynth, no munt, no bassmidi.  That is the
# same feature set the bare metal port has, so a comparison is meaningful.
make -C "$DOSBOX_DIR/libretro" \
	platform=unix \
	WITH_FAKE_SDL=0 \
	WITH_FLUIDSYNTH=0 \
	WITH_BASSMIDI=0 \
	-j"$(nproc)"

echo
echo "=== done"
ls -la "$DOSBOX_DIR"/libretro/*.so
cat <<'EOF'

To try it in RetroArch:

  1. copy dosbox_core_libretro.so into ~/.config/retroarch/cores/
  2. copy dosbox_core_libretro.info into ~/.config/retroarch/info/
  3. load any DOS .conf / .exe / .iso as content

The behaviour you see there is the behaviour the bare metal port aims for; if
something differs, the difference is in src/platform/.
EOF
