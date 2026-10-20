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
KBUILD_CFLAGS += -m64 -mcmodel=small -fno-red-zone \
		 -fshort-wchar -mno-mmx -mno-sse \
		 -ffreestanding -fno-stack-protector \
		 -fno-strict-aliasing \
		 -fno-asynchronous-unwind-tables \
		 -D__EML__ -D__KERNEL__

KBUILD_AFLAGS += -m64 -D__EML__

KBUILD_LDFLAGS += -m elf_x86_64 --no-undefined

core-y += arch/eml/kernel/

PHONY += linux.efi

all: linux.efi

# The vmlinux ELF is converted to a flat binary which is the PE/COFF image
# (the PE headers are embedded in the .head section by head.S)
OBJCOPYFLAGS_linux.efi := -O binary
$(obj)/arch/eml/boot/linux.efi: vmlinux FORCE
	$(call if_changed,objcopy)

linux.efi: $(obj)/arch/eml/boot/linux.efi
	@mkdir -p $(obj)/arch/eml/boot
	@echo '  EFI     $@'

archprepare:
	@mkdir -p $(obj)/arch/eml/boot

define archhelp
  echo '* linux.efi     - EFI application binary (arch/eml/boot/linux.efi)'
endef

CLEAN_FILES += arch/eml/boot/linux.efi
