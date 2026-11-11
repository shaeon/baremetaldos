//
// build_env.h
//
// by @mmoroca & arena.ai 2026  -  https://arena.ai
//
// BM-DOS - DOSBox-core running bare metal on the Raspberry Pi (Circle)
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
#ifndef _build_env_h
#define _build_env_h

// This header is force-included (-include) into every DOSBox translation unit.
// It exists because two well known headers are called config.h and dosbox.h:
// Circle owns /include/circle/config.h and DOSBox owns /include/dosbox.h, and
// both are on the include path.  Rather than fight with include order or with
// -iquote tricks, we redirect them once, here.
//
// Circle's config.h is reachable from Circle's own sources as <circle/config.h>
// only if that file exists, so we use the absolute-in-include-path form that
// Circle's headers themselves use.

// 1) DOSBox's own global header must never be shadowed by Circle's.
//    (Circle's config.h has a different include guard, so both can coexist,
//    we just have to make sure the *right* one is picked up depending on who
//    is asking.  This is handled in the makefile by include search order;  the
//    redefine below only documents it.)

// 2) DOSBox-core assumes the libretro build on POSIX-ish hosts and pulls in a
//    handful of things that do not exist bare metal.  We disable them here,
//    in one place, with a comment each, instead of sprinkling #ifdefs in the
//    DOSBox sources we do not own.

// No POSIX process/mman/socket layer in Circle.
#undef  C_HAVE_MPROTECT

// No IPX (needs SDL_net or BSD sockets).  DOSBox falls back to no networking.
#undef  C_IPX
#define C_IPX 0

// The modem and the null-modem talk TCP through src/dosbox-stub/SDL_net.h,
// which src/platform/bare_sdlnet.cpp implements on Circle's network: C_MODEM
// comes from the Makefile (1).  Serial passthrough to a host device does not
// exist here.  The dummy serial port (used by some games to probe COM1) stays
// available.
#undef  C_DIRECTSERIAL

// 3) Circle interrupts are documented to work on core 0 only, and DOSBox-core
//    runs its emulation thread with std::thread (Circle's cooperative
//    scheduler).  We keep everything on core 0 for the first milestone.
//    Nothing to define here, but it explains why ARM_ALLOW_MULTI_CORE is off.

#endif
