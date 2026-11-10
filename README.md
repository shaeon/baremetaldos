# BM-DOS

**DOSBox on bare metal Raspberry Pi.**

by [@mmoroca](https://github.com/mmoroca) & [arena.ai](https://arena.ai), 2026

![BM-DOS booting into the DOSBox prompt](images/bm-dos.png)

BM-DOS turns a Raspberry Pi into a period-correct DOS machine. There is no Linux,
no desktop and no window manager: the Pi boots straight into the DOSBox command
prompt, and DOS programs run on it. The same source tree builds for the Pi 3, the
Pi 4 family (including the 400 and the CM4) and the Pi 5.

---

## Contents

- [What it is](#what-it-is)
- [Hardware](#hardware)
- [Quick start](#quick-start)
- [Build environment](#build-environment)
- [Building from scratch](#building-from-scratch)
- [Putting it on a card](#putting-it-on-a-card)
- [Configuration: `boot.conf`](#configuration-bootconf)
- [Configuration: `cmdline.txt`](#configuration-cmdlinetxt)
- [Configuration: `config.txt` and `dosbox.conf`](#configuration-configtxt-and-dosboxconf)
- [Keys](#keys)
- [The status panel](#the-status-panel)
- [How it works](#how-it-works)
- [Tests](#tests)
- [Troubleshooting](#troubleshooting)
- [Repository layout](#repository-layout)
- [Licence](#licence)
- [Credits](#credits)
- [Acknowledgements](#acknowledgements)

---

## What it is

A Raspberry Pi boots the Circle bare metal environment, which brings up the
framebuffer, USB, the SD card and an audio device. On top of that runs
**DOSBox-core** - the libretro fork of DOSBox - driven by a frontend written for
this port. The kernel image is a single file of about 4 MB, named after the board
it targets (`kernel8-rpi4.img` on a Pi 4).

What works today:

| Feature | State |
|---|---|
| DOS shell, `C:`/`D:` drives from the card | works |
| `dosbox.conf` read from the card, `[autoexec]` honoured | works |
| VGA text and graphics modes, scaling and aspect ratio | works |
| Sound Blaster, Adlib, PC speaker (HDMI or 3.5 mm jack) | works |
| Dynamic recompiler (JIT), selectable from `dosbox.conf` | works |
| USB keyboard, mouse, gamepads (HID) | works |
| ARM clock governor, 600 MHz at the DOS prompt | works |
| Status panel, screenshots, safe shutdown | works |
| MIDI: MT-32 (Munt) and General MIDI (FluidSynth), rendered on core 1 | works when built with `scripts/build-midi-libs.sh` |
| CD-ROM, networking, IPX, modem | not implemented |
| Voodoo / 3dfx | not implemented |

The emulated machine is a 486DX2-66 (23880 cycles) unless `dosbox.conf` says
otherwise.

## Hardware

Any Raspberry Pi from the 3 onwards, and later models as Circle supports them.
The board is chosen at build time with `RASPPI`, and the same source tree covers
all of them:

| `RASPPI` | Boards | Kernel image |
|---|---|---|
| `3` | Pi 3, Pi 3+, Zero 2 W | `kernel8.img` |
| `4` | **default**: Pi 4, Pi 400, CM4, CM5 | `kernel8-rpi4.img` |
| `5` | Pi 5 | `kernel_2712.img` |

The minimum is a board with 1 GB of RAM; 2 GB or more is comfortable, because the
frame buffer and the emulator's memory come out of the same pool.

Beyond the board:

* A microSD card, 2 GB or more, FAT32, single partition. **Once written, the card
  is plain FAT32**, so games, `dosbox.conf` and `boot.conf` can be edited from any
  computer - Windows, macOS or Linux - with no special tools. The Linux host is
  only needed to *build* the kernel and to *write* the card the first time.
* An HDMI display and a USB keyboard. A mouse and a gamepad are optional.
* No fan required: the governor keeps the SoC at 600 MHz whenever nothing is
  being emulated.

Tested on a **Raspberry Pi 400** with a 1080p HDMI panel.

## Quick start

If everything is already set up on this machine:

```bash
make                       # build kernel8-rpi4.img
make card-check            # show what a card update would change
make card-update           # write a card that already boots, keeping dos/
```

From an empty checkout, the whole sequence is [Building from
scratch](#building-from-scratch) below. It takes between 30 and 90 minutes, most
of it compiling libc++.

## Build environment

### Host

A Linux x86_64 machine. Tested on CachyOS/Arch; the Debian/Ubuntu commands are
given alongside.

| Package | Why |
|---|---|
| `git` | fetching Circle, circle-stdlib and DOSBox-core |
| `make` | the build itself |
| `python3` | the source-list check and the generated host tests |
| `mtools`, `dosfstools` | writing the FAT32 card |
| `parted` | partitioning the card |
| `curl` | downloading the toolchain |

```bash
# Arch / CachyOS
sudo pacman -S --needed base-devel git python mtools dosfstools parted curl

# Debian / Ubuntu
sudo apt install build-essential git python3 mtools dosfstools parted curl
```

About 8 GB of disk for the build trees and the toolchain.

### Cross toolchain

**Arm GNU Toolchain for the AArch64 ELF bare-metal target**, prefix
`aarch64-none-elf-`, version **15.2 or newer**. This is not negotiable: the
libc++ that circle-stdlib builds uses compiler builtins (`__is_array`,
`__has_feature`, `__builtin_operator_new`) that GCC 13 and 14 do not have, and
the build fails thousands of lines deep inside libc++ with errors that look like
a broken library.

From <https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads>, or:

```bash
mkdir -p opt && cd opt
curl -LO https://developer.arm.com/-/media/Files/downloads/gnu/15.2.rel1/binrel/arm-gnu-toolchain-15.2.rel1-x86_64-aarch64-none-elf.tar.xz
tar -xf arm-gnu-toolchain-15.2.rel1-x86_64-aarch64-none-elf.tar.xz
```

It lives **inside the project**, in `opt/`, which is in `.gitignore`. Nothing is
written to `$HOME` and `$PATH` is not modified: the `Makefile` prepends
`opt/arm-gnu-toolchain-*/bin` for its own invocation, and `make version` prints
which compiler it is using. To use a toolchain from somewhere else:

```bash
make TOOLCHAIN_DIR=/path/with/arm-gnu-toolchain-*
```

Do **not** use the `gcc-aarch64-linux-gnu` package or the `aarch64-none-elf-gcc`
from most distributions: those are GCC 13/14 and the build will fail.

## Building from scratch

```bash
git clone https://github.com/mmoroca/baremetaldos.git
cd baremetaldos          # the directory is named after the repository
chmod +x scripts/*.sh scripts/hostcheck/*.sh
```

The `chmod` is not optional and it is worth knowing why: git only keeps the
executable bit on the files that were committed with it, and a checkout that
lost it produces scripts that answer `Permission denied` - or, on a filesystem
that does not store the bit at all, scripts that do not start. Doing it once
after cloning costs nothing and removes a class of confusing failures.

**1. Fetch and build Circle, newlib and libc++** (30-60 minutes, the long part):

```bash
./scripts/setup.sh              # Pi 3, 4 family or 5 by RASPPI, default 4
RASPPI=3 ./scripts/setup.sh     # Pi 3 / Zero 2 W
RASPPI=5 ./scripts/setup.sh     # Pi 5
```

The script checks the toolchain, clones `circle-stdlib` with its submodules into
`third_party/`, configures it with libc++, a 64 MB kernel and multi-core support,
and builds it. If the toolchain is missing it prints the two commands that fetch
one into `opt/` and stops.

**2. Fetch DOSBox-core** at the pinned revision:

```bash
make fetch
make check-rev        # confirms the revision the port expects
```

**2b. Optional: build the MIDI synthesizers** (Munt and FluidSynth, a few
minutes; needs `cmake`):

```bash
./scripts/build-midi-libs.sh
```

They go to `third_party/midi-rpi$RASPPI/`.  The kernel links them when they
are there and builds without MIDI when they are not; `make MIDI=0` forces it
off.  Then, on the card: the MT-32 ROMs (`MT32_CONTROL.ROM` and `MT32_PCM.ROM`,
or the `CM32L_*` pair) or a SoundFont (`default.sf2`) in `/dosbox/system/`,
and in `dosbox.conf`:

```ini
[midi]
mididevice=mt32        # or fluidsynth
mpu401=intelligent
```

The ROMs and SoundFonts are not included.  The synthesizer runs on core 1 and
DOSBox stays on core 0: nothing of DOSBox is shared, only a ring of MIDI bytes
in and a ring of samples out (`src/platform/bare_midi.cpp`).  The approach and
the FluidSynth patch come from [mt32-pi](https://github.com/dwhinham/mt32-pi).

**3. Build the kernel image:**

```bash
rm -rf build
make -j"$(nproc)"
```

The result is `kernel8-rpi4.img` in the repository root (on `RASPPI=3` it is
`kernel8.img`, on `RASPPI=5` it is `kernel_2712.img`). `make version` prints the
snapshot stamp, the Makefile hash and the state of `third_party/`.

For the 3.5 mm jack instead of HDMI audio:

```bash
make AUDIO_DEVICE=pwm
```

### The Pi 4 ARM stub

Circle needs `armstub8-rpi4.bin` next to `config.txt` on a Pi 4. `make-sd.sh`
fetches the firmware, builds the stub and copies it, so a card written with that
script needs nothing else. If you are assembling a card by hand:

```bash
make -C third_party/circle-stdlib/libs/circle/boot armstub64
cp third_party/circle-stdlib/libs/circle/boot/armstub8-rpi4.bin sd/
```

## Putting it on a card

Two paths, and the order matters: **first write a card from nothing, then update
it** without destroying anything. Both need the card inserted in the computer and
**mounted** - the scripts take a block device (`/dev/sdX`) or a mount point, and
on Windows use a tool such as Rufus, balenaEtcher or Win32DiskImager for the first
write.

> **Find the right device first.** `/dev/sdX` in the commands below is the card,
> not your hard disk. `lsblk` (Linux) or `diskutil list` (macOS) before, always;
> a wrong letter here destroys the wrong disk.

### 1. A card from nothing

**Destructive: partitions and formats the device.** It is deliberately not a
`make` goal, so it can never be one keystroke away from `make`.

```bash
./scripts/make-sd.sh --stage ./sdcard     # assemble the tree first, look at it
sudo ./scripts/make-sd.sh /dev/sdX        # then write it to the card
```

`--stage` is worth the extra step: it produces exactly the files a card needs in
a local directory, so you can check them before anything is partitioned.

The script partitions the card as **one FAT32 partition**, copies the kernel, the
firmware files and the contents of `sd/`, and leaves it ready to boot. The layout
is:

```
/            boot.conf, config.txt, cmdline.txt, kernel8-rpi4.img, firmware
/dosbox/     dosbox.conf
/dos/        games and programs - empty; put them here yourself
```

Unmount the card, put it in the Pi and power it on. The emulator starts at the
`C:` prompt.

### 2. A card that already boots

This is the everyday path after a new kernel has been built. It replaces the
kernel, adds anything missing, and **never deletes** `dos/`, `dosbox/saves` or
anything edited by hand:

```bash
make card-check      # print the plan, write nothing
make card-update     # do it
```

or directly, with the card mounted:

```bash
sudo ./scripts/update-sd.sh /dev/sdX
sudo ./scripts/update-sd.sh --dry-run /dev/sdX
./scripts/update-sd.sh --stage ./sdcard     # assemble a directory instead
```

A card can also carry the kernel of **every** board: `config.txt` selects one per
model, so a single card boots a Pi 3, a Pi 4 and a Pi 5 if the three images are in
the root. The script copies the board it was built for by default, and copies a
list instead when `KERNELS` says so:

```bash
RASPPI=4 KERNELS="kernel8.img kernel8-rpi4.img kernel_2712.img" \
    ./scripts/make-sd.sh --stage ./sdcard
```

Each kernel has to exist in the tree, and each one has to come from a build of
its own board: Circle is board-specific at compile time (`-DRASPPI` and `-mcpu`
come from `Rules.mk`, and `circle-stdlib`'s `configure -r` decides the same in
`Config.mk`), so one build cannot produce another board's kernel. The script
checks that every `kernel=` line in `config.txt` names a file that is on the
card.

### 3. After that, any computer will do

The card is plain FAT32 with no special structure, so once it has been written it
can be edited from **any** machine - Windows, macOS or Linux:

* `boot.conf`, `config.txt` and `cmdline.txt` live in the root and are text;
* `dosbox.conf` is plain text in `/dosbox/`, and its `[autoexec]` section is where
  the drives are mounted;
* games and programs are files and directories in `/dos/`, copied like any other
  file.

No Linux host, no toolchain and no scripts are needed for any of that. The Linux
machine is only needed to build the kernel and to write a card the first time.

## Configuration: `boot.conf`

`/boot.conf` on the first partition of the card holds the settings that belong to
the **port** rather than to DOSBox. It is read by the kernel itself, before
anything else is set up, which is why it is a separate file: the display mode has
to be chosen before the framebuffer exists, and `dosbox.conf` is parsed much
later by DOSBox.

The format is one `key = value` per line, `#` starts a comment, unknown keys are
ignored **and reported in the log**, and a missing file is not an error - every
value has a default. A malformed file cannot make the board fail to boot; that is
deliberate.

### `display = WIDTHxHEIGHT`

**This is the one value the scripts take from this file**, and it depends on which
board the card is in. What it does is set `width=` / `height=` in `cmdline.txt`
when a card is made or updated - and `cmdline.txt` is what actually decides, see
the note below and [Configuration: `cmdline.txt`](#configuration-cmdlinetxt). The framebuffer is where DOSBox's output is converted and
copied, so the per-frame cost is proportional to this and not to the size of the
emulated screen: every extra pixel is paid for on every frame, and on the
smaller boards that is where the frame rate goes.

| Board | Start at | Why | May go up to |
|---|---|---|---|
| **Pi 3** | **`800x600`** | the CPU is the limit, not the display: 800x600 is 0.48 M pixels to convert per frame against 1280x720's 0.92 M | 1024x768 if the games you run are light |
| **Pi 4** | **`800x600` to `1280x720`** | 1280x720 is an exact 1.5x on a 1080p panel (~55 MB/s at 60 Hz); start at 800x600 if the emulated machine is a fast 486 or a Pentium | 1920x1080 on a light machine |
| **Pi 5 and later** | **`1280x720` and up** | there is headroom for the copy; take the pixels | 1920x1080 |

The other two values exist for a reason:

| Value | When |
|---|---|
| `1920x1080` | native on a 1080p panel; the most pixels to copy |
| `1024x768` | 4:3 monitors and VGA-era games |
| `640x480` | VGA exactly, one output pixel per emulated pixel, no scaler |

If the mode is refused, the next one down is tried automatically, so an
unsupported value cannot leave a black screen.

> **`cmdline.txt` has the last word.** Circle creates its own console framebuffer
> before this port creates its own, and its size comes from `width=` / `height=` in
> `cmdline.txt`, not from here. Two different geometries mean a mode change at run
> time, which is what used to leave the screen covered in garbage. So the console
> wins: the port follows whatever `cmdline.txt` gave it, and if this line says
> something else the boot carries on anyway and the panel says which file was
> used - see [Configuration: `cmdline.txt`](#configuration-cmdlinetxt). The
> scripts write this line into `cmdline.txt` when the card is built, which is why
> it is still worth editing here and re-running `scripts/update-sd.sh`; the file
> itself is authoritative for a card that already exists.

> **Note on the status panel width.** The panel is 118 characters in its typical
> state and up to 133 with every field at its maximum, which is 1064 pixels at 8
> px per character. In a mode narrower than 1088 the right-hand fields are
> clipped, and the last field is `drop`. Use 1280x720 or wider to see the whole
> line.

### `cpu_max_mhz = N`

The ceiling the clock governor may use, in MHz. The governor has three levels of
its own - 600 at the DOS prompt, 1500 for DOSBox's default cycles, 2000 only when
frames do not fit - and this caps the top of that range. `0` (the default) lets
the firmware decide what the board can do. It is a ceiling, not a fixed speed: the
governor still goes down to 600 when there is nothing to run.

### Choosing the emulated CPU speed (cycles)

`display` decides the cost of *drawing*; `cycles` decides the cost of
*emulating*. They are the two numbers that make the difference between a game
that runs and one that stutters, and they are set in different files.

The speed of the emulated machine is `cycles` in `dosbox/dosbox.conf`:

```ini
cycles=fixed 24000     # 24000 = a 486DX2-66 (see the table below)
cycleup=2000           # Ctrl+F11 and Ctrl+F12 move it by this much
cycledown=2000
```

The shipped default is 24000, a 486DX2-66, and `cycleup`/`cycledown` are 2000 so
one key press is roughly one machine generation. The card holds a default; the
running value can be adjusted in fine steps without touching any file:

- **Ctrl+F11 / Ctrl+F12** change the cycle count while the machine runs, by
  `cycleup` / `cycledown` (2000 with the shipped file). Get the number right on
  the panel first, then write it into `dosbox.conf` so it survives a reboot.
- `config -set cycles=...` from the DOS prompt also works, and the core treats a
  change made after startup as authoritative.

Target cycle counts, by emulated CPU. These are the figures DOSBox's own
documentation uses for `cycles` in `fixed` mode:

| Emulated CPU | MHz | cycles |
|---|---|---|
| 386DX | 25 | 4595 |
| 386DX | 33 | 6075 |
| 486DX | 33 | 12019 |
| 486DX2 | 66 | 23880 |
| 486DX4 | 100 | 33445 |
| 486DX5 | 133 | 47810 |
| Pentium | 60 | 31545 |
| Pentium | 66 | 35620 |
| Pentium | 75 | 43500 |
| Pentium | 90 | 52000 |
| Pentium | 100 | 60000 |
| Pentium | 120 | 74000 |

How to use it: pick the machine the game expects, set `cycles` near that figure,
then use Ctrl+F11/F12 to land on the value that *feels* right - the table is a
starting point, not a law, because a game's own frame limiter and the host's
speed both move the answer. If the panel says `pace` well below what the mode
refreshes at, the emulated machine is being asked for more than the host can
give, and the answer is fewer cycles or a smaller `display` - in that order.

> The core's `cycles` option is a grid (multiples of 10000 plus a "fine" part in
> thousands), so an exact figure like 23880 is not expressible from the core's
> option table: 24000 is what the card ships, and it is 0.5 % above a real
> 486DX2-66. `cycles=fixed 23880` written in `dosbox.conf` *is* honoured, because
> the core parses the file; see `src/platform/bare_coreopts.h` for why those two
> paths differ.

### `mode = auto | performance | powersave`

Parsed and validated, but not applied yet: it would mean overriding the governor,
which is not in this milestone. Setting anything other than `auto` produces a
warning in the log.

### What is deliberately *not* in `boot.conf`

* **The frame rate.** The core announces the refresh rate of its own video mode
  and the port obeys it. Forcing a number here would fight the emulation: a DOS
  text mode runs at 70 Hz, and pinning it to 60 makes the emulated machine run 17 %
  slower.
* **The aspect ratio and the scaling.** Decided by the core (4:3 for everything
  that matters) and respected by the scaler.
* **The CPU clock itself.** That is the governor's job, and pinning `arm_freq` in
  `config.txt` would take the decision away from it.

## Configuration: `cmdline.txt`

`cmdline.txt` is **not this port's file**: it is Circle's, and the firmware hands
it over as the kernel's command line. What it sets is everything that has to be
decided before or outside DOSBox, and the display mode is the one that matters
most here, because it is the size of the framebuffer everything is drawn into.

**It is one line, and only spaces separate options.** Circle does not read the
file from the card: it asks the firmware for it (`PROPTAG_GET_COMMAND_LINE`) and
splits the answer on `' '` - Circle's `doc/cmdline.txt`: *"Multiple options must
be placed on a single line delimited by a space character"*. Newlines and `#`
comments are **data**: a commented-out option is not an option, it is part of the
value of the one before it. Above 2048 bytes the whole string does not fit the
buffer the firmware tag declares, `CKernelOptions` gives up, and **every option in
the file** is ignored without a word. `scripts/make-sd.sh` and
`scripts/update-sd.sh` therefore collapse whatever they find into one line and
refuse to write a file that reaches 2048 bytes.

The card this project builds carries:

```
loglevel=4 logdev=tty1 keymap=es usbpowerdelay=500 width=1280 height=720 socmaxtemp=80
```

That is 87 bytes. `width=` and `height=` are rewritten from `boot.conf` every time
a card is made or updated.

### The options that matter for this port

| Option | Values | What it does here |
|---|---|---|
| `width=` / `height=` | up to `1920` / `1080` | **The display mode, and it wins over `boot.conf`.** These are the pixels the port converts every emulated frame into, so the per-frame cost is proportional to them. Unset, Circle asks the firmware for the display's own size, which arrives with the overscan border already subtracted. See the table of modes by board [above](#configuration-bootconf). |
| `loglevel=` | `0` panic ... `4` debug, `5` trace | **A ceiling on severity, not a filter on destination.** `4` (the default) shows everything up to debug; `0` shows only panics. It does *not* choose where the log goes - `boot_log = quiet` in `boot.conf` is what keeps forty core messages off the screen. |
| `logdev=` | `tty1` (screen), `ttyS1` (UART) | Where Circle's logger writes. `tty1` on a board with no serial cable attached. On a Pi 5 the UART is `ttyS11`. |
| `keymap=` | `DE` `DV` `ES` `FR` `IT` `UK` `US` | The **physical** layout Circle decodes, so that the USB report for the key marked `ñ` becomes the right one. It has nothing to do with the layout DOSBox sends to the emulated machine, which stays US unless `keyboardlayout` says otherwise in `dosbox.conf`. This port feeds DOSBox with USB usage codes and never consults Circle's keymap, so the line is set for the console layer (Circle's logger, and the terminal) rather than for the emulated machine. |
| `usbpowerdelay=` | `200` ... `8000` ms | Delay between powering a port and talking to the device. `500` is the value here; raise it if a keyboard or a stick is missed at boot on a board with a hub. |
| `socmaxtemp=` | `40` ... `85` C | The temperature the port's governor treats as a veto: at or above it, the clock drops one level. **`80` here, and 80 is the internal default as well** - so this line changes nothing. Circle itself accepts `40`..`78`; above `78` the key is ignored by Circle and only the port's own default remains, which is the same `80`. |

### The rest of Circle's options

Circle accepts many more, and they are documented where they are implemented -
`doc/cmdline.txt` in the Circle you built against
(<https://github.com/rsta2/circle/blob/master/doc/cmdline.txt>). They reach
hardware this port does not use on purpose: USB MIDI boost, touchscreen
calibration, the WM8960 codec, an I2S DAC, `gpiofanpin` for a case fan,
`usbspeed=full`, `usbignore`. None of them is needed to boot a card built by these
scripts, and none is set.

Two are worth knowing about even so, because they explain what this port does
**not** use:

* **`fast=true`** asks Circle's own `CCPUThrottle` for the maximum clock. There is
  no `CCPUThrottle` in this kernel: the clock is the governor's job (see
  [The status panel](#the-status-panel) and the Pi 3 note under
  [Troubleshooting](#troubleshooting)), so the key does nothing here.
* **`sounddev=`** picks Circle's sound device. This port opens its devices in order
  - HDMI, then PWM (the jack), then I2S - and takes the first that starts, because
  a device that constructs successfully is not a device that makes a noise. To
  force the jack, build with `make AUDIO_DEVICE=pwm`.

## Configuration: `config.txt` and `dosbox.conf`

`sd/config.txt` is the firmware's file: it selects the kernel image per Pi model,
fixes the HDMI mode, enables the audio subsystem, disables overscan and sets the
clock and voltage ceilings the firmware will allow. It intentionally sets no
`arm_freq`: the governor owns the clock, and pinning it would take the decision
away from it.

`sd/dosbox.conf` is the emulator's file, with DOSBox's own syntax:

* **`[cpu] cycles` is what decides whether a game is playable.** There is a full
  table of target values by emulated CPU in the `boot.conf` section above, and
  Ctrl+F11 / Ctrl+F12 change it while the machine runs.
* **`core = auto|dynamic|normal`** selects the JIT. `auto` runs the interpreter
  in real mode and switches to the dynamic recompiler in protected mode, which is
  the default and the fastest for most games. `normal` forces the interpreter and
  is useful when comparing behaviour.

## Keys

| Key | What it does |
|---|---|
| `PetSis` | show / hide the status panel |
| `Ctrl+Alt+PetSis` | reboot the board |
| `Ctrl+F9` | shut down safely, then power may be cut |
| `Alt+Pause` | pause / resume the emulated machine |
| `Ctrl+F5` | screenshot to `/DOSBOX/SHOTnnnn.BMP` |
| `Ctrl+F11` / `Ctrl+F12` | emulated cycles, 2000 per press |

Pulling the power is **not** a safe way to stop the machine: FAT on the card and
the SD card's own write cache can leave `dosbox.conf`, a mount or a save file half
written, and there is no filesystem check on a bare metal Pi to repair it
afterwards. `Ctrl+F9` flushes the card, says so on the panel, and only then says
it is safe to cut the power. If the flush fails, it says that instead, with the
card's own error code.

## The status panel

`PetSis` shows one line in a strip at the bottom of the screen that the emulator
never draws over. It is a diagnostic, so it is off by default.

```
Pi 400  4 cores  600 MHz  43C  L2/3 idle idle   pace 70/70 w62% bd70 dup 0   fr 8200us  HDMI 1397/5292  drop 0
└─┬──┘  └──┬──┘  └──┬───┘  └┬┘  └──────┬──────┘   └───────────┬────────────┘   └──┬──┘  └────┬─────┘  └──┬─┘
  board  cores   clock    temp   governor              pace and advances        frame cost   audio    dropped
```

### `pace R/W` - what was drawn against what the mode refreshes at

`R` is the number of pictures that reached the video callback per second; `W` is
the refresh rate the core announced for the current video mode. **They are two
rates, not a target and a result**, so the fraction is not a percentage of
success: a game running perfectly in a 60 Hz mode prints 60/60, and the same game
running perfectly in 70 Hz prints 70/70.

| On screen | Meaning |
|---|---|
| `pace 70/70` | the mode asks for 70 and gets 70 |
| `pace 34/70` | the mode asks for 70 and only 34 are produced: the host cannot keep up, or the game is paced by something else |
| `pace 24/70 dup 46` | judder: only a third of the frames are new, the rest are repeats |
| `pace 70/70` with `fr 45ms` | paced correctly, but each frame is expensive |

**The rule: `R` should equal `W`, and `W` should be the mode's own rate.**

### `bd N` - how often the emulated machine advanced

`pace` counts **pictures**. `bd` counts **frame boundaries the emulated machine
crossed**, per second. On a machine that is metered correctly the two agree; the
difference between them is what answers "is it running too fast?".

| On screen | Meaning |
|---|---|
| `pace 70/70 bd70` | the frame loop is the metronome: each advance produced its picture |
| `pace 33/70 bd70` | the emulator advances at its own rate and draws too little: it is slow, not fast |
| `pace 33/70 bd140` | **too fast**: the emulated machine advances twice as often as its mode asks for |
| `pace 70/70 bd70 +` | plus, some of those advances happened without the loop asking: nothing in the frame loop can meter those |

### `wNN%` - how much of the second was spent waiting

The fraction of the last wall-clock second the frame loop spent waiting for its
own next frame. A paced machine waits for most of every second (40-90 %) and calls
the emulator with the rest. **0 % means the loop is not metering at all** - but it
also happens legitimately when a frame costs more than the mode's period, so it is
never read on its own.

### `fr NNus` - the cost of one `retro_run()`

Microseconds per call. This is what distinguishes "the core produces one frame per
call" from "the core produces six": on a fast machine it is the difference between
plenty of room and none.

### `lm/max overNNN` - the clock governor

Two levels of three (the board's), then one word:

| Word | Meaning |
|---|---|
| `idle` | nothing is changing on screen; no clock is justified |
| `comf` | the frames are comfortable: the clock may come down |
| `busy` | over budget with a level above: the governor is climbing |
| `max` | over budget at the top level: no clock will help |
| `-` | between comfort and overrun: no evidence either way |

`overNNN` is how many consecutive frames have been over budget. A `max` word over
the DOS prompt means the machine believes it is working while the picture is not
changing, which is a fault worth reporting.

### `dup N`, `drop N`, `HDMI n/m`

`dup` is frames the core asked to repeat (the same picture again) per second;
`drop` is audio samples discarded per second; `HDMI 1397/5292` is the audio
device's queue in hardware frames. Constants are normal. A `drop` that climbs
continuously while the sound stutters is not.

## How it works

The port is a **libretro frontend** for exactly one core. DOSBox-core was written
as a libretro core, so it expects a host that registers callbacks (video, audio,
input, environment, log) and calls `retro_run()` once per frame. Everything below
that is this port's own:

* `src/kernel.cpp` - the Circle application: boot order, the frame loop, the
  pacer, the status panel.
* `src/platform/bare_libretro.cpp` - the libretro frontend: callbacks, options,
  keys, shutdown.
* `src/platform/bare_video.cpp` - framebuffer, scaling, aspect ratio, the panel
  slots, screenshots.
* `src/platform/bare_audio.cpp` - HDMI and PWM output, resampling.
* `src/platform/bare_thread.cpp` - the single-threaded replacement for
  DOSBox-core's emulator thread, built on Circle's cooperative scheduler.
* `src/platform/bare_sysinfo.cpp` and `governor_policy.h` - the ARM clock
  governor, its level table and its decision rules.
* `src/platform/bare_dynarec.cpp` - the code cache the JIT needs and that bare
  metal does not otherwise provide.
* `src/platform/bare_storage.h` - the card flush, which is what makes `Ctrl+F9` a
  safe way to stop the machine.

Three decisions shape the rest:

1. **The frame rate is the mode's, not 60.** With `RUN_SYNC` the core returns from
   `retro_run()` once per emulated frame, so a 70 Hz text mode returns 70 times a
   second. Pacing that at 60 would make the machine run 17 % fast, and no cycle
   count can fix it, because cycles decide how much work fits in a frame, not how
   many frames happen per second.
2. **The clock is set at run time.** The governor starts at 600 MHz, measures the
   cost of each frame against the budget the mode implies, and climbs only when
   the frames do not fit - and only while the picture is actually changing.
3. **Everything that can be tested on a PC is tested on a PC.** The build has
   more than 300 host assertions, against stand-ins for Circle and FatFs, because
   a check that needs the board is a check that runs twice a week.

## Tests

```bash
make check-host      # everything that runs on the host, no toolchain needed
make check           # include paths and headers only
make distclean       # remove build/ and third_party/ (keeps opt/)
```

`check-host` is a set of small programs and scripts:

| Target | What it checks |
|---|---|
| `check-platform` | the symbols `bare_env.h` promises against the ones the platform files define |
| `check-kernel` | `kernel.cpp` compiles against a stand-in for circle-stdlib |
| `check-bootconf` | the `/boot.conf` parser, on files people actually write |
| `check-scaling` | aspect ratio and screen usage of the scaler, per mode |
| `check-dynarec` | the page-table walk that makes the JIT cache executable |
| `check-coreopts` | the CPU cycles the frontend asks the core for |
| `check-resample` | the image filter, against patterns with a known answer |
| `check-storage` | the card flush sequence, against a fake FatFs |
| `check-governor` | the clock governor's level table and its decision rules |
| `check-portorder` | when the game-port announcement may happen |
| `check-dynarec-hook` | the JIT hook, compiled against the real core sources |
| `check-sources` | this Makefile's source list against DOSBox-core's |

`check-dynarec-hook` and `check-sources` need `third_party/dosbox-core`; without
it they report **OMITIDO with a non-zero status**, on purpose. A check that cannot
look is not a check that passes.

## Troubleshooting

**`make` says `Config.mk not found`.** Circle is not built. Run
`./scripts/setup.sh`.

**The build dies inside libc++ with errors about `__is_array` or
`__builtin_operator_new`.** The toolchain is too old. It must be GCC 15.2 or
newer; see [Build environment](#build-environment). `make version` prints which
compiler the build is using.

**A game is too slow.** That is the emulated CPU, not the port. The cycle count is
answered by the port, not read from `dosbox.conf` - the core rewrites that
variable from its option table during start-up - so editing `cycles` there
changes nothing. If the panel shows the clock dropping to 600 MHz while the game
is still asking for frames, that is a separate fault: see
[the note on the Pi 3 clock](#the-clock-on-a-pi-3).

**The picture does not fill the screen.** That is the aspect ratio being
respected. A 320x200 mode is 4:3 on real hardware, and stretching it to 16:9 is
wrong. Use `display = 640x480` if you want VGA exactly.

**No sound over HDMI but sound over the jack.** The display is the problem, not
the port: `hdmi_drive=2` is already in `config.txt`, and the port picks the jack
when HDMI has no sink. `make AUDIO_DEVICE=pwm` forces the jack.

**The status panel is clipped on the right.** The mode is narrower than 1088
pixels; see [the note on panel width](#display--widthxheight).

**It runs too fast and `cycles` does not help.** Look at `bd` on the panel: if it
is above `W`, the emulated machine is advancing faster than its own mode and the
evidence is in the panel, not in the cycle count. Include the whole panel line in
a bug report.

## Repository layout

```
Makefile                  build, checks and card goals
DOSBOX_REV                the pinned DOSBox-core revision
LICENSE                   the GPL-3.0 text, verbatim and unmodified
NOTICE                    copyright, why GPL-3.0, combined work, acknowledgements
README.md                 this file
README.es.md              the same, in Spanish
src/kernel.cpp            the Circle application and the frame loop
src/kernel.h
src/main.cpp              entry point
src/platform/             everything that talks to Circle, FatFs or the firmware
src/dosbox-stub/          the few POSIX and MIDI symbols DOSBox needs and the port
                          has to supply
scripts/                  setup, build, card writing, host checks
  setup.sh                toolchain + circle-stdlib, per board
  make-sd.sh              one board's card, from nothing (destructive)
  update-sd.sh            a card that already works (never deletes)
sd/                       the files that go on the card, as they ship
patches/                  fixes for upstream code, applied by scripts/setup.sh
```

`third_party/`, `build/`, `opt/` and `sdcard/` are generated and are in
`.gitignore`.

## Lessons learned

These are the traps that cost real time while bringing this port up. They are
here because anyone porting something to Circle, or to bare metal on a Raspberry
Pi, will step in the same ones - and every one of them is invisible from the sofa.

### `cmdline.txt` is not a file of options: it is one line

Circle does not read `cmdline.txt` from the card. It asks the firmware for it with
`PROPTAG_GET_COMMAND_LINE`, and what comes back is **one string that is split on
spaces only** (Circle's `doc/cmdline.txt`: *"Multiple options must be placed on a
single line delimited by a space character"*). Newlines are data, so every comment
line and every line break becomes part of the value of the option before it.

Worse: `TPropertyTagCommandLine::String[2048]` in `circle/bcmpropertytags.h`, and
`CKernelOptions` bails out entirely above it:

```cpp
if (m_TagCommandLine.Tag.nValueLength >= sizeof m_TagCommandLine.String) return;
```

**Every option is dropped, with no message.** This port shipped a nicely commented
2623-byte `cmdline.txt` for two rounds, with `width=800 height=600` on line 46, and
the board used the firmware's default because Circle never saw the key.

*Keep it to one line, no comments, under 2048 bytes.* `boot.conf` is the opposite:
it is read with `fopen` by the port itself, so comments there are fine.

### Two framebuffers exist before DOSBox draws anything

`CStdlibAppStdio::Initialize()` creates Circle's console framebuffer, sized from
`cmdline.txt`. This port then creates its own. Asking the firmware for a
different geometry at that point is a mode change at run time, which Circle's own
documentation says may be refused - and when it half-works, the display ends up
scanning memory that holds the console's text, in the console's palette. That was
the "garbage" on screen: not corruption, just a framebuffer nobody was going to
repaint.

**So `cmdline.txt` decides and the port follows it.** Every file that claims to
set the display mode is a claim, and only one of them is read before the first
framebuffer exists. The port used to refuse to boot when `boot.conf` and
`cmdline.txt` disagreed, which turned a cosmetic disagreement between two text
files into a dead board, and that is worse than the thing it reported: so now it
prints both numbers, says which one it is using, and carries on. The scripts keep
writing `width=`/`height=` from `boot.conf`, which is the reason editing
`boot.conf` still works when a card is rebuilt.

### `-Wunused-function` needs code generation

`gcc -fsyntax-only` will not report a static function nobody calls. A check built
on `-fsyntax-only` therefore announces "no warnings" while being unable to see that
class of failure; `-c` sees it. The same shape of mistake applies to tests: a test
that *prints* a value without comparing it passes every sabotage.

### A still picture is not an idle machine, and `fr` is not work

The clock governor's hardest measurement, and the one that took three attempts.

`fr 42254us` is how long `retro_run()` took, and on a DOS text screen most of that
is the emulated machine **sleeping** on its own timer - so a prompt looks like a
frame that took three budgets. Charging that to the frame is what kept a Pi 3 at
1200 MHz over a screen doing nothing, at 63 C.

The obvious counters are worse than they look. A healthy machine reads `pace 30/60`
or `35/70` with everything smooth, and a game repeating its picture - menus, status
bars, the gap between two tics - looks exactly like a prompt through any of
`pace`, `bd` or their ratio. Two rules were tried and both were wrong on the board:
"identical frame is idle" (a prompt stepped down, and so did DOOM, and the audio
broke up) and "identical *and* cheap is idle" (DOOM fine, prompt pinned at the top).

What separates them is **duration**: a DOS prompt leaves the screen literally
identical for minutes, and a game repaints something within seconds. So the rule
waits ten seconds before calling a still screen idle. It is not clever, it is
observable, and being wrong costs one level for three seconds.

### Nothing here is on more than one core

Circle serves interrupts on core 0 only, and DOSBox-core is not reentrant: its
globals are touched without locking. So the emulator runs on core 0, in one thread,
and `4 cores` on the panel is what the SoC has, not what the port uses. `fr` on a
text screen is mostly sleep, so a high number there is not a call for more cores.

### The firmware subtracts an overscan border from your mode

In CEA modes (`hdmi_group=1`, what a TV uses) the firmware applies a 48-pixel
overscan border per edge by default, and the framebuffer it allocates is *"the
display's size minus overscan"* - the wording in the firmware's own `config.txt`
template. On 1080p that is 1824x984, not 1920x1080. If a size comes back short,
look for `disable_overscan=1` before suspecting the code.

## Licence

**GNU General Public License version 3.0**, in `LICENSE`.

A more permissive licence is not available for the image, and not by choice: the
kernel statically links Circle (GPL-3.0) and DOSBox-core (GPL-2.0-or-later), so
the combined binary must be GPL-3.0 and, if it is distributed, its source must be
available. This repository is that source. `NOTICE` has the copyright line, the
reasoning, and the licence of every component that ends up in the image;
`LICENSE` is the verbatim GPL-3.0 text, nothing else.

## Credits

* **DOSBox-core** - the DOSBox Team, and Nikos Chantziaras for the libretro
  port. <https://github.com/libretro/dosbox-core>
* **DOSBox** - the DOSBox Team. <https://www.dosbox.com>
* **Circle** - Rene Stange, the bare metal C++ environment for the Raspberry Pi.
  <https://github.com/rsta2/circle>
* **circle-stdlib** - Stephan Muehlstrasser. <https://github.com/smuehlst/circle-stdlib>
* Everything written for this port - **@mmoroca** & [arena.ai](https://arena.ai),
  2026.

### The clock on a Pi 3

On a Pi 3 the firmware owns the ARM clock: 600 MHz, 1200 MHz and 1.4 GHz are its
own steps, and it moves between them by itself. The port asks for a rate and the
firmware may grant something else - the panel shows what the clock *is*, not what
was asked for. If it reads 600 MHz while the emulator is clearly busy, the game
runs at about the rate the clock allows and the sound stutters.

The governor's own thermal veto is not the cause when the panel shows 60 C or
less: it starts at 80 C, and `socmaxtemp=` in `cmdline.txt` can only *lower* that
(the accepted range is 40-78). Setting `cpu_max_mhz` in `boot.conf` removes the
gap between the governor's levels, which makes the fault easier to see but does
not fix it.

### Circle patches

`patches/` holds fixes for upstream code, and **they are applied automatically**
by `scripts/setup.sh`, which is the step that clones and configures
circle-stdlib. Applying a patch twice is not an error: the script asks
`git apply --reverse --check` first and skips what is already in the tree, so
re-running `setup.sh` on an existing checkout is safe.

That changed on purpose. Until .74 this section said "applied by hand", with the
commands below, and the consequence was that a `git clone` followed by a plain
`make` produced a kernel that stops on a Pi 3 - and nothing said why.

`patches/circle-rpi3-usbstring.patch` fixes a Circle abort on the Pi 3. On a Pi 3
`CUSBString::GetFromDescriptor()` asks for a 4-byte string descriptor, and a USB
device that answers with an empty one (valid, and accepted by the check just
above) then fails `assert (m_pUSBString->bLength > 2)` in
`lib/usb/usbstring.cpp`, which halts the whole system - a red
`assertion failed` on screen after the device enumeration. The code path only
exists for `RASPPI <= 3`, so a Pi 4 never runs it. The patch turns the assertion
into the "no name" answer the caller already handles.

You do not have to do anything: `./scripts/setup.sh` applies it. If you want to
check that it landed:

```bash
git -C third_party/circle-stdlib/libs/circle \
    apply --reverse --check patches/circle-rpi3-usbstring.patch && echo "applied"
```

The patch applies to Circle Step51.1.1, the revision circle-stdlib carries at the
time of writing. If Circle moves and a patch no longer fits, `setup.sh` stops
with the exact `git apply --check` command to run, instead of building a kernel
that is missing the fix.

## Acknowledgements

Inspired by the work of [@rtomasa](https://github.com/rtomasa) on Pi-DOS
(<https://github.com/rtomasa/Pi-DOS>), a DOSBox for the Raspberry Pi running
under Linux, and by the challenge proposed by [@shaeon](https://github.com/shaeon),
author of ZXMetalCircle (<https://github.com/shaeon/ZXMetalCircle>).

No code is copied from either project. What they gave this one is the idea that a
Raspberry Pi can run a whole computer's worth of software with no operating
system underneath, and the reason to try it with DOSBox.
