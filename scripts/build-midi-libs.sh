#!/bin/sh
#
# scripts/build-midi-libs.sh
#
# BM-DOS - fetch, patch and build the two MIDI synthesizers as static
# libraries for the board circle-stdlib was configured for:
#
#   * Munt (libmt32emu)  - Roland MT-32 / CM-32L emulation, LGPL-2.1+
#   * FluidSynth         - General MIDI from a SoundFont, LGPL-2.1+
#
# The approach, the pinned revisions and patches/fluidsynth/ come from mt32-pi
# by Dale Whinham (https://github.com/dwhinham/mt32-pi), GPL-3.0, which runs
# both synthesizers on bare metal Circle.  The difference here is the C++
# library: mt32-pi uses the toolchain's libstdc++, this port links libc++, so
# the flags below put circle-stdlib's libc++ headers first and keep the
# toolchain's out (-nostdinc).
#
# Usage:   ./scripts/build-midi-libs.sh          (after ./scripts/setup.sh)
# Output:  third_party/midi-rpi$RASPPI/{include,lib}
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.

set -eu

REPO_ROOT=$(cd "$(dirname "$0")/.." && pwd)
TP="$REPO_ROOT/third_party"
STDLIB="$TP/circle-stdlib"

MUNT_REPO=https://github.com/munt/munt.git
FLUID_REPO=https://github.com/FluidSynth/fluidsynth.git
MUNT_REV=$(cat "$REPO_ROOT/MUNT_REV")
FLUID_REV=$(cat "$REPO_ROOT/FLUIDSYNTH_REV")
FLUID_PATCH="$REPO_ROOT/patches/fluidsynth/fluidsynth-2.3.1-circle.patch"

die() { echo "build-midi-libs: $*" >&2; exit 1; }

# --- toolchain and circle-stdlib --------------------------------------------

TOOLCHAIN_BIN=$(ls -d "$REPO_ROOT"/opt/arm-gnu-toolchain-*/bin 2>/dev/null | head -n 1 || true)
[ -n "$TOOLCHAIN_BIN" ] && PATH="$TOOLCHAIN_BIN:$PATH" && export PATH
command -v aarch64-none-elf-gcc >/dev/null || die "aarch64-none-elf-gcc not found (see README: Cross toolchain)"
command -v cmake >/dev/null || die "cmake not found"
[ -f "$STDLIB/Config.mk" ] || die "circle-stdlib is not configured - run ./scripts/setup.sh first"

# Read one "KEY = value" line from a Makefile fragment.
mkvar() { sed -n "s/^$1[[:space:]]*=[[:space:]]*//p" "$2" | head -n 1; }

CIRCLE_CFG="$STDLIB/libs/circle/Config.mk"
RASPPI=$(mkvar RASPPI "$CIRCLE_CFG")
ARCHCPU=$(mkvar ARCHCPU "$STDLIB/Config.mk")
CIRCLE_DEFINES=$(mkvar DEFINE "$CIRCLE_CFG")
[ -n "$RASPPI" ] && [ -n "$ARCHCPU" ] || die "could not read RASPPI/ARCHCPU from circle-stdlib"

INSTALL="$STDLIB/install"
NEWLIB_INC="$INSTALL/aarch64-none-circle/include"
LIBCXX_INC="$INSTALL/aarch64-none-circle-libc++/include"
LIBCXX_THR_INC="$INSTALL/aarch64-none-circle-libc++-threading/include"
GCC_INC=$(aarch64-none-elf-gcc -print-file-name=include)
for d in "$NEWLIB_INC" "$LIBCXX_INC/c++/v1" "$GCC_INC"; do
	[ -d "$d" ] || die "missing include directory: $d"
done

# Same order as circle-stdlib's own CFLAGS: libc++ first, newlib, then GCC's
# freestanding headers (stdarg.h, arm_neon.h ...).
BASE_FLAGS="$ARCHCPU -DAARCH=64 -D__circle__ -DRASPPI=$RASPPI $CIRCLE_DEFINES \
-D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L -ffunction-sections -fdata-sections"
C_INC="-nostdinc -isystem $NEWLIB_INC -isystem $GCC_INC"
CXX_INC="-nostdinc -isystem $LIBCXX_INC/c++/v1 -isystem $LIBCXX_INC \
-isystem $LIBCXX_THR_INC -isystem $NEWLIB_INC -isystem $GCC_INC"

OUT="$TP/midi-rpi$RASPPI"
BUILD="$REPO_ROOT/build/midi-rpi$RASPPI"
TOOLCHAIN_FILE="$REPO_ROOT/cmake/aarch64-none-circle.cmake"

