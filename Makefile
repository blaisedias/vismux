CC = gcc
AR = ar
# Used to have -lrt (librt.so) but not needed with modern glibc
LIBS = 
LIBSOSX = 

# Shared compilation parameters across all hardware environments
CFLAGS_COMMON = -Wall -Wextra -O3 -pthread -D_FILE_OFFSET_BITS=64

# Target-Specific Variable Extensions
CFLAGS_NAT     = 
CFLAGS_NATOSX  = -DNODAEMON
CFLAGS_INTEL64 = 
CFLAGS_INTEL32 = 
CFLAGS_INTEL32_OLD = -m32 -DVISMUX_ALL
CFLAGS_VISMUX_ALL = -DVISMUX_ALL
CFLAGS_VISMUX_DEST = -DVISMUX_DEST
CFLAGS_VISMUX_SRC = -DVISMUX_SRC
CFLAGS_VISMUX_DISC = -DVISMUX_DISC

# ARM Cross-Compilers & Architecture Tuning Flags
# (Requires: sudo apt install gcc-arm-linux-gnueabihf gcc-aarch64-linux-gnu)
INTEL32_CC    = i686-linux-gnu-gcc
INTEL32_AR    = i686-linux-gnu-ar
ARM64_CC      = aarch64-linux-gnu-gcc
ARM64_AR      = aarch64-linux-gnu-ar
ARM32_CC      = arm-linux-gnueabihf-gcc
ARM32_AR      = arm-linux-gnueabihf-ar
ARM32V6_CC    = armv6-linux-musleabihf-gcc
ARM32V6_AR    = armv6-linux-musleabihf-ar
ARM32V5_CC    = armel-linux-musleabi-gcc
ARM32V5_AR    = armel-linux-musleabi-ar
CFLAGS_ARM64  = -march=armv8-a
CFLAGS_ARMHF  = -march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=hard
# Use -static for ARMV6 because being built with mus library which might not be on target system
CFLAGS_ARMV6  = -march=armv6 -static -marm -mfpu=vfp -mfloat-abi=hard
CFLAGS_ARMV5  = -march=armv5te -static -marm -mfloat-abi=soft -mthumb -fPIC -O3

# Explicitly Defined Architectural Output Signatures
TARGET_NATIVE  = vismux
TARGET_NATIVEOSX  = vismux-osx
TARGET_X86_64  = vismux-x86_64
TARGET_X86_32  = vismux-x86_32
TARGET_ARM64   = vismux-aarch64
TARGET_ARMHF   = vismux-armhf
TARGET_ARMV6   = vismux-armv6
TARGET_ARMV5   = vismux-armv5

CROSS_ARCHES = x86_64 x86_32 aarch64 armhf armv6 armv5
CROSS_EXECUTABLES = $(foreach arch,$(CROSS_ARCHES),vismux-$(arch) vismux_destination-$(arch) vismux_discover-$(arch) vismux_source-$(arch))

CROSS_CC_x86_64 = $(CC)
CROSS_AR_x86_64 = $(AR)
CROSS_CFLAGS_x86_64 = $(CFLAGS_INTEL64)
CROSS_CC_x86_32 = $(INTEL32_CC)
CROSS_AR_x86_32 = $(INTEL32_AR)
CROSS_CFLAGS_x86_32 = $(CFLAGS_INTEL32)
CROSS_CC_aarch64 = $(ARM64_CC)
CROSS_AR_aarch64 = $(ARM64_AR)
CROSS_CFLAGS_aarch64 = $(CFLAGS_ARM64)
CROSS_CC_armhf = $(ARM32_CC)
CROSS_AR_armhf = $(ARM32_AR)
CROSS_CFLAGS_armhf = $(CFLAGS_ARMHF)
CROSS_CC_armv6 = $(ARM32V6_CC)
CROSS_AR_armv6 = $(ARM32V6_AR)
CROSS_CFLAGS_armv6 = $(CFLAGS_ARMV6)
CROSS_CC_armv5 = $(ARM32V5_CC)
CROSS_AR_armv5 = $(ARM32V5_AR)
CROSS_CFLAGS_armv5 = $(CFLAGS_ARMV5)

.PHONY: all clean $(CROSS_ARCHES)

# Running a bare 'make' compiles the host's native setup
all: native vismux_destination vismux_discover vismux_source

# 0. Native functional targets

# source file dependencies
vismux_destination.c: vismux.h
vismux_source.c: vismux.h
vismux_discover.c: vismux.h
vismux.c: vismux.h

%.o: %.c vismux.h
	$(CC) $(CFLAGS_COMMON) $(CFLAGS_NAT) -I ./ $< -c

vismux.a: source.o discover.o destination.o console.o common.o
	ar rcs $(@) $^
	@echo "[+] Compiled local native static ibrary: $(@)"

# The standalone native executable is a build target in its own right.
vismux: vismux.c vismux.a
	$(CC) $(CFLAGS_COMMON) $(CFLAGS_VISMUX_ALL) $(CFLAGS_NAT) -o $@ $^ $(LIBS)
	@echo "[+] Compiled local native binary: $@"

