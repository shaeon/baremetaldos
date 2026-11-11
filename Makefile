#
# Makefile
#
# BM-DOS - DOSBox-core on bare metal Raspberry Pi (Circle)
#
# Two kinds of build live in this file:
#
#   make            build kernel8-rpi4.img (or whichever RASPPI you configured)
#   make fetch      fetch DOSBox-core at the pinned commit
#   make distclean  remove the build tree and the fetched sources
#
# The interesting part is the include-path and source-list juggling further
# down: DOSBox-core was written to be built by its own automake/Makefile.libretro
# system as a shared library for a POSIX host.  We build the same sources into a
# Circle kernel, so this makefile has to reproduce three things that system
# provided:
#
#   1. The set of defines that select the libretro platform (__LIBRETRO__,
#      C_TARGETCPU, ...), reproduced from libretro/Makefile.common.
#   2. The subset of the tree that is actually needed (no SDL, no MIDI
#      backends, no fluidsynth/munt, no virtual keyboard, no frontend bits
#      replaced by src/dosbox-stub).
#   3. Include path ordering, because two well known headers collide:
#      DOSBox has include/config.h and include/dosbox.h, and Circle has
#      include/circle/config.h.  We put DOSBox's tree first and let Circle's
#      headers be found through <circle/...> as they are meant to be.
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#

# ---------------------------------------------------------------------------
# What to build
# ---------------------------------------------------------------------------

# Version stamp.  Printed by "make check" and "make version" so that a support
# conversation can start with "which snapshot are you running?" instead of
# guessing.  Bump it whenever the build system changes.
PROJECT_SNAPSHOT ?= 2026-11-10.78
PROJECT_SNAPSHOT_DATE ?= 2026-11-10

# Raspberry Pi model: 3, 4 or 5.  4 is the default because it is the sweet
# spot for this emulator (AArch64, fast enough for the interpreter core).
RASPPI		?= 4
empty :=
space := $(empty) $(empty)

AARCH		?= 64

# Where DOSBox-core lives.  Fetched by "make fetch" if missing.
DOSBOX_DIR	?= $(CURDIR)/third_party/dosbox-core

# Pinned upstream revision.  Bump deliberately, not automatically: the stub
# layer and the minimal libretro.h have to be re-checked against it.
DOSBOX_REV	?= $(shell cat ./DOSBOX_REV)
DOSBOX_REPO	?= https://github.com/realnc/dosbox-core.git
DOSBOX_BRANCH	?= libretro

# circle-stdlib checkout.  It carries Circle itself as a submodule, plus
# newlib and (because we ask for it) libc++ - which is what gives us
# std::filesystem, std::thread and std::mutex on bare metal.
CIRCLE_STDLIB	?= $(CURDIR)/third_party/circle-stdlib

# Where the Arm toolchain lives.  Everything this project downloads lives under
# third_party/, and the toolchain is no exception: one directory, self-contained,
# nothing in $HOME.
#
# This is not cosmetic.  The tree is synced from the workspace with rsync, and a
# sync that deletes (which is needed when a source file is retired) removes
# anything in the destination that is not in the workspace.  A 1.2 GB toolchain
# sitting in an unexcluded directory of this tree - opt/, say - is deleted by
# that sync.  That is exactly what happened once.  So: opt/ is in .gitignore, in
# how they are excluded from a sync, and the toolchain check below looks
# here first.
TOOLCHAIN_DIR	?= $(CURDIR)/opt

# $(PREFIX) comes from circle-stdlib's Config.mk as PREFIX64.  Circle's own
# Config.mk hardcodes it ("PREFIX64 = aarch64-none-elf-") and Circle has no
# notion of an install directory, so the path is put in front of PATH here, for
# this make invocation only.  If the toolchain is not there, PATH is left alone
# and the check further down says so.
TOOLCHAIN_BIN := $(firstword $(wildcard $(TOOLCHAIN_DIR)/arm-gnu-toolchain-*/bin))
ifneq ($(TOOLCHAIN_BIN),)
export PATH := $(TOOLCHAIN_BIN):$(PATH)
endif

# ---------------------------------------------------------------------------
# Circle's own rules
# ---------------------------------------------------------------------------

# The staging rules below are the first targets make sees, so state the goal
# explicitly - otherwise "make" builds one symlink and stops.
.DEFAULT_GOAL := all

# ---------------------------------------------------------------------------
# Administrative goals vs. real builds
# ---------------------------------------------------------------------------
#
# Three goals have to work on a bare checkout, before anything is downloaded or
# configured:
#
#   make fetch      clone DOSBox-core
#   make check-rev  compare the tree against DOSBOX_REV
#   make distclean  throw everything away
#
# "make check" is deliberately NOT in that list: it compiles a probe with the
# real flags, so it needs both includes below and it is useless without them.
# Listing it here made it run with an empty $(CXXFLAGS_APP) and report every
# include directory as missing - a false alarm that would have cost another
# round trip.
#
# They must not require third_party/circle-stdlib to exist or the toolchain to
# be installed, because they are precisely what you run when it is not.  So the
# two includes below - which are what define $(CPP), $(TARGET), $(CFLAGS) and
# friends - are skipped when one of those is the only goal.
ADMIN_GOALS := fetch check-rev distclean version help check-sources print-srcs upstream-srcs deps
REAL_GOALS  := $(filter-out $(ADMIN_GOALS),$(if $(MAKECMDGOALS),$(MAKECMDGOALS),all))

ifneq ($(REAL_GOALS),)

# What is missing, if anything.  This is only *reported* at run time, by the
# "setup" rule at the very end of this file: a bare $(error) here would abort
# the parse, which is the one thing that must never happen for "make fetch".
ifeq ($(wildcard $(CIRCLE_STDLIB)/Config.mk),)
ACTIVE_ERR := $(CIRCLE_STDLIB)/Config.mk not found.\n\nCircle and libc++ are not built. Run:\n\n    ./scripts/setup.sh\n\n(that script also fetches the Arm toolchain into $(TOOLCHAIN_DIR) if it is\nmissing; both live inside this project on purpose)\n
else
# Every *.mk fragment is guarded with $(wildcard ...) before it is included.
# The reason is not cosmetic: if the file appears and disappears between the
# check two lines above and the include, make stops with
# "Config.mk: No such file or directory", which is a confusing way to say "the
# file was not there".  $(wildcard) is expanded right here, so what is tested is
# what is included.
include $(wildcard $(CIRCLE_STDLIB)/Config.mk)

# CIRCLEHOME comes from circle-stdlib's Config.mk when it defines it, and from
# here otherwise.  It has to be assigned *after* the include for exactly that
# reason: with "?=" before the include, Config.mk would overwrite nothing and a
# relocateable circle-stdlib tree would build with the wrong CIRCLEHOME - and,
# worse, "make check" would report "FALTA /addon/" for a path that starts with
# an empty directory.
CIRCLEHOME ?= $(CIRCLE_STDLIB)/libs/circle
export CIRCLEHOME

# NOT included here, deliberately.  Rules.mk's link rule is
#
#     $(TARGET).img: $(OBJS) $(LIBS) $(CIRCLEHOME)/circle.ld
#
# and a rule's prerequisites are expanded when the rule is READ.  Including
# Rules.mk before the source lists exist would therefore give it empty $(OBJS)
# and $(LIBS): the link line would have no objects in it, and - worse - make
# would consider the image up to date without compiling anything.
# Rules.mk is included further down, once $(OBJS) and $(LIBS) are defined.
CIRCLE_STDLIB_CONFIGURED := 1

# circle-stdlib has to be *complete*: its Config.mk can exist while the tree is
# only half cloned (or while third_party/circle-stdlib/include is missing), and
# then the first thing that fails is kernel.h, with a message about
# circle_stdlib_app.h that says nothing about the real cause.
ifeq ($(wildcard $(CIRCLE_STDLIB)/include/circle_stdlib_app.h),)
ACTIVE_ERR := $(CIRCLE_STDLIB)/include/circle_stdlib_app.h not found.\n\ncircle-stdlib is incomplete or was removed. Delete it and run:\n\n    ./scripts/setup.sh\n
endif

# DOSBox-core has to be there too; "make fetch" is a separate step on purpose,
# so that a fresh clone does not silently pull a network dependency.
ifeq ($(wildcard $(DOSBOX_DIR)/src/dosbox.cpp),)
ACTIVE_ERR := $(DOSBOX_DIR) not found.\n\nDOSBox-core is not fetched. Run:\n\n    make fetch\n
endif

# Everything is missing when Config.mk is absent, and the checks above are
# meaningless in that case (no $(PREFIX), no $(TARGET)).
endif # Config.mk exists

endif # REAL_GOALS

# ---------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------

# TARGET comes from Circle: kernel8 (Pi 3), kernel8-rpi4 (Pi 4), kernel_2712 (Pi 5).

# ---------------------------------------------------------------------------
# Compile flags for the DOSBox tree
# ---------------------------------------------------------------------------

# The fmt headers ship inside the dosbox-core tree itself (libretro/deps/fmt),
# they are not a system package: that is why the libretro build has no
# dependencies at all.
# newlib's headers come first and that ordering is not cosmetic: the toolchain
# ships its own libc headers for a bare metal target with no C library (stddef.h
# and friends), and newlib's versions have to win.  Getting this wrong produces
# errors inside <string.h>, far from the real cause.
#
# Note what is NOT added here: the libc++ include paths.  They are already in
# CFLAGS, generated by circle-stdlib's configure in the exact order that
# libc++'s #include_next chain requires.  Appending them again (or reordering
# them) breaks libc++ in ways that look like a compiler bug.
DOSBOX_INCLUDES = \
	-isystem$(CIRCLE_STDLIB_INSTALL_DIR)/include \
	$(FATFS_INCLUDES) \
	-iquote$(DOSBOX_DIR)/include \
	-I$(DOSBOX_DIR)/include -I$(DOSBOX_DIR)/src \
	-I$(DOSBOX_DIR)/libretro -I$(DOSBOX_DIR)/libretro/src \
	-I$(DOSBOX_DIR)/libretro/src/virtual_keyboard -I$(DOSBOX_DIR)/libretro/src/fakesdl \
	-I$(DOSBOX_DIR)/libretro/src/fakesdl/SDL \
	-I$(DOSBOX_DIR)/libretro/deps/fmt/include -I$(DOSBOX_DIR)/libretro/deps/common/include \
	-I$(CURDIR)/src/dosbox-stub -I$(CURDIR)/src/platform
# NOTE: src/libretro-common is deliberately NOT on this path any more.  It
# holds a hand-written subset of libretro.h, and `#include "libretro.h"`
# resolves to whichever -I comes first: with both paths that is the empty
# $(DOSBOX_DIR)/libretro directory, so the real header from
# deps/common/include won.  Which means the subset was dead code and the
# port was built against upstream's libretro.h all along - while the
# comments in our own platform code talked about the subset's typedefs.
# One header, the upstream one, is the only arrangement in which the
# answer to "what does this typedef say" is not a matter of path order.

