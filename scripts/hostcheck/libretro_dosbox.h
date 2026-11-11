//
// libretro_dosbox.h - a stand-in for dosbox-core's own shim header, so that
// bare_libretro.cpp can be compiled on the host.
//
// Why this file exists: bare_libretro.cpp includes two headers that live in
// third_party/dosbox-core - "libretro.h" (the libretro API, a fixed public
// contract) and "libretro_dosbox.h" (DOSBox-core's own declarations).  Neither
// is in this repository, so the file was outside every check we run in the PC,
// and that is exactly where a one-character mistake ("pData" instead of the
// parameter's real name "data") produced a broken build twice in a row.
//
// Declares only what bare_libretro.cpp uses today.  A signature that differs
// from the real one passes here - this is a smoke test for our own code, not a
// conformance test against DOSBox-core.  If bare_libretro.cpp starts using
// something else from the core, this file grows a line.
//
#pragma once

#include <cstddef>
#include <cstdint>

// The keyboard map lives in DOSBox-core (libretro_input.cpp) and is the thing
// that turns a RETROK_* into a DOS scan code.
unsigned char retro_dosbox_map (unsigned nRetroKey);

// The core's exit flags (libretro.cpp), put back by BareCoreRestart.
extern bool dosbox_exit;
extern bool frontend_exit;
