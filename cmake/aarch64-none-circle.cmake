# cmake/aarch64-none-circle.cmake
#
# BM-DOS - CMake toolchain file for the external libraries linked into the
# kernel (Munt and FluidSynth).  Adapted from mt32-pi by Dale Whinham
# (https://github.com/dwhinham/mt32-pi), GPL-3.0.
#
# The compiler flags are NOT set here: scripts/build-midi-libs.sh derives them
# from circle-stdlib's Config.mk (CPU, defines, and the libc++ include paths),
# so that these libraries are compiled against exactly the same C++ library as
# DOSBox-core.  Mixing libstdc++ headers into a libc++ image is an ODR problem
# that does not show up at link time.
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# There is no OS to link a test program against: let try_compile() build a
# static library instead of an executable.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_C_COMPILER   aarch64-none-elf-gcc)
set(CMAKE_CXX_COMPILER aarch64-none-elf-g++)
set(CMAKE_AR           aarch64-none-elf-gcc-ar)
set(CMAKE_RANLIB       aarch64-none-elf-gcc-ranlib)

# Never pick up host headers or libraries.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