# The defines the libretro build uses, minus the ones we cannot support:
#
#   __LIBRETRO__     selects the libretro platform layer inside DOSBox
#   C_TARGETCPU      picks the CPU engine; we start with the interpreter
#   FMT_HEADER_ONLY  fmt is header-only in the port (no separate .cc to build)
#   C_IPX=0          no IPX: it wants BSD sockets / SDL_net
#   C_MODEM=0        no host modem
#   C_HAVE_MPROTECT  must NOT be defined: no mprotect before the JIT milestone
#
# C_STANDARD/C++ standard: DOSBox-core needs C++17 at least; its own build uses
# gnu++17 for GCC, and so do we.
# The CPU engine is selected with *no value*, exactly as libretro/Makefile.common
# does it (core_dynrec.cpp compares C_TARGETCPU against an enum of values).  For
# milestone 1 C_DYNREC is not defined at all, so DOSBox falls back to the
# portable interpreter.  Uncomment the last line when the JIT milestone starts;
# see the mprotect and I-cache notes in README.md, "How it works".
# WITH_FAKE_SDL is what the libretro platform layer uses to mean "there is no
# SDL here".  It is not only about the fakesdl headers: DOSBox's own code is
# guarded by it too (src/dos/cdrom.cpp, src/dos/dos_mscdex.cpp), which is why it
# must be set even in a build that links no SDL at all.
DOSBOX_DEFINES := \
	-D__LIBRETRO__ \
	-DWITH_FAKE_SDL \
	-DC_IPX=0 \
	-DC_MODEM=0 \
	-DFMT_HEADER_ONLY \
	-DGIT_VERSION=\"baremetal\" \
	-DSVN_VERSION=\"\" \
	-DPAGESIZE=4096 \
	-D__QNX__ \
	-DNO_PWD_H
#
# __QNX__ and NO_PWD_H are not a joke, and they are the same kind of decision as
# C_IPX=0: they switch off two POSIX features of DOSBox's config.h that Circle
# does not have.  They are defined on the *command line* because include/config.h
# has no include guard (it is a generated file, meant to be included once), so a
# -include wrapper cannot patch it - the second inclusion would redefine
# everything and there is nothing to stop it.
#
#   __QNX__     config.h does:
#                   #if !defined(__QNX__)
#                   #define DIRENT_HAS_D_TYPE 1
#                   #endif
#               i.e. it assumes struct dirent has d_type.  newlib's does not, and
#               src/misc/cross.cpp uses dentry->d_type under that macro:
#                   error: 'struct dirent' has no member named 'd_type'
#               With it off, cross.cpp falls back to the stat() path that it
#               uses for filesystems that do not report the type, which is
#               exactly right for FatFs.  QNX is used as the switch because
#               config.h already reasons in those terms; nothing else in the
#               tree tests __QNX__ except a few libretro-common files we do not
#               build.
#
#   NO_PWD_H    config.h also does:
#                   #if !defined(__WIN32__) && !defined(__POWERPC__)
#                   #define HAVE_PWD_H 1
#                   #endif
#               and cross.cpp includes <pwd.h> and calls getpwnam() under
#               HAVE_PWD_H, to expand "~username".  newlib *does* ship <pwd.h>
#               (so this is not a compile error), but circle-newlib's libgloss
#               implements neither getpwnam() nor getpwuid() - there is no passwd
#               database on the board - so it would be an undefined reference at
#               link time, the least pleasant moment to find out.  NO_PWD_H is
#               not tested by DOSBox at all: it is a libretro-common convention,
#               and turning it off simply drops the lookup.  "~" and "~/x" keep
#               working, through $HOME, which bare_fs.cpp sets.

# The dynarec, off on purpose.  Uncomment the line below and read
# the dynarec notes in bare_dynarec.cpp first: the backend compiles, but Circle
# marks the heap execute-never (PXN = 1 after _etext) and the JIT needs it
# executable, plus per-range I/D cache maintenance.  Two real problems.
#
#	-DC_DYNREC -DC_TARGETCPU=ARMV8LE

# Two lists, not one, because they answer different questions.
#
# DOSBOX_WARNINGS applies only to DOSBox's own sources - 115 upstream files we
# do not maintain.  Every suppression in it is a warning that fires in *their*
# code, and turning it off there keeps the noise down without hiding anything in
# ours.
#
# APP_WARNINGS applies to src/ - our code - and suppresses almost nothing.
# Keeping them apart is what makes "no warnings in our code" checkable.
# What fires in DOSBox's code that is not a bug there, and why.  Kept as its own
# variable so that "make WARN_FULL=1" can empty it and show everything.
#
#   -Walloc-size-larger-than   fires inside libc++ when GCC inlines any of
#                              std::string/std::filesystem into our code or into
#                              DOSBox's.  Two places seen so far:
#                                dosbox-core/src/misc/setup.cpp  (std::getline)
#                                src/platform/bare_fs.cpp        (path from a
#                                                                 const char*)
#                              The allocator's size is `size_t` and GCC proves it
#                              *could* be negative (it prints 18446744073709551608
#                              = -8 as unsigned).  It is a libc++ internals
#                              warning, not a bug in the call site, and it cannot
#                              be fixed without patching libc++ - which would be
#                              a fork of circle-stdlib for a warning that is not
#                              real.  WARN_FULL=1 to see it again.
#
#   -Wdeprecated-literal-operator
#                              fmt, vendored inside dosbox-core, writes
#                              operator"" _format with a space, which C++23
#                              deprecated.  Their code, their literal; fixing it
#                              means patching a vendored dependency for a
#                              warning.  WARN_FULL=1 to see it again.
DOSBOX_WARNINGS_EXTRA := $(if $(WARN_FULL),,-Wno-alloc-size-larger-than \
					   -Wno-deprecated-literal-operator)

DOSBOX_WARNINGS := -Wno-register -Wno-unused-parameter -Wno-sign-compare \
		   -Wno-unused-variable -Wno-unused-but-set-variable \
		   -Wno-format-truncation -Wno-stringop-truncation \
		   -Wno-maybe-uninitialized -Wno-class-memaccess \
		   $(DOSBOX_WARNINGS_EXTRA)

# Our code.  -Wall -Wextra come from Circle's Rules.mk; this only adds what is
# worth having and cannot be satisfied upstream-style (-Wno-unused-parameter,
# because a libretro callback has to take parameters it may not use).
#
#   -Wno-alloc-size-larger-than  the same libc++ internals warning as above, but
#                                reached from *our* bare_fs.cpp: constructing a
#                                std::filesystem::path from a const char*.  It is
#                                not our arithmetic and it is not a real
#                                possibility (the string is a fixed path), but it
#                                fires in a file we own, so the honest thing is
#                                to suppress it here, by name, with the reason -
#                                not to hope nobody notices.  WARN_FULL=1 keeps
#                                it suppressed on purpose: what that variable
#                                shows is DOSBox's noise, and this is libc++'s.
APP_WARNINGS := -Wno-unused-parameter -Wno-alloc-size-larger-than

# Circle's Rules.mk already put -I circle/include etc. into $(INCLUDE), and
# $(CPPFLAGS) carries the board defines (RASPPI, AARCH, STDLIB_SUPPORT...).
# We append ours so Circle's headers win for <circle/...> and DOSBox's win for
# everything else.
# C++23, because libc++ was built that way (see the note on CXXFLAGS_APP below).
# DOSBox-core needs std::filesystem plus a few details from C++17 and up, so
# C++23 is a superset of what it asks for.
# build_env.h is force-included into every DOSBox translation unit; see that
# file for why (it is the one place where the "no POSIX, no IPX" decisions live).
# NOTE the "=" (recursive, expanded at use) and not ":=".  $(CPPFLAGS) and
# $(CFLAGS) are built by Circle's Rules.mk, which is included further down (it
# has to be: its link rule reads $(OBJS)).  With ":=" these three variables
# would be frozen empty here, and every compile would fail on the first Circle
# header - "circle/types.h: No such file or directory" - because none of
# Circle's -I flags would be on the command line.
CXXFLAGS_DOSBOX = $(CPPFLAGS) -std=gnu++23 \
		   -include $(CURDIR)/src/platform/build_env.h \
		   $(DOSBOX_INCLUDES) $(DOSBOX_DEFINES) $(DOSBOX_WARNINGS)
CFLAGS_DOSBOX   = $(CFLAGS)   $(DOSBOX_INCLUDES) $(DOSBOX_DEFINES) $(DOSBOX_WARNINGS)

# Our own sources: Circle's flags (which already carry the stdlib include path
# and the board defines) plus the few DOSBox headers bare_libretro.cpp needs.
# Note what is NOT here: build_env.h is force-included only into DOSBox's
# sources, because it neutralises POSIX assumptions that our own code does not
# make in the first place.
# Our own sources need three extra groups of include paths beyond Circle's own
# (which $(CPPFLAGS) already provides through $(INCLUDE)):
#
#   $(CIRCLE_STDLIB_INCLUDES)  circle_stdlib_app.h, circle_glue.h,
#                              wrap_fatfs.h - i.e. circle-stdlib's own headers,
#                              which live in circle-stdlib/include, NOT in
#                              Circle's include/ directory.  This is what
#                              kernel.h needs.
#   $(DOSBOX_DIR)/libretro     libretro_dosbox.h and the libretro platform
#                              headers (emu_thread.h, libretro_gfx.h...).
#   $(DOSBOX_DIR)/libretro/src same, for the headers that live one level down.
#
# $(CIRCLE_STDLIB_INCLUDES) comes from circle-stdlib's Config.mk.  It is written
# as a recursive "=" assignment for the same reason as the flags below: this
# file is read before $(CPPFLAGS) exists.
# fatfs.  This one is not cosmetic, and it is not about FatFs itself:
#
#   newlib's  <dirent.h>
#     -> <sys/dirent.h>                  (from circle-newlib)
#          -> <wrap_fatfs.h>             (from circle-stdlib/include)
#               -> <fatfs/ff.h>          (from Circle's addon/fatfs)
#
# So "#include <dirent.h>" only works if *both* of those directories are on the
# search path, and the failure when one is missing points at the wrong file: the
# compiler says "wrap_fatfs.h: No such file or directory" while it is compiling
# libretro.cpp, which never mentions FatFs.  That is exactly what happened.
#
# Why this is here and not in APP_INCLUDES: our own sources include
# <circle_stdlib_app.h>, which pulls in wrap_fatfs.h from the same directory, so
# $(CIRCLE_STDLIB_INCLUDES) already covers them and they were compiling fine.
# DOSBox's sources do not; they only ever touch <dirent.h>, and that is enough.
#
# Neither path comes from anywhere else on the DOSBox command line:
#
#   - $(CIRCLE_STDLIB_INSTALL_DIR)/include is newlib's installed tree; the
#     circle-stdlib headers (wrap_fatfs.h, circle_stdlib_app.h) live one level
#     up, in $(CIRCLE_STDLIB)/include.  That is the one that was missing.
#   - <addon>/ is on the path for Circle's own compiles, because Rules.mk puts
#     it in $(INCLUDE); it is repeated here so the DOSBox command line does not
#     depend on that.  Note the trailing slash: it is what makes
#     "fatfs/ff.h" resolve to <addon>/fatfs/ff.h.
FATFS_INCLUDES = -I$(CIRCLE_STDLIB)/include -I$(CIRCLEHOME)/addon/