# --- sources at the pinned revisions ----------------------------------------

fetch() {	# dir repo rev
	if [ ! -d "$1/.git" ]; then
		git init -q "$1"
		git -C "$1" remote add origin "$2"
	fi
	if [ "$(git -C "$1" rev-parse HEAD 2>/dev/null || true)" != "$3" ]; then
		git -C "$1" fetch -q --depth 1 origin "$3"
		git -C "$1" checkout -q FETCH_HEAD
	fi
	echo "$(basename "$1") at $(git -C "$1" rev-parse --short HEAD)"
}
fetch "$TP/munt" "$MUNT_REPO" "$MUNT_REV"
fetch "$TP/fluidsynth" "$FLUID_REPO" "$FLUID_REV"

# Apply the FluidSynth patch once; applying it twice is not an error, and a
# patch that does not fit stops the build here instead of 2000 lines later.
if patch --dry-run --reverse --force -p1 -d "$TP/fluidsynth" < "$FLUID_PATCH" >/dev/null 2>&1; then
	echo "fluidsynth patch already applied"
else
	patch --forward -p1 --no-backup-if-mismatch -r - -d "$TP/fluidsynth" < "$FLUID_PATCH" \
		|| die "fluidsynth patch does not apply to $FLUID_REV"
fi

# --- Munt ----
#
# Munt builds with -ansi (C++98) by default; libc++ refuses anything before
# C++11 under GCC.  libmt32emu_REQUIRE_ANSI=FALSE drops it, and the standard is
# set to the same gnu++23 the rest of the port uses.
#----------------------------------------------------------------

echo "building libmt32emu for Raspberry Pi $RASPPI"
cmake -S "$TP/munt/mt32emu" -B "$BUILD/munt" \
	-DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_C_FLAGS="$BASE_FLAGS $C_INC" \
	-DCMAKE_CXX_FLAGS="$BASE_FLAGS $CXX_INC" \
	-DCMAKE_CXX_FLAGS_RELEASE="-Ofast" \
	-DCMAKE_CXX_STANDARD=23 -DCMAKE_CXX_EXTENSIONS=ON \
	-Dlibmt32emu_REQUIRE_ANSI=FALSE \
	-Dlibmt32emu_C_INTERFACE=FALSE \
	-Dlibmt32emu_SHARED=FALSE \
	-DCMAKE_INSTALL_PREFIX="$OUT" >/dev/null
cmake --build "$BUILD/munt"
cmake --install "$BUILD/munt" >/dev/null

# --- FluidSynth --------------------------------------------------------------

echo "building libfluidsynth for Raspberry Pi $RASPPI"
cmake -S "$TP/fluidsynth" -B "$BUILD/fluidsynth" \
	-DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_C_FLAGS="$BASE_FLAGS $C_INC" \
	-DCMAKE_CXX_FLAGS="$BASE_FLAGS $CXX_INC" \
	-DCMAKE_C_FLAGS_RELEASE="-Ofast -fopenmp-simd" \
	-DBUILD_SHARED_LIBS=OFF \
	-Denable-aufile=OFF -Denable-dbus=OFF -Denable-dsound=OFF \
	-Denable-floats=ON -Denable-ipv6=OFF -Denable-jack=OFF \
	-Denable-ladspa=OFF -Denable-libinstpatch=OFF -Denable-libsndfile=OFF \
	-Denable-midishare=OFF -Denable-network=OFF -Denable-oboe=OFF \
	-Denable-openmp=OFF -Denable-opensles=OFF -Denable-oss=OFF \
	-Denable-pipewire=OFF -Denable-pulseaudio=OFF -Denable-readline=OFF \
	-Denable-sdl2=OFF -Denable-threads=OFF -Denable-waveout=OFF \
	-Denable-winmidi=OFF >/dev/null
cmake --build "$BUILD/fluidsynth" --target libfluidsynth

# libfluidsynth has no install rule of its own when only that target is
# built, so copy the library and the public headers (the generated
# version.h lives in the build tree).
mkdir -p "$OUT/lib" "$OUT/include/fluidsynth"
find "$BUILD/fluidsynth" -name 'libfluidsynth*.a' -exec cp {} "$OUT/lib/libfluidsynth.a" \;
cp "$BUILD/fluidsynth/include/fluidsynth.h" "$OUT/include/"	# generated
cp "$TP/fluidsynth/include/fluidsynth/"*.h "$OUT/include/fluidsynth/"
cp "$BUILD/fluidsynth/include/fluidsynth/"*.h "$OUT/include/fluidsynth/"	# version.h

echo
echo "done: $OUT"
ls -l "$OUT/lib"
