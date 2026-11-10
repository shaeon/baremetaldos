#!/usr/bin/env bash
#
# Fetch and build the toolchain side of BM-DOS (Circle, circle-stdlib, newlib,
# libc++ and the Arm cross compiler).  Run **once** per machine.
#
# Usage:
#   ./scripts/setup.sh           # ~30-60 min, ~1.2 GB in opt/ and third_party/
#   make fetch                   # only the DOSBox-core tree
#
# It does three things, in this order, and stops at the first problem:
#
#   1. checks that an AArch64 bare metal toolchain is on PATH (the Arm GNU
#      Toolchain, prefix aarch64-none-elf-) and tells you where to get it if not;
#   2. clones circle-stdlib with its submodules - which brings Circle itself,
#      newlib and the LLVM libc++ checkout - and configures it for your board
#      with libc++, a 64 MB kernel and multi-core support;
#   3. builds Circle, newlib and libc++.  This is the long part: expect 30-60
#      minutes on a laptop, and about 4 GB of disk for the build tree.
#
# Then "make" in the repository root builds the emulator itself.
#
# Everything is installed under third_party/, inside the repository, so you can
# delete the whole thing with "make distclean".
#
# Tested on CachyOS/Arch and Debian/Ubuntu.  On Arch the packages are:
#   sudo pacman -S base-devel git python mtools dosfstools
# and the toolchain is either from the Arm downloads page or:
#   sudo pacman -S aarch64-none-elf-gcc    (if your AUR helper provides it)
#
set -euo pipefail

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

# Raspberry Pi model: 3, 4 or 5.  Override with RASPPI=3 ./scripts/setup.sh
RASPPI="${RASPPI:-4}"

# 64 MB: DOSBox plus libc++ is far bigger than Circle's 4 MB default.
KERNEL_MAX_MB="${KERNEL_MAX_MB:-64}"

TOOLCHAIN_PREFIX="${TOOLCHAIN_PREFIX:-aarch64-none-elf-}"

# circle-stdlib v20+ builds libc++ from the LLVM monorepo, and the current
# libc++ requires GCC 15: its headers use compiler builtins (__is_array,
# __is_reference, __type_pack_element, __has_feature, __builtin_operator_new)
# that GCC 13/14 do not have.  With an older toolchain the build dies thousands
# of lines deep inside libc++ with errors that look like a broken library.
# Circle itself recommends 15.2.Rel1 as well.
TOOLCHAIN_MIN_MAJOR="${TOOLCHAIN_MIN_MAJOR:-15}"
TOOLCHAIN_VERSION="${TOOLCHAIN_VERSION:-15.2.rel1}"
TOOLCHAIN_ARCHIVE="arm-gnu-toolchain-${TOOLCHAIN_VERSION}-x86_64-aarch64-none-elf.tar.xz"
TOOLCHAIN_URL="https://developer.arm.com/-/media/Files/downloads/gnu/${TOOLCHAIN_VERSION}/binrel/${TOOLCHAIN_ARCHIVE}"
CIRCLE_STDLIB_DIR="${CIRCLE_STDLIB_DIR:-$REPO_ROOT/third_party/circle-stdlib}"

# Everything this project downloads lives inside the project: third_party/ for
# the sources, opt/ for the toolchain.  Nothing goes in $HOME.  One directory to
# copy, one directory to delete, and no way for a sync from the workspace to
# take the toolchain with it (opt/ is in .gitignore and in the rsync excludes).
TOOLCHAIN_DIR="${TOOLCHAIN_DIR:-$REPO_ROOT/opt}"

die () { echo "error: $*" >&2; exit 1; }
say () { printf '\n=== %s\n' "$*"; }

# ---------------------------------------------------------------------------
# 1. Toolchain
# ---------------------------------------------------------------------------

say "Checking the AArch64 bare metal toolchain"

# The toolchain is installed INSIDE this checkout, in opt/, and that is a
# deliberate decision, not an accident: the whole point of this tree is to be one
# self-contained directory.  It is ~1.2 GB.  Because a sync from the workspace
# can delete (that is how retired source files disappear), opt/ is in .gitignore
# and it is excluded from any sync of this tree - a toolchain that
# the project itself installed must not be something the project can delete.

# Does the tree already carry a toolchain?  The Makefile puts
# $(TOOLCHAIN_DIR)/arm-gnu-toolchain-*/bin in front of PATH for its own
# invocation, so a build works without this script ever touching ~/.bashrc.
FOUND_BIN="$(ls -d "${TOOLCHAIN_DIR}"/arm-gnu-toolchain-*/bin 2>/dev/null | head -1)"

