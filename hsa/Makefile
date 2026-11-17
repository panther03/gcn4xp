# Builds the gcn4xp compute runtime (libhsa-runtime.a) and its tools with a
# 32-bit MinGW-w64 GCC (msvcrt), e.g. MSYS2's mingw32 toolchain on PATH.
# The SDK script (../sdk/prepare-sdk.sh) runs it; see README.md.
#
#   make                       build/libhsa-runtime.a, build/gcngpu.exe,
#                              build/hsatest.exe
#   make install PREFIX=<dir>  <dir>/lib, <dir>/include/hsa, <dir>/bin
CC      := gcc
AR      := ar
# GCC 15's late combine pass miscompiles some 64-bit arithmetic on i686
# (see ../sdk/gcc-check.c); turn it off where the compiler has it
GCC_FIX ?= $(shell echo 'int x;' | $(CC) -fno-late-combine-instructions \
             -fsyntax-only -x c - 2>/dev/null && \
             echo -fno-late-combine-instructions)
CFLAGS  := -O2 -Wall $(GCC_FIX)
# only what XP provides; MinGW's printf (msvcrt's has no %lld/%llx on XP)
CPPFLAGS := -Isrc -Iinclude -I../include -D_WIN32_WINNT=0x0501 -DWINVER=0x0501 \
            -D__USE_MINGW_ANSI_STDIO=1
LDFLAGS := -static
LDLIBS  := -lgdi32 -luser32 -ladvapi32

CORE = build/gcn_dev.o build/gcn_gpu.o
RT   = build/hsa_rt.o build/hsa_loader.o
HEADERS = $(wildcard src/*.h) $(wildcard include/hsa/*.h) ../include/gcn_shared.h

all: build/libhsa-runtime.a build/gcngpu.exe build/hsatest.exe

build/%.o: src/%.c $(HEADERS) | build
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

build/libhsa-runtime.a: $(CORE) $(RT)
	rm -f $@
	$(AR) rcs $@ $^

build/gcngpu.exe: tools/gcngpu.c $(CORE)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

build/hsatest.exe: tools/hsatest.c build/libhsa-runtime.a
	$(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

build:
	mkdir -p $@

install: all
	test -n "$(PREFIX)"
	mkdir -p "$(PREFIX)/lib" "$(PREFIX)/include/hsa" "$(PREFIX)/bin"
	cp build/libhsa-runtime.a "$(PREFIX)/lib/"
	cp include/hsa/*.h "$(PREFIX)/include/hsa/"
	cp build/gcngpu.exe build/hsatest.exe "$(PREFIX)/bin/"

clean:
	rm -rf build

.PHONY: all install clean
