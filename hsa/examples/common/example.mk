# example.mk - build rules shared by the examples.
#
# An example's Makefile sets NAME (and optionally KERNEL_FLAGS) and includes
# this file.  Building needs only the gcn4xp SDK: GCN4XP_SDK_HOME must name
# the directory it was extracted to.  Works with the SDK's make from cmd.exe
# as well as from a POSIX shell.
#
#   make          host program NAME.exe and GPU code object NAME.co
#   make run      build, then run it (it loads NAME.co from here)
#   make clean

ifeq ($(strip $(GCN4XP_SDK_HOME)),)
$(error Set GCN4XP_SDK_HOME to the directory the gcn4xp SDK was extracted to)
endif
SDK := $(subst \,/,$(GCN4XP_SDK_HOME))

CC  := "$(SDK)/bin/clang.exe"
CXX := "$(SDK)/bin/clang++.exe"

# GPU code: the Polaris instruction set, no device libraries
GPU_ARCH   ?= gfx803
GPU_FLAGS  := --target=amdgcn-amd-amdhsa -mcpu=$(GPU_ARCH) -nogpulib
# host code: the SDK's clang targets XP by default
CXXFLAGS   ?= -O2 -Wall
HSA_LIBS   := -lhsa-runtime -lgdi32 -luser32 -ladvapi32

COMMON     := ../common
HOST_SRCS  := $(NAME).cpp $(COMMON)/dispatch.cpp $(COMMON)/half.cpp
KERNEL_SRC := $(firstword $(wildcard $(NAME).s $(NAME).cl))

# cmd.exe or a POSIX shell?  (cmd expands %ComSpec%, sh leaves it alone)
ifneq ($(findstring cmd,$(shell echo %ComSpec%)),)
RM_F := del /q
HERE := $(subst /,\,./)
else
RM_F := rm -f
HERE := ./
endif

all: $(NAME).exe $(NAME).co

$(NAME).exe: $(HOST_SRCS) $(wildcard $(COMMON)/*.h $(COMMON)/*.hpp)
	$(CXX) $(CXXFLAGS) -I$(COMMON) -o $@ $(HOST_SRCS) $(HSA_LIBS)

# clang assembles or compiles the kernel and links the code object
$(NAME).co: $(KERNEL_SRC)
	$(CC) $(GPU_FLAGS) $(KERNEL_FLAGS) -o $@ $<

run: all
	$(HERE)$(NAME).exe

clean:
	-$(RM_F) $(NAME).exe $(NAME).co

.PHONY: all run clean