if [ -z "${FOUND_BIN}" ] && ! command -v "${TOOLCHAIN_PREFIX}g++" >/dev/null 2>&1; then
	cat >&2 <<EOF
error: ${TOOLCHAIN_PREFIX}g++ not found.

BM-DOS builds AArch64 bare metal code.  You need the Arm GNU Toolchain
for the "AArch64 ELF bare-metal target (aarch64-none-elf)", version
${TOOLCHAIN_VERSION} or newer.

It goes INSIDE this project, in ${TOOLCHAIN_DIR}:

  mkdir -p ${TOOLCHAIN_DIR}
  cd ${TOOLCHAIN_DIR}
  curl -LO ${TOOLCHAIN_URL}
  tar -xf ${TOOLCHAIN_ARCHIVE}

That is ~1.2 GB, and it stays in the tree.  Nothing is written to \$HOME and
\$PATH is not modified: the Makefile prepends
${TOOLCHAIN_DIR}/arm-gnu-toolchain-*/bin for its own invocation.

If you already have a toolchain elsewhere and would rather not download
another one, point the build at it instead:

  make TOOLCHAIN_DIR=/path/with/arm-gnu-toolchain-*

Then run this script again.
EOF
	exit 1
fi
if [ -n "${FOUND_BIN}" ]; then
	export PATH="${FOUND_BIN}:${PATH}"
	echo "found: ${FOUND_BIN}/${TOOLCHAIN_PREFIX}g++"
fi

TOOLCHAIN_FOUND_VERSION="$("${TOOLCHAIN_PREFIX}g++" -dumpfullversion -dumpversion)"
echo "found: $(command -v "${TOOLCHAIN_PREFIX}g++") (GCC ${TOOLCHAIN_FOUND_VERSION})"

TOOLCHAIN_FOUND_MAJOR="${TOOLCHAIN_FOUND_VERSION%%.*}"

if [ "${TOOLCHAIN_FOUND_MAJOR}" -lt "${TOOLCHAIN_MIN_MAJOR}" ]; then
	cat >&2 <<EOF

error: GCC ${TOOLCHAIN_FOUND_VERSION} is too old.

circle-stdlib builds the C++ standard library as LLVM libc++ (that is where
std::filesystem comes from, and DOSBox-core does not build without it).  The
current libc++ requires GCC ${TOOLCHAIN_MIN_MAJOR} or later; with GCC 13/14 the
build fails a long way into libc++ with errors about __is_array, __is_reference,
__has_feature and __builtin_operator_new.  Those come from the library needing
compiler builtins your GCC does not have - it is not something this project can
patch around, and there is no "--libcxx with an old compiler" combination.

Install the recommended toolchain instead:

  $TOOLCHAIN_URL

  mkdir -p ${TOOLCHAIN_DIR} && cd ${TOOLCHAIN_DIR}
  curl -LO ${TOOLCHAIN_URL}
  tar -xf ${TOOLCHAIN_ARCHIVE}

(that is where this project expects it: ${TOOLCHAIN_DIR}/arm-gnu-toolchain-*/
bin.  The Makefile puts it first for its own invocation, so the old toolchain on
PATH stops mattering.)

Then delete the half-configured tree and start over:

  rm -rf $CIRCLE_STDLIB_DIR
  ./scripts/setup.sh

(Do not try "make clean" inside it: the old configuration was generated for
AArch32 in the default case and has to be redone anyway.)
EOF
	exit 1
fi

if ! command -v git >/dev/null 2>&1; then
	die "git is required"
fi

# ---------------------------------------------------------------------------
# 2. circle-stdlib
# ---------------------------------------------------------------------------

say "Fetching circle-stdlib (this pulls Circle, newlib and libc++)"

if [ ! -d "$CIRCLE_STDLIB_DIR/.git" ]; then
	mkdir -p "$(dirname "$CIRCLE_STDLIB_DIR")"
	git clone --recursive https://github.com/smuehlst/circle-stdlib.git \
		"$CIRCLE_STDLIB_DIR"
else
	echo "already cloned: $CIRCLE_STDLIB_DIR"
	git -C "$CIRCLE_STDLIB_DIR" submodule update --init --recursive
fi

# Circle keeps its WLAN stack (hostap) and LVGL as submodules, and
# circle-stdlib's CStdlibApp headers include the WLAN headers, so they have to
# be present or the application will not compile.
say "Initialising Circle's own submodules (hostap, lvgl)"
git -C "$CIRCLE_STDLIB_DIR/libs/circle" submodule update --init --recursive

