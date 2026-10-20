#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Test EML's UEFI chainloader — boots the host kernel + a minimal
# initramfs via EML's LoadImage/StartImage chainloader.
#
# Requires: host kernel with CONFIG_EFI_STUB, gcc, cpio, OVMF, QEMU.
#
set -e

OVMF=/usr/share/edk2/ovmf/OVMF.stateless.fd
TIMEOUT=${TIMEOUT:-30}
HOST_VMLINUZ=/boot/vmlinuz-$(uname -r)
TMPDIR=$(mktemp -d /tmp/eml-boot-XXXXXX)
trap 'rm -rf "$TMPDIR"' EXIT

if [ ! -f "$OVMF" ]; then
	echo "error: OVMF firmware not found at $OVMF" >&2
	exit 1
fi

if [ ! -f "$HOST_VMLINUZ" ]; then
	echo "error: host kernel not found at $HOST_VMLINUZ" >&2
	exit 1
fi

echo "=== Building minimal init ==="
cat > "$TMPDIR/init.S" << 'INITEOF'
.globl _start
_start:
	mov $1, %rax
	mov $1, %rdi
	lea msg(%rip), %rsi
	mov $msglen, %rdx
	syscall
	mov $169, %rax
	mov $0xfee1dead, %rdi
	mov $0x28121969, %rsi
	mov $0x4321fedc, %rdx
	xor %r10, %r10
	syscall
1:	hlt
	jmp 1b
.section .rodata
msg:	.ascii "### EML chainboot init running! ###\n"
msglen = . - msg
INITEOF
gcc -nostdlib -static -o "$TMPDIR/init" "$TMPDIR/init.S"
(cd "$TMPDIR" && echo init | cpio -o -H newc 2>/dev/null) > "$TMPDIR/initramfs.img"

echo "=== Building EML with chainloader cmdline ==="
ORIG_CMDLINE=$(grep '^CONFIG_EML_CMDLINE=' .config)
sed -i 's|^CONFIG_EML_CMDLINE=.*|CONFIG_EML_CMDLINE="eml.kernel=/vmlinuz eml.initrd=/initramfs.img eml.bootargs=console=ttyS0 console=ttyEML0"|' .config
make ARCH=eml -j"$(nproc)" 2>&1 | tail -3

echo "=== Creating ESP ==="
IMG="$TMPDIR/esp.img"
dd if=/dev/zero of="$IMG" bs=1M count=128 2>/dev/null
mkfs.vfat -F 32 "$IMG" >/dev/null 2>&1
mmd -i "$IMG" ::EFI ::EFI/BOOT
mcopy -i "$IMG" arch/eml/boot/linux.efi ::EFI/BOOT/BOOTX64.EFI
mcopy -i "$IMG" "$HOST_VMLINUZ" ::vmlinuz
mcopy -i "$IMG" "$TMPDIR/initramfs.img" ::initramfs.img

echo "=== Booting (timeout ${TIMEOUT}s) ==="
timeout "$TIMEOUT" qemu-system-x86_64 \
	-bios "$OVMF" \
	-drive file="$IMG",format=raw \
	-nographic -no-reboot -m 1G \
	-serial file:"$TMPDIR/serial.log" \
	> /dev/null 2>&1 || true

echo "=== Restoring config ==="
sed -i "s|^CONFIG_EML_CMDLINE=.*|$ORIG_CMDLINE|" .config
make ARCH=eml -j"$(nproc)" > /dev/null 2>&1

OUTPUT=$(cat "$TMPDIR/serial.log")
PASS=0
FAIL=0

check() {
	if echo "$OUTPUT" | grep -q "$1"; then
		echo "PASS: $1"
		PASS=$((PASS + 1))
	else
		echo "FAIL: expected '$1' in output" >&2
		FAIL=$((FAIL + 1))
	fi
}

check "EML boot: loading kernel from"
check "EML boot: kernel loaded"
check "EML boot: loading initrd from"
check "EML boot: initrd loaded"
check "EML boot: starting kernel"
check "Kernel command line: console=ttyS0"
check "unpack rootfs image as initramfs"
check "Run /init"
check "EML chainboot init running"

echo "---"
echo "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
