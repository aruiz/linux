# SPDX-License-Identifier: GPL-2.0
#
# arch/eml/Makefile - EFI Mode Linux top-level arch Makefile
#
# EML compiles the kernel as a UEFI application (PE/COFF binary).
# The underlying CPU is x86_64 but we never touch hardware directly;
# all I/O goes through UEFI Boot Services protocols.
#

KBUILD_DEFCONFIG := defconfig

HEADER_ARCH := x86

# x86_64 UEFI application compiler flags
KBUILD_CFLAGS += -m64 -mcmodel=small -mno-red-zone \
		 -fshort-wchar -mno-mmx -mno-sse \
		 -ffreestanding -fno-stack-protector \
		 -fno-strict-aliasing \
		 -fno-asynchronous-unwind-tables \
		 -fPIE -fvisibility=hidden \
		 -D__EML__ -D__KERNEL__

KBUILD_AFLAGS += -m64 -D__EML__

KBUILD_LDFLAGS += -m elf_x86_64 --no-undefined --emit-relocs

PHONY += linux.efi

all: linux.efi

# The vmlinux ELF is converted to a flat binary which is the PE/COFF image
# (the PE headers are embedded in the .head section by head.S)
quiet_cmd_efi = EFI     $@
      cmd_efi = mkdir -p $(dir $@) && $(OBJCOPY) -O binary $< $@ && \
		truncate -s %512 $@ && \
		python3 $(srctree)/scripts/gen-pe-relocs.py $< $@

arch/eml/boot/linux.efi: vmlinux FORCE
	$(call if_changed,efi)

linux.efi: arch/eml/boot/linux.efi

define archhelp
  echo '* linux.efi     - EFI application binary (arch/eml/boot/linux.efi)'
endef

CLEAN_FILES += arch/eml/boot/linux.efi