# fmt is not a system package: dosbox-core ships it under libretro/deps/fmt and
# its own headers (#include <fmt/format.h> from log.h, reached through
# CoreOptions.h) assume it is on the include path.  Our src/dosbox-stub includes
# those headers, so this path is needed here too - not just for DOSBox.
#
# deps/common/include is where the ONLY libretro.h of this build lives (the one
# dosbox-core ships).  Our own code includes "libretro.h" too, so this path has
# to be here as well, or our files would compile against nothing.
APP_INCLUDES = $(CIRCLE_STDLIB_INCLUDES) \
		-I$(DOSBOX_DIR)/include -I$(DOSBOX_DIR)/libretro -I$(DOSBOX_DIR)/libretro/src \
		-I$(DOSBOX_DIR)/libretro/deps/common/include \
		-I$(DOSBOX_DIR)/libretro/deps/fmt/include \
		-I$(CURDIR)/src/platform -I$(CURDIR)/src/dosbox-stub
# C++23, not C++20: circle-stdlib builds libc++ from the LLVM monorepo with
# -std=c++23, and its headers use C++23-only constructs (for example a "static
# constexpr" local inside a constexpr function, in __algorithm/equal.h and
# ranges_equal.h).  Compiling consumers with -std=c++20 fails inside <algorithm>
# at the first include of <vector>/<string>, far from any of our code.  The
# standard has to match what the library was built with.
# The project stamp is visible to our own sources so that the status overlay can
# print it: when the screen shows a snapshot id, there is no doubt about which
# build is running on the card.  Only the APP translation units get it.
# Sound device priority.  "auto" (the default) tries HDMI, then the 3.5 mm jack
# through PWM, then I2S - and picks the first one that actually starts, which is
# a real test: Circle's HDMI device refuses to start when the display does not
# announce audio.  What no test can detect is a display that announces audio and
# has no speakers, which is the usual case on a monitor.  So:
#
#   make AUDIO_DEVICE=pwm      force the headphone jack (PWM)
#   make AUDIO_DEVICE=auto     HDMI first, jack as fallback (default)
#
# Changing it requires a rebuild of bare_audio.o only, and the status panel
# shows which device won ("PWM (jack, forced)" / "HDMI").
AUDIO_DEVICE ?= auto
ifeq ($(AUDIO_DEVICE),pwm)
BARE_AUDIO_DEVICE_DEFINE := -DBARE_AUDIO_DEVICE=1
else
BARE_AUDIO_DEVICE_DEFINE := -DBARE_AUDIO_DEVICE=0
endif

# ---------------------------------------------------------------------------
# The dynamic recompiler
# ---------------------------------------------------------------------------
#   make               build WITH the JIT (default since .63)
#   make DYNAREC=0     build the portable interpreter, to A/B the same game
#
# Whether the JIT is *used* is a runtime question and DOSBox already has the
# switch for it: `core=` in [cpu] of dosbox.conf (dynamic / auto / normal, see
# README.md).  This variable only decides whether the engine is in the
# binary - with DYNAREC=0, `core=dynamic` is not even offered and the setting
# falls back to the interpreter without saying anything.
#
# Two things had to exist for the engine to be usable, and neither is upstream's
# business:
#
#   * the code cache is malloc'd, so it lands above _etext, which Circle maps
#     with PXN = 1: a JIT executing from there faults on its first block.
#     src/platform/bare_dynarec.cpp clears PXN on exactly those pages by walking
#     the page tables (Circle has no API for it), and refuses the whole range if
#     the tables are not what it expects.
#
#   * ordering: that has to happen after cache_init() has malloc'd the buffer,
#     and cache_code_start_ptr is `static` inside dyn_cache.h.  The way in is a
#     generated translation unit that #includes core_dynrec.cpp and defines
#     CPU_Core_Dynrec_Cache_Init() on top of the engine's own, which a -D
#     renames out of the way.  See the long block comment further down, next to
#     DOSBOX_DYNAREC_UNIT, and `make check-dynarec-hook`, which compiles it
#     against the pinned sources.
DYNAREC ?= 1
ifeq ($(DYNAREC),1)
# NO quotes around ARMV8LE, and that is the whole point of this line.
#
# Upstream writes -DC_TARGETCPU="ARMV8LE" (libretro/Makefile.common:239) and
# relies on the shell eating the quotes, so the compiler sees the bare token.
# Written with the quotes escaped into the Makefile - which is what this port
# did until .63 - the compiler instead sees a *string*, and core_dynrec.cpp's
# "#if C_TARGETCPU == ARMV8LE" line stops with
#
#     error: token "\"ARMV8LE\"" is not valid in preprocessor expressions
#
# And the token is not the string that gets compared: ARMV8LE is #define'd to
# 0x07 inside core_dynrec.cpp, so the comparison is numeric.  Both facts live
# in src/platform/bare_dynarec_flags.h, and `make check-dynarec-target`
# compiles that header with this exact flag and fails if either stops being
# true - which is what makes this line checkable instead of merely written.
DOSBOX_DYNAREC_DEFINE := -DC_DYNREC -DC_TARGETCPU=ARMV8LE
else
DOSBOX_DYNAREC_DEFINE :=
endif

# The JIT hook: one generated translation unit that *replaces* the engine's.
#
# Why a wrapper TU and not an -include: dyn_cache.h keeps `cache_code` with
# internal linkage, and an -include'd header is processed before the source, so
# it cannot see it.  Both alternatives were tried and both fail - the -include
# at the first declaration, and a second TU by silently reading a *different*,
# null copy of the pointer.  What works is being in the same TU, and the only
# way in without editing upstream is to compile a file that includes their .cpp:
#
#     build/bare_dynrec_unit.cpp  #includes src/cpu/core_dynrec.cpp, then
#                                 defines CPU_Core_Dynrec_Cache_Init() on top
#
# and the engine's own function is renamed out of the way with a command-line
# macro, which applies from the file's first line - so its prototype at the top
# is renamed with it and nothing is left half-renamed:
#
#     -DCPU_Core_Dynrec_Cache_Init=BareDynrecCacheInitOriginal
#
# Generated by scripts/hostcheck/gen-dynarec-unit.py, which also checks that the
# pinned revision still has the three properties this depends on.  Compiled
# INSTEAD OF core_dynrec.cpp, never as well as: two copies of the engine would be
# two definitions of everything it exports.
# (The object lands in build/build/ because OBJ_OF prefixes build/ to a path
# that is already inside it.  Harmless, and visible here so that nobody spends
# ten minutes wondering why.)
ifeq ($(DYNAREC),1)
DOSBOX_DYNAREC_UNIT := $(CURDIR)/build/bare_dynrec_unit.cpp
DOSBOX_DYNAREC_HOOK := -DCPU_Core_Dynrec_Cache_Init=BareDynrecCacheInitOriginal
else
DOSBOX_DYNAREC_UNIT :=
DOSBOX_DYNAREC_HOOK :=
endif

# C_DYNREC selects a CPU engine *inside DOSBox*: it belongs with the DOSBox
# defines, not with ours.  Appending instead of editing the list above keeps the
# two directions visible - the interpreter default is still what the list says,
# and this is the one line that changes it.
DOSBOX_DEFINES += $(DOSBOX_DYNAREC_DEFINE)

# MT-32 / General MIDI on core 1 (src/platform/bare_midi.cpp).  The two
# synthesizer libraries are built by scripts/build-midi-libs.sh into
# third_party/midi-rpi$(RASPPI); when they are there the kernel links them and
# [midi] mididevice=mt32|fluidsynth works, when they are not the build is the
# same as before and MIDI stays off.  Detection, not a switch, so that a clone
# that never ran the script still builds; "make MIDI=0" forces it off.
MIDI_DIR	?= $(CURDIR)/third_party/midi-rpi$(RASPPI)
ifeq ($(MIDI),0)
MIDI_ENABLED	:= 0
else ifneq ($(wildcard $(MIDI_DIR)/lib/libmt32emu.a),)
ifneq ($(wildcard $(MIDI_DIR)/lib/libfluidsynth.a),)
MIDI_ENABLED	:= 1
else
MIDI_ENABLED	:= 0
endif
else
MIDI_ENABLED	:= 0
endif

ifeq ($(MIDI_ENABLED),1)
BARE_MIDI_DEFINE := -DBARE_MIDI=1
BARE_MIDI_INCLUDES := -I$(MIDI_DIR)/include
BARE_MIDI_LIBS := $(MIDI_DIR)/lib/libmt32emu.a $(MIDI_DIR)/lib/libfluidsynth.a
else
BARE_MIDI_DEFINE := -DBARE_MIDI=0
BARE_MIDI_INCLUDES :=
BARE_MIDI_LIBS :=
endif

APP_DEFINES = -DDOSBOX_SNAPSHOT=\"$(PROJECT_SNAPSHOT)\" $(BARE_AUDIO_DEVICE_DEFINE) $(BARE_MIDI_DEFINE)

CXXFLAGS_APP = $(CPPFLAGS) -std=gnu++23 $(APP_INCLUDES) $(BARE_MIDI_INCLUDES) $(APP_DEFINES) $(APP_WARNINGS)

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------

# Our own code: Circle kernel + platform layer + stubs.
APP_SRCS := \
	src/main.cpp \
	src/kernel.cpp \
	src/platform/bare_libretro.cpp \
	src/platform/bare_video.cpp \
	src/platform/bare_audio.cpp \
	src/platform/bare_input.cpp \
	src/platform/bare_thread.cpp \
	src/platform/bare_time.cpp \
	src/platform/bare_fs.cpp \
	src/platform/bare_sysinfo.cpp \
	src/platform/bare_dynarec.cpp \
	src/platform/bare_cores.cpp \
	src/platform/bare_menu.cpp \
	src/platform/bare_midi.cpp \
	src/dosbox-stub/midi_stub.cpp \
	src/dosbox-stub/posix_stubs.cpp

# DOSBox-core's libretro platform layer: the parts we keep.
DOSBOX_PLATFORM_SRCS := \
	$(DOSBOX_DIR)/libretro/src/CoreOptionCategory.cpp \
	$(DOSBOX_DIR)/libretro/src/CoreOptionDefinition.cpp \
	$(DOSBOX_DIR)/libretro/src/CoreOptionValue.cpp \
	$(DOSBOX_DIR)/libretro/src/CoreOptions.cpp \
	$(DOSBOX_DIR)/libretro/src/libretro_core_options.cpp \
	$(DOSBOX_DIR)/libretro/src/disk_control.cpp \
	$(DOSBOX_DIR)/libretro/src/virtual_keyboard/libretro-graph.cpp \
	$(DOSBOX_DIR)/libretro/src/virtual_keyboard/libretro-vkbd.cpp \
	$(DOSBOX_DIR)/libretro/src/fake_timing.cpp \
	$(DOSBOX_DIR)/libretro/src/libretro.cpp \
	$(DOSBOX_DIR)/libretro/src/libretro_audio.cpp \
	$(DOSBOX_DIR)/libretro/src/libretro_gfx.cpp \
	$(DOSBOX_DIR)/libretro/src/libretro_input.cpp \
	$(DOSBOX_DIR)/libretro/src/libretro_message.cpp \
	$(DOSBOX_DIR)/libretro/src/log.cpp \
	$(DOSBOX_DIR)/libretro/src/util.cpp \
	$(DOSBOX_DIR)/libretro/src/emu_thread.cpp

