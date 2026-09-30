CC = gcc
# Used to have -lrt (librt.so) but not needed with modern glibc
LIBS = 
LIBSOSX = 

# Shared compilation parameters across all hardware environments
CFLAGS_COMMON = -Wall -Wextra -O3 -pthread -D_FILE_OFFSET_BITS=64 -g

# Target-Specific Variable Extensions
CFLAGS_NAT     = 
CFLAGS_NATOSX  = -DNOELF
CFLAGS_INTEL64 = 
CFLAGS_INTEL32 = 
CFLAGS_INTEL32_OLD = -m32

# ARM Cross-Compilers & Architecture Tuning Flags
# (Requires: sudo apt install gcc-arm-linux-gnueabihf gcc-aarch64-linux-gnu)
INTEL32_CC    = i686-linux-gnu-gcc
ARM64_CC      = aarch64-linux-gnu-gcc
ARM32_CC      = arm-linux-gnueabihf-gcc
ARM32V6_CC    = armv6-linux-musleabihf-gcc
CFLAGS_ARM64  = -march=armv8-a
CFLAGS_ARMHF  = -march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=hard
# Use -static for ARMV6 because being built with mus library which might not be on target system
CFLAGS_ARMV6  = -march=armv6 -static -marm -mfpu=vfp -mfloat-abi=hard

# Explicitly Defined Architectural Output Signatures
TARGET_NATIVE  = vismux
TARGET_NATIVEOSX  = vismux-osx
TARGET_X86_64  = vismux-x86_64
TARGET_X86_32  = vismux-x86-32
TARGET_ARM64   = vismux-aarch64
TARGET_ARMHF   = vismux-armhf
TARGET_ARMV6   = vismux-armv6

.PHONY: clean

# Running a bare 'make' compiles the host's native setup
all: dvismux discover

# 1. Native Shorthand Target (Matches current host CPU architecture layout)
native: $(TARGET_NATIVE)

vismux_destination.o: vismux_destination.c vismux.h
	$(CC) $(CFLAGS_COMMON) $(CFLAGS_NAT) $< -c

vismux_discover.o: vismux_discover.c vismux.h
	$(CC) $(CFLAGS_COMMON) $(CFLAGS_NAT) $< -c

console.o: console.c vismux.h
	$(CC) $(CFLAGS_COMMON) $(CFLAGS_NAT) $< -c

vismux_common.o: vismux_common.c vismux.h
	$(CC) $(CFLAGS_COMMON) $(CFLAGS_NAT) $< -c


dvismux: dvismux.c vismux_destination.o console.o vismux_common.o
	$(CC) $(CFLAGS_COMMON) $(CFLAGS_NAT) $< -o dvismux $(LIBS) vismux_destination.o console.o vismux_common.o
	@echo "[+] Compiled local native binary: dvismux"

discover: discover.c vismux_discover.o vismux_common.o
	$(CC) $(CFLAGS_COMMON) $(CFLAGS_NAT) $< -o discover $(LIBS) vismux_discover.o vismux_common.o
	@echo "[+] Compiled local native binary: discover"

$(TARGET_NATIVE): vismux.c vismux_destination.o
	$(CC) $(CFLAGS_COMMON) $(CFLAGS_NAT) $< -o $(TARGET_NATIVE) $(LIBS) vismux_destination.o
	@echo "[+] Compiled local native binary: $(TARGET_NATIVE)"

# 2. Intel/AMD 64-bit Explicit Target
x86_64: vismux.c
	$(CC) $(CFLAGS_COMMON) $(CFLAGS_INTEL64) $< -o $(TARGET_X86_64) $(LIBS)
	@echo "[+] Compiled Intel/AMD 64-bit binary: $(TARGET_X86_64)"

# 3. Intel x86 32-bit Target
x86-32: vismux.c
	$(INTEL32_CC) $(CFLAGS_COMMON) $(CFLAGS_INTEL32) $< -o $(TARGET_X86_32) $(LIBS)
	@echo "[+] Compiled Intel 32-bit binary: $(TARGET_X86_32)"

# 4. ARM 64-bit Explicit Target (Raspberry Pi 3/4/5 running 64-bit OS)
aarch64: vismux.c
	$(ARM64_CC) $(CFLAGS_COMMON) $(CFLAGS_ARM64) $< -o $(TARGET_ARM64) $(LIBS)
	@echo "[+] Cross-compiled ARM 64-bit (AArch64) binary: $(TARGET_ARM64)"

# 5. ARMhf 32-bit Target (Raspberry Pi 2/3/4 running legacy 32-bit OS)
armhf: vismux.c
	$(ARM32_CC) $(CFLAGS_COMMON) $(CFLAGS_ARMHF) $< -o $(TARGET_ARMHF) $(LIBS)
	@echo "[+] Cross-compiled ARMhf (v7) binary: $(TARGET_ARMHF)"

# 6. ARMv6 32-bit Target (Raspberry Pi 1 & Raspberry Pi Zero Classic)
armv6: vismux.c
	$(ARM32V6_CC) $(CFLAGS_COMMON) $(CFLAGS_ARMV6) $< -o $(TARGET_ARMV6) $(LIBS)
	@echo "[+] Cross-compiled ARMv6 (Pi Zero/1) binary: $(TARGET_ARMV6)"

# 7. native on OSX
nativeosx: vismux.c
	$(CC) $(CFLAGS_COMMON) $(CFLAGS_NATOSX) $< -o $(TARGET_NATIVEOSX) $(LIBSOSX)
	@echo "[+] Compiled local native binary: $(TARGET_NATIVEOSX)"
	
clean:
	rm -f $(TARGET_NATIVE) $(TARGET_X86_64) $(TARGET_X86_32) $(TARGET_ARM64) $(TARGET_ARMHF) $(TARGET_ARMV6) *.o dvismux discover