# ---------------------------------------------------------------------------
# 2b. Our patches to Circle, applied automatically
# ---------------------------------------------------------------------------
#
# A clone that has to be patched by hand is a clone that does not work, and the
# README saying "apply this" is not a build step.  The patches live in
# patches/*.patch, they are applied here, and applying one twice is not an
# error:
#
#   * --reverse --check is asked first: if the patch is already in the tree the
#     reverse direction applies cleanly, and that is the test.  (git apply has no
#     --forward - that is patch(1), which is not what is being used here.)
#   * --check is asked second, so a patch that does not fit the tree fails with
#     an explanation instead of half a hunk.
#
# The failure this closes: patches/circle-rpi3-usbstring.patch fixes an
# assertion abort in Circle's USB string handling that only a Pi 3 can reach
# (RASPPI <= 3).  Without it, a Pi 3 stops after USB enumeration with
# "assertion failed" in red and the port never starts.  That was documented in
# the README as a manual step; a Pi 3 user who did not read that far got a
# board that looks broken.
CIRCLE_TREE="$CIRCLE_STDLIB_DIR/libs/circle"

for p in "$REPO_ROOT"/patches/*.patch; do
	[ -f "$p" ] || continue

	if git -C "$CIRCLE_TREE" apply --reverse --check "$p" >/dev/null 2>&1; then
		echo "  already applied: $(basename "$p")"
		continue
	fi

	if ! git -C "$CIRCLE_TREE" apply --check "$p" >/dev/null 2>&1; then
		die "cannot apply $p to $CIRCLE_TREE - the tree has changed; see\n  git -C $CIRCLE_TREE apply --check $p"
	fi

	if git -C "$CIRCLE_TREE" apply "$p"; then
		echo "  applied: $(basename "$p")"
	else
		die "could not apply $p to $CIRCLE_TREE -\n  the Circle revision may have moved; check with\n  git -C $CIRCLE_TREE apply --check $p"
	fi
done

# ---------------------------------------------------------------------------
# 3. Configure and build
# ---------------------------------------------------------------------------

say "Configuring circle-stdlib for a Raspberry Pi $RASPPI, AArch64, libc++"

# A previous configuration is not reusable: "make clean" keeps Config.mk, and a
# tree configured without --aarch64 silently stays 32-bit.  configure is cheap,
# so start from a clean slate whenever the existing configuration does not match
# what we are about to ask for.
if [ -f Config.mk ] && ! grep -q '^AARCH = 64' Config.mk; then
	say "Existing configuration is not AArch64 - reconfiguring from scratch"
	make mrproper >/dev/null 2>&1 || true
fi

cd "$CIRCLE_STDLIB_DIR"
./configure \
	-r "$RASPPI" \
	-p "$TOOLCHAIN_PREFIX" \
	--aarch64 \
	--libcxx \
	--kernel-max-size "$KERNEL_MAX_MB" \
	-o ARM_ALLOW_MULTI_CORE \
	-o KERNEL_STACK_SIZE=0x200000

# A note on the options:
#
#   --aarch64              THIS ONE IS NOT OPTIONAL and it is easy to miss:
#                          configure builds for AArch32 by default, even with an
#                          aarch64-none-elf- prefix.  Without it you get a 32-bit
#                          kernel and no libc++ (there is no 32-bit libc++ in
#                          circle-stdlib), and the build fails while linking.
#   --libcxx               gives us <filesystem>, <thread>, <mutex> and
#                          thread_local on bare metal.  std::filesystem is not
#                          optional for DOSBox-core: it is used for its overlay
#                          drive and for enumerating a game directory.
#   --kernel-max-size 64   the kernel image is ~10x the default limit.
#   ARM_ALLOW_MULTI_CORE   so that a later milestone can put presentation (and
#                          eventually the JIT) on a second core without
#                          reconfiguring everything.  It is inert until the
#                          application actually starts another core.
#   KERNEL_STACK_SIZE=2M   DOSBox's interpreter recurses through the CPU core;
#                          Circle's default 128 KB stack is not enough.

say "Building Circle, newlib and libc++ (30-60 minutes, once)"
make -j"$(nproc)"

say "Done"
cat <<EOF

circle-stdlib is built.  Now build the emulator itself:

    cd $REPO_ROOT
    make fetch              # clone dosbox-core at the pinned revision
    make -j\$(nproc)        # build kernel8-rpi4.img (or your board's kernel)

Then prepare an SD card with:

    sudo ./scripts/make-sd.sh /dev/sdX

See README.md for the SD card layout and for how the port works.
EOF