# NOTE on emu_thread.cpp: it is built, but when DBC_SINGLE_THREADED is set the
# port provides switchThread() itself in bare_thread.cpp, so emu_thread.cpp is
# removed from the list and that symbol comes from us.  See bare_thread.cpp
# for the reasoning.  Flip the variable below to 0 to go back to std::thread.
DBC_SINGLE_THREADED ?= 1
ifeq ($(DBC_SINGLE_THREADED),1)
DOSBOX_PLATFORM_SRCS := $(filter-out %/emu_thread.cpp,$(DOSBOX_PLATFORM_SRCS))
endif

# The DOSBox core itself: exactly the list libretro/Makefile.common uses,
# minus what we cannot build (see the comments).
# The dynrec entry is $(DOSBOX_DYNAREC_UNIT) in a DYNAREC=1 build: it is the
# generated wrapper that #includes this very file (see DOSBOX_DYNAREC_HOOK
# above).  With DYNAREC=0 it is upstream's file, unchanged, which is also what
# makes `make DYNAREC=0` a true interpreter build to compare against.
ifeq ($(DYNAREC),1)
DOSBOX_DYNREC_SRC := $(DOSBOX_DYNAREC_UNIT)
else
DOSBOX_DYNREC_SRC := $(DOSBOX_DIR)/src/cpu/core_dynrec.cpp
endif

DOSBOX_CORE_SRCS := \
	$(DOSBOX_DIR)/src/cpu/callback.cpp \
	$(DOSBOX_DIR)/src/cpu/core_dyn_x86.cpp \
	$(DOSBOX_DYNREC_SRC) \
	$(DOSBOX_DIR)/src/cpu/core_full.cpp \
	$(DOSBOX_DIR)/src/cpu/core_normal.cpp \
	$(DOSBOX_DIR)/src/cpu/core_prefetch.cpp \
	$(DOSBOX_DIR)/src/cpu/core_simple.cpp \
	$(DOSBOX_DIR)/src/cpu/cpu.cpp \
	$(DOSBOX_DIR)/src/cpu/flags.cpp \
	$(DOSBOX_DIR)/src/cpu/modrm.cpp \
	$(DOSBOX_DIR)/src/cpu/paging.cpp \
	$(DOSBOX_DIR)/src/dos/cdrom.cpp \
	$(DOSBOX_DIR)/src/dos/cdrom_image.cpp \
	$(DOSBOX_DIR)/src/dos/dos.cpp \
	$(DOSBOX_DIR)/src/dos/dos_classes.cpp \
	$(DOSBOX_DIR)/src/dos/dos_devices.cpp \
	$(DOSBOX_DIR)/src/dos/dos_execute.cpp \
	$(DOSBOX_DIR)/src/dos/dos_files.cpp \
	$(DOSBOX_DIR)/src/dos/dos_ioctl.cpp \
	$(DOSBOX_DIR)/src/dos/dos_keyboard_layout.cpp \
	$(DOSBOX_DIR)/src/dos/dos_memory.cpp \
	$(DOSBOX_DIR)/src/dos/dos_misc.cpp \
	$(DOSBOX_DIR)/src/dos/dos_mscdex.cpp \
	$(DOSBOX_DIR)/src/dos/dos_programs.cpp \
	$(DOSBOX_DIR)/src/dos/dos_tables.cpp \
	$(DOSBOX_DIR)/src/dos/drive_cache.cpp \
	$(DOSBOX_DIR)/src/dos/drive_fat.cpp \
	$(DOSBOX_DIR)/src/dos/drive_iso.cpp \
	$(DOSBOX_DIR)/src/dos/drive_local.cpp \
	$(DOSBOX_DIR)/src/dos/drive_overlay.cpp \
	$(DOSBOX_DIR)/src/dos/drive_virtual.cpp \
	$(DOSBOX_DIR)/src/dos/drives.cpp \
	$(DOSBOX_DIR)/src/dosbox.cpp \
	$(DOSBOX_DIR)/src/fpu/fpu.cpp \
	$(DOSBOX_DIR)/src/gui/render.cpp \
	$(DOSBOX_DIR)/src/gui/render_scalers.cpp \
	$(DOSBOX_DIR)/src/hardware/adlib.cpp \
	$(DOSBOX_DIR)/src/hardware/cmos.cpp \
	$(DOSBOX_DIR)/src/hardware/dbopl.cpp \
	$(DOSBOX_DIR)/src/hardware/disney.cpp \
	$(DOSBOX_DIR)/src/hardware/dma.cpp \
	$(DOSBOX_DIR)/src/hardware/gameblaster.cpp \
	$(DOSBOX_DIR)/src/hardware/gus.cpp \
	$(DOSBOX_DIR)/src/hardware/hardware.cpp \
	$(DOSBOX_DIR)/src/hardware/iohandler.cpp \
	$(DOSBOX_DIR)/src/hardware/joystick.cpp \
	$(DOSBOX_DIR)/src/hardware/keyboard.cpp \
	$(DOSBOX_DIR)/src/hardware/mame/fmopl.cpp \
	$(DOSBOX_DIR)/src/hardware/mame/saa1099.cpp \
	$(DOSBOX_DIR)/src/hardware/mame/sn76496.cpp \
	$(DOSBOX_DIR)/src/hardware/mame/ymf262.cpp \
	$(DOSBOX_DIR)/src/hardware/memory.cpp \
	$(DOSBOX_DIR)/src/hardware/mixer.cpp \
	$(DOSBOX_DIR)/src/hardware/mpu401.cpp \
	$(DOSBOX_DIR)/src/hardware/pci_bus.cpp \
	$(DOSBOX_DIR)/src/hardware/pcspeaker.cpp \
	$(DOSBOX_DIR)/src/hardware/pic.cpp \
	$(DOSBOX_DIR)/src/hardware/sblaster.cpp \
	$(DOSBOX_DIR)/src/hardware/serialport/directserial.cpp \
	$(DOSBOX_DIR)/src/hardware/serialport/libserial.cpp \
	$(DOSBOX_DIR)/src/hardware/serialport/misc_util.cpp \
	$(DOSBOX_DIR)/src/hardware/serialport/nullmodem.cpp \
	$(DOSBOX_DIR)/src/hardware/serialport/serialdummy.cpp \
	$(DOSBOX_DIR)/src/hardware/serialport/serialport.cpp \
	$(DOSBOX_DIR)/src/hardware/serialport/softmodem.cpp \
	$(DOSBOX_DIR)/src/hardware/tandy_sound.cpp \
	$(DOSBOX_DIR)/src/hardware/timer.cpp \
	$(DOSBOX_DIR)/src/hardware/vga.cpp \
	$(DOSBOX_DIR)/src/hardware/vga_attr.cpp \
	$(DOSBOX_DIR)/src/hardware/vga_crtc.cpp \
	$(DOSBOX_DIR)/src/hardware/vga_dac.cpp \
	$(DOSBOX_DIR)/src/hardware/vga_draw.cpp \
	$(DOSBOX_DIR)/src/hardware/vga_gfx.cpp \
	$(DOSBOX_DIR)/src/hardware/vga_memory.cpp \
	$(DOSBOX_DIR)/src/hardware/vga_misc.cpp \
	$(DOSBOX_DIR)/src/hardware/vga_other.cpp \
	$(DOSBOX_DIR)/src/hardware/vga_paradise.cpp \
	$(DOSBOX_DIR)/src/hardware/vga_s3.cpp \
	$(DOSBOX_DIR)/src/hardware/vga_seq.cpp \
	$(DOSBOX_DIR)/src/hardware/vga_tseng.cpp \
	$(DOSBOX_DIR)/src/hardware/vga_xga.cpp \
	$(DOSBOX_DIR)/src/ints/bios.cpp \
	$(DOSBOX_DIR)/src/ints/bios_disk.cpp \
	$(DOSBOX_DIR)/src/ints/bios_keyboard.cpp \
	$(DOSBOX_DIR)/src/ints/ems.cpp \
	$(DOSBOX_DIR)/src/ints/int10.cpp \
	$(DOSBOX_DIR)/src/ints/int10_char.cpp \
	$(DOSBOX_DIR)/src/ints/int10_memory.cpp \
	$(DOSBOX_DIR)/src/ints/int10_misc.cpp \
	$(DOSBOX_DIR)/src/ints/int10_modes.cpp \
	$(DOSBOX_DIR)/src/ints/int10_pal.cpp \
	$(DOSBOX_DIR)/src/ints/int10_put_pixel.cpp \
	$(DOSBOX_DIR)/src/ints/int10_vesa.cpp \
	$(DOSBOX_DIR)/src/ints/int10_video_state.cpp \
	$(DOSBOX_DIR)/src/ints/int10_vptable.cpp \
	$(DOSBOX_DIR)/src/ints/mouse.cpp \
	$(DOSBOX_DIR)/src/ints/xms.cpp \
	$(DOSBOX_DIR)/src/misc/cross.cpp \
	$(DOSBOX_DIR)/src/misc/messages.cpp \
	$(DOSBOX_DIR)/src/misc/programs.cpp \
	$(DOSBOX_DIR)/src/misc/setup.cpp \
	$(DOSBOX_DIR)/src/misc/support.cpp \
	$(DOSBOX_DIR)/src/shell/shell.cpp \
	$(DOSBOX_DIR)/src/shell/shell_batch.cpp \
	$(DOSBOX_DIR)/src/shell/shell_cmds.cpp \
	$(DOSBOX_DIR)/src/shell/shell_misc.cpp

# C sources in the DOSBox tree.
DOSBOX_C_SRCS := \
	$(DOSBOX_DIR)/src/hardware/nukedopl.c

# libretro-common: the same four files upstream builds (libretro/Makefile.common).
# They are C, so they are compiled by gcc, and the two "compat" helpers are only
# used on Windows - but upstream compiles all four anyway, and matching upstream
# exactly is cheaper than arguing about four small files:
#   encoding_utf.c  libretro.cpp converts between UTF-8 and DOS's code pages
#                   for the keyboard layout.
#   compat_strcasestr.c, fopen_utf8.c
#                   MSVC/Windows shims. Nothing on this port calls them; they
#                   compile clean here and cost one object each.
#   compat_strl.c   defines strlcpy_retro__ / strlcat_retro__, used by
#                   encoding_utf.c itself.  Upstream links it; without it the
#                   link fails in encoding_utf.o.
LIBRETRO_COMMON_SRCS := \
	$(DOSBOX_DIR)/libretro/deps/common/encodings/encoding_utf.c \
	$(DOSBOX_DIR)/libretro/deps/common/compat/compat_strcasestr.c \
	$(DOSBOX_DIR)/libretro/deps/common/compat/compat_strl.c \
	$(DOSBOX_DIR)/libretro/deps/common/compat/fopen_utf8.c