vismux_destination: vismux_destination.c vismux.a
	$(CC) $(CFLAGS_COMMON) $(CFLAGS_NAT) $(CFLAGS_VISMUX_DEST) -o $(@) $^ $(LIBS)
	@echo "[+] Compiled local native binary: $(@)"

vismux_source: vismux_source.c vismux.a
	$(CC) $(CFLAGS_COMMON) $(CFLAGS_NAT) $(CFLAGS_VISMUX_SRC) -o $(@) $^ $(LIBS)
	@echo "[+] Compiled local native binary: $(@)"

vismux_discover: vismux_discover.c vismux.a
	$(CC) $(CFLAGS_COMMON) $(CFLAGS_NAT) $(CFLAGS_VISMUX_DISC) -o $(@) $^ $(LIBS)
	@echo "[+] Compiled local native binary: $(@)"

# 1. Native Shorthand Target (Matches current host CPU architecture layout)
native: vismux

define CROSS_BUILD
build/$(1):
	mkdir -p $$@

build/$(1)/%.o: %.c vismux.h | build/$(1)
	$$(CROSS_CC_$(1)) $$(CFLAGS_COMMON) $$(CROSS_CFLAGS_$(1)) -I ./ $$< -c -o $$@

build/$(1)/vismux_all.o: vismux.c vismux.h | build/$(1)
	$$(CROSS_CC_$(1)) $$(CFLAGS_COMMON) $$(CFLAGS_VISMUX_ALL) $$(CROSS_CFLAGS_$(1)) -I ./ $$< -c -o $$@

build/$(1)/vismux_dest.o: vismux.c vismux.h | build/$(1)
	$$(CROSS_CC_$(1)) $$(CFLAGS_COMMON) $$(CFLAGS_VISMUX_DEST) $$(CROSS_CFLAGS_$(1)) -I ./ $$< -c -o $$@

build/$(1)/vismux_src.o: vismux.c vismux.h | build/$(1)
	$$(CROSS_CC_$(1)) $$(CFLAGS_COMMON) $$(CFLAGS_VISMUX_SRC) $$(CROSS_CFLAGS_$(1)) -I ./ $$< -c -o $$@

build/$(1)/vismux_disc.o: vismux.c vismux.h | build/$(1)
	$$(CROSS_CC_$(1)) $$(CFLAGS_COMMON) $$(CFLAGS_VISMUX_DISC) $$(CROSS_CFLAGS_$(1)) -I ./ $$< -c -o $$@

build/$(1)/vismux.a: $(addprefix build/$(1)/,source.o discover.o destination.o console.o common.o) | build/$(1)
	$$(CROSS_AR_$(1)) rcs $$@ $$^

vismux-$(1): build/$(1)/vismux_all.o build/$(1)/vismux.a
	$$(CROSS_CC_$(1)) $$(CFLAGS_COMMON) $$(CROSS_CFLAGS_$(1)) -o $$@ $$^ $$(LIBS)

vismux_destination-$(1): build/$(1)/vismux_dest.o build/$(1)/vismux.a
	$$(CROSS_CC_$(1)) $$(CFLAGS_COMMON) $$(CROSS_CFLAGS_$(1)) -o $$@ $$^ $$(LIBS)

vismux_discover-$(1): build/$(1)/vismux_disc.o build/$(1)/vismux.a
	$$(CROSS_CC_$(1)) $$(CFLAGS_COMMON) $$(CROSS_CFLAGS_$(1)) -o $$@ $$^ $$(LIBS)

vismux_source-$(1): build/$(1)/vismux_src.o build/$(1)/vismux.a
	$$(CROSS_CC_$(1)) $$(CFLAGS_COMMON) $$(CROSS_CFLAGS_$(1)) -o $$@ $$^ $$(LIBS)

$(1): vismux-$(1) vismux_destination-$(1) vismux_discover-$(1) vismux_source-$(1)
	@echo "[+] Compiled all $(1) binaries"
endef

$(foreach arch,$(CROSS_ARCHES),$(eval $(call CROSS_BUILD,$(arch))))

# 7. native on OSX
nativeosx: vismux.c vismux.a
	$(CC) $(CFLAGS_COMMON) $(CFLAGS_VISMUX_ALL) $(CFLAGS_NATOSX) -o $(TARGET_NATIVEOSX) $^ $(LIBSOSX)
	@echo "[+] Compiled local native binary: $(TARGET_NATIVEOSX)"
	
clean:
	rm -f $(TARGET_NATIVE) $(TARGET_X86_64) $(TARGET_X86_32) $(TARGET_ARM64) $(TARGET_ARMHF) $(TARGET_ARMV6) $(TARGET_ARMV5) $(CROSS_EXECUTABLES) \
		vismux_destination vismux_discover vismux_source \
		*.o *.a
	rm -rf build
