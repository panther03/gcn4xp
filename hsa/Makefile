# Builds the gcn4xp compute runtime and its tools with GCC on the Windows XP
# machine itself (Cygwin or MinGW).
#
#   make                     runtime library and gcngpu
#   make examples EXAMPLES=<path to LLVM-AMDGPU-Assembler-Extra/examples>
#                            host programs of the assembler examples; the
#                            kernels (.co) are built elsewhere, see
#                            README.md
CC      ?= gcc
CXX     ?= g++
AR      ?= ar
CFLAGS  ?= -O2 -Wall
CXXFLAGS ?= -O2 -Wall -std=gnu++11
CPPFLAGS = -Isrc -Iinclude -I../include
LDLIBS   = -lgdi32 -luser32 -ladvapi32

CORE = build/gcn_dev.o build/gcn_gpu.o
RT   = build/hsa_rt.o build/hsa_loader.o

all: build/gcngpu.exe build/libhsa-runtime.a build/hsatest.exe

build/hsatest.exe: tools/hsatest.c build/libhsa-runtime.a
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $^ $(LDLIBS)

build/%.o: src/%.c $(wildcard src/*.h) $(wildcard include/*.h) \
           ../include/gcn_shared.h | build
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

build/libhsa-runtime.a: $(CORE) $(RT)
	rm -f $@
	$(AR) rcs $@ $^

build/gcngpu.exe: tools/gcngpu.c $(CORE)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $^ $(LDLIBS)

# ---- examples ------------------------------------------------------------

EXAMPLE_PROGRAMS = asm-kernel dpp_reduce ds_bpermute fp16_native \
                   fp16_storage s_memrealtime s_memrealtime_inline
EXAMPLE_COMMON = build/ex/dispatch.o build/ex/half.o

examples: $(addprefix build/ex/,$(addsuffix .exe,$(EXAMPLE_PROGRAMS)))

check-examples-dir:
	@test -n "$(EXAMPLES)" -a -f "$(EXAMPLES)/common/dispatch.cpp" || \
	    { echo "set EXAMPLES to the examples directory of" \
	           "LLVM-AMDGPU-Assembler-Extra"; exit 1; }

build/ex/half.o: check-examples-dir | build/ex
	$(CXX) $(CXXFLAGS) -Iinclude -I"$(EXAMPLES)/common" -c -o $@ \
	    "$(EXAMPLES)/common/half.cpp"

# The kernels take 64-bit pointers, but dispatch.cpp passes a buffer as a
# void *, which is 4 bytes in a 32-bit process.  Compile a copy that passes
# it as a 64-bit integer instead.
build/ex/dispatch.cpp: check-examples-dir | build/ex
	sed 's/^\( *\)void\* localPtr = buffer->LocalPtr();/\1uint64_t localPtr = (uintptr_t)buffer->LocalPtr();/' \
	    "$(EXAMPLES)/common/dispatch.cpp" > $@
	grep -q "uint64_t localPtr" $@

build/ex/dispatch.o: build/ex/dispatch.cpp
	$(CXX) $(CXXFLAGS) -Iinclude -I"$(EXAMPLES)/common" -c -o $@ $<

build/ex/asm-kernel.exe: $(EXAMPLE_COMMON) build/libhsa-runtime.a
	$(CXX) $(CXXFLAGS) -Iinclude -I"$(EXAMPLES)/common" -o $@ \
	    "$(EXAMPLES)/asm-kernel/asm-kernel.cpp" $(EXAMPLE_COMMON) \
	    build/libhsa-runtime.a $(LDLIBS)

build/ex/%.exe: $(EXAMPLE_COMMON) build/libhsa-runtime.a
	$(CXX) $(CXXFLAGS) -Iinclude -I"$(EXAMPLES)/common" -o $@ \
	    "$(EXAMPLES)/gfx8/$*.cpp" $(EXAMPLE_COMMON) \
	    build/libhsa-runtime.a $(LDLIBS)

build build/ex:
	mkdir -p $@

clean:
	rm -rf build

.PHONY: all clean examples check-examples-dir