# Deliberately NOT built, with the reason.  The rule is the one
# libretro/Makefile.common applies: everything the core can call must be linked,
# even if the feature is switched off at run time, because DOSBox's registries
# call into these functions unconditionally.  A stub that does nothing is
# cheaper than a missing symbol, and it keeps the port honest - it is the
# difference between "feature off" and "link error".
#
# This list is not free text any more: `make check-sources` compares it against
# DOSBox-core's own file list and fails if a file is missing here without a
# reason (scripts/check-sources.py holds the reasons, one line each).  What
# follows is the longer version of the same reasons.
#
#   libretro/src/midi_alsa.cpp, midi_win32.cpp
#                             - host MIDI backends for the midi.cpp we do not
#                               build.  Upstream only adds the ALSA one when it
#                               is not using fakesdl, which is our case.
#   libretro/src/fakesdl/SDL/{SDL_endian,SDL_thread,SDL_timer}.cpp
#                             - upstream links them; our platform layer
#                               provides the same SDL_* symbols
#                               (bare_thread.cpp has its own SDL_Thread shims).
#                               Linking both is "multiple definition".
#   libretro/src/emu_thread.cpp
#                             - switchThread() comes from bare_thread.cpp while
#                               DBC_SINGLE_THREADED=1, because a Pi has no
#                               std::thread to hand the other core.  Set the
#                               variable to 0 and this file is built instead.
#   src/hardware/voodoo*.cpp  - OpenGL.  Not a define on our side: WITH_VOODOO=0,
#                               which is what upstream's parser script uses too.
#   src/hardware/ipx*.cpp     - C_IPX=0, no sockets.  Upstream does not build
#                               them in a fakesdl build either.
#   src/dos/cdrom_aspi_win32.cpp, cdrom_ioctl_win32.cpp
#                             - the entire body is inside #if defined(WIN32);
#                               the objects are empty.  Left out.
#   src/gui/midi.cpp, midi_mt32.cpp
#                             - the real MIDI device layer, replaced by
#                               src/dosbox-stub/midi_stub.cpp.  It negotiates
#                               OSS/ALSA/CoreMIDI devices that do not exist on
#                               the board, and midi_mt32.h includes
#                               <mt32emu/mt32emu.h> (the Munt library) at the
#                               top with no #if around it: there is no
#                               configuration in which that header compiles
#                               here.  OPL/AdLib - what DOS games actually use -
#                               is a different code path and is unaffected.
#   src/gui/sdlmain.cpp, sdl_gui.cpp, sdl_mapper.cpp
#                             - the SDL frontend; the libretro platform layer
#                               replaces all three.  Upstream does not compile
#                               sdl_mapper.cpp in this build either: MAPPER_*
#                               come from libretro/src/libretro_input.cpp.
#   src/hardware/nukedopl.c IS built (see DOSBOX_C_SRCS): plain C, portable, and
#            upstream compiles it too.
#
# Two files used to be listed here and no longer are: libretro/src/
# libretro_core_options.cpp and libretro/src/disk_control.cpp.  Both are built
# now.  The second one is worth remembering: the first version of this port
# stubbed disk_control::mount() with a local signature (const std::string&) that
# no longer matched upstream's header (std::filesystem::path), so the core's own
# call sites kept failing to link while the stub looked like it covered them.
# When a symbol is missing, read the declaration the caller sees.

# ---------------------------------------------------------------------------
# Object lists and rules
# ---------------------------------------------------------------------------

ALL_SRCS := $(APP_SRCS) $(DOSBOX_PLATFORM_SRCS) \
	    $(DOSBOX_CORE_SRCS) $(DOSBOX_C_SRCS) $(LIBRETRO_COMMON_SRCS)

# ---------------------------------------------------------------------------
# How the sources are compiled
# ---------------------------------------------------------------------------
#
# Two source trees, two sets of flags:
#
#   src/...                      our own code.  Circle's flags, no DOSBox
#                                defines (in particular no build_env.h)
#   third_party/dosbox-core/...  DOSBox.  DOSBox's defines plus build_env.h
#
# Both are handled by *generated explicit rules*, one per source file, rather
# than by pattern rules.  Two attempts at something cleverer both failed, and
# the reasons are worth keeping:
#
#   1. Pattern rules can only reach outside this directory through a symlink
#      (make has no "match a pattern against an absolute path" that also works
#      as a target name).  But compiling through a symlink makes
#      "#include \"kernel.h\"" resolve next to the symlink, so -MMD records a
#      bare "kernel.h" and make then looks for it in the repository root:
#      "No rule to make target 'kernel.h'".
#   2. Creating that symlink as a prerequisite does not help either: make
#      resolves the whole graph before running any recipe, so the .cpp
#      prerequisite is "missing" at the moment the graph is built.
#
# Explicit rules sidestep both: the prerequisite is the real path, so quoted
# includes resolve where the source expects and the dependency files are
# correct, and there is no staging directory to go stale.
#
# Objects still land under build/ mirroring their source path, so the several
# cpu.cpp / mixer.cpp style basenames stay apart.  While debugging a build
# problem, this is the identity to use:
#
#     make -p | grep '^build/.*\.o:'

# Object path for a source: build/ + its path relative to the repository root.
# $(1) may be relative ("src/main.cpp") or absolute (the third_party tree), so
# it is made absolute before stripping the root - otherwise an absolute source
# keeps its leading slash and produces "build//tmp/...".
OBJ_OF = $(patsubst %.c,%.o,$(patsubst %.cpp,%.o,$(addprefix build/,$(patsubst $(CURDIR)/%,%,$(abspath $(1))))))

# Timestamps, and why this is here.
#
# Everything in this tree arrives with the same frozen modification time (the
# snapshot is generated that way; ls -l shows "Jan  2  1980").  make decides
# whether to rebuild by comparing mtimes, so as soon as build/ contains an
# object newer than those sources - which is to say, after the very first build -
# make considers every file "up to date" and recompiles nothing, however much the
# sources changed.  It does not even print "Nothing to be done": the target it
# was asked for already exists, so it prints nothing at all.
#
# So the recipes below depend on the Makefile's *content* instead of on the
# clock.  BUILDSTAMP is a file holding the hash of everything that decides what
# the objects contain: this Makefile and every DOSBOX_DEFINES flag.  Change any of
# them and the stamp is rewritten, which makes every object older than its
# prerequisite and forces a full rebuild.  Touch nothing and nothing rebuilds.
#
# $(shell) is evaluated once per invocation; the recipe that writes the file is
# made quiet so it does not interleave with the compiler lines.
BUILDSTAMP := build/.makefile

# The stamp is a *copy* of this Makefile, refreshed only when the content
# changes (`cmp -s` before `cp`).  Reusing the mtime that make already
# understands, instead of inventing a hash, is deliberate:
#
#   * Hashing was tried twice and was wrong twice.  Hashing the literal string
#     "Makefile <flags>" never noticed a change to the Makefile at all, so the
#     tree stayed frozen after the first build.  Hashing the file's content
#     cannot be dropped into a shell command either, because this Makefile
#     contains single quotes, which end the quoting and hand the shell
#     fragments of itself.
#   * A copy has no quoting, no hash, no extra tool, and it is exactly what make
#     needs: if the Makefile's content changed, the copy is newer than every
#     object again and everything rebuilds; if it did not, nothing is touched.
#
# The flags that matter for object contents (DOSBOX_DEFINES) live inside this
# Makefile, so changing one changes the content and is covered by the copy.
$(BUILDSTAMP): FORCE
	@mkdir -p $(dir $@)
	@cmp -s Makefile $@ || cp Makefile $@

OBJS := $(foreach f,$(APP_SRCS),$(call OBJ_OF,$(f))) \
        $(foreach f,$(DOSBOX_PLATFORM_SRCS) $(DOSBOX_CORE_SRCS),$(call OBJ_OF,$(f))) \
        $(foreach f,$(DOSBOX_C_SRCS) $(LIBRETRO_COMMON_SRCS),$(call OBJ_OF,$(f)))

# Our own code.
define APP_OBJ_RULE
$(call OBJ_OF,$(1)): $(1) $(BUILDSTAMP)
	@echo "  CPP   $(1)"
	@mkdir -p $$(dir $$@)
	@$$(CPP) $$(CXXFLAGS_APP) -MMD -MP -c -o $$@ $(1)
endef

$(foreach src,$(APP_SRCS),$(eval $(call APP_OBJ_RULE,$(src))))

# DOSBox, C++.
#
# $(2) is an optional extra flag, used by exactly one file - see
# DOSBOX_DYNAREC_HOOK below.  Quoted so that a flag containing a comma or a
# space survives being passed through $(eval).
define DOSBOX_OBJ_RULE
$(call OBJ_OF,$(1)): $(1) $(BUILDSTAMP)
	@echo "  CPP   $(1)"
	@mkdir -p $$(dir $$@)
	@$$(CPP) $$(CXXFLAGS_DOSBOX) $(2) -MMD -MP -c -o $$@ $(1)
endef

$(foreach src,$(DOSBOX_PLATFORM_SRCS) $(DOSBOX_CORE_SRCS),$(eval $(call DOSBOX_OBJ_RULE,$(src),$(if $(filter %/bare_dynrec_unit.cpp,$(src)),$(DOSBOX_DYNAREC_HOOK)))))

# The generated wrapper, written by the script that also verifies the pinned
# revision still has the shape it depends on (see gen-dynarec-unit.py).  It is
# a prerequisite in its own right so that editing the generator regenerates it.
ifeq ($(DYNAREC),1)
$(DOSBOX_DYNAREC_UNIT): scripts/hostcheck/gen-dynarec-unit.py $(DOSBOX_DIR)/src/cpu/core_dynrec.cpp
	@mkdir -p $(dir $@)
	@python3 scripts/hostcheck/gen-dynarec-unit.py $@ $(DOSBOX_DIR)
endif

# DOSBox, plain C (nukedopl.c and the libretro-common encoding helper).
define DOSBOX_C_OBJ_RULE
$(call OBJ_OF,$(1)): $(1) $(BUILDSTAMP)
	@echo "  CC    $(1)"
	@mkdir -p $$(dir $$@)
	@$$(CC) $$(CFLAGS_DOSBOX) -MMD -MP -c -o $$@ $(1)
endef

$(foreach src,$(DOSBOX_C_SRCS) $(LIBRETRO_COMMON_SRCS),$(eval $(call DOSBOX_C_OBJ_RULE,$(src))))

# ---------------------------------------------------------------------------
# Link
# ---------------------------------------------------------------------------
#
# No link rule here: Circle's Rules.mk already has one, and it is a better one
# than anything written by hand.  Its rule is
#
#     $(TARGET).img: $(OBJS) $(LIBS) $(CIRCLEHOME)/circle.ld
#         $(LD) -o $(TARGET).elf -Map $(TARGET).map $(LDFLAGS) \
#               -T $(CIRCLEHOME)/circle.ld $(CRTBEGIN) $(OBJS) \
#               --start-group $(LIBS) $(EXTRALIBS) --end-group $(CRTEND)
#         ... then objdump -> .lst, objcopy -> .img
#
# It already knows about $(LOADADDR), $(CRTBEGIN)/$(CRTEND) from newlib, and
# the .lst/.img steps.  So all this Makefile has to do is fill in the two
# variables it expects:
#
#   OBJS  the objects listed above (defined next to the source lists)
#   LIBS  libc++, libc++abi, libunwind, the threading support library, libm,
#         newlib and the Circle core, in exactly the order circle-stdlib's
#         configure generated.  Do not reorder it: with a static link, order is
#         meaning.
#
# $(EXTRALIBS) (libgcc.a, libgcc_eh.a) is computed by Rules.mk itself.
#
# Note what is deliberately NOT done: defining $(TARGET).img or $(TARGET).elf
# again.  Mixing ":" and "::" for the same target is a hard error in make
# ("target file 'kernel8-rpi4.img' has both : and :: entries"), and redefining
# with ":" only produces an "overriding recipe" warning and a broken build.
# $(CIRCLE_STDLIB_LIBS) comes from circle-stdlib's configure, and it lists the
# libraries it knows the *samples* need.  It does not list lib/sound, which is
# where CSoundBaseDevice and the HDMI/I2S/PWM backends live - so bare_audio.cpp
# failed to link with eight "undefined reference to CSoundBaseDevice::...".
#
# lib/sound is built by Circle's own makeall ("cd sound"), it is simply missing
# from the variable.  Appended at the end for readability: Rules.mk wraps all of
# $(LIBS) in --start-group/--end-group, so link order does not matter here.
LIBS := $(CIRCLE_STDLIB_LIBS) $(CIRCLEHOME)/lib/sound/libsound.a $(BARE_MIDI_LIBS)

# ---------------------------------------------------------------------------
# Overriding a libgloss stub that returns ENOSYS
# ---------------------------------------------------------------------------
#
# circle-newlib implements realpath() as a stub that does nothing but set
# errno = ENOSYS:
#
#     libgloss/circle/fsstubs.cpp:101   char *realpath(...) { errno = ENOSYS; return nullptr; }
#
# std::filesystem::canonical() goes through it, dosbox-core canonicalises the
# content path before anything else, so with that stub nothing can be loaded
# ("canonical FAILED: Function not implemented").  src/dosbox-stub/posix_stubs.cpp
# has a working implementation, named dosbox_realpath().
#
# How to make the linker use it.  Three approaches, two measured and rejected:
#
#   1. Define realpath() ourselves.  Fails: the symbol exists in the archive, so
#      this is a duplicate, not a gap - "multiple definition of `realpath'".
#   2. Rely on link order (our object before the archive).  Fails for the same
#      reason it is tempting: fsstubs.o is pulled out of the archive anyway for
#      fchmod() and pathconf(), and it brings its realpath() with it.  Measured.
#   3. -Wl,--allow-multiple-definition.  Measured, and it silently picks the
#      stub: the call returned nullptr.  Worse than failing, it builds.
#   4. --defsym=realpath=dosbox_realpath.  Measured: links, and the call lands
#      in our function.  This is the one.  No -Wl, prefix, because $(LD) here is
#      aarch64-none-elf-ld and not a gcc driver: -Wl, is for a driver, and ld
#      itself rejects "-Wl,--defsym=..." as an unrecognised option.
#
# The alias is only added when the archive is present and really does export
# realpath(); otherwise there is nothing to alias and the flag would create a
# symbol nobody defined.
NEWLIB_ARCHIVE := $(firstword $(wildcard $(CIRCLE_STDLIB)/install/*/lib/libcirclenewlib.a))
REALPATH_OVERRIDE :=
ifneq ($(NEWLIB_ARCHIVE),)
# nm without a prefix on purpose: $(PREFIX) comes from Rules.mk, which is
# included further down.  This reads a symbol table, not an object, so the host
# nm sees exactly the names the cross linker will.
ifeq ($(shell nm -g --defined-only $(NEWLIB_ARCHIVE) 2>/dev/null | grep -c ' T realpath$$'),1)
REALPATH_OVERRIDE := 1
# Bare --defsym, with no -Wl, prefix: Circle links with $(LD), which is
# aarch64-none-elf-ld itself, not a gcc driver.  The -Wl, form is only for when
# a compiler driver is what reads LDFLAGS; here the linker would take it
# literally and answer "unrecognized option '-Wl,--defsym=...'".  Circle's own
# Rules.mk makes exactly this distinction (--section-start= for ld, -Wl, form
# for clang), which is where the pattern comes from.
LDFLAGS += --defsym=realpath=dosbox_realpath
endif
endif

# Circle's clean recipe does "rm -f *.o *.elf ... $(EXTRACLEAN)".  Our objects
# live in build/, so list them explicitly: rm -f cannot remove a directory, and
# this keeps "make clean" exact instead of "rm -rf" of something inferred.
EXTRACLEAN := $(OBJS) $(OBJS:.o=.d)

# ---------------------------------------------------------------------------
# Circle's own rules - and this is where the order finally pays off
# ---------------------------------------------------------------------------

# Now that $(OBJS) and $(LIBS) are defined, Rules.mk's link rule gets them.
# This pulls in Circle's Rules.mk (which the toolchain check above also needs:
# $(PREFIX) and $(TARGET) come from it).
ifeq ($(CIRCLE_STDLIB_CONFIGURED),1)
include $(wildcard $(CIRCLEHOME)/Rules.mk)

# The toolchain check lives here because $(PREFIX) only exists once Rules.mk has
# been read (it is $(PREFIX64) with AARCH=64).
#
# The first test is not about the toolchain, it is about $(PREFIX) being empty:
# if Rules.mk or Config.mk did not define PREFIX64, then
#
#     command -v $(PREFIX)g++
#
# degenerates into "command -v g++", which finds the HOST compiler, succeeds,
# and lets the build produce x86 objects for an AArch64 link.  A quiet wrong
# answer is worse than any error message, so it is caught here.
ifeq ($(strip $(PREFIX)),)
ACTIVE_ERR := $(CIRCLEHOME)/Rules.mk did not define a compiler prefix (PREFIX64),\nso the build would use the host compiler by accident.\n\nThat means circle-stdlib's Config.mk is incomplete: delete the tree and run\n    ./scripts/setup.sh\n
else ifeq ($(shell command -v $(PREFIX)g++ 2>/dev/null),)
ACTIVE_ERR := $(PREFIX)g++ not found.\n\nLooked in $(TOOLCHAIN_DIR)/arm-gnu-toolchain-*/bin (this project keeps\neverything it downloads there) and then on PATH.\n\nRun ./scripts/setup.sh: with no toolchain present it prints the two commands\nthat fetch one into $(TOOLCHAIN_DIR).\n(the toolchain has to be GCC 15.2 or newer).\n
endif

# Circle's Rules.mk sets CC/CPP/LD, the CPU flags, LOADADDR and TARGET for the
# configured board.  It also pulls in $(CIRCLEHOME)/Config.mk, which is where
# PREFIX64 (and therefore the AArch64 toolchain prefix) comes from.
endif

# ---------------------------------------------------------------------------
# What "make" builds
# ---------------------------------------------------------------------------

# Circle's Rules.mk defines no "all", so this one is ours alone.  When something
# is missing it points at the explanation instead of at the image, because
# reporting it at parse time would break "make fetch" - the command you run in
# exactly that situation.
.PHONY: FORCE
FORCE:

.PHONY: all
ifneq ($(ACTIVE_ERR),)
all: setup-check
else
# announce is the FIRST prerequisite on purpose: make builds prerequisites in
# the order they are listed, so this line is printed before anything else even
# when the build then fails in the first translation unit.  "make did nothing"
# used to be indistinguishable from "make never started" - it no longer is.
all: announce $(TARGET).img
.PHONY: announce
announce:
	@echo "  BUILD  $(TARGET).img$(if $(REALPATH_OVERRIDE),  [realpath: ours, via --defsym],  [realpath: libgloss stub])"
endif

# ---------------------------------------------------------------------------
# make check
# ---------------------------------------------------------------------------
#
# Verifies, in about a second and without building anything, that the two things
# that went wrong by hand twice are actually right:
#
#   1. every -I/-isystem directory in the flags exists on disk;
#   2. the headers that matter are really reachable: a probe translation unit
#      includes them with the same flags the build uses, so this is the
#      compiler's opinion, not a grep.
#
# Written as a target on purpose.  Two previous attempts at this were shell
# one-liners pasted into a chat, and the second one was wrong in a way that
# produced five "NO EXISTE:" lines with no filename: Circle writes "-I dir"
# (with a space), so "tr ' ' '\n'" split it into a bare "-I" whose path was the
# empty string.  Anything that matters belongs in the Makefile, where it is
# versioned and testable.
.PHONY: check version help warnings
# What the two warning lists actually are, after make has expanded them.  This
# is the answer to "are our own files compiled with -Wall -Wextra, or with the
# pile of -Wno- that DOSBox needs?" - a question worth being able to answer
# without reading the Makefile.
warnings:
	@echo "our code (src/):     $(APP_WARNINGS)"
	@echo "DOSBox (third_party): $(DOSBOX_WARNINGS)"
	@echo "add WARN_FULL=1 to the build to see everything DOSBox's code says."

check:
	@echo "snapshot: $(PROJECT_SNAPSHOT)"
	@mkdir -p build
	@echo "=== include directories"
	@for d in $(patsubst -I%,%,$(filter -I%,$(APP_INCLUDES) $(DOSBOX_INCLUDES) $(INCLUDE))) \
	          $(patsubst -isystem%,%,$(filter -isystem%,$(APP_INCLUDES) $(DOSBOX_INCLUDES))) \
	          $(patsubst -iquote%,%,$(filter -iquote%,$(APP_INCLUDES) $(DOSBOX_INCLUDES))) ; do \
		if [ -d "$$d" ]; then echo "  ok    $$d"; else echo "  FALTA $$d"; fi; \
	done
	@echo "=== headers"
	@printf '#include <circle_stdlib_app.h>\n#include <circle/types.h>\n#include <circle/timer.h>\n#include <dirent.h>\n#include "libretro.h"\n#include "libretro_dosbox.h"\n#include "emu_thread.h"\n' > build/probe.cpp
	@$(CPP) $(CXXFLAGS_APP) -fsyntax-only build/probe.cpp \
		&& echo "  ok    circle_stdlib_app.h, circle/*.h, dirent.h, libretro.h, libretro_dosbox.h" \
		|| ( echo "  FALLO: alguna de esas cabeceras no se encuentra con estos flags"; exit 1 )
	@echo "=== dirent.h: la cadena completa"
	@for f in $(CIRCLE_STDLIB)/include/wrap_fatfs.h $(CIRCLEHOME)/addon/fatfs/ff.h; do \
		if [ -f "$$f" ]; then echo "  ok    $$f"; else echo "  FALTA $$f"; fi; \
	done
	@printf '#include <dirent.h>\n#include "cross.h"\n#include "dos/drives.h"\nstatic struct dirent *de;\nstatic DIR *d;\nstatic char name[CROSS_LEN];\n' > build/probe_dirent.cpp
	@$(CPP) $(CXXFLAGS_DOSBOX) -fsyntax-only build/probe_dirent.cpp \
		&& echo "  ok    <dirent.h> + cross.h + dos/drives.h en la misma unidad" \
		|| ( echo "  FALLO: dirent.h/fatfs o los headers de DOSBox no resuelven con estos flags"; exit 1 )
	@echo "=== done"

# ---------------------------------------------------------------------------
# make check-sources
# ---------------------------------------------------------------------------
#
# The link of this port has now failed four times, always for the same reason:
# a symbol the DOSBox core calls that no file in our list defines.  Each round
# cost a full rebuild on the user's machine to discover one more name.
#
# This target is the answer to that: it compares the list of files we compile
# against the list libretro/Makefile.common compiles - upstream's build, i.e. the
# set of files that made a complete, linkable core - and fails if any file is
# neither compiled nor listed as a documented exception.  See
# scripts/check-sources.py for why a file-list check and not a symbol check.
#
# print-srcs exists so the script reads $(ALL_SRCS) from make instead of
# parsing this file with a regex: one source of truth.
# ---------------------------------------------------------------------------
# make check-platform
# ---------------------------------------------------------------------------
#
# The .52 round shipped a link error: a rewrite of bare_video.cpp dropped
# BareVideoFramesPresented(), the file still compiled because it no longer used
# it, and the three undefined references appeared at link time on the user's
# machine.  A syntax check cannot see that; the symbol table can.
#
# This compiles the platform files against stub Circle headers (scripts/
# hostcheck) with the host compiler and compares nm's output with what
# bare_env.h promises.  Two seconds, no toolchain, and it names the function.
.PHONY: check-platform
# The host compiler, used only by the tools that need no toolchain.  Not $(CXX):
# that one may be the ARM cross compiler, and these tools run here.
HOST_CXX		?= g++

check-platform:
	@sh scripts/hostcheck/setup.sh > /dev/null
	@sh scripts/hostcheck/check-symbols.sh "$(CURDIR)"

# /boot.conf is the only file a user is meant to edit after flashing a card, and
# its parser runs before there is a screen to complain on.  kernel.cpp itself
# cannot be compiled on the host (circle_stdlib_app.h and the whole kernel come
# with it), so the parser lives in src/platform/boot_config.h, which needs
# nothing from Circle, and this target compiles it here against a table of the
# files people actually write - including the wrong ones, which is where the
# first version of the parser failed silently.
#
# -DBOOT_CONFIG='"boot.conf"' sends the fixture files to the working directory:
# the header's default is "SD:/boot.conf", the path on the card, which does not
# exist on a PC.
check-bootconf:
	@mkdir -p build/bootconf
	@$(HOST_CXX) -std=gnu++23 -O0 -Wall -Wextra -Wpedantic -Wshadow \
		-DBOOT_CONFIG='"boot.conf"' \
		-o build/bootconf_test scripts/hostcheck/bootconf_test.cpp
	@cd build/bootconf && ../bootconf_test

# kernel.cpp, on the host, against a stand-in for circle-stdlib.  It is the one
# file of ours that neither the real build (no toolchain here) nor check-platform
# (which covers src/platform) looks at, and the .54 round paid for that: a free
# function called CKernel::Report(), which is a member.  Catches wrong types,
# wrong call targets, undeclared members and missing includes in two seconds.
#
# What it does not catch: anything about the real Circle.  The headers it uses
# declare only what we use, so a signature that differs from Circle's passes.
# A pass means "our code is consistent with the interfaces as documented".
check-kernel:
	@HOST_CXX="$(HOST_CXX)" sh scripts/hostcheck/check-kernel.sh

# The destination rectangle: aspect ratio and how much of the screen the picture
# gets, for every mode boot.conf can ask for and every source DOSBox produces.
# The arithmetic is copied verbatim out of bare_video.cpp by the generator, so
# this cannot drift from what is compiled; the generator runs here on purpose.
check-scaling:
	@python3 scripts/hostcheck/gen-scaling.py
	@$(HOST_CXX) -std=gnu++23 -O0 -Wall -Wextra -o build/scaling_test \
		scripts/hostcheck/scaling_test.cpp
	@./build/scaling_test

.PHONY: check-sources print-srcs upstream-srcs check-bootconf check-kernel
# The page-table walk of bare_dynarec.cpp, run against page tables built in
# Circle's own layout.  This is the one piece of the port that can corrupt the
# machine quietly - it writes a bit into live page tables - so it is not left to
# "it compiles, it must be right".  The generator extracts the walk verbatim, so
# the test cannot drift from the code it is testing.
# Two programs, one generator: the first walks one 512 MB block, the second
# walks a range that starts in one and ends in the next - which is the shape an
# 8 MB malloc() takes when it lands unluckily, and the case where a wrong answer
# means a JIT that cannot start on that boot and says nothing about why.
#
# Both exit 77 if the host will not give them a 512 MB-aligned table; that is
# reported as OMITIDO below, never as a pass.
.PHONY: check-dynarec
check-dynarec:
	@python3 scripts/hostcheck/gen-dynarec.py
	@$(HOST_CXX) -std=gnu++23 -Wall -Wextra -O1 \
		-o build/dynarec_test scripts/hostcheck/dynarec_test.cpp
	@build/dynarec_test
	@$(HOST_CXX) -std=gnu++23 -Wall -Wextra -O1 \
		-o build/dynarec_cross_test scripts/hostcheck/dynarec_cross_test.cpp
	@build/dynarec_cross_test || { \
		echo "check-dynarec: el test del cruce de 512 MB no pudo ejecutarse"; \
		exit 1; }

# The default CPU cycles, checked where it can be checked: the five option
# values have to multiply out to the number the header claims, and the option
# *names* have to be the core's own keys - a typo there does not fail, it falls
# through to the core default and the machine runs at 386 speed without a word.
#
# The core's header is looked for in the two places it can be: the extracted
# tree this build uses ($(DOSBOX_DIR)) and a copy checked out by hand.  If
# neither is there the test says OMITIDO and checks the arithmetic only; it does
# not pretend to have compared anything.
ifneq ($(wildcard $(DOSBOX_DIR)/libretro/src/libretro_core_options.h),)
COREOPTS_HDR = $(DOSBOX_DIR)/libretro/src/libretro_core_options.h
else
COREOPTS_HDR =
endif

# The horizontal half of the image smoothing (ResampleRowBox in bare_video.cpp),
# extracted verbatim by its generator and run against patterns whose right
# answer is known: a flat colour must survive exactly, a ramp must stay
# monotonic with both endpoints intact, a one-pixel spike must spread instead of
# disappearing or aliasing, and 1:1 must be the identity.
#
# This target exists because the first version of the filter failed three of
# those four and none of it was visible in a build: a flat white row came out
# 254 and a 640 -> 1 reduction wrapped a 32-bit accumulator.  Both were found by
# the test, not by looking at a picture.
# The governor's level table.  This exists because of .60: the table was built
# from the wrong firmware tag, came out with no 600 MHz in it, and the port ran
# two levels - which compiles, passes every other check, and reads on the board
# as "the clock never comes down".  The maths is a copy of BuildLevels() with
# the mailbox taken out; see the header of the test for what keeps the copy
# honest.
.PHONY: check-governor
check-governor:
	@mkdir -p build
	@$(HOST_CXX) -std=gnu++23 -O0 -Wall -Wextra -Wpedantic -Wshadow \
		-o build/governor_test scripts/hostcheck/governor_test.cpp
	@build/governor_test

# The MIDI byte parser and the core 0 <-> core 1 rings (midi_queue.h).  Two
# threads on the host stand in for the two cores; -pthread for that.
.PHONY: check-midi
check-midi:
	@mkdir -p build
	@$(HOST_CXX) -std=gnu++23 -O1 -Wall -Wextra -Wpedantic -Wshadow -pthread \
		-o build/midi_test scripts/hostcheck/midi_test.cpp
	@build/midi_test

# Changing one value in boot.conf / dosbox.conf / cmdline.txt without
# touching anything else (conf_edit.h), including the files in sd/.
.PHONY: check-conf
check-conf:
	@mkdir -p build
	@$(HOST_CXX) -std=gnu++23 -O1 -Wall -Wextra -Wpedantic -Wshadow \
		-o build/conf_edit_test scripts/hostcheck/conf_edit_test.cpp
	@build/conf_edit_test sd

# The setup menu's model and layout (menu_model.h): pages, cursor, keys, and
# the 80x30 grid.
.PHONY: check-menu
check-menu:
	@mkdir -p build
	@$(HOST_CXX) -std=gnu++23 -O1 -Wall -Wextra -Wpedantic -Wshadow \
		-o build/menu_test scripts/hostcheck/menu_test.cpp
	@build/menu_test

# Which source rows make each output row (scale_weights.h): sharp/smooth.
.PHONY: check-scale
check-scale:
	@mkdir -p build
	@$(HOST_CXX) -std=gnu++23 -O1 -Wall -Wextra -Wpedantic -Wshadow \
		-o build/scale_weights_test scripts/hostcheck/scale_weights_test.cpp
	@build/scale_weights_test

# The DC blocker on the final audio output (audio_dcblock.h): removes a held
# level, keeps music.
.PHONY: check-audio
check-audio:
	@mkdir -p build
	@$(HOST_CXX) -std=gnu++23 -O1 -Wall -Wextra -Wpedantic -Wshadow \
		-o build/dcblock_test scripts/hostcheck/dcblock_test.cpp
	@build/dcblock_test

# The core 0 -> core 2 frame hand-over (frame_exchange.h): a lock-free triple
# buffer.  Two host threads; a torn or out-of-order frame fails it.
.PHONY: check-frames
check-frames:
	@mkdir -p build
	@$(HOST_CXX) -std=gnu++23 -O1 -Wall -Wextra -Wpedantic -Wshadow -pthread \
		-o build/frame_exchange_test scripts/hostcheck/frame_exchange_test.cpp
	@build/frame_exchange_test

# The flush sequence (bare_storage.h), against a fake FatFs.  This target exists
# because .59 did not link: kernel.cpp called POSIX sync(), which circle-stdlib
# does not provide, and nothing before the cross build could have noticed.  What
# is tested here is the *sequence* - fflush, then open, then f_sync, then close,
# and what each failure means - not FatFs itself, which cannot run on the host.
.PHONY: check-storage
check-storage:
	@mkdir -p build
	@$(HOST_CXX) -std=gnu++23 -O0 -Wall -Wextra -Wpedantic -Wshadow \
		-DBARE_STORAGE_HOST -I$(CURDIR)/src/platform \
		-o build/storage_test scripts/hostcheck/storage_test.cpp
	@build/storage_test

.PHONY: check-resample
check-resample:
	@python3 scripts/hostcheck/gen-resample.py
	@$(HOST_CXX) -std=gnu++23 -O1 -Wall -Wextra \
		-o build/resample_test scripts/hostcheck/resample_test.cpp
	@build/resample_test

.PHONY: check-coreopts
check-coreopts:
	@mkdir -p build
	@$(HOST_CXX) -std=gnu++23 -O0 -Wall -Wextra -Wpedantic -Wshadow \
		-I$(CURDIR)/src/platform \
		-o build/coreopts_test scripts/hostcheck/coreopts_test.cpp
	@build/coreopts_test $(COREOPTS_HDR)

# When this port is allowed to talk to the core about its game port, and where
# the call sits in bare_libretro.cpp.  .61 put it before retro_load_game() and
# the board stopped dead on the next line: inside the core that call ends in
# control->GetSection(), and `control` is null until the emulation task exists.
# The rules live in bare_portorder.h; this target compiles them, runs them, and
# then checks the line order in the real file - a correct state machine nobody
# calls is worth nothing, and that is exactly how .60 shipped the bug.
.PHONY: check-portorder
check-portorder:
	@HOST_CXX="$(HOST_CXX)" sh scripts/hostcheck/check-portorder.sh

# The JIT hook, compiled for real: the generated wrapper unit against the pinned
# DOSBox sources, with the port's flags.  This is the check that would have
# caught .62's -DC_TARGETCPU=\"ARMV8LE\" (a string compared against a number)
# and every other way this hook can be wired wrong without failing on the host.
#
# It needs the DOSBox tree, so it says OMITIDO and exits nonzero when the tree is
# not there: a check that cannot look is not a check that passed.
.PHONY: check-dynarec-hook
check-dynarec-hook:
	@DOSBOX_DIR="$(DOSBOX_DIR)" HOST_CXX="$(HOST_CXX)" \
		sh scripts/hostcheck/check-dynarec-hook.sh

.PHONY: check-host

# Everything that can be checked without the ARM toolchain, in the order that
# makes failure cheapest to read: the contract, then our kernel, then the
# config parser.  This is the target to run before a cross build.
check-host: check-platform check-kernel check-bootconf check-scaling check-dynarec \
            check-coreopts check-resample check-storage check-governor check-portorder \
            check-dynarec-hook check-midi check-frames check-audio check-scale check-menu check-conf
print-srcs:
	@echo $(ALL_SRCS)

# The DOSBox include list, for the host checks that need to compile a DOSBox
# source.  One line, spaces inside a path escaped: Circle's install directory
# can contain one, and a check script has to be able to split this list without
# also splitting a path.
#
# This exists because check-dynarec-hook used to write its own -I list, and by
# .63 the two had drifted: the check passed -I$(DOSBOX_DIR)/src/cpu and the real
# build does not, so the check was green while the board build stopped at line
# 20 of the generated file.  A check compiled with different flags than the
# build is testing a different program.
.PHONY: print-dosbox-includes
print-dosbox-includes:
	@printf '%s\n' "$(subst $(space),\\ ,$(DOSBOX_INCLUDES))"

# The same list, cut down to what a HOST compiler can use: the paths inside
# third_party/dosbox-core and our own src/, and nothing else.
#
# Why keep instead of drop: the previous two versions of this were lists of
# exclusions, and each round found one more path to exclude.  The paths that
# break a host build are not one thing, they are everything to do with Circle -
# and all of it is correct in an AArch64 build:
#
#   .../circle-stdlib/install/aarch64-none-circle/include   newlib's libc for the
#                                                           board: two libc's at
#                                                           once, div_t,
#                                                           locale_t, pthread_*
#   .../circle-stdlib/libs/circle/addon/                    Circle's `linux/`
#                                                           stand-in headers,
#                                                           which collide with the
#                                                           real <linux/stat.h>
#                                                           and <linux/sched.h>
#
# Allow list, not a list of exclusions: `$(filter)` here compares *words*
# (paths), so a leading `-I` has to be put back afterwards.  Every exclusion
# list tried before this one grew by one entry per round - first newlib's libc,
# then Circle's `addon/linux/` stand-ins - because the paths that break a host
# build are not one thing, they are everything to do with Circle.  Now a path
# either lives inside DOSBox or inside our src/, or the host compiler never sees
# it.  What the host does need from Circle (circle/logger.h, circle/types.h,
# fatfs/ff.h) are the stand-ins in scripts/hostcheck, added by the check itself
# with -idirafter.
HOST_INCLUDES := $(strip $(foreach f,$(DOSBOX_INCLUDES),\
	$(if $(or $(findstring /dosbox-core/,$(f)),$(findstring $(CURDIR)/src/,$(f))),$(f))))

.PHONY: print-host-includes
print-host-includes:
	@printf '%s\n' "$(subst $(space),\\ ,$(HOST_INCLUDES))"

# The same list, for DOSBox-core's own build (libretro/Makefile.common expanded
# by make with the flags upstream uses for a fake-SDL build).  make check-sources
# compares the two.
upstream-srcs:
	@$(MAKE) -s --no-print-directory -f scripts/upstream-srcs.mk CORE_DIR=$(DOSBOX_DIR) print-upstream-srcs

check-sources:
	@echo "=== ficheros frente a DOSBox-core"
	@DOSBOX_DIR=$(DOSBOX_DIR) python3 scripts/check-sources.py

# Which snapshot is this tree?  First line is the answer to "why does my make
# not have target X" - because it is not this Makefile.
# What this tree has and what it is missing.  Safe to run at any moment, with or
# without toolchain, because it only looks at files: no $(PREFIX), no include of
# Rules.mk.  Its whole job is to be the one command that answers "why did my
# build stop before it started" - and to name the directory, because these three
# things live inside the project and a sync can take them away.
deps:
	@echo "BM-DOS $(PROJECT_SNAPSHOT)"
	@printf "  %-16s %s\n" "toolchain:"   "$(if $(TOOLCHAIN_BIN),$(TOOLCHAIN_BIN),MISSING - expected in $(TOOLCHAIN_DIR)/arm-gnu-toolchain-*/bin)"
	@printf "  %-16s %s\n" "circle-stdlib:" "$(if $(wildcard $(CIRCLE_STDLIB)/libs/circle/Config.mk),present,MISSING - run ./scripts/setup.sh)"
	@printf "  %-16s %s\n" "dosbox-core:" "$(if $(wildcard $(DOSBOX_DIR)/src/dosbox.cpp),present,MISSING - run make fetch)"
	@printf "  %-16s %s\n" "build/:"      "$(if $(wildcard build),present,absent)"
	@echo
	@echo "  the three of them live inside this project on purpose: nothing is written"
	@echo "  to $$HOME.  A sync from the workspace must exclude third_party/ and opt/."

version:
	@echo "BM-DOS snapshot $(PROJECT_SNAPSHOT)  ($(PROJECT_SNAPSHOT_DATE))"
	@echo "  makefile leido:  $(firstword $(MAKEFILE_LIST))"
	@echo "  (si este sello no coincide con el que se te ha dado, la copia no ha llegado)"
	@echo "  Makefile sha256: $$(sha256sum Makefile | cut -c1-12)"
	@echo "  audio device:    $(AUDIO_DEVICE)$(if $(filter pwm,$(AUDIO_DEVICE)),  (jack forced, HDMI not tried),  (HDMI first, jack as fallback))"
	@echo "  dosbox-core:     $$(cat DOSBOX_REV 2>/dev/null)"
	@echo "  circle-stdlib:   $$(test -f $(CIRCLE_STDLIB)/libs/circle/Config.mk && echo configured || echo 'not configured')"
	@echo "  toolchain:       $$($(if $(TOOLCHAIN_BIN),echo $(TOOLCHAIN_BIN),echo 'not in $(TOOLCHAIN_DIR)'))"

help:
	@echo "make fetch      clone DOSBox-core at the pinned revision"
	@echo "make check      verify include paths and headers (no build)"
	@echo "make check-sources  compare our file list with DOSBox-core's (no build)"
	@echo "make print-srcs     print the files we compile"
	@echo "make deps       what this tree has and what it is missing (no toolchain needed)"
	@echo "make check-platform  symbols of the platform contract vs what is defined (no toolchain)"
	@echo "make check-kernel    compile kernel.cpp on the host against a Circle stand-in"
	@echo "make check-bootconf  run the /boot.conf parser against the files people write"
	@echo "make check-scaling   aspect ratio and screen usage of the scaler, per mode"
	@echo "make check-dynarec   the page-table walk that makes the JIT cache executable"
	@echo "make check-coreopts  the CPU cycles the frontend asks the core for (486DX2-66)"
	@echo "make check-resample  the image filter, against patterns with a known answer"
	@echo "make check-storage   the flush sequence that replaced sync(), against a fake FatFs"
	@echo "make check-portorder the order of the game-port announcement vs the game load"
	@echo "make check-dynarec-hook  the JIT hook, compiled against the real core sources"
	@echo "make check-governor  the clock governor's level table, per board"
	@echo "make warnings   which warnings are on for our code and which for DOSBox"
	@echo "make AUDIO_DEVICE=pwm   build with the 3.5 mm jack instead of HDMI"
	@echo "make version    print the snapshot and the state of third_party"
	@echo "make            build the kernel image"
	@echo "make card-check  show what a card update would change (writes nothing)"
	@echo "make card-update update a card that already works, keeping dos/"
	@echo "make distclean  remove build/ and third_party/"

# ---------------------------------------------------------------------------
# Writing a card
# ---------------------------------------------------------------------------
#
# Three goals, three different situations.  The names say which is destructive:
#
#   card-update   a card that already works: replaces the kernel, adds what is
#                 missing, never deletes anything, never touches dos/ or
#                 dosbox/saves.  This is the one to use day to day.
#   card-check    the same, with --dry-run: prints the plan and writes nothing.
#   (make-sd.sh)  a card from nothing: partitions and formats.  Deliberately NOT
#                 a make goal - it takes a block device and destroys it, and that
#                 should never be one keystroke away from "make".
.PHONY: card-update card-check
card-check:
	@./scripts/update-sd.sh --dry-run

card-update:
	@./scripts/update-sd.sh

# ---------------------------------------------------------------------------
# Fetching and maintenance
# ---------------------------------------------------------------------------

# Turns ACTIVE_ERR into a failure.  It is the first prerequisite of the image, so
# it runs before anything is compiled, and it only exists when something is
# actually wrong - otherwise its recipe is empty.
ifneq ($(ACTIVE_ERR),)
.PHONY: setup-check
setup-check:
	@printf "\n$(ACTIVE_ERR)\n" >&2
	@false
endif

# "make fetch" is safe to run at any time, including on a fresh clone with no
# circle-stdlib and no toolchain: it is the first command of the quick start.
fetch:
	@if [ ! -d "$(DOSBOX_DIR)/.git" ]; then \
		mkdir -p $(dir $(DOSBOX_DIR)); \
		git clone --filter=blob:none $(DOSBOX_REPO) $(DOSBOX_DIR); \
	fi
	@cd $(DOSBOX_DIR) && git fetch --all --tags --quiet && \
		git checkout --quiet $(DOSBOX_REV) && \
		echo "dosbox-core at $$(git rev-parse --short HEAD)"
	@echo "pinned revision: $(DOSBOX_REV)"

# Quick check that the tree we were given is the revision the port expects.
check-rev:
	@if [ ! -d "$(DOSBOX_DIR)/.git" ]; then \
		echo "dosbox-core is not fetched yet - run 'make fetch'"; exit 1; \
	fi
	@cd $(DOSBOX_DIR) && \
		if [ "$$(git rev-parse HEAD)" = "$(DOSBOX_REV)" ]; then \
			echo "dosbox-core is at the pinned revision ($(DOSBOX_REV))"; \
		else \
			echo "WARNING: dosbox-core is not at the pinned revision"; \
			echo "  expected: $(DOSBOX_REV)"; \
			echo "  actual:   $$(git rev-parse HEAD)"; \
			echo "  the stub layer and the libretro.h typedefs may need updating"; \
		fi

# "clean" belongs to Circle's Rules.mk (it removes the objects listed in
# $(EXTRACLEAN), which is why that variable is set above).  What is missing from
# it is the build/ directory itself, hence distclean:
distclean:
	@echo "  DISTCLEAN"
	@rm -rf build third_party
	@rm -f kernel*.elf kernel*.img kernel*.lst kernel*.map

.PHONY: all fetch check-rev distclean setup-check

# Dependency files written by -MMD.  Guarded: when the administrative goals skip
# the includes above, $(OBJS) is empty and there is nothing to include anyway.
ifneq ($(OBJS),)
-include $(OBJS:.o=.d)
endif
